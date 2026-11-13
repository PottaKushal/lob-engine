// Bounded single-producer single-consumer lock-free ring buffer.
//
// Exactly one thread may call try_push and exactly one (other) thread may
// call try_pop. Both are wait-free: a bounded number of steps, no CAS loop.
//
// Memory-ordering argument
// ------------------------
// head_ is written only by the producer, tail_ only by the consumer. Indices
// grow without wrapping to the capacity (they are masked on access), so
// head_ - tail_ is the number of items in the queue.
//
//  * The producer writes the slot, then publishes it with
//    head_.store(release). The consumer reads head_ with acquire. That
//    release/acquire pair creates the happens-before edge that makes the
//    slot write visible to the consumer before it reads the slot. Without it
//    the consumer could observe the new head but stale slot contents.
//  * Symmetrically, the consumer reads the slot, then publishes the free
//    space with tail_.store(release); the producer reads tail_ with acquire,
//    so it never overwrites a slot the consumer is still reading.
//  * Each side loads its own index with relaxed: nobody else writes it.
//  * No seq_cst is needed. Nothing here depends on a single total order of
//    the two stores; each side only needs a lower bound on the other's
//    progress, and a stale value is merely conservative (the queue looks
//    fuller or emptier than it is, never the reverse).
//
// False sharing
// -------------
// head_ and tail_ are written by different cores on every operation. If
// they shared a cache line each write would invalidate the other core's
// copy. They are placed on separate lines, and each side keeps a private
// cached copy of the other's index on its own line, so the shared index is
// only re-read when the queue looks full (producer) or empty (consumer).
#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <type_traits>

namespace lob {

// Apple Silicon uses 128-byte cache lines; x86-64 and most other arm64 use 64.
#if defined(__APPLE__) && defined(__aarch64__)
inline constexpr std::size_t kCacheLine = 128;
#else
inline constexpr std::size_t kCacheLine = 64;
#endif

template <class T>
class SpscQueue {
    static_assert(std::is_trivially_copyable_v<T>, "slots are copied without construction or destruction");

public:
    // Capacity is rounded up to a power of two so indexing is a mask, not a division.
    explicit SpscQueue(std::size_t min_capacity) {
        std::size_t cap = 2;
        while (cap < min_capacity) cap <<= 1;
        mask_ = cap - 1;
        buf_ = std::make_unique<T[]>(cap);
    }
    SpscQueue(const SpscQueue&) = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;

    std::size_t capacity() const noexcept { return mask_ + 1; }

    // Producer thread only.
    bool try_push(const T& v) noexcept {
        const std::size_t h = head_.load(std::memory_order_relaxed);
        if (h - tail_cache_ > mask_) {  // looks full: refresh our view of the consumer
            tail_cache_ = tail_.load(std::memory_order_acquire);
            if (h - tail_cache_ > mask_) return false;
        }
        buf_[h & mask_] = v;
        head_.store(h + 1, std::memory_order_release);
        return true;
    }

    // Consumer thread only.
    bool try_pop(T& out) noexcept {
        const std::size_t t = tail_.load(std::memory_order_relaxed);
        if (t == head_cache_) {  // looks empty: refresh our view of the producer
            head_cache_ = head_.load(std::memory_order_acquire);
            if (t == head_cache_) return false;
        }
        out = buf_[t & mask_];
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }

    // Approximate; safe to call from any thread, for monitoring only.
    std::size_t size_approx() const noexcept {
        return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire);
    }

private:
    // Producer-owned line.
    alignas(kCacheLine) std::atomic<std::size_t> head_{0};
    std::size_t tail_cache_ = 0;
    // Consumer-owned line.
    alignas(kCacheLine) std::atomic<std::size_t> tail_{0};
    std::size_t head_cache_ = 0;
    // Read-mostly line shared by both.
    alignas(kCacheLine) std::size_t mask_ = 0;
    std::unique_ptr<T[]> buf_;
};

}  // namespace lob
