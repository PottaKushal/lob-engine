#include <gtest/gtest.h>

#include <cstdlib>
#include <thread>

#include "lob/spsc_queue.hpp"

using namespace lob;

TEST(SpscQueue, CapacityRoundsUpToPowerOfTwo) {
    EXPECT_EQ(SpscQueue<int>(1).capacity(), 2u);
    EXPECT_EQ(SpscQueue<int>(5).capacity(), 8u);
    EXPECT_EQ(SpscQueue<int>(1024).capacity(), 1024u);
}

TEST(SpscQueue, LayoutSeparatesProducerAndConsumerState) {
    // Three cache-line-aligned regions: producer, consumer, shared read-only.
    EXPECT_EQ(alignof(SpscQueue<int>), kCacheLine);
    EXPECT_GE(sizeof(SpscQueue<int>), 3 * kCacheLine);
}

TEST(SpscQueue, FifoFullAndEmpty) {
    SpscQueue<int> q(4);
    int v = -1;
    EXPECT_FALSE(q.try_pop(v));
    for (int i = 0; i < 4; ++i) EXPECT_TRUE(q.try_push(i));
    EXPECT_FALSE(q.try_push(99));  // full
    EXPECT_EQ(q.size_approx(), 4u);
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(q.try_pop(v));
        EXPECT_EQ(v, i);
    }
    EXPECT_FALSE(q.try_pop(v));
}

TEST(SpscQueue, WrapsAroundManyTimes) {
    SpscQueue<std::uint64_t> q(8);
    std::uint64_t next_out = 0, v = 0;
    for (std::uint64_t i = 0; i < 100000; ++i) {
        ASSERT_TRUE(q.try_push(i));
        if (i % 3 != 0) continue;  // let it fill and drain at different phases
        while (q.try_pop(v)) ASSERT_EQ(v, next_out++);
    }
    while (q.try_pop(v)) ASSERT_EQ(v, next_out++);
    EXPECT_EQ(next_out, 100000u);
}

// Two real threads. Run under ThreadSanitizer in CI: a wrong memory order or a
// missing release/acquire pair shows up as a reported race on the slot.
TEST(SpscQueue, TwoThreadsTransferEverythingInOrder) {
    const char* env = std::getenv("LOB_DIFF_EVENTS");  // sanitizer presets set this lower
    const std::uint64_t n = env ? 300000 : 5000000;
    SpscQueue<std::uint64_t> q(1024);
    std::uint64_t sum = 0;
    bool ordered = true;
    std::thread consumer([&] {
        std::uint64_t v = 0;
        for (std::uint64_t expect = 0; expect < n;) {
            if (!q.try_pop(v)) { std::this_thread::yield(); continue; }
            ordered = ordered && v == expect;
            sum += v;
            ++expect;
        }
    });
    for (std::uint64_t i = 0; i < n;) {
        if (q.try_push(i)) ++i;
        else std::this_thread::yield();
    }
    consumer.join();
    EXPECT_TRUE(ordered);
    EXPECT_EQ(sum, n * (n - 1) / 2);
}
