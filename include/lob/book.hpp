// v3: the production book. No heap allocation after construction.
//
//  * Price levels: one flat array per side indexed directly by price.
//  * Orders: 32-byte nodes in a preallocated pool, linked into a FIFO per
//    level by 32-bit indices (intrusive doubly linked list). Freed nodes go
//    on a free list, so the most recently freed (cache-warm) slot is reused.
//  * Id lookup: preallocated open-addressing hash table (linear probing,
//    backward-shift deletion, so there are no tombstones).
//  * Best prices are cached. When the best level empties the next one is
//    found by a linear scan, or by a two-level bitmap of non-empty levels when
//    UseBitmap is true (measured in BENCHMARKS.md).
#pragma once

#include <algorithm>
#include <bit>
#include <cstdint>
#include <vector>

#include "lob/types.hpp"

namespace lob {

inline constexpr std::uint32_t kNil = 0xFFFFFFFFu;

// Fixed-capacity map from OrderId to a 32-bit pool index.
class IdMap {
public:
    explicit IdMap(std::uint32_t max_items) {
        std::size_t cap = 16;
        while (cap < 2 * static_cast<std::size_t>(max_items)) cap <<= 1;  // load factor <= 0.5
        slots_.assign(cap, Slot{});
        mask_ = cap - 1;
        shift_ = static_cast<unsigned>(64 - std::countr_zero(cap));
    }

    std::uint32_t get(OrderId key) const noexcept {
        for (std::size_t i = home(key);; i = (i + 1) & mask_) {
            const Slot& s = slots_[i];
            if (s.val == kNil) return kNil;
            if (s.key == key) return s.val;
        }
    }
    // Precondition: key absent and the table is not full.
    void put(OrderId key, std::uint32_t val) noexcept {
        std::size_t i = home(key);
        while (slots_[i].val != kNil) i = (i + 1) & mask_;
        slots_[i] = Slot{key, val};
    }
    // Precondition: key present.
    void erase(OrderId key) noexcept {
        std::size_t i = home(key);
        while (slots_[i].key != key || slots_[i].val == kNil) i = (i + 1) & mask_;
        // Backward shift: pull later entries of the same probe run into the hole.
        for (std::size_t j = (i + 1) & mask_; slots_[j].val != kNil; j = (j + 1) & mask_) {
            const std::size_t k = home(slots_[j].key);
            if (((i - k) & mask_) < ((j - k) & mask_)) {  // hole lies on j's probe path
                slots_[i] = slots_[j];
                i = j;
            }
        }
        slots_[i].val = kNil;
    }

private:
    struct Slot {
        OrderId key = 0;
        std::uint32_t val = kNil;
    };
    std::size_t home(OrderId key) const noexcept {
        return static_cast<std::size_t>((key * 0x9E3779B97F4A7C15ull) >> shift_);  // Fibonacci hashing
    }

    std::vector<Slot> slots_;
    std::size_t mask_ = 0;
    unsigned shift_ = 0;
};

template <bool UseBitmap>
class BasicBook {
public:
    explicit BasicBook(const BookConfig& cfg = {})
        : cfg_(cfg), ids_(cfg.max_orders), pool_(cfg.max_orders) {
        const std::size_t n = static_cast<std::size_t>(cfg.max_price) + 2;
        for (auto& l : levels_) l.assign(n, Level{});
        if (UseBitmap) {
            const std::size_t words = (n + 63) / 64;
            for (auto& b : bits_) b.assign(words, 0);
            for (auto& b : sum_) b.assign((words + 63) / 64, 0);
        }
        best_[0] = 0;                  // no bid
        best_[1] = cfg.max_price + 1;  // no ask
        for (std::uint32_t i = 0; i < cfg.max_orders; ++i) pool_[i].next = i + 1 < cfg.max_orders ? i + 1 : kNil;
        free_ = cfg.max_orders ? 0 : kNil;
    }

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
        if (ids_.get(e.id) != kNil) {
            sink(make_out(OutKind::Rejected, Reason::DuplicateId, e.id, e.side, e.price, e.qty));
            return;
        }
        if (can_rest(e.type) && live_ >= cfg_.max_orders) {
            sink(make_out(OutKind::Rejected, Reason::BookFull, e.id, e.side, e.price, e.qty));
            return;
        }
        enter(e, sink);
    }

    template <class Sink>
    void cancel(OrderId id, Sink&& sink) {
        const std::uint32_t i = ids_.get(id);
        if (i == kNil) {
            sink(make_out(OutKind::Rejected, Reason::UnknownOrder, id, Side::Buy, 0, 0));
            return;
        }
        const Node n = pool_[i];
        remove(i);
        sink(make_out(OutKind::Cancelled, Reason::UserCancel, id, n.side, n.price, n.qty));
    }

    template <class Sink>
    void modify(const InEvent& e, Sink&& sink) {
        const std::uint32_t i = ids_.get(e.id);
        if (i == kNil) {
            sink(make_out(OutKind::Rejected, Reason::UnknownOrder, e.id, Side::Buy, 0, 0));
            return;
        }
        Node& n = pool_[i];
        if (e.qty == 0) {
            sink(make_out(OutKind::Rejected, Reason::BadQty, e.id, n.side, e.price, e.qty));
            return;
        }
        if (e.price < 1 || e.price > cfg_.max_price) {
            sink(make_out(OutKind::Rejected, Reason::BadPrice, e.id, n.side, e.price, e.qty));
            return;
        }
        if (e.price == n.price && e.qty <= n.qty) {
            level(n.side, n.price).total -= n.qty - e.qty;
            n.qty = e.qty;
            sink(make_out(OutKind::Modified, Reason::None, e.id, n.side, n.price, n.qty));
            return;
        }
        InEvent re = e;
        re.kind = InKind::New;
        re.type = OrderType::Limit;
        re.side = n.side;
        re.owner = n.owner;
        sink(make_out(OutKind::Cancelled, Reason::Replaced, n.id, n.side, n.price, n.qty));
        remove(i);
        enter(re, sink);
    }

    // --- introspection -----------------------------------------------------
    Price best_bid() const noexcept { return best_[0]; }
    Price best_ask() const noexcept { return best_[1] > cfg_.max_price ? 0 : best_[1]; }
    std::uint64_t level_qty(Side s, Price p) const noexcept {
        return (p < 1 || p > cfg_.max_price) ? 0 : level(s, p).total;
    }
    Qty open_qty(OrderId id) const noexcept {
        const std::uint32_t i = ids_.get(id);
        return i == kNil ? 0 : pool_[i].qty;
    }
    Price price_of(OrderId id) const noexcept {
        const std::uint32_t i = ids_.get(id);
        return i == kNil ? 0 : pool_[i].price;
    }
    std::size_t order_count() const noexcept { return live_; }
    const BookConfig& config() const noexcept { return cfg_; }

    // Visits bids best-first then asks best-first, each in time priority.
    template <class F>
    void for_each_order(F&& f) const {
        for (Price p = best_[0]; p >= 1; --p) visit_level(Side::Buy, p, f);
        for (Price p = best_[1]; p <= cfg_.max_price; ++p) visit_level(Side::Sell, p, f);
    }
    // Visits up to n non-empty levels of one side, best first: f(price, total_qty).
    template <class F>
    void for_each_level(Side s, int n, F&& f) const {
        const Price step = s == Side::Buy ? -1 : 1;
        for (Price p = best_[idx(s)]; n > 0 && p >= 1 && p <= cfg_.max_price; p += step) {
            const std::uint64_t t = level(s, p).total;
            if (t == 0) continue;
            f(p, t);
            --n;
        }
    }

private:
    struct Node {
        OrderId id = 0;
        Qty qty = 0;
        OwnerId owner = 0;
        Price price = 0;
        std::uint32_t next = kNil;
        std::uint32_t prev = kNil;
        Side side = Side::Buy;
    };
    static_assert(sizeof(Node) == 32);

    struct Level {
        std::uint64_t total = 0;  // sum of resting quantity; 0 means empty
        std::uint32_t head = kNil;
        std::uint32_t tail = kNil;
    };
    static_assert(sizeof(Level) == 16);

    static constexpr std::size_t idx(Side s) noexcept { return static_cast<std::size_t>(s); }
    Level& level(Side s, Price p) noexcept { return levels_[idx(s)][static_cast<std::size_t>(p)]; }
    const Level& level(Side s, Price p) const noexcept { return levels_[idx(s)][static_cast<std::size_t>(p)]; }

    template <class F>
    void visit_level(Side s, Price p, F&& f) const {
        for (std::uint32_t i = level(s, p).head; i != kNil; i = pool_[i].next) {
            const Node& n = pool_[i];
            f(n.side, n.price, n.id, n.qty, n.owner);
        }
    }

    // True if a taker on `side` with price bound `limit` can trade now.
    bool crosses(Side side, Price limit) const noexcept {
        return side == Side::Buy ? best_[1] <= limit : best_[0] >= limit;
    }

    // --- two-level bitmap of non-empty levels (UseBitmap only) ---------------
    // bits_: one bit per price. sum_: one bit per 64-bit word of bits_, set when
    // that word is non-zero. Finding the next non-empty level is then at most
    // one masked word, a scan of the small summary (16 words for 65,536
    // prices), and one more word: effectively constant time for any gap.
    void set_bit(Side s, Price p) noexcept {
        const std::size_t w = static_cast<std::size_t>(p) >> 6;
        bits_[idx(s)][w] |= 1ull << (p & 63);
        sum_[idx(s)][w >> 6] |= 1ull << (w & 63);
    }
    void clear_bit(Side s, Price p) noexcept {
        const std::size_t w = static_cast<std::size_t>(p) >> 6;
        bits_[idx(s)][w] &= ~(1ull << (p & 63));
        if (bits_[idx(s)][w] == 0) sum_[idx(s)][w >> 6] &= ~(1ull << (w & 63));
    }
    static Price lowest(std::size_t word_index, std::uint64_t word) noexcept {
        return static_cast<Price>((word_index << 6) + static_cast<std::size_t>(std::countr_zero(word)));
    }
    static Price highest(std::size_t word_index, std::uint64_t word) noexcept {
        return static_cast<Price>((word_index << 6) + 63 - static_cast<std::size_t>(std::countl_zero(word)));
    }
    Price next_above(Side s, Price p) const noexcept {  // lowest non-empty price > p, or max_price + 1
        const auto& b = bits_[idx(s)];
        const auto& sm = sum_[idx(s)];
        const std::size_t i = static_cast<std::size_t>(p) + 1;
        const std::size_t w = i >> 6;
        if (const std::uint64_t word = b[w] & (~0ull << (i & 63))) return lowest(w, word);
        const std::size_t v = w + 1;  // first whole word to consider
        std::size_t sw = v >> 6;
        if (sw >= sm.size()) return cfg_.max_price + 1;
        std::uint64_t sword = sm[sw] & (~0ull << (v & 63));
        for (;;) {
            if (sword) {
                const std::size_t w2 = (sw << 6) + static_cast<std::size_t>(std::countr_zero(sword));
                return lowest(w2, b[w2]);
            }
            if (++sw == sm.size()) return cfg_.max_price + 1;
            sword = sm[sw];
        }
    }
    Price next_below(Side s, Price p) const noexcept {  // highest non-empty price < p, or 0
        const auto& b = bits_[idx(s)];
        const auto& sm = sum_[idx(s)];
        const std::size_t i = static_cast<std::size_t>(p) - 1;  // p >= 1 always
        const std::size_t w = i >> 6;
        if (const std::uint64_t word = b[w] & (~0ull >> (63 - (i & 63)))) return highest(w, word);
        if (w == 0) return 0;
        const std::size_t v = w - 1;  // last whole word to consider
        std::size_t sw = v >> 6;
        std::uint64_t sword = sm[sw] & (~0ull >> (63 - (v & 63)));
        for (;;) {
            if (sword) {
                const std::size_t w2 = (sw << 6) + 63 - static_cast<std::size_t>(std::countl_zero(sword));
                return highest(w2, b[w2]);
            }
            if (sw == 0) return 0;
            sword = sm[--sw];
        }
    }

    // Called when level (s, p) has just become empty.
    void level_emptied(Side s, Price p) noexcept {
        if (UseBitmap) clear_bit(s, p);
        if (p != best_[idx(s)]) return;
        if (UseBitmap) {
            best_[idx(s)] = s == Side::Buy ? next_below(s, p) : next_above(s, p);
        } else if (s == Side::Buy) {
            Price q = p - 1;
            while (q >= 1 && level(s, q).total == 0) --q;
            best_[0] = q;
        } else {
            Price q = p + 1;
            while (q <= cfg_.max_price && level(s, q).total == 0) ++q;
            best_[1] = q;
        }
    }

    void release(std::uint32_t i) noexcept {
        ids_.erase(pool_[i].id);
        pool_[i].next = free_;
        free_ = i;
        --live_;
    }

    // Unlinks a resting order from anywhere in its level and frees it.
    void remove(std::uint32_t i) noexcept {
        const Node& n = pool_[i];
        Level& lv = level(n.side, n.price);
        if (n.prev != kNil) pool_[n.prev].next = n.next; else lv.head = n.next;
        if (n.next != kNil) pool_[n.next].prev = n.prev; else lv.tail = n.prev;
        lv.total -= n.qty;
        const Side s = n.side;
        const Price p = n.price;
        release(i);
        if (lv.total == 0) level_emptied(s, p);
    }

    void rest(const InEvent& e, Qty qty) noexcept {
        const std::uint32_t i = free_;
        Node& n = pool_[i];
        free_ = n.next;
        Level& lv = level(e.side, e.price);
        n = Node{e.id, qty, e.owner, e.price, kNil, lv.tail, e.side};
        if (lv.tail != kNil) pool_[lv.tail].next = i; else lv.head = i;
        lv.tail = i;
        if (lv.total == 0 && UseBitmap) set_bit(e.side, e.price);
        lv.total += qty;
        if (e.side == Side::Buy) best_[0] = std::max(best_[0], e.price);
        else best_[1] = std::min(best_[1], e.price);
        ids_.put(e.id, i);
        ++live_;
    }

    // Would a FOK order fill completely? Stops at the first same-owner order,
    // because matching would stop there too.
    bool fillable(const InEvent& e, Price limit) const noexcept {
        const Side ms = opposite(e.side);
        const Price step = ms == Side::Sell ? 1 : -1;
        std::uint64_t need = e.qty;
        for (Price p = best_[idx(ms)]; p >= 1 && p <= cfg_.max_price; p += step) {
            if (e.side == Side::Buy ? p > limit : p < limit) return false;
            const Level& lv = level(ms, p);
            if (e.owner == 0) {  // no self-trade check needed: use the level total
                if (lv.total >= need) return true;
                need -= lv.total;
                continue;
            }
            for (std::uint32_t i = lv.head; i != kNil; i = pool_[i].next) {
                const Node& m = pool_[i];
                if (m.owner == e.owner) return false;
                if (m.qty >= need) return true;
                need -= m.qty;
            }
        }
        return false;
    }

    template <class Sink>
    void enter(const InEvent& e, Sink&& sink) {
        const Price limit = sweep_limit(cfg_, e);
        if (e.type == OrderType::PostOnly && crosses(e.side, limit)) {
            sink(make_out(OutKind::Cancelled, Reason::PostOnlyCross, e.id, e.side, e.price, e.qty));
            return;
        }
        if (e.type == OrderType::FOK && !fillable(e, limit)) {
            sink(make_out(OutKind::Cancelled, Reason::FokUnfillable, e.id, e.side, e.price, e.qty));
            return;
        }

        const Side ms = opposite(e.side);
        Qty rem = e.qty;
        bool self = false;
        while (rem > 0 && !self && crosses(e.side, limit)) {
            const Price px = best_[idx(ms)];
            Level& lv = level(ms, px);
            while (rem > 0 && lv.head != kNil) {
                const std::uint32_t mi = lv.head;
                Node& m = pool_[mi];
                if (e.owner != 0 && m.owner == e.owner) {
                    self = true;
                    break;
                }
                const Qty fill = std::min(rem, m.qty);
                sink(make_out(OutKind::Trade, Reason::None, e.id, e.side, px, fill, m.id));
                m.qty -= fill;
                lv.total -= fill;
                rem -= fill;
                if (m.qty == 0) {  // pop the fully filled maker from the front
                    lv.head = m.next;
                    if (lv.head != kNil) pool_[lv.head].prev = kNil; else lv.tail = kNil;
                    release(mi);
                }
            }
            if (lv.total == 0) level_emptied(ms, px);
        }
        if (rem == 0) return;
        if (self) {
            sink(make_out(OutKind::Cancelled, Reason::SelfTrade, e.id, e.side, e.price, rem));
        } else if (can_rest(e.type)) {
            rest(e, rem);
            sink(make_out(OutKind::Accepted, Reason::None, e.id, e.side, e.price, rem));
        } else {
            sink(make_out(OutKind::Cancelled, Reason::Unfilled, e.id, e.side, e.price, rem));
        }
    }

    BookConfig cfg_;
    IdMap ids_;
    std::vector<Node> pool_;
    std::vector<Level> levels_[2];
    std::vector<std::uint64_t> bits_[2];
    std::vector<std::uint64_t> sum_[2];
    Price best_[2];
    std::uint32_t free_ = kNil;
    std::uint32_t live_ = 0;
};

using BookV3 = BasicBook<false>;
using BookV3Bitmap = BasicBook<true>;
using Book = BookV3Bitmap;  // default; see BENCHMARKS.md for why

}  // namespace lob
