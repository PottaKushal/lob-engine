// v1 and v2: the textbook node-based order book.
//
// Orders live in a std::list per price level (one heap allocation per order)
// and are found by id through a std::unordered_map (another allocation per
// order). The two versions differ only in how price levels are located:
//
//   BookV1 = ListBook<MapLevels>   std::map keyed by price (red-black tree)
//   BookV2 = ListBook<FlatLevels>  std::vector indexed directly by price
//
// Keeping everything else identical isolates the cost of the level container.
#pragma once

#include <algorithm>
#include <cstdint>
#include <list>
#include <map>
#include <unordered_map>
#include <vector>

#include "lob/types.hpp"

namespace lob {

struct ListOrder {
    OrderId id;
    OwnerId owner;
    Qty qty;
    Price price;
    Side side;
};
using OrderList = std::list<ListOrder>;

// Price levels in a balanced tree. Bids are stored under the negated price so
// that begin() is always the best level on either side.
class MapLevels {
public:
    MapLevels(Side side, const BookConfig&) : sign_(side == Side::Buy ? -1 : 1) {}

    OrderList* best() { return m_.empty() ? nullptr : &m_.begin()->second; }
    OrderList& at(Price p) { return m_[sign_ * p]; }
    const OrderList* find(Price p) const {
        auto it = m_.find(sign_ * p);
        return it == m_.end() ? nullptr : &it->second;
    }
    void drop_if_empty(Price p) {
        auto it = m_.find(sign_ * p);
        if (it != m_.end() && it->second.empty()) m_.erase(it);
    }
    // Visits non-empty levels best-first until f returns false.
    template <class F>
    void walk(F&& f) const {
        for (const auto& kv : m_)
            if (!f(kv.second)) return;
    }

private:
    Price sign_;
    std::map<Price, OrderList> m_;
};

// Price levels in a flat array indexed by price, plus a cached best price.
class FlatLevels {
public:
    FlatLevels(Side side, const BookConfig& cfg)
        : buy_(side == Side::Buy),
          max_(cfg.max_price),
          best_(buy_ ? 0 : cfg.max_price + 1),
          v_(static_cast<std::size_t>(cfg.max_price) + 2) {}

    OrderList* best() { return has_best() ? &v_[idx(best_)] : nullptr; }
    // The caller always inserts into the returned level, so best_ may be updated here.
    OrderList& at(Price p) {
        best_ = buy_ ? std::max(best_, p) : std::min(best_, p);
        return v_[idx(p)];
    }
    const OrderList* find(Price p) const { return v_[idx(p)].empty() ? nullptr : &v_[idx(p)]; }
    void drop_if_empty(Price p) {
        if (p != best_ || !v_[idx(p)].empty()) return;
        if (buy_) {
            while (best_ >= 1 && v_[idx(best_)].empty()) --best_;
        } else {
            while (best_ <= max_ && v_[idx(best_)].empty()) ++best_;
        }
    }
    template <class F>
    void walk(F&& f) const {
        if (buy_) {
            for (Price p = best_; p >= 1; --p)
                if (!v_[idx(p)].empty() && !f(v_[idx(p)])) return;
        } else {
            for (Price p = best_; p <= max_; ++p)
                if (!v_[idx(p)].empty() && !f(v_[idx(p)])) return;
        }
    }

private:
    static std::size_t idx(Price p) { return static_cast<std::size_t>(p); }
    bool has_best() const { return buy_ ? best_ >= 1 : best_ <= max_; }

    bool buy_;
    Price max_;
    Price best_;
    std::vector<OrderList> v_;
};

template <class Levels>
class ListBook {
public:
    explicit ListBook(const BookConfig& cfg = {})
        : cfg_(cfg), sides_{Levels(Side::Buy, cfg), Levels(Side::Sell, cfg)} {}

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
        if (index_.find(e.id) != index_.end()) {
            sink(make_out(OutKind::Rejected, Reason::DuplicateId, e.id, e.side, e.price, e.qty));
            return;
        }
        if (can_rest(e.type) && index_.size() >= cfg_.max_orders) {
            sink(make_out(OutKind::Rejected, Reason::BookFull, e.id, e.side, e.price, e.qty));
            return;
        }
        enter(e, sink);
    }

    template <class Sink>
    void cancel(OrderId id, Sink&& sink) {
        auto it = index_.find(id);
        if (it == index_.end()) {
            sink(make_out(OutKind::Rejected, Reason::UnknownOrder, id, Side::Buy, 0, 0));
            return;
        }
        const ListOrder o = *it->second;
        remove(it);
        sink(make_out(OutKind::Cancelled, Reason::UserCancel, id, o.side, o.price, o.qty));
    }

    template <class Sink>
    void modify(const InEvent& e, Sink&& sink) {
        auto it = index_.find(e.id);
        if (it == index_.end()) {
            sink(make_out(OutKind::Rejected, Reason::UnknownOrder, e.id, Side::Buy, 0, 0));
            return;
        }
        ListOrder& o = *it->second;
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
        remove(it);
        enter(re, sink);
    }

    // --- introspection -----------------------------------------------------
    Price best_bid() const { return best_price(Side::Buy); }
    Price best_ask() const { return best_price(Side::Sell); }
    std::uint64_t level_qty(Side s, Price p) const {
        std::uint64_t t = 0;
        if (p < 1 || p > cfg_.max_price) return 0;
        if (const OrderList* l = side(s).find(p))
            for (const ListOrder& o : *l) t += o.qty;
        return t;
    }
    Qty open_qty(OrderId id) const {
        auto it = index_.find(id);
        return it == index_.end() ? 0 : it->second->qty;
    }
    std::size_t order_count() const { return index_.size(); }

    template <class F>
    void for_each_order(F&& f) const {
        for (Side s : {Side::Buy, Side::Sell})
            side(s).walk([&](const OrderList& l) {
                for (const ListOrder& o : l) f(o.side, o.price, o.id, o.qty, o.owner);
                return true;
            });
    }

private:
    using Index = std::unordered_map<OrderId, OrderList::iterator>;

    Levels& side(Side s) { return sides_[static_cast<std::size_t>(s)]; }
    const Levels& side(Side s) const { return sides_[static_cast<std::size_t>(s)]; }

    Price best_price(Side s) const {
        Price p = 0;
        side(s).walk([&](const OrderList& l) {
            p = l.front().price;
            return false;
        });
        return p;
    }
    static bool acceptable(Side taker, Price limit, Price maker_px) {
        return taker == Side::Buy ? maker_px <= limit : maker_px >= limit;
    }
    void remove(typename Index::iterator it) {
        const Side s = it->second->side;
        const Price p = it->second->price;
        side(s).at(p).erase(it->second);
        side(s).drop_if_empty(p);
        index_.erase(it);
    }

    template <class Sink>
    void enter(const InEvent& e, Sink&& sink) {
        const Price limit = sweep_limit(cfg_, e);
        Levels& opp = side(opposite(e.side));

        if (e.type == OrderType::PostOnly) {
            const OrderList* b = opp.best();
            if (b && acceptable(e.side, limit, b->front().price)) {
                sink(make_out(OutKind::Cancelled, Reason::PostOnlyCross, e.id, e.side, e.price, e.qty));
                return;
            }
        }
        if (e.type == OrderType::FOK) {
            std::uint64_t avail = 0;
            opp.walk([&](const OrderList& l) {
                if (!acceptable(e.side, limit, l.front().price)) return false;
                for (const ListOrder& o : l) {
                    if (e.owner != 0 && o.owner == e.owner) return false;
                    avail += o.qty;
                    if (avail >= e.qty) return false;
                }
                return true;
            });
            if (avail < e.qty) {
                sink(make_out(OutKind::Cancelled, Reason::FokUnfillable, e.id, e.side, e.price, e.qty));
                return;
            }
        }

        Qty rem = e.qty;
        bool self = false;
        while (rem > 0 && !self) {
            OrderList* lv = opp.best();
            if (!lv) break;
            const Price px = lv->front().price;
            if (!acceptable(e.side, limit, px)) break;
            while (rem > 0 && !lv->empty()) {
                ListOrder& m = lv->front();
                if (e.owner != 0 && m.owner == e.owner) {
                    self = true;
                    break;
                }
                const Qty fill = std::min(rem, m.qty);
                sink(make_out(OutKind::Trade, Reason::None, e.id, e.side, px, fill, m.id));
                m.qty -= fill;
                rem -= fill;
                if (m.qty == 0) {
                    index_.erase(m.id);
                    lv->pop_front();
                }
            }
            opp.drop_if_empty(px);
        }
        if (rem == 0) return;
        if (self) {
            sink(make_out(OutKind::Cancelled, Reason::SelfTrade, e.id, e.side, e.price, rem));
        } else if (can_rest(e.type)) {
            OrderList& l = side(e.side).at(e.price);
            l.push_back(ListOrder{e.id, e.owner, rem, e.price, e.side});
            index_.emplace(e.id, std::prev(l.end()));
            sink(make_out(OutKind::Accepted, Reason::None, e.id, e.side, e.price, rem));
        } else {
            sink(make_out(OutKind::Cancelled, Reason::Unfilled, e.id, e.side, e.price, rem));
        }
    }

    BookConfig cfg_;
    Levels sides_[2];
    Index index_;
};

using BookV1 = ListBook<MapLevels>;
using BookV2 = ListBook<FlatLevels>;

}  // namespace lob
