#include <gtest/gtest.h>

#include <vector>

#include "lob/book.hpp"
#include "lob/book_list.hpp"
#include "lob/flow_gen.hpp"
#include "lob/hash.hpp"
#include "lob/histogram.hpp"
#include "lob/pipeline.hpp"
#include "lob/protocol.hpp"

using namespace lob;

namespace {
struct Wire {
    FlowConfig fc;
    std::vector<InEvent> events;
    std::vector<std::uint8_t> log;
    const std::uint8_t* msgs() const { return log.data() + proto::kHeaderSize; }
    std::size_t msgs_len() const { return log.size() - proto::kHeaderSize; }
};
Wire make_wire(std::size_t n, std::uint64_t seed) {
    Wire w;
    w.fc = FlowConfig::adversarial(seed);
    w.fc.book = BookConfig{400, 512};
    w.fc.target_live = 300;
    w.fc.max_dist = 60;
    w.fc.place_ppm = 60000;
    w.events = FlowGenerator(w.fc).generate(n);
    w.log = proto::encode_log(w.fc.book, w.events);
    return w;
}
}  // namespace

// Replaying a log must reproduce identical trades and identical book state,
// on every book version, every time.
TEST(Replay, LogReproducesIdenticalOutputAndState) {
    const Wire w = make_wire(200000, 7);
    BookConfig cfg;
    std::vector<InEvent> decoded;
    ASSERT_EQ(proto::decode_log(w.log.data(), w.log.size(), cfg, decoded).status, proto::Status::Ok);
    ASSERT_TRUE(decoded == w.events);

    auto replay = [&](auto book) {
        Fnv f;
        for (const InEvent& e : decoded) book.apply(e, [&](const OutEvent& o) { f.mix(o); });
        return std::pair{f.h, state_hash(book)};
    };
    const auto first = replay(Book(cfg));
    EXPECT_EQ(first, replay(Book(cfg)));
    EXPECT_EQ(first, replay(BookV3(cfg)));
    EXPECT_EQ(first, replay(BookV2(cfg)));
    EXPECT_EQ(first, replay(BookV1(cfg)));
}

// The three-thread pipeline must publish exactly what a single thread would.
TEST(Pipeline, MatchesSerialOutput) {
    const Wire w = make_wire(300000, 8);
    Fnv serial_hash, pipe_hash;
    Book b1(w.fc.book), b2(w.fc.book);
    const PipelineStats s = run_serial(b1, w.msgs(), w.msgs_len(), [&](const OutEvent& o) { serial_hash.mix(o); });
    const PipelineStats p = run_pipeline(b2, w.msgs(), w.msgs_len(), [&](const OutEvent& o) { pipe_hash.mix(o); });
    EXPECT_EQ(s.events_in, w.events.size());
    EXPECT_EQ(p.events_in, w.events.size());
    EXPECT_EQ(p.events_out, s.events_out);
    EXPECT_EQ(pipe_hash.h, serial_hash.h);
    EXPECT_EQ(state_hash(b1), state_hash(b2));
    EXPECT_EQ(p.parse_status, proto::Status::Ok);
}

// A tiny queue forces constant full/empty transitions, the paths where an
// ordering bug or a lost wakeup would hide.
TEST(Pipeline, TinyQueuesStillDeliverEverythingInOrder) {
    const Wire w = make_wire(120000, 9);
    Fnv serial_hash, pipe_hash;
    Book b1(w.fc.book), b2(w.fc.book);
    run_serial(b1, w.msgs(), w.msgs_len(), [&](const OutEvent& o) { serial_hash.mix(o); });
    PipelineOptions opt;
    opt.queue_capacity = 2;
    opt.measure_latency = true;
    const PipelineStats p = run_pipeline(b2, w.msgs(), w.msgs_len(), [&](const OutEvent& o) { pipe_hash.mix(o); }, opt);
    EXPECT_EQ(pipe_hash.h, serial_hash.h);
    EXPECT_EQ(p.latency.count(), w.events.size());  // one latency sample per input message
}

TEST(Pipeline, StopsCleanlyOnMalformedInput) {
    Wire w = make_wire(1000, 10);
    w.log.push_back('!');
    Book book(w.fc.book);
    std::size_t outs = 0;
    const PipelineStats p = run_pipeline(book, w.msgs(), w.msgs_len(), [&](const OutEvent&) { ++outs; });
    EXPECT_EQ(p.parse_status, proto::Status::BadType);
    EXPECT_EQ(p.events_in, 1000u);  // everything before the bad byte was processed
    EXPECT_EQ(p.events_out, outs);
}

TEST(Histogram, ExactBelow64AndBoundedErrorAbove) {
    Histogram h;
    for (std::uint64_t v = 0; v < 64; ++v) EXPECT_EQ(Histogram::upper(Histogram::index(v)), v);
    for (std::uint64_t v : {64ull, 100ull, 1000ull, 123456ull, 9999999ull, 1ull << 40, ~0ull}) {
        const std::uint64_t up = Histogram::upper(Histogram::index(v));
        EXPECT_GE(up, v);
        EXPECT_LE(static_cast<double>(up - v), static_cast<double>(v) / 32.0 + 1.0);
        EXPECT_LT(Histogram::index(v), Histogram::kBuckets);
    }
    EXPECT_EQ(h.percentile(0.5), 0u);
}

TEST(Histogram, Percentiles) {
    Histogram h;
    for (std::uint64_t v = 1; v <= 1000; ++v) h.record(v);
    EXPECT_EQ(h.count(), 1000u);
    EXPECT_EQ(h.min(), 1u);
    EXPECT_EQ(h.max(), 1000u);
    EXPECT_DOUBLE_EQ(h.mean(), 500.5);
    EXPECT_NEAR(static_cast<double>(h.percentile(0.50)), 500.0, 500.0 / 32 + 1);
    EXPECT_NEAR(static_cast<double>(h.percentile(0.99)), 990.0, 990.0 / 32 + 1);
    EXPECT_EQ(h.percentile(1.0), 1000u);
    Histogram g;
    g.record(5000);
    h.merge(g);
    EXPECT_EQ(h.max(), 5000u);
    EXPECT_EQ(h.count(), 1001u);
}
