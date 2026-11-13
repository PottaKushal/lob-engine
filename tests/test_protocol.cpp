#include <gtest/gtest.h>

#include <vector>

#include "lob/flow_gen.hpp"
#include "lob/protocol.hpp"
#include "lob/rng.hpp"

using namespace lob;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);

namespace {
InEvent sample(InKind k) {
    InEvent e;
    e.kind = k;
    e.ts = 0x0102030405060708ull;
    e.id = 0xA1B2C3D4E5F60718ull;
    if (k != InKind::Cancel) { e.price = 12345; e.qty = 678; }
    if (k == InKind::New) { e.owner = 42; e.side = Side::Sell; e.type = OrderType::FOK; }
    return e;
}
}  // namespace

TEST(Protocol, RoundTripsEveryMessageKind) {
    for (InKind k : {InKind::New, InKind::Cancel, InKind::Modify}) {
        const InEvent e = sample(k);
        std::uint8_t buf[proto::kMaxMsgSize];
        const std::size_t n = proto::encode(e, buf);
        InEvent d;
        const proto::Decoded r = proto::decode(buf, n, d);
        EXPECT_EQ(r.status, proto::Status::Ok);
        EXPECT_EQ(r.consumed, n);
        EXPECT_EQ(d, e);
    }
}

TEST(Protocol, IsLittleEndianOnTheWire) {
    std::uint8_t buf[proto::kMaxMsgSize];
    proto::encode(sample(InKind::Cancel), buf);
    EXPECT_EQ(buf[0], 'X');
    EXPECT_EQ(buf[1], 0x08);  // low byte of ts first
    EXPECT_EQ(buf[8], 0x01);
}

TEST(Protocol, NegativePriceSurvivesTheWire) {
    InEvent e = sample(InKind::New);
    e.price = -7;  // invalid for the book, but the codec must carry it faithfully
    std::uint8_t buf[proto::kMaxMsgSize];
    InEvent d;
    ASSERT_EQ(proto::decode(buf, proto::encode(e, buf), d).status, proto::Status::Ok);
    EXPECT_EQ(d.price, -7);
}

TEST(Protocol, EveryTruncationIsDetected) {
    for (InKind k : {InKind::New, InKind::Cancel, InKind::Modify}) {
        std::uint8_t buf[proto::kMaxMsgSize];
        const std::size_t n = proto::encode(sample(k), buf);
        for (std::size_t len = 0; len < n; ++len) {
            // Exact-size heap copy so AddressSanitizer would flag any over-read.
            std::vector<std::uint8_t> cut(buf, buf + len);
            InEvent d;
            const proto::Decoded r = proto::decode(cut.data(), cut.size(), d);
            EXPECT_EQ(r.status, proto::Status::Truncated);
            EXPECT_EQ(r.consumed, 0u);
        }
    }
}

TEST(Protocol, RejectsBadEnumBytes) {
    std::uint8_t buf[proto::kMaxMsgSize];
    InEvent d;
    const std::size_t n = proto::encode(sample(InKind::New), buf);
    buf[0] = 'Q';
    EXPECT_EQ(proto::decode(buf, n, d).status, proto::Status::BadType);
    buf[0] = 'A';
    buf[29] = 'x';
    EXPECT_EQ(proto::decode(buf, n, d).status, proto::Status::BadSide);
    buf[29] = 'B';
    buf[30] = 5;
    EXPECT_EQ(proto::decode(buf, n, d).status, proto::Status::BadOrderType);
}

TEST(Protocol, HeaderValidation) {
    std::uint8_t h[proto::kHeaderSize];
    BookConfig cfg;
    proto::encode_header(BookConfig{5000, 77}, h);
    ASSERT_EQ(proto::decode_header(h, sizeof h, cfg), proto::Status::Ok);
    EXPECT_EQ(cfg.max_price, 5000);
    EXPECT_EQ(cfg.max_orders, 77u);
    EXPECT_EQ(proto::decode_header(h, 15, cfg), proto::Status::Truncated);
    h[0] = 'X';
    EXPECT_EQ(proto::decode_header(h, sizeof h, cfg), proto::Status::BadHeader);
    h[0] = 'L';
    h[11] = 0xFF;  // absurd max_price: refuse rather than allocate gigabytes
    EXPECT_EQ(proto::decode_header(h, sizeof h, cfg), proto::Status::BadHeader);
}

TEST(Protocol, LogRoundTripAndErrorOffset) {
    const FlowConfig fc = FlowConfig::adversarial(3);
    const std::vector<InEvent> ev = FlowGenerator(fc).generate(5000);
    std::vector<std::uint8_t> bytes = proto::encode_log(fc.book, ev);
    BookConfig cfg;
    std::vector<InEvent> back;
    proto::LogResult r = proto::decode_log(bytes.data(), bytes.size(), cfg, back);
    EXPECT_EQ(r.status, proto::Status::Ok);
    EXPECT_EQ(r.offset, bytes.size());
    EXPECT_TRUE(back == ev);
    EXPECT_EQ(cfg.max_price, fc.book.max_price);

    const std::size_t good = bytes.size();
    bytes.push_back('?');
    back.clear();
    r = proto::decode_log(bytes.data(), bytes.size(), cfg, back);
    EXPECT_EQ(r.status, proto::Status::BadType);
    EXPECT_EQ(r.offset, good);
    EXPECT_EQ(back.size(), ev.size());
}

TEST(Protocol, OutEventEncoding) {
    std::uint8_t b[proto::kOutSize];
    proto::encode_out(make_out(OutKind::Trade, Reason::None, 0x1122, Side::Sell, 300, 9, 0x3344), b);
    EXPECT_EQ(proto::get(b, 8), 0x1122u);
    EXPECT_EQ(proto::get(b + 8, 8), 0x3344u);
    EXPECT_EQ(proto::get(b + 16, 4), 300u);
    EXPECT_EQ(proto::get(b + 20, 4), 9u);
    EXPECT_EQ(b[24], 1);
    EXPECT_EQ(b[26], 1);
}

// Deterministic mutation fuzzing through the libFuzzer entry point. The target
// aborts on any decoder inconsistency or any fast-book/reference divergence.
TEST(Protocol, FuzzSmoke) {
    Rng rng(2026);
    const FlowConfig fc = FlowConfig::adversarial(11);
    FlowGenerator gen(fc);
    for (int iter = 0; iter < 4000; ++iter) {
        std::vector<std::uint8_t> buf;
        if (iter % 4 == 0) {  // pure noise
            buf.resize(rng.below(200));
            for (auto& x : buf) x = static_cast<std::uint8_t>(rng.below(256));
        } else {  // a valid stream, then corrupted
            const std::vector<InEvent> ev = gen.generate(1 + rng.below(120));
            buf = proto::encode_log(fc.book, ev);
            if (iter % 4 != 1) buf.erase(buf.begin(), buf.begin() + proto::kHeaderSize);  // message path
            const std::uint32_t flips = rng.below(6);
            for (std::uint32_t f = 0; f < flips && !buf.empty(); ++f)
                buf[rng.below(static_cast<std::uint32_t>(buf.size()))] = static_cast<std::uint8_t>(rng.below(256));
            if (rng.below(3) == 0 && !buf.empty()) buf.resize(rng.below(static_cast<std::uint32_t>(buf.size())));
        }
        LLVMFuzzerTestOneInput(buf.data(), buf.size());
    }
    SUCCEED();
}
