// Reference implementation used only by tests.
//
// Deliberately the simplest thing that can be correct: every resting order
// lives in one unsorted vector, and every decision is made by scanning it.
// There is no index, no per-level structure and no cached best price, so it
// shares no data-structure logic with the fast books. O(n) per operation.
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "lob/types.hpp"

namespace lob {

class ReferenceBook {
public:
    explicit ReferenceBook(const BookConfig& cfg = {}) : cfg_(cfg) {}

    template <class Sink>
    void apply(const InEvent& e, Sink&& sink) {
        switch (e.kind) {
            case InKind::New: submit(e, sink); break;
            case InKind::Cancel: cancel(e.id, sink); break;
            case InKind::Modify: modify(e, sink); break;
        }
    }

    template <class Sink>
    void submit(const InEvent& e, Sink&& sink) {
        if (Reason r = validate_new(cfg_, e); r != Reason::None) {
            sink(make_out(OutKind::Rejected, r, e.id, e.side, e.price, e.qty));
            return;
        }
        if (find(e.id) >= 0) {
            sink(make_out(OutKind::Rejected, Reason::DuplicateId, e.id, e.side, e.price, e.qty));
            return;
        }
        if (can_rest(e.type) && orders_.size() >= cfg_.max_orders) {
            sink(make_out(OutKind::Rejected, Reason::BookFull, e.id, e.side, e.price, e.qty));
            return;
        }
        enter(e, sink);
    }

    template <class Sink>
    void cancel(OrderId id, Sink&& sink) {
        const int i = find(id);
        if (i < 0) {
            sink(make_out(OutKind::Rejected, Reason::UnknownOrder, id, Side::Buy, 0, 0));
            return;
        }
        const O o = orders_[static_cast<std::size_t>(i)];
        erase(i);
        sink(make_out(OutKind::Cancelled, Reason::UserCancel, id, o.side, o.price, o.qty));
    }

    template <class Sink>
    void modify(const InEvent& e, Sink&& sink) {
        const int i = find(e.id);
        if (i < 0) {
            sink(make_out(OutKind::Rejected, Reason::UnknownOrder, e.id, Side::Buy, 0, 0));
            return;
        }
        O& o = orders_[static_cast<std::size_t>(i)];
        if (e.qty == 0) {
            sink(make_out(OutKind::Rejected, Reason::BadQty, e.id, o.side, e.price, e.qty));
            return;
        }
        if (e.price < 1 || e.price > cfg_.max_price) {
            sink(make_out(OutKind::Rejected, Reason::BadPrice, e.id, o.side, e.price, e.qty));
            return;
        }
        if (e.price == o.price && e.qty <= o.qty) {
            o.qty = e.qty;
            sink(make_out(OutKind::Modified, Reason::None, e.id, o.side, o.price, o.qty));
            return;
        }
        InEvent re = e;
        re.kind = InKind::New;
        re.type = OrderType::Limit;
        re.side = o.side;
        re.owner = o.owner;
        sink(make_out(OutKind::Cancelled, Reason::Replaced, o.id, o.side, o.price, o.qty));
        erase(i);
        enter(re, sink);
    }

    // --- introspection -----------------------------------------------------
    Price best_bid() const {
        Price b = 0;
        for (const O& o : orders_)
            if (o.side == Side::Buy) b = std::max(b, o.price);
        return b;
    }
    Price best_ask() const {
        Price a = 0;
        for (const O& o : orders_)
            if (o.side == Side::Sell && (a == 0 || o.price < a)) a = o.price;
        return a;
    }
    std::uint64_t level_qty(Side s, Price p) const {
        std::uint64_t t = 0;
        for (const O& o : orders_)
            if (o.side == s && o.price == p) t += o.qty;
        return t;
    }
    Qty open_qty(OrderId id) const {
        const int i = find(id);
        return i < 0 ? 0 : orders_[static_cast<std::size_t>(i)].qty;
    }
    std::size_t order_count() const { return orders_.size(); }

    // Visits bids best-first then asks best-first, each in time priority.
    template <class F>
    void for_each_order(F&& f) const {
        for (Side s : {Side::Buy, Side::Sell})
            for (const O& o : sorted(s)) f(o.side, o.price, o.id, o.qty, o.owner);
    }

private:
    struct O {
        OrderId id;
        OwnerId owner;
        Side side;
        Price price;
        Qty qty;
        std::uint64_t seq;
    };

    int find(OrderId id) const {
        for (std::size_t i = 0; i < orders_.size(); ++i)
            if (orders_[i].id == id) return static_cast<int>(i);
        return -1;
    }
    void erase(int i) { orders_.erase(orders_.begin() + i); }

    static bool better(const O& a, const O& b) {
        if (a.price != b.price) return a.side == Side::Buy ? a.price > b.price : a.price < b.price;
        return a.seq < b.seq;
    }
    std::vector<O> sorted(Side s) const {
        std::vector<O> v;
        for (const O& o : orders_)
            if (o.side == s) v.push_back(o);
        std::sort(v.begin(), v.end(), better);
        return v;
    }
    static bool acceptable(Side taker, Price limit, Price maker_px) {
        return taker == Side::Buy ? maker_px <= limit : maker_px >= limit;
    }
    // Index of the highest-priority resting order the taker may trade with.
    int best_maker(Side taker, Price limit) const {
        int best = -1;
        for (std::size_t i = 0; i < orders_.size(); ++i) {
            const O& o = orders_[i];
            if (o.side == taker || !acceptable(taker, limit, o.price)) continue;
            if (best < 0 || better(o, orders_[static_cast<std::size_t>(best)])) best = static_cast<int>(i);
        }
        return best;
    }

    template <class Sink>
    void enter(const InEvent& e, Sink&& sink) {
        const Price limit = sweep_limit(cfg_, e);
        if (e.type == OrderType::PostOnly && best_maker(e.side, limit) >= 0) {
            sink(make_out(OutKind::Cancelled, Reason::PostOnlyCross, e.id, e.side, e.price, e.qty));
            return;
        }
        if (e.type == OrderType::FOK) {
            std::uint64_t avail = 0;
            for (const O& o : sorted(opposite(e.side))) {
                if (!acceptable(e.side, limit, o.price)) break;
                if (e.owner != 0 && o.owner == e.owner) break;
                avail += o.qty;
            }
            if (avail < e.qty) {
                sink(make_out(OutKind::Cancelled, Reason::FokUnfillable, e.id, e.side, e.price, e.qty));
                return;
            }
        }
        Qty rem = e.qty;
        bool self = false;
        while (rem > 0) {
            const int i = best_maker(e.side, limit);
            if (i < 0) break;
            O& m = orders_[static_cast<std::size_t>(i)];
            if (e.owner != 0 && m.owner == e.owner) {
                self = true;
                break;
            }
            const Qty fill = std::min(rem, m.qty);
            sink(make_out(OutKind::Trade, Reason::None, e.id, e.side, m.price, fill, m.id));
            m.qty -= fill;
            rem -= fill;
            if (m.qty == 0) erase(i);
        }
        if (rem == 0) return;
        if (self) {
            sink(make_out(OutKind::Cancelled, Reason::SelfTrade, e.id, e.side, e.price, rem));
        } else if (can_rest(e.type)) {
            orders_.push_back(O{e.id, e.owner, e.side, e.price, rem, next_seq_++});
            sink(make_out(OutKind::Accepted, Reason::None, e.id, e.side, e.price, rem));
        } else {
            sink(make_out(OutKind::Cancelled, Reason::Unfilled, e.id, e.side, e.price, rem));
        }
    }

    BookConfig cfg_;
    std::vector<O> orders_;
    std::uint64_t next_seq_ = 0;
};

}  // namespace lob
