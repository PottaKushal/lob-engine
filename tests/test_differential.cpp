// Differential tests: every fast book must produce exactly the same output
// events and the same resting state as the brute-force reference, over
// randomized event streams.
//
// LOB_DIFF_EVENTS sets the events per (book, seed) run. Default 400,000,
// so the default suite checks 4 books x 5 seeds x 400k = 8 million events.
#include <gtest/gtest.h>

#include <array>
#include <cstdlib>
#include <tuple>
#include <vector>

#include "lob/book.hpp"
#include "lob/book_list.hpp"
#include "lob/flow_gen.hpp"
#include "lob/hash.hpp"
#include "lob/reference_book.hpp"

using namespace lob;

namespace {

std::size_t events_per_run() {
    if (const char* s = std::getenv("LOB_DIFF_EVENTS")) return static_cast<std::size_t>(std::strtoull(s, nullptr, 10));
    return 400000;
}

using Row = std::tuple<Side, Price, OrderId, Qty, OwnerId>;
template <class Bk>
std::vector<Row> snapshot(const Bk& b) {
    std::vector<Row> v;
    b.for_each_order([&](Side s, Price p, OrderId id, Qty q, OwnerId o) { v.emplace_back(s, p, id, q, o); });
    return v;
}

template <class Fast>
void run_differential(const FlowConfig& cfg, std::size_t n, bool expect_full_coverage) {
    FlowGenerator gen(cfg);
    ReferenceBook ref(cfg.book);
    Fast fast(cfg.book);
    std::vector<OutEvent> a, b;
    std::array<std::size_t, 16> reasons{};
    std::size_t trades = 0;

    for (std::size_t i = 0; i < n; ++i) {
        const InEvent e = gen.next();
        a.clear();
        b.clear();
        ref.apply(e, [&](const OutEvent& o) { a.push_back(o); });
        fast.apply(e, [&](const OutEvent& o) { b.push_back(o); });
        ASSERT_EQ(a, b) << "output diverged at event " << i << " seed " << cfg.seed;
        for (const OutEvent& o : a) {
            ++reasons[static_cast<std::size_t>(o.reason)];
            trades += o.kind == OutKind::Trade;
        }
        ASSERT_EQ(ref.best_bid(), fast.best_bid()) << "at event " << i;
        ASSERT_EQ(ref.best_ask(), fast.best_ask()) << "at event " << i;
        if (i % 257 == 0) {
            ASSERT_EQ(snapshot(ref), snapshot(fast)) << "state diverged at event " << i << " seed " << cfg.seed;
            ASSERT_EQ(ref.order_count(), fast.order_count());
            const Price p = 1 + static_cast<Price>(i % static_cast<std::size_t>(cfg.book.max_price));
            ASSERT_EQ(ref.level_qty(Side::Buy, p), fast.level_qty(Side::Buy, p));
            ASSERT_EQ(ref.level_qty(Side::Sell, p), fast.level_qty(Side::Sell, p));
        }
    }
    ASSERT_EQ(snapshot(ref), snapshot(fast));
    ASSERT_EQ(state_hash(ref), state_hash(fast));
    EXPECT_GT(trades, n / 50) << "workload should trade often";
    if (expect_full_coverage && n >= 100000) {
        // The adversarial mix must exercise every cancel and reject reason.
        for (std::size_t r = static_cast<std::size_t>(Reason::UserCancel); r <= static_cast<std::size_t>(Reason::BookFull); ++r)
            EXPECT_GT(reasons[r], 0u) << "reason " << r << " never occurred";
    }
}

template <class Bk>
class Differential : public ::testing::Test {};
using FastBooks = ::testing::Types<BookV1, BookV2, BookV3, BookV3Bitmap>;
TYPED_TEST_SUITE(Differential, FastBooks);

TYPED_TEST(Differential, AdversarialMixMatchesReference) {
    for (std::uint64_t seed = 1; seed <= 4; ++seed)
        run_differential<TypeParam>(FlowConfig::adversarial(seed), events_per_run(), true);
}

// Wider price grid and a deeper book: exercises gaps between levels and long queues.
TYPED_TEST(Differential, WideBookMatchesReference) {
    FlowConfig cfg = FlowConfig::adversarial(99);
    cfg.book = BookConfig{2000, 400};
    cfg.target_live = 250;
    cfg.max_dist = 300;
    cfg.place_ppm = 20000;
    cfg.w_invalid = 10;
    run_differential<TypeParam>(cfg, events_per_run(), false);
}

}  // namespace

// The generator itself must be deterministic: same seed, same stream.
TEST(FlowGenerator, SameSeedSameStream) {
    FlowGenerator g1(FlowConfig::adversarial(5)), g2(FlowConfig::adversarial(5)), g3(FlowConfig::adversarial(6));
    const auto a = g1.generate(20000), b = g2.generate(20000), c = g3.generate(20000);
    EXPECT_TRUE(a == b);
    EXPECT_FALSE(a == c);
}

// Golden hash pins the workload and the matching rules across platforms and
// standard libraries. If this changes, either the generator or the rules changed.
TEST(FlowGenerator, GoldenHashIsPlatformIndependent) {
    FlowGenerator gen(FlowConfig::adversarial(42));
    Book book(FlowConfig::adversarial(42).book);
    Fnv f;
    for (int i = 0; i < 200000; ++i) book.apply(gen.next(), [&](const OutEvent& o) { f.mix(o); });
    f.mix(state_hash(book));
    EXPECT_EQ(f.h, 1805677145256488346ull);
}
