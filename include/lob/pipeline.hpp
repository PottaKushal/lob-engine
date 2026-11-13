// v4: three-stage pipeline connected by SPSC queues.
//
//   feed thread      decodes wire bytes into InEvent        --q_in-->
//   matcher thread   owns the book, applies events          --q_out-->
//   publisher thread hands every OutEvent to a callback (encode, hash, ...)
//
// The book is still touched by exactly one thread, so matching stays
// deterministic and needs no locks. The queues only move work that can be
// done off the matching thread (parsing, serialization) onto other cores.
//
// Latency is measured open-loop: the feed releases message i at
// start + i * period and stamps it with that *scheduled* time, not the time
// it actually got round to sending. If the system stalls, the backlog shows
// up as latency instead of being hidden (avoids coordinated omission).
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#include "lob/histogram.hpp"
#include "lob/protocol.hpp"
#include "lob/spsc_queue.hpp"
#include "lob/types.hpp"

#if defined(__APPLE__)
#include <pthread.h>
#include <pthread/qos.h>
#elif defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

namespace lob {

inline std::uint64_t now_ns() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

inline void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ __volatile__("yield");
#endif
}

// Best-effort scheduling hint for a latency-sensitive thread.
// macOS has no affinity API on Apple Silicon; the highest QoS class keeps the
// thread on performance cores. On Linux, `core >= 0` pins the thread.
inline void prefer_fast_core(int core = -1) noexcept {
#if defined(__APPLE__)
    (void)core;
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#elif defined(__linux__)
    if (core >= 0) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(static_cast<unsigned>(core), &set);
        pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
    }
#else
    (void)core;
#endif
}

// Spin briefly, then yield, so the pipeline also behaves on machines with
// fewer cores than threads (CI runners, sanitizer builds).
class Backoff {
public:
    explicit Backoff(std::uint32_t spin_limit) : limit_(spin_limit) {}
    void pause() noexcept {
        if (spins_ < limit_) { ++spins_; cpu_relax(); }
        else std::this_thread::yield();
    }
    void reset() noexcept { spins_ = 0; }

private:
    std::uint32_t limit_;
    std::uint32_t spins_ = 0;
};

struct PipelineOptions {
    std::size_t queue_capacity = 1 << 14;
    std::uint64_t period_ns = 0;  // 0: send as fast as possible. >0: one message every period_ns.
    bool measure_latency = false; // record scheduled-send to publish latency per input message
    bool pin = false;             // Linux only: pin the three threads to cores 0, 1, 2
    // Idle spins before a waiting thread starts yielding its time slice. Yielding is
    // polite on shared machines but costs a wakeup; ~0u means never yield (busy spin).
    std::uint32_t spin_limit = 2000;
    std::uint64_t latency_skip = 0; // inputs to ignore before recording latency (warmup)
};

struct PipelineStats {
    std::uint64_t events_in = 0;
    std::uint64_t events_out = 0;
    proto::Status parse_status = proto::Status::Ok;
    double seconds = 0;
    Histogram latency;  // nanoseconds, filled when measure_latency is set
};

namespace detail {
struct InItem {
    InEvent ev;
    std::uint64_t t0;
};
struct OutItem {
    OutEvent ev;
    std::uint64_t t0;    // scheduled send time of the originating input
    std::uint32_t last;  // 1 on the final output of that input
};
}  // namespace detail

// Runs `wire` (message bytes, no log header) through the pipeline.
// `publish(const OutEvent&)` runs on the publisher thread, in matching order.
template <class BookT, class Publish>
PipelineStats run_pipeline(BookT& book, const std::uint8_t* wire, std::size_t len, Publish&& publish,
                           const PipelineOptions& opt = {}) {
    using detail::InItem;
    using detail::OutItem;
    SpscQueue<InItem> q_in(opt.queue_capacity);
    SpscQueue<OutItem> q_out(opt.queue_capacity);
    std::atomic<bool> feed_done{false}, match_done{false};
    std::atomic<int> ready{0};  // downstream threads running; the clock starts only after that
    PipelineStats st;
    // Each of these is written by exactly one thread and read after join().
    std::uint64_t n_in = 0, n_out = 0;
    proto::Status parse_status = proto::Status::Ok;

    std::uint64_t start = 0;  // written by the feed thread, read after join()

    std::thread feed([&] {
        prefer_fast_core(opt.pin ? 0 : -1);
        while (ready.load(std::memory_order_acquire) < 2) cpu_relax();
        start = now_ns();
        std::size_t off = 0;
        std::uint64_t i = 0;
        Backoff bo(opt.spin_limit);
        while (off < len) {
            InItem it{};
            const proto::Decoded d = proto::decode(wire + off, len - off, it.ev);
            if (d.status == proto::Status::Sync) { off += d.consumed; continue; }
            if (d.status != proto::Status::Ok) { parse_status = d.status; break; }
            off += d.consumed;
            if (opt.period_ns) {
                it.t0 = start + i * opt.period_ns;
                while (now_ns() < it.t0) cpu_relax();
            } else if (opt.measure_latency) {
                it.t0 = now_ns();
            }
            ++i;
            bo.reset();
            while (!q_in.try_push(it)) bo.pause();
        }
        n_in = i;
        feed_done.store(true, std::memory_order_release);
    });

    std::thread matcher([&] {
        prefer_fast_core(opt.pin ? 1 : -1);
        ready.fetch_add(1, std::memory_order_release);
        InItem it;
        OutItem pending{};
        bool have = false;
        Backoff bo(opt.spin_limit);
        auto push = [&](const OutItem& o) {
            Backoff b2(opt.spin_limit);
            while (!q_out.try_push(o)) b2.pause();
        };
        for (;;) {
            if (!q_in.try_pop(it)) {
                // Read the flag first, then retry once: anything pushed before
                // the flag was set is guaranteed visible to the second pop.
                if (!feed_done.load(std::memory_order_acquire)) { bo.pause(); continue; }
                if (!q_in.try_pop(it)) break;
            }
            bo.reset();
            have = false;
            book.apply(it.ev, [&](const OutEvent& o) {
                if (have) push(pending);  // one event behind, so the last can be flagged
                pending = OutItem{o, it.t0, 0};
                have = true;
            });
            if (have) {
                pending.last = 1;
                push(pending);
            }
        }
        match_done.store(true, std::memory_order_release);
    });

    std::thread publisher([&] {
        prefer_fast_core(opt.pin ? 2 : -1);
        ready.fetch_add(1, std::memory_order_release);
        std::uint64_t inputs_seen = 0;
        OutItem o;
        Backoff bo(opt.spin_limit);
        for (;;) {
            if (!q_out.try_pop(o)) {
                if (!match_done.load(std::memory_order_acquire)) { bo.pause(); continue; }
                if (!q_out.try_pop(o)) break;
            }
            bo.reset();
            publish(o.ev);
            ++n_out;
            if (opt.measure_latency && o.last && ++inputs_seen > opt.latency_skip) {
                const std::uint64_t t = now_ns();
                st.latency.record(t > o.t0 ? t - o.t0 : 0);
            }
        }
    });

    feed.join();
    matcher.join();
    publisher.join();
    st.seconds = static_cast<double>(now_ns() - start) * 1e-9;
    st.events_in = n_in;
    st.events_out = n_out;
    st.parse_status = parse_status;
    return st;
}

// The same work on one thread: decode, match, publish. Baseline for the pipeline.
template <class BookT, class Publish>
PipelineStats run_serial(BookT& book, const std::uint8_t* wire, std::size_t len, Publish&& publish) {
    PipelineStats st;
    const std::uint64_t start = now_ns();
    std::size_t off = 0;
    while (off < len) {
        InEvent e;
        const proto::Decoded d = proto::decode(wire + off, len - off, e);
        if (d.status == proto::Status::Sync) { off += d.consumed; continue; }
        if (d.status != proto::Status::Ok) { st.parse_status = d.status; break; }
        off += d.consumed;
        ++st.events_in;
        book.apply(e, [&](const OutEvent& o) {
            publish(o);
            ++st.events_out;
        });
    }
    st.seconds = static_cast<double>(now_ns() - start) * 1e-9;
    return st;
}

}  // namespace lob
