// FNV-1a hashing of event streams and book state, for replay verification.
#pragma once

#include <cstdint>

#include "lob/types.hpp"

namespace lob {

struct Fnv {
    std::uint64_t h = 1469598103934665603ull;
    void mix(std::uint64_t v) noexcept {
        for (int i = 0; i < 8; ++i) {
            h ^= (v >> (8 * i)) & 0xFF;
            h *= 1099511628211ull;
        }
    }
    void mix(const OutEvent& o) noexcept {
        mix(o.id);
        mix(o.other);
        mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(o.price)) << 32 | o.qty);
        mix(static_cast<std::uint64_t>(o.kind) << 16 | static_cast<std::uint64_t>(o.reason) << 8 |
            static_cast<std::uint64_t>(o.side));
    }
};

// Hash of every resting order in priority order. Two books with equal hashes
// hold the same orders in the same queue positions.
template <class Book>
std::uint64_t state_hash(const Book& b) {
    Fnv f;
    b.for_each_order([&](Side s, Price p, OrderId id, Qty q, OwnerId owner) {
        f.mix(id);
        f.mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(p)) << 32 | q);
        f.mix(static_cast<std::uint64_t>(owner) << 8 | static_cast<std::uint64_t>(s));
    });
    return f.h;
}

}  // namespace lob
