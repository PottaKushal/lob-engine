# PLAN

Goal: a small, fully correct, well-measured C++20 limit order book and matching
engine, with a replay pipeline, benchmarks, a Python backtester and a static demo.
Smaller and correct beats larger and unfinished.

## Milestones

| # | Scope | Done when |
|---|-------|-----------|
| M1 | Matching core (three storage versions), brute-force reference book, unit and differential tests | `ctest` green; fast books equal the reference over millions of random events |
| M2 | Benchmark harness, histogram, v1 to v3 measured, profiling write-up | `lob_bench` prints a table and writes JSON with hardware and flags |
| M3 | SPSC ring buffer, three-thread pipeline (v4), binary protocol, replay, fuzz target, ASan/UBSan/TSan presets | sanitizer presets green; replay reproduces identical hashes |
| M4 | Python synthetic flow, closed-loop market-making backtest through the real engine, plots | `python/run_backtest.py` regenerates the committed figures |
| M5 | Static web visualizer fed by JSON generated in CI, GitHub Pages deploy; WASM build last | live URL works on desktop and phone, light and dark |
| M6 | README, DESIGN, BENCHMARKS, INTERVIEW_NOTES, RESUME_BULLETS, portfolio page | docs match the measured numbers |

## Design decisions fixed up front

- One set of matching rules, implemented four times: a brute-force reference
  (tests only), v1 `std::map` + `std::list`, v2 flat price array + `std::list`,
  v3 flat price array + index-linked pool + preallocated open-addressing id map.
- All books are header-only templates over an output sink, so the hot path has
  no virtual calls and no allocation in v3.
- Own PRNG and own sampling so generated workloads are bit-identical across
  libc++ and libstdc++ (the standard distributions are not).
- Every number in the docs comes from `lob_bench` JSON output.

## Risks

| Risk | Mitigation |
|------|------------|
| Apple Silicon timer resolution is about 42 ns, close to the per-event cost | Report throughput from wall time over millions of events; state the resolution next to latency percentiles; report Linux CI numbers separately |
| No thread pinning on macOS, noisy shared CI runners | Multiple runs, report median and spread, label CI numbers as indicative |
| Apple clang ships no libFuzzer | Fuzz target built with libFuzzer on Linux clang in CI; same entry point run by a deterministic mutation driver in `ctest` everywhere |
| The pipeline may be slower than single-threaded matching | Measure and report it honestly; explain what the pipeline buys |

## Cut order if time runs short

1. WebAssembly build of the engine for the browser.
2. Extra backtester features (Hawkes arrivals, public data adapter).
3. Never cut: tests, benchmarks, docs.
