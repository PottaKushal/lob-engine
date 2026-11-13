// Deterministic synthetic order-flow generator (C++ side).
//
// Used by the tests (an adversarial mix that hits every rule and rejection)
// and by the benchmarks (a book-like mix dominated by adds and cancels).
// The data is synthetic: it is a stress workload, not a market model. The
// statistical flow model used for the strategy study lives in python/.
//
// The generator runs its own shadow book so that cancels and modifies target
// orders that are really live, the way a real feed would.
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "lob/book.hpp"
#include "lob/rng.hpp"
#include "lob/types.hpp"

namespace lob {

struct FlowConfig {
    std::uint64_t seed = 1;
    BookConfig book{};
    std::uint32_t target_live = 20000;  // cancel pressure rises above this many resting orders
    // Relative weights of each action.
    std::uint32_t w_limit = 460, w_cancel = 400, w_modify = 60, w_ioc = 40, w_market = 20, w_fok = 5,
                  w_postonly = 15, w_invalid = 0;
    std::uint32_t place_ppm = 60000;  // geometric parameter for distance from mid, in ppm
    Price max_dist = 400;             // cap on distance from mid, ticks
    Qty max_qty = 100;
    std::uint32_t owners = 0;         // 0: every order has owner 0 (no self-trade prevention)
    std::uint32_t mid_move_ppm = 20000;

    // Book-like mix on a 65,536-tick grid; about 20k resting orders in steady state.
    static FlowConfig realistic(std::uint64_t seed = 1) {
        FlowConfig c;
        c.seed = seed;
        return c;
    }
    // Tiny price grid, tiny capacity, few owners, every order type, invalid input.
    static FlowConfig adversarial(std::uint64_t seed = 1) {
        FlowConfig c;
        c.seed = seed;
        c.book = BookConfig{40, 96};
        c.target_live = 60;
        c.w_limit = 300; c.w_cancel = 160; c.w_modify = 140; c.w_ioc = 90; c.w_market = 60;
        c.w_fok = 70; c.w_postonly = 80; c.w_invalid = 100;
        c.place_ppm = 250000;
        c.max_dist = 8;
        c.max_qty = 20;
        c.owners = 3;
        c.mid_move_ppm = 100000;
        return c;
    }
};

class FlowGenerator {
public:
    explicit FlowGenerator(const FlowConfig& cfg)
        : cfg_(cfg), rng_(cfg.seed), shadow_(cfg.book), mid_(cfg.book.max_price / 2) {}

    InEvent next() {
        ts_ += 1 + rng_.below(2000);
        if (rng_.chance_ppm(cfg_.mid_move_ppm)) {
            mid_ += rng_.below(2) ? 1 : -1;
            const Price lo = std::min(cfg_.max_dist + 2, cfg_.book.max_price / 2);
            mid_ = std::clamp(mid_, lo, cfg_.book.max_price - lo);
        }
        // Cancel pressure scales with book size so the book stays near target_live.
        const std::uint64_t live = shadow_.order_count();
        const std::uint32_t wc = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(2 * cfg_.w_cancel, cfg_.w_cancel * live / std::max(1u, cfg_.target_live)));
        const std::uint32_t total = cfg_.w_limit + wc + cfg_.w_modify + cfg_.w_ioc + cfg_.w_market + cfg_.w_fok +
                                    cfg_.w_postonly + cfg_.w_invalid;
        std::uint32_t r = rng_.below(total);
        InEvent e;
        if (r < cfg_.w_limit) e = make_new(OrderType::Limit);
        else if ((r -= cfg_.w_limit) < wc) e = make_cancel();
        else if ((r -= wc) < cfg_.w_modify) e = make_modify();
        else if ((r -= cfg_.w_modify) < cfg_.w_ioc) e = make_new(OrderType::IOC);
        else if ((r -= cfg_.w_ioc) < cfg_.w_market) e = make_new(OrderType::Market);
        else if ((r -= cfg_.w_market) < cfg_.w_fok) e = make_new(OrderType::FOK);
        else if ((r -= cfg_.w_fok) < cfg_.w_postonly) e = make_new(OrderType::PostOnly);
        else e = make_invalid();
        e.ts = ts_;

        shadow_.apply(e, [](const OutEvent&) {});
        if (e.kind == InKind::New && e.id == next_id_ - 1 && shadow_.open_qty(e.id) > 0) live_.push_back(e.id);
        return e;
    }

    std::vector<InEvent> generate(std::size_t n) {
        std::vector<InEvent> v;
        v.reserve(n);
        for (std::size_t i = 0; i < n; ++i) v.push_back(next());
        return v;
    }

    const Book& shadow() const { return shadow_; }

private:
    Qty rand_qty() { return 1 + rng_.below(cfg_.max_qty); }
    OwnerId rand_owner() { return cfg_.owners ? rng_.below(cfg_.owners + 1) : 0; }
    Price clamp_px(Price p) const { return std::clamp(p, Price{1}, cfg_.book.max_price); }

    InEvent make_new(OrderType t) {
        InEvent e;
        e.kind = InKind::New;
        e.type = t;
        e.id = next_id_++;
        e.side = rng_.below(2) ? Side::Buy : Side::Sell;
        e.qty = rand_qty();
        e.owner = rand_owner();
        const Price dir = e.side == Side::Buy ? 1 : -1;
        if (t == OrderType::Limit || t == OrderType::PostOnly) {
            Price d = 0;
            while (d < cfg_.max_dist && !rng_.chance_ppm(cfg_.place_ppm)) ++d;
            // Passive by default; a post-only order is sometimes aimed across the spread.
            const bool aim_across = t == OrderType::PostOnly && rng_.below(4) == 0;
            e.price = clamp_px(aim_across ? mid_ + dir * 2 : mid_ - dir * (1 + d));
        } else if (t == OrderType::Market) {
            e.price = 0;
        } else {  // IOC, FOK: marketable limit a few ticks through the mid
            e.price = clamp_px(mid_ + dir * static_cast<Price>(rng_.below(4)));
            if (t == OrderType::FOK) e.qty = 1 + rng_.below(3 * cfg_.max_qty);
        }
        return e;
    }

    // A resting order id, or 0 if none could be found quickly.
    OrderId pick_live() {
        for (int tries = 0; tries < 8 && !live_.empty(); ++tries) {
            const std::size_t k = rng_.below(static_cast<std::uint32_t>(live_.size()));
            const OrderId id = live_[k];
            if (shadow_.open_qty(id) > 0) return id;
            live_[k] = live_.back();  // filled or cancelled since we recorded it
            live_.pop_back();
        }
        return 0;
    }

    InEvent make_cancel() {
        const OrderId id = pick_live();
        if (id == 0) return make_new(OrderType::Limit);
        InEvent e;
        e.kind = InKind::Cancel;
        e.id = id;
        return e;
    }

    InEvent make_modify() {
        const OrderId id = pick_live();
        if (id == 0) return make_new(OrderType::Limit);
        InEvent e;
        e.kind = InKind::Modify;
        e.id = id;
        const Qty cur = shadow_.open_qty(id);
        const Price px = shadow_.price_of(id);
        if (rng_.below(2)) {
            e.price = px;
            e.qty = 1 + rng_.below(cur + 2);  // usually a reduction, sometimes an increase
        } else {
            e.price = clamp_px(px + static_cast<Price>(rng_.below(7)) - 3);
            e.qty = rand_qty();
        }
        return e;
    }

    InEvent make_invalid() {
        InEvent e = make_new(OrderType::Limit);
        switch (rng_.below(7)) {
            case 0: e.kind = InKind::Cancel; e.id = next_id_ + 1000000; break;  // unknown id
            case 1: e.kind = InKind::Modify; e.id = next_id_ + 1000000; break;
            case 2: e.qty = 0; break;
            case 3: e.price = rng_.below(2) ? 0 : cfg_.book.max_price + 1; break;
            case 4: if (OrderId id = pick_live()) e.id = id; break;  // duplicate id
            case 5: if (OrderId id = pick_live()) { e.kind = InKind::Modify; e.id = id; e.qty = 0; } break;
            default: if (OrderId id = pick_live()) { e.kind = InKind::Modify; e.id = id; e.price = -5; } break;
        }
        return e;
    }

    FlowConfig cfg_;
    Rng rng_;
    Book shadow_;
    Price mid_;
    std::uint64_t ts_ = 0;
    OrderId next_id_ = 1;
    std::vector<OrderId> live_;
};

}  // namespace lob
