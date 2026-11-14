## lob_bench

- CPU: Apple M4 (10 logical cores)
- OS: Darwin 24.5.0 arm64
- Compiler: clang 17.0.0 (clang-1700.0.13.5)
- Build: Release, flags: ` -O3 -DNDEBUG -std=c++20`
- Clock: steady_clock, resolution 41 ns, about 27 ns per call
- Events per replay: 5000000, timed runs: 7, seed: 1, pinned: no

Workloads (synthetic, seeded):
- dense: 5000000 events (61.6% new, 31.7% cancel, 6.7% modify), 1329960 trades, 15003 resting orders on 249 levels at end
- sparse: 5000000 events (51.4% new, 42.9% cancel, 5.7% modify), 214529 trades, 1714 resting orders on 1316 levels at end

### Matching core, dense book (single thread, events already decoded)

| Version | Throughput, M events/s (median) | min..max | CV | p50 ns | p99 ns | p99.9 ns | p99.99 ns | max ns |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| v1 map+list | 12.90 | 12.81..13.01 | 0.6% | 42 | 251 | 375 | 3135 | 60750 |
| v2 flat levels | 18.16 | 17.88..18.19 | 0.6% | 42 | 251 | 335 | 959 | 56209 |
| v3 pool | 82.31 | 81.66..83.22 | 0.7% | 0 | 42 | 85 | 211 | 45333 |
| v3 pool+bitmap | 80.77 | 80.01..81.17 | 0.5% | 0 | 42 | 85 | 211 | 37584 |

### Matching core, sparse book

| Version | Throughput, M events/s (median) | min..max | CV | p50 ns | p99 ns | p99.9 ns | p99.99 ns | max ns |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| v1 map+list | 11.00 | 10.97..11.07 | 0.4% | 83 | 211 | 295 | 2047 | 54500 |
| v2 flat levels | 32.41 | 31.43..32.83 | 1.4% | 41 | 85 | 167 | 251 | 49083 |
| v3 pool | 67.92 | 67.16..68.67 | 0.7% | 0 | 42 | 125 | 211 | 42666 |
| v3 pool+bitmap | 64.58 | 63.86..65.31 | 0.7% | 0 | 42 | 85 | 211 | 26709 |

### Matching core, worst-case gap (best level empties, next level 59,900 ticks away)

| Version | Throughput, M events/s (median) | min..max | CV | p50 ns | p99 ns | p99.9 ns | p99.99 ns | max ns |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| v1 map+list | 37.82 | 36.71..38.26 | 1.4% | 41 | 42 | 83 | 167 | 11583 |
| v2 flat levels | 0.13 | 0.13..0.13 | 0.1% | 13567 | 18943 | 27647 | 56319 | 87958 |
| v3 pool | 0.12 | 0.12..0.12 | 0.9% | 13567 | 20991 | 33791 | 69631 | 467959 |
| v3 pool+bitmap | 152.92 | 142.12..156.21 | 3.4% | 0 | 42 | 125 | 211 | 17750 |

### Wire to wire, dense book (decode, match, encode every output)

| Configuration | Throughput, M msgs/s (median) | min..max | CV |
|---|---:|---:|---:|
| serial, 1 thread | 54.42 | 54.01..55.06 | 0.7% |
| v4 pipeline, 3 threads + 2 SPSC queues | 22.57 | 14.58..22.91 | 14.2% |

### v4 pipeline latency, open loop (scheduled send to publish)

| Arrival rate, msgs/s | Messages | p50 ns | p90 ns | p99 ns | p99.9 ns | p99.99 ns | max ns |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 100000 | 1080000 | 167 | 463 | 1375 | 6527 | 720895 | 967208 |
| 500000 | 5400000 | 167 | 167 | 335 | 3135 | 9983 | 59833 |
| 1000000 | 10800000 | 167 | 167 | 251 | 3519 | 13055 | 48542 |
| 2000000 | 13500000 | 167 | 167 | 251 | 3199 | 15871 | 152167 |
| 4000000 | 13500000 | 167 | 167 | 211 | 2687 | 7935 | 32250 |

wrote results/bench-apple-m4.json
