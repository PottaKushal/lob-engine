# Design

This document records what was built, why, what was rejected, and what is known to be missing.
Numbers quoted here come from [BENCHMARKS.md](BENCHMARKS.md).

## 1. Matching rules (the specification)

These rules are implemented five times (one brute-force reference and four fast books) and the
differential tests require all five to agree event for event, so they are written down precisely.

**Prices and quantities.** Prices are integer ticks in `[1, max_price]`; quantities are integer lots.
There is no floating point anywhere in the book. Integer ticks make equality exact, make a price usable
as an array index, and make replay bit-reproducible.

**Priority.** Price first, then arrival time. A trade always executes at the resting (maker) order's price.

**Order types.**

| Type | Behaviour |
|---|---|
| Limit | Trades against the opposite side while prices cross, then rests any remainder. |
| Market | Trades until filled or the opposite side is empty; the remainder is cancelled (`Unfilled`). Never rests. |
| IOC | Like a limit order, but the remainder is cancelled instead of resting. |
| FOK | Fills completely and immediately, or does nothing (`FokUnfillable`). The check runs before any trade. |
| Post-only | Rests only if it would not trade on entry; otherwise it is cancelled (`PostOnlyCross`). |

**Cancel.** Removes a resting order. An unknown id is rejected (`UnknownOrder`); this is the normal outcome
of a cancel that races with a fill.

**Modify (cancel/replace).**
- Same price and a quantity that is less than or equal to the remaining quantity: reduced in place, **time
  priority is kept**. Shrinking an order does not disadvantage anyone behind it.
- Any price change, or a quantity increase: the order is cancelled (`Replaced`) and re-entered as a new
  limit order with the same id, side and owner. It **goes to the back of the queue** and may trade
  immediately if the new price crosses. Otherwise a participant could gain size at the front of a queue for free.

**Self-trade prevention.** Every order carries an owner id; owner 0 opts out. If an incoming order would
trade with a resting order of the same owner, the **incoming order's remainder is cancelled** (`SelfTrade`)
and the resting order is untouched ("cancel newest"). Trades already made against other owners at better
priority stand. A FOK order counts as unfillable if one of its own orders stands in the way, because
matching would stop there. Cancel-newest was chosen because it never touches resting state, so it cannot
cost a participant queue position.

**Validation order.** Zero quantity (`BadQty`), price out of range (`BadPrice`), id already live
(`DuplicateId`), and then book full for orders that could rest (`BookFull`). The order is part of the
specification because every implementation must reject identically. An id may be reused once the earlier
order is dead.

**Determinism.** The book is a pure function of its input sequence. There is no clock, no randomness, no
threads and no iteration over unordered containers on any path that affects output.

## 2. Data structures, version by version

Each version changes one thing, so each measurement isolates one cost.

| Version | Price levels | Orders in a level | Id lookup | Heap allocation per order |
|---|---|---|---|---|
| Reference (tests only) | none: one unsorted vector, scanned | | linear scan | amortised |
| v1 | `std::map` (red-black tree) | `std::list` | `std::unordered_map` | 2 or 3 |
| v2 | flat array indexed by price | `std::list` | `std::unordered_map` | 2 |
| v3 | flat array indexed by price | index-linked nodes in a pool | open-addressing table | 0 |
| v3 + bitmap (default) | v3 plus a two-level bitmap of non-empty levels | | | 0 |
| v4 | v3 + bitmap inside a three-thread pipeline | | | 0 |

### v1 to v2: flat price levels

A tree costs a pointer chase per level of depth and a node allocation whenever a price level is created.
Prices are bounded integers, so the level for price `p` can simply be `levels[p]`: 16 bytes per level,
2 MB for both sides of a 65,536-tick grid, of which only the region near the touch is hot. The best bid and
ask are cached, and re-found by scanning when the best level empties.

On the dense workload this was worth 1.4x; on the sparse workload 3x. The profile explains the modest dense
result: with the tree gone, the allocator dominates (57% of samples in `malloc`/`free`).

### v2 to v3: no allocation on the hot path

- **Order pool.** All order nodes are preallocated in one `std::vector<Node>`. A node is 32 bytes: id,
  quantity, owner, price, `next`, `prev`, side. Links are 32-bit indices rather than pointers, which halves
  their size and keeps the structure relocatable. Free nodes form a LIFO free list, so the next order reuses
  the most recently freed, cache-warm slot.
- **Intrusive FIFO per level.** A level stores `head`, `tail` and the total resting quantity. Append, pop
  from the front (a fill) and unlink from the middle (a cancel) are all O(1) with no allocation.
- **Id table.** A preallocated open-addressing hash table with linear probing at a load factor of at most
  0.5, Fibonacci hashing, and backward-shift deletion. Backward shift keeps probe sequences short without
  tombstones, which matters because order books delete as often as they insert.

Result: 4.5x over v2 on the dense workload, with `malloc`/`free` at 0% of samples and no output event
changed.

**Rejected: `std::deque` or a ring buffer per level.** Cancelling from the middle is O(n) or needs
tombstones, and cancels are 30 to 40% of the workload.
**Rejected: pointers instead of indices.** Twice the link size and no benefit, since everything lives in
one array.
**Rejected: a custom allocator under `std::list`.** It removes the allocator cost but keeps a separate
hash-map node per order; the intrusive design removes both.

### The bitmap: paying 2 to 5% on average to bound the worst case

A flat array has one bad case. When the best level empties, finding the next non-empty level by linear scan
costs time proportional to the gap. If the next bid is 59,900 ticks away, one cancel walks about 1 MB of
level array: about 14 microseconds per event instead of about 12 nanoseconds.

The fix is a bitmap with one bit per price and a second-level summary with one bit per 64-bit word. Finding
the next non-empty level is then one masked word, a scan of at most 16 summary words, and one more word,
using `countr_zero`/`countl_zero`.

Measured: on ordinary workloads the bitmap is slightly *slower* (about 2% on the dense book, about 5% on
the sparse one) because every level transition also updates the bitmap. On the worst case it is over 1,000x
faster. A first, single-level version of the bitmap still took about 210 ns per event in the worst case
because it scanned up to 936 words; the summary level removed that. The bitmap is the default because a
matching engine should not have an input that makes it 1,000x slower, and an attacker (or an unlucky quote)
can produce that input on purpose.

### v4: SPSC pipeline

```
wire bytes -> [feed: decode] --SPSC--> [matcher: book] --SPSC--> [publisher: encode] -> wire bytes
```

The book is still owned by exactly one thread, so matching stays deterministic and lock-free by
construction. The queues are bounded single-producer single-consumer rings; the memory-ordering argument is
in the header comment of [`spsc_queue.hpp`](include/lob/spsc_queue.hpp) and summarised here:

- Each index has exactly one writer. The writer stores it with `release`; the other side loads it with
  `acquire`. That pair is what makes the slot contents visible before the index that announces them.
- Each side loads its own index with `relaxed`, because nobody else writes it.
- `seq_cst` is unnecessary: neither side needs a global order of the two indices, only a lower bound on the
  other's progress, and a stale value is merely conservative.
- `head` and `tail` sit on separate cache lines (128 bytes on Apple Silicon, 64 elsewhere), and each side
  keeps a private cached copy of the other's index so the shared line is read only when the queue looks
  full or empty.
- Shutdown uses a done flag read *before* a final pop attempt, so nothing pushed before the flag can be missed.

**The honest result: the pipeline is slower at peak throughput.** Wire to wire on one thread runs at
about 54 million messages per second; the three-thread pipeline runs at about 23 million. Decoding,
matching and encoding one message cost about 18 ns in total, which is less than the cost of moving a
40-byte item between cores twice. When the consumer is faster than the producer the queue is nearly always
empty, so every pop re-reads the producer's index and the cache line holding it bounces between cores.

What the pipeline does buy is isolation: the matcher never blocks on a slow consumer or a system call, and
a burst is absorbed by the queue rather than by the matching thread. It is the right structure once the
feed and publisher do real I/O (sockets, disk), which this project does not do. With in-memory stages the
single-threaded path is the fast one, and the README says so.

## 3. Wire protocol and replay

A little-endian, fixed-size-per-type binary format (one type byte, then the fields), modelled loosely on
ITCH. Integers are read and written byte by byte, so there are no alignment or host-endianness assumptions
and no `reinterpret_cast` on input.

The decoder treats input as hostile: it never reads past the given length, rejects unknown message types
and out-of-range enum bytes, and refuses headers that ask for absurd capacities. Business rules stay in the
book, so the two layers can be tested separately.

A log is a header (book configuration) followed by messages. Because the book is deterministic, replaying a
log reproduces the same output-event hash and the same resting-state hash, on every book version and every
platform. `lob_cli verify` and a unit test check this.

## 4. Testing strategy

1. **Rule tests** (typed tests): each rule above, run against the reference and all four fast books.
2. **Differential tests**: 8 million randomized events per run by default. After every event the output
   events and best prices must equal the reference's; every 257 events the full resting state must match.
   The generator uses a tiny price grid, a tiny capacity and three owners, so collisions, full-book
   rejections, self-trades and empty-book states are frequent. A coverage assertion fails the test if any
   cancel or reject reason never occurred.
3. **Mutation check**: five deliberate bugs were seeded into the v3 book (wrong modify comparison, FOK
   ignoring own orders, LIFO instead of FIFO, a broken hash-table deletion condition, a missing rescan).
   The differential suite caught all five.
4. **Fuzzing**: a libFuzzer target feeds arbitrary bytes to the decoder and applies whatever decodes to
   both the fast book and the reference, aborting on any divergence. Apple clang has no libFuzzer, so the
   same entry point is also driven by a deterministic mutation loop inside `ctest`.
5. **Sanitizers**: the whole suite runs under AddressSanitizer + UndefinedBehaviorSanitizer and under
   ThreadSanitizer (the SPSC queue and pipeline tests are the reason for the latter).
6. **Golden hash**: a fixed seed must produce a fixed output hash on every platform and standard library.
   This is why the project has its own PRNG; `<random>` distributions differ between libc++ and libstdc++.

## 5. Known limitations

- **One instrument, one book.** No symbol routing, sessions, auctions, stop orders, icebergs or
  good-till-date orders.
- **Bounded price range.** The flat array needs `max_price` up front. A real venue would re-centre a
  window around the market or fall back to a tree for far-away prices.
- **Fixed capacity.** The order pool and id table are sized at construction; beyond that, new resting
  orders are rejected with `BookFull` rather than allocating.
- **Client-chosen ids** are trusted to be unique among live orders (duplicates are rejected, not repaired).
- **No persistence or recovery** beyond replaying the input log from the start; no snapshots.
- **No network layer.** The pipeline stages exchange memory, so the pipeline numbers say nothing about
  kernel or NIC latency.
- **Latency numbers are from a desktop OS** without core isolation; tails include scheduler noise, and
  Apple Silicon's 41 ns clock tick is coarse relative to a 12 ns operation.
- **The strategy study is a model study** on synthetic flow. See [docs/STRATEGY.md](docs/STRATEGY.md).

## 6. What I would do next

1. Batch the queue hand-off (publish the head index once per burst rather than per item) and re-measure
   the pipeline; this is the most likely way to make v4 beat the serial path.
2. A sliding price window so the level array stays small and cache-resident for instruments whose price
   range is wide.
3. Periodic book snapshots so replay can start from a checkpoint instead of the beginning of the log.
4. Linux numbers with isolated cores (`isolcpus`, pinned threads, `perf stat` counters for cache misses
   and branch mispredictions) to replace sampling-based profiling.
5. Multi-instrument routing: one book per symbol, one matcher thread per shard.
