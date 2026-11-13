// Compact binary wire format (ITCH-inspired: one type byte, fixed-size body).
//
// All integers are little-endian and are read and written one byte at a
// time, so the code has no alignment or host-endianness assumptions.
//
//   Log header (16 bytes): "LOB1" | u16 version | u16 reserved | u32 max_price | u32 max_orders
//   'A' add    (31 bytes): type | u64 ts | u64 id | u32 owner | i32 price | u32 qty | u8 side | u8 order_type
//   'X' cancel (17 bytes): type | u64 ts | u64 id
//   'M' modify (25 bytes): type | u64 ts | u64 id | i32 price | u32 qty
//   'S' sync   ( 1 byte ): flush marker used by the streaming CLI
//
// The decoder treats its input as untrusted: it never reads past `len`, and
// it rejects unknown message types and out-of-range enum bytes. Business
// rules (price range, zero quantity, duplicate ids) are enforced by the book.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "lob/types.hpp"

namespace lob::proto {

inline constexpr std::uint16_t kVersion = 1;
inline constexpr std::size_t kHeaderSize = 16;
inline constexpr std::size_t kAddSize = 31, kCancelSize = 17, kModifySize = 25, kSyncSize = 1;
inline constexpr std::size_t kMaxMsgSize = kAddSize;
inline constexpr std::size_t kOutSize = 32;
inline constexpr std::uint8_t kOutBookTop = 5;  // streaming-only pseudo event: top of book

enum class Status : std::uint8_t { Ok, Sync, Truncated, BadType, BadSide, BadOrderType, BadHeader };

inline void put(std::uint8_t* p, std::uint64_t v, int bytes) noexcept {
    for (int i = 0; i < bytes; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}
inline std::uint64_t get(const std::uint8_t* p, int bytes) noexcept {
    std::uint64_t v = 0;
    for (int i = 0; i < bytes; ++i) v |= static_cast<std::uint64_t>(p[i]) << (8 * i);
    return v;
}

inline void encode_header(const BookConfig& cfg, std::uint8_t* out) noexcept {
    out[0] = 'L'; out[1] = 'O'; out[2] = 'B'; out[3] = '1';
    put(out + 4, kVersion, 2);
    put(out + 6, 0, 2);
    put(out + 8, static_cast<std::uint32_t>(cfg.max_price), 4);
    put(out + 12, cfg.max_orders, 4);
}

// Limits a header may ask for. A hostile header must not make us allocate gigabytes.
inline constexpr std::uint32_t kMaxHeaderPrice = 1u << 22, kMaxHeaderOrders = 1u << 24;

inline Status decode_header(const std::uint8_t* data, std::size_t len, BookConfig& cfg) noexcept {
    if (len < kHeaderSize) return Status::Truncated;
    if (data[0] != 'L' || data[1] != 'O' || data[2] != 'B' || data[3] != '1') return Status::BadHeader;
    if (get(data + 4, 2) != kVersion) return Status::BadHeader;
    const std::uint64_t max_price = get(data + 8, 4), max_orders = get(data + 12, 4);
    if (max_price < 1 || max_price > kMaxHeaderPrice || max_orders > kMaxHeaderOrders) return Status::BadHeader;
    cfg.max_price = static_cast<Price>(max_price);
    cfg.max_orders = static_cast<std::uint32_t>(max_orders);
    return Status::Ok;
}

// Writes one message; `out` must have room for kMaxMsgSize bytes. Returns bytes written.
inline std::size_t encode(const InEvent& e, std::uint8_t* out) noexcept {
    put(out + 1, e.ts, 8);
    put(out + 9, e.id, 8);
    switch (e.kind) {
        case InKind::New:
            out[0] = 'A';
            put(out + 17, e.owner, 4);
            put(out + 21, static_cast<std::uint32_t>(e.price), 4);
            put(out + 25, e.qty, 4);
            out[29] = e.side == Side::Buy ? 'B' : 'S';
            out[30] = static_cast<std::uint8_t>(e.type);
            return kAddSize;
        case InKind::Cancel:
            out[0] = 'X';
            return kCancelSize;
        case InKind::Modify:
            out[0] = 'M';
            put(out + 17, static_cast<std::uint32_t>(e.price), 4);
            put(out + 21, e.qty, 4);
            return kModifySize;
    }
    return 0;
}

struct Decoded {
    Status status;
    std::size_t consumed;  // bytes to advance on Ok or Sync; 0 otherwise
};

// Decodes one message from data[0, len). Never reads beyond len.
inline Decoded decode(const std::uint8_t* data, std::size_t len, InEvent& e) noexcept {
    if (len == 0) return {Status::Truncated, 0};
    std::size_t need = 0;
    switch (data[0]) {
        case 'A': need = kAddSize; break;
        case 'X': need = kCancelSize; break;
        case 'M': need = kModifySize; break;
        case 'S': return {Status::Sync, kSyncSize};
        default: return {Status::BadType, 0};
    }
    if (len < need) return {Status::Truncated, 0};
    e = InEvent{};
    e.ts = get(data + 1, 8);
    e.id = get(data + 9, 8);
    if (data[0] == 'A') {
        e.kind = InKind::New;
        e.owner = static_cast<OwnerId>(get(data + 17, 4));
        e.price = static_cast<Price>(static_cast<std::uint32_t>(get(data + 21, 4)));
        e.qty = static_cast<Qty>(get(data + 25, 4));
        if (data[29] == 'B') e.side = Side::Buy;
        else if (data[29] == 'S') e.side = Side::Sell;
        else return {Status::BadSide, 0};
        if (data[30] > static_cast<std::uint8_t>(OrderType::PostOnly)) return {Status::BadOrderType, 0};
        e.type = static_cast<OrderType>(data[30]);
    } else if (data[0] == 'X') {
        e.kind = InKind::Cancel;
    } else {
        e.kind = InKind::Modify;
        e.price = static_cast<Price>(static_cast<std::uint32_t>(get(data + 17, 4)));
        e.qty = static_cast<Qty>(get(data + 21, 4));
    }
    return {Status::Ok, need};
}

inline void encode_out(const OutEvent& o, std::uint8_t* out) noexcept {
    put(out, o.id, 8);
    put(out + 8, o.other, 8);
    put(out + 16, static_cast<std::uint32_t>(o.price), 4);
    put(out + 20, o.qty, 4);
    out[24] = static_cast<std::uint8_t>(o.kind);
    out[25] = static_cast<std::uint8_t>(o.reason);
    out[26] = static_cast<std::uint8_t>(o.side);
    out[27] = 0;
    put(out + 28, 0, 4);
}

// Whole-log helpers.
inline std::vector<std::uint8_t> encode_log(const BookConfig& cfg, const std::vector<InEvent>& events) {
    std::vector<std::uint8_t> buf(kHeaderSize + events.size() * kMaxMsgSize);
    encode_header(cfg, buf.data());
    std::size_t n = kHeaderSize;
    for (const InEvent& e : events) n += encode(e, buf.data() + n);
    buf.resize(n);
    return buf;
}

struct LogResult {
    Status status = Status::Ok;  // Ok if the whole buffer parsed
    std::size_t offset = 0;      // offset of the first byte not consumed
};

// Decodes header + messages. Stops at the first malformed message; sync markers are skipped.
inline LogResult decode_log(const std::uint8_t* data, std::size_t len, BookConfig& cfg, std::vector<InEvent>& events) {
    LogResult r;
    r.status = decode_header(data, len, cfg);
    if (r.status != Status::Ok) return r;
    r.offset = kHeaderSize;
    while (r.offset < len) {
        InEvent e;
        const Decoded d = decode(data + r.offset, len - r.offset, e);
        if (d.status == Status::Ok) events.push_back(e);
        else if (d.status != Status::Sync) { r.status = d.status; return r; }
        r.offset += d.consumed;
    }
    return r;
}

}  // namespace lob::proto
