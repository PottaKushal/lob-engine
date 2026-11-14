# Resume bullets

Candidate bullets for `lob-engine`. Every number is from the Apple M4 run recorded in
[`results/bench-apple-m4.json`](results/bench-apple-m4.json) and [BENCHMARKS.md](BENCHMARKS.md), or from the
test suite. If you re-run on other hardware, replace the numbers with what you measure there.

**Suggested heading**

> **lob-engine: C++20 Limit Order Book and Matching Engine** | C++20, CMake, GoogleTest, Python, WebAssembly
> | github.com/PottaKushal/lob-engine

## Recommended set (pick 3 or 4)

1. Built a price-time-priority matching engine in C++20 (limit, market, IOC, FOK, post-only, modify,
   self-trade prevention) that processes **82M events/s single-threaded with p99 latency of 42 ns**
   (Apple M4, clang `-O3`), **6.4x faster** than a `std::map`/`std::list` baseline measured on the same
   event stream.

2. Profiled the baseline, found **57% of time in `malloc`/`free`**, and removed all hot-path heap
   allocation with an order pool, intrusive FIFO queues and an open-addressing id table, for a **4.5x**
   gain; added a two-level bitmap that removed a worst case from **0.12M to 153M events/s** at a 2%
   average-case cost.

3. Verified correctness by **differential testing against a brute-force reference over 8M randomized
   events per run**, plus a libFuzzer target and AddressSanitizer, UBSan and ThreadSanitizer builds; all
   five deliberately seeded bugs were caught.

4. Implemented a lock-free SPSC ring buffer and a three-thread feed/match/publish pipeline with
   deterministic binary replay; measured **p99 of 251 ns at 1M messages/s** using open-loop timing, and
   documented why the pipeline is slower than the single-threaded path at peak load.

## Alternates

- Wrote a Python backtester that drives the C++ engine over a pipe, so strategy orders get real queue
  priority; on synthetic order flow, an Avellaneda-Stoikov inventory skew cut inventory standard deviation
  **from 11.3 to 1.1 lots** across 50 seeds.

- Compiled the engine to WebAssembly and shipped a static demo (order book replay, benchmark tables,
  in-browser order entry) deployed with GitHub Pages and no backend.

- Made replay bit-reproducible across compilers and standard libraries (Apple clang/libc++ and
  GCC/libstdc++) with integer prices, a custom PRNG and a pinned golden hash.

## Shorter variants (if space is tight)

- C++20 matching engine: 82M events/s single-threaded, p99 42 ns (Apple M4); 6.4x over a
  `std::map` baseline after removing hot-path allocation identified by profiling.
- Differential-tested against a reference implementation over 8M random events per run; fuzzed; clean under
  ASan, UBSan and TSan.

## Notes on using these honestly

- Keep the hardware in the bullet or be ready to state it. "82M events/s" without "single-threaded, Apple
  M4, in-memory" invites a comparison the project does not support.
- The p99 of 42 ns is one tick of a 41 ns clock. If asked, say so: the mean cost is about 12 ns per event,
  derived from throughput.
- Bullet 4 reports a negative result on purpose. If you would rather not lead with it, drop the last clause
  from the resume but be ready to bring it up yourself in the interview.
- The strategy bullet must always carry the words "synthetic order flow".
- Do not add "low-latency trading system", "production", "HFT" or any comparison with a real venue.
