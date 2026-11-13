// Core value types shared by every book implementation.
#pragma once

#include <cstddef>
#include <cstdint>

namespace lob {

using Price = std::int32_t;     // integer ticks; valid resting prices are [1, max_price]
using Qty = std::uint32_t;      // integer lots
using OrderId = std::uint64_t;  // chosen by the client, unique among live orders
using OwnerId = std::uint32_t;  // 0 means "no self-trade prevention"

enum class Side : std::uint8_t { Buy = 0, Sell = 1 };
enum class OrderType : std::uint8_t { Limit = 0, Market = 1, IOC = 2, FOK = 3, PostOnly = 4 };
enum class InKind : std::uint8_t { New = 0, Cancel = 1, Modify = 2 };

constexpr Side opposite(Side s) noexcept {
    return s == Side::Buy ? Side::Sell : Side::Buy;
}

// One inbound instruction. 32 bytes, trivially copyable, two per cache line.
struct InEvent {
    std::uint64_t ts = 0;  // nanoseconds, carried through but not used by matching
    OrderId id = 0;
    Price price = 0;       // New: limit price (ignored for Market). Modify: new price.
    Qty qty = 0;           // New: quantity. Modify: new remaining quantity.
    OwnerId owner = 0;
    InKind kind = InKind::New;
    Side side = Side::Buy;
    OrderType type = OrderType::Limit;
    std::uint8_t pad = 0;

    friend bool operator==(const InEvent&, const InEvent&) = default;
};
static_assert(sizeof(InEvent) == 32);

enum class OutKind : std::uint8_t {
    Accepted = 0,   // order (or its remainder) now rests on the book
    Trade = 1,      // id = taker, other = maker, price = maker's price
    Cancelled = 2,  // order left the book or never rested; qty = quantity removed
    Rejected = 3,   // instruction refused, book unchanged
    Modified = 4,   // in-place quantity reduction, time priority kept
};

enum class Reason : std::uint8_t {
    None = 0,
    UserCancel = 1,
    Unfilled = 2,        // IOC or market remainder
    FokUnfillable = 3,
    PostOnlyCross = 4,
    SelfTrade = 5,       // incoming order would have traded with its own owner
    Replaced = 6,        // modify that lost priority: cancel then re-enter
    DuplicateId = 7,
    BadPrice = 8,
    BadQty = 9,
    UnknownOrder = 10,
    BookFull = 11,
};

// One outbound event. 32 bytes.
struct OutEvent {
    OrderId id = 0;
    OrderId other = 0;
    Price price = 0;
    Qty qty = 0;
    OutKind kind = OutKind::Accepted;
    Reason reason = Reason::None;
    Side side = Side::Buy;
    std::uint8_t pad = 0;
    std::uint32_t pad2 = 0;

    friend bool operator==(const OutEvent&, const OutEvent&) = default;
};
static_assert(sizeof(OutEvent) == 32);

struct BookConfig {
    Price max_price = 1 << 16;           // highest valid price in ticks
    std::uint32_t max_orders = 1u << 20; // capacity for resting orders
};

// Validation shared by all implementations so they reject identically.
// Order of checks is part of the specification: quantity, then price.
constexpr Reason validate_new(const BookConfig& cfg, const InEvent& e) noexcept {
    if (e.qty == 0) return Reason::BadQty;
    if (e.type != OrderType::Market && (e.price < 1 || e.price > cfg.max_price)) return Reason::BadPrice;
    return Reason::None;
}

constexpr bool can_rest(OrderType t) noexcept {
    return t == OrderType::Limit || t == OrderType::PostOnly;
}

// The price bound used when sweeping the opposite side.
constexpr Price sweep_limit(const BookConfig& cfg, const InEvent& e) noexcept {
    if (e.type != OrderType::Market) return e.price;
    return e.side == Side::Buy ? cfg.max_price : 1;
}

constexpr OutEvent make_out(OutKind k, Reason r, OrderId id, Side side, Price px, Qty qty,
                            OrderId other = 0) noexcept {
    OutEvent o;
    o.id = id;
    o.other = other;
    o.price = px;
    o.qty = qty;
    o.kind = k;
    o.reason = r;
    o.side = side;
    return o;
}

}  // namespace lob
