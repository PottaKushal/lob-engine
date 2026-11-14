# Interview notes

Private study notes for explaining and defending this project. Plain language, grounded in the code in
this repository. All figures are from the Apple M4 run in [BENCHMARKS.md](BENCHMARKS.md).

Before any interview: re-run the tests and the benchmark on your own machine, and read `book.hpp` and
`spsc_queue.hpp` until you could rewrite them. If you cannot explain a line, change it or learn it.

## The 2-minute version

> I built a limit order book and matching engine in C++20 and treated it as a measurement exercise. I wrote
> it four times. Version one is the textbook design: a `std::map` of price levels, a `std::list` of orders
> at each level, and a hash map from order id to list node. It does about 13 million events per second on
> my machine. Version two replaces the map with a flat array indexed by price, which only got me 1.4x. So I
> profiled it, and more than half the time was in `malloc` and `free`. Version three puts orders in a
> preallocated pool with intrusive linked lists and an open-addressing id table, so there is no heap
> allocation on the hot path. That runs at about 82 million events per second, 6.4x the first version, with
> a p99 under 50 nanoseconds.
>
> The part I am most confident in is correctness. I wrote a deliberately dumb reference implementation, one
> unsorted vector that gets scanned for every decision, and a randomized test that feeds 8 million events
> to both and requires identical output after every single event. I also seeded five bugs into the fast
> book to check that the test catches them, and it caught all five.
>
> Two results surprised me. A bitmap I added made the average case 2% slower, but I kept it because it fixes
> a worst case that was a thousand times slower. And the multi-threaded pipeline I built is slower than the
> single-threaded path, because each message is only about 18 nanoseconds of work, which is less than the
> cost of passing it between cores.

## The 10-minute version

Follow this order. Each step has one number.

1. **The rules (1 minute).** Price-time priority. Limit, market, IOC, FOK, post-only. Cancel. Modify: a
   size reduction keeps queue position, anything else goes to the back. Self-trade prevention cancels the
   incoming order. Prices are integer ticks. The book is a deterministic function of its input.
2. **v1, the baseline (1 minute).** `std::map<price, std::list<order>>` plus
   `std::unordered_map<id, list iterator>`. Easy to get right. 12.9M events/s.
3. **v2, flat levels (1 minute).** Prices are bounded integers, so `levels[price]` replaces a tree lookup.
   Cached best bid and ask. 18.2M events/s, only 1.4x. Say that this was disappointing and that it sent you
   to the profiler.
4. **The profile (1 minute).** `sample` showed 42% of v1 and 57% of v2 inside the allocator. Every order
   costs two allocations (list node, hash node) and two frees.
5. **v3, no allocation (2 minutes).** A pool of 32-byte nodes linked by 32-bit indices, a free list, a FIFO
   per level with head, tail and total quantity, and an open-addressing table for ids with backward-shift
   deletion. 82.3M events/s. Allocator share of the profile: 0%.
6. **The bitmap (1 minute).** Flat arrays have a worst case: the best level empties and the next one is far
   away. Linear scan: 14 microseconds per event. Two-level bitmap: back to nanoseconds. Cost: 2 to 5% on
   normal workloads. You kept it because bounded worst-case behaviour matters more than 2%.
7. **Testing (2 minutes).** Reference implementation, differential test over 8 million events, coverage
   assertion, mutation check, fuzz target that is itself differential, ASan, UBSan, TSan, golden hash across
   two standard libraries.
8. **The pipeline (1 minute).** SPSC ring buffers, feed to matcher to publisher. Correct under TSan.
   2.4x slower than one thread. Explain why, and say what you would try next (batching the index
   publication).

## The 45-minute version

Use the 10-minute outline as the spine and go deep where the interviewer pulls. Have these ready:

- **Whiteboard the v3 layout.** Draw the level array, three nodes linked in one level, the free list, and
  the id table. Walk one add, one fill from the front, one cancel from the middle.
- **Walk the matching loop** in `BasicBook::enter`: validation, post-only check, FOK pre-check, the sweep
  loop, then rest, cancel remainder or self-trade cancel.
- **Derive the SPSC queue** from scratch and justify each memory order (see question 9).
- **Explain open-loop latency measurement** and coordinated omission (question 17).
- **Tell the bitmap story with its false start**: the first, single-level bitmap still took about 210 ns in
  the worst case because it scanned up to 936 words. The summary level fixed it.
- **Show the strategy study** and immediately say what is wrong with it (question 23). Interviewers at
  trading firms react badly to anyone who takes a synthetic Sharpe ratio seriously.
- **Close with limitations and next steps** from DESIGN.md sections 5 and 6. Volunteer them.

## 25 likely questions

### Data structures

**1. Why a flat array for price levels instead of a `std::map` or a heap?**
Prices are bounded integers, so the price is the index: O(1) with no pointer chasing and no allocation when
a level appears. Levels near the touch are adjacent in memory. A heap gives the best price quickly but
cannot find an arbitrary level for a cancel. The cost of the array is memory (16 bytes times 65,536 levels
times two sides is 2 MB) and the worst-case rescan, which the bitmap fixes.

**2. What does it cost when the best level empties?**
Without the bitmap, a linear scan to the next non-empty level: proportional to the gap. Measured worst
case: 59,900 empty levels, about 14 microseconds. With the two-level bitmap: mask one word, scan at most 16
summary words, read one more word. Measured: the same workload at 153M events/s.

**3. Why did you keep the bitmap if it made the benchmark slower?**
It costs 2% on the dense workload and 5% on the sparse one, because every level that becomes empty or
non-empty also updates two bits. In exchange there is no input that is a thousand times slower. For a
matching engine, a predictable worst case is worth more than 2% of average throughput, and the bad input is
one that a participant could send deliberately.

**4. Why a linked list per level? Linked lists are cache-unfriendly.**
A level needs three operations in O(1): append at the back, pop from the front, and remove from the middle
for a cancel, and cancels are a third of the workload. A ring buffer or `std::deque` makes middle removal
O(n) or needs tombstones. The list is intrusive and its nodes come from one contiguous pool with LIFO
reuse, so in practice neighbours in a queue are often close in memory. It is still a pointer chase per
fill; that is the price of O(1) cancel.

**5. Why 32-bit indices instead of pointers?**
A node is 32 bytes with index links and would be 40 or 48 with pointers, so two nodes fit in a 64-byte
cache line. Indices also remain valid if the pool is moved or mapped elsewhere. The cost is one add to form
the address, and a 4-billion-order limit that does not matter here.

**6. How does the id table work, and why not `std::unordered_map`?**
`std::unordered_map` allocates a node per element and chases a pointer per lookup. Mine is one flat array
of 16-byte slots, linear probing, load factor at most 0.5, Fibonacci hashing (multiply by 2^64 divided by
the golden ratio, take the top bits). Deletion uses backward shift: after removing an entry, later entries
in the same probe run are moved back if the hole lies on their probe path, so there are no tombstones and
lookups never slow down as the table churns.

**7. What happens when the pool is full?**
Orders that could rest (limit, post-only) are rejected with `BookFull` before matching. IOC, FOK and market
orders are still accepted because they never need a slot. Nothing allocates. The reference implementation
has the same limit, so the differential test covers this path, and the test configuration uses a capacity
of 96 so it happens constantly.

### Allocation and performance

**8. Why does "no allocation on the hot path" matter so much?**
Three reasons, in the order I measured them. Throughput: the allocator was 42 to 57% of the time. Tail
latency: v1's p99.99 is 3,135 ns against 211 ns for v3, because an allocation occasionally takes a slow
path. Locality: allocator-chosen addresses scatter orders across the heap, while a pool keeps them in one
array. A general-purpose allocator may also take a lock or make a system call, which is unacceptable on a
matching thread.

**9. Walk me through the memory ordering in your SPSC queue.**
There are two indices. `head` is written only by the producer, `tail` only by the consumer. The producer
writes the slot and then stores `head` with `release`. The consumer loads `head` with `acquire`. That pair
creates a happens-before edge, so the consumer is guaranteed to see the slot contents before it sees the
index that says the slot is ready. The same argument in the other direction (consumer stores `tail` with
`release`, producer loads it with `acquire`) guarantees the producer never overwrites a slot the consumer
is still reading. Each side reads its own index with `relaxed` because nobody else writes it.

**10. Why not `seq_cst` everywhere, to be safe?**
`seq_cst` adds a guarantee I do not use: a single global order of all atomic operations. Neither side needs
to know the order of a `head` store relative to a `tail` store. Each only needs a lower bound on the other
side's progress, and reading a stale value is safe: the queue looks fuller or emptier than it is, never the
reverse. On x86 a `seq_cst` store is an `xchg` or a store plus `mfence`, a full barrier, on every push and
pop. On arm64 the difference is smaller, but the principle is to ask for exactly the guarantee the proof
needs.

**11. What is false sharing and where did you prevent it?**
Two threads writing different variables that happen to share a cache line. Each write invalidates the
other core's copy, so the line bounces between cores although the data is logically unrelated. In the queue,
`head` (producer) and `tail` (consumer) are on separate lines via `alignas`. I use 128 bytes on Apple
Silicon, whose cache lines are 128 bytes, and 64 elsewhere. Each side also keeps a private cached copy of
the other side's index on its own line, so the shared line is read only when the queue looks full or empty.

**12. Your pipeline is slower than a single thread. Why, and why ship it?**
One message is about 18 ns of work end to end: 54 million per second on one thread. The pipeline moves a
40-byte item between cores twice. When the consumer keeps up, the queue is almost always empty, so every
pop has to re-read the producer's index, and that cache line ping-pongs between the two cores at tens of
nanoseconds per transfer. The hand-off costs more than the work it offloads. I shipped it because the
structure is right when the outer stages do real I/O, because it is a correct lock-free queue under TSan,
and because reporting a negative result honestly is more useful than hiding it. Next step: batch the index
publication so the line moves once per burst.

**13. How would you make it faster?**
In order: batch hand-offs; `-mcpu=native` and link-time optimisation, which I left off deliberately so the
build is portable; huge pages for the pool and level arrays to cut TLB misses; a sliding price window so
the level array fits in L1 or L2; prefetch the next node while processing the current fill. I would
measure each one, because two of my guesses in this project were wrong.

### Determinism and correctness

**14. How do you know the engine is correct?**
I do not prove it; I test it against an independent implementation. The reference book shares no data
structure with the fast ones: one vector, linear scans, sort to find priority. The differential test feeds
both the same random stream, 8 million events by default, and requires identical output events after every
event and identical resting state every 257 events. The stream is adversarial: a 40-tick grid, a 96-order
capacity, three owners, every order type and invalid input, so edge cases happen thousands of times. A
coverage check fails the test if any cancel or reject reason never occurred.

**15. How do you know the test itself is any good?**
Mutation testing by hand. I seeded five bugs into the v3 book: `<=` changed to `<` in the modify rule, FOK
skipping instead of stopping at its own order, appending at the head instead of the tail, a wrong condition
in the hash-table backward shift, and a missing rescan when the best level empties. All five were caught.

**16. What makes the engine deterministic, and why does that matter?**
No clocks, no randomness, no threads inside the book, no iteration over hash containers on any path that
affects output, integer prices. Same input, same output, bit for bit. That gives replay (reproduce any
state from the log), debuggability (a failing seed fails every time) and cross-checking (v1 to v3 produce
identical hashes, under both libc++ and libstdc++). It is also why I wrote my own PRNG: the standard
library's distributions produce different numbers on different standard libraries.

**17. How did you measure latency, and what is coordinated omission?**
Two ways. For the matching core, a clock read before and after each event into a log-linear histogram. For
the pipeline, open loop: message `i` is scheduled at `start + i * period` and latency runs from the
scheduled time to publication. If instead you timestamp when you actually send, a stall in the system also
stalls the sender, the sender produces fewer samples during the stall, and the histogram under-reports
exactly the events you care about. That is coordinated omission.

**18. What is wrong with your latency numbers?**
The Apple Silicon clock ticks every 41 ns and an operation takes about 12 ns, so the median reads as zero
and everything is quantised. Each sample also includes two clock reads. The maxima, tens of microseconds,
are scheduler preemptions on a laptop with no core isolation. So I trust throughput (measured over 5
million events without per-event clocks) and the shape of the tail, and I would redo the tail on Linux with
isolated cores before quoting it to anyone as a system property.

**19. How do you treat untrusted input?**
The decoder checks the length before reading any field, rejects unknown message types and out-of-range
enum bytes, and refuses headers that request absurd capacities so a hostile file cannot make it allocate
gigabytes. Integers are assembled byte by byte, so there are no unaligned reads or casts. Business rules
are validated separately in the book. A fuzz target runs arbitrary bytes through the decoder and into both
books under ASan and UBSan.

### Rules and design choices

**20. Explain your modify semantics. Why does increasing size lose priority?**
Reducing size at the same price keeps the order's place: it harms nobody behind it. A price change or a
size increase is a cancel plus a new order: back of the queue, and it can trade immediately if it now
crosses. If an increase kept priority, a participant could park one lot at the front of a queue and inflate
it when the price became attractive, jumping everyone who committed size earlier.

**21. Why "cancel newest" for self-trade prevention?**
The incoming order's remainder is cancelled and the resting order is untouched. It never modifies resting
state, so it cannot cost anyone queue position and it is the simplest policy to reason about. The
alternatives are cancel-oldest (remove the resting order and continue) and decrement-both. They are equally
valid policies; a venue picks one. FOK interacts with it: the fillability check stops at the first own
order because matching would stop there.

**22. FOK needs to know if it can fill before it trades. How?**
A read-only pre-pass over the opposite side up to the limit price. With owner 0 it uses the per-level total
quantity, so it is one read per level. With an owner it walks individual orders, because it must stop at
the first order with the same owner. Only if the pass succeeds does the normal matching loop run, and then
it is guaranteed to fill completely.

### The research layer

**23. Your backtest shows a Sharpe-like statistic near 9. Do you believe it?**
No, and the write-up says so. It is a property of the simulator. The strategy is the only liquidity
provider inside the background book, the background flow does not react to it, there are no fees or
latency, and the fair value is exactly the Gaussian process the strategy's formula assumes. What I do
believe is the comparison: with identical flow, the inventory skew cuts inventory standard deviation from
11.3 lots to 1.1 and PnL dispersion by a factor of twenty, at a similar mean. The difference in mean PnL
between the two is not statistically significant with 50 seeds; the difference in variance is.

**24. Explain the Avellaneda-Stoikov reservation price.**
A dealer with inventory `q` carries price risk with variance `q^2 sigma^2 tau`. With exponential utility
the certainty-equivalent cost of that risk is `(gamma / 2) q^2 sigma^2 tau`. Differentiate with respect to
`q` to get the marginal cost of one more unit: `q gamma sigma^2 tau`. So the dealer values the asset at
`r = s - q gamma sigma^2 tau` and centres quotes there. If long, both quotes shift down: more likely to
sell, less likely to buy. The spread adds `(2 / gamma) ln(1 + gamma / k)`, which comes from maximising
expected profit when the fill rate decays as `exp(-k delta)`.

**25. What would you change for production?**
Many things, and I would say so first. Multiple instruments with one book per symbol and sharding across
threads. A sliding price window. Snapshots and a recovery path. A real network layer with kernel bypass,
and sequenced, gap-detecting feeds. Risk checks before the book. Auctions, stop orders, icebergs. Core
isolation, huge pages, hardware timestamping. Monitoring that does not touch the hot path. And a much
longer soak test. This project is one data structure done carefully, not an exchange.

## Five things to be able to write from memory

Practise each on paper, then compile it.

1. **The SPSC queue**: `try_push` and `try_pop` with the correct memory orders, the cached indices and the
   cache-line alignment. About 30 lines. Reference: `include/lob/spsc_queue.hpp`.
2. **A price-level book**: flat levels, intrusive FIFO, add, cancel and the matching loop for a limit
   order. About 80 lines. Reference: `BasicBook::rest`, `remove` and `enter` in `include/lob/book.hpp`.
   A simplified version of this is a known timed interview exercise.
3. **An open-addressing hash table** with linear probing and backward-shift deletion. Reference: `IdMap`.
4. **A free-list pool allocator**: allocate from the head of the free list, release back to the head.
   Reference: the constructor, `rest` and `release` in `BasicBook`.
5. **Next set bit in a bitmap** using `std::countr_zero` and `std::countl_zero`, including the mask for the
   partial first word. Reference: `next_above` and `next_below`.

## Numbers to remember

| | |
|---|---|
| v1 / v2 / v3 / v3+bitmap throughput | 12.9 / 18.2 / 82.3 / 80.8 M events/s |
| v3 versus v1 | 6.4x |
| Allocator share of profile, v1 / v2 / v3 | 42% / 57% / 0% |
| v3+bitmap latency, p99 / p99.9 / p99.99 | 42 / 85 / 211 ns (41 ns clock tick) |
| Worst-case gap, v3 / v3+bitmap | 0.12 / 153 M events/s |
| Wire to wire, 1 thread / pipeline | 54.4 / 22.6 M msgs/s |
| Pipeline latency at 1M msgs/s, p50 / p99 / p99.9 | 167 / 251 / 3,519 ns |
| Differential test size | 8 million events per run, 110 tests |
| Hardware | Apple M4, macOS 15.5, Apple clang 17, `-O3` |

If asked for a number you do not remember, say so and say where it is recorded. Do not guess.

## Things not to claim

- Do not compare these numbers with any real exchange or trading firm system.
- Do not describe the strategy study as a trading result.
- Do not say "lock-free matching engine". The book is single-threaded by design; the queue is lock-free.
- Do not call it "production-grade". Call it carefully tested and measured.
- If asked how the project was built, answer truthfully, including any AI assistance. The commit history
  records it. What matters in the room is that you can explain and extend every part yourself.
