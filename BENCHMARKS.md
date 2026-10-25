# Benchmarks

## Reproducing these results

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target run_benchmark
./build/run_benchmark <num_events> <seed> <map|flat>     # single run
bench/run_benchmarks.sh ./build/run_benchmark <seed>      # full 100k/1M/10M sweep, both engines
```

`<seed>` fixes the synthetic event stream (see `bench/event_generator.hpp`
for the generator itself) so a given seed reproduces the identical
sequence of events on any machine; the absolute throughput/latency
numbers will still vary with the host's hardware (these were produced on
a single-vCPU sandbox -- see the machine note below -- so they are
directional, not something to expect to match exactly elsewhere).
`run_benchmark` is not registered as a CI test (`add_test`); it is a
benchmark, not a pass/fail correctness check, and is run manually.

## Methodology

**Event generation.** `bench/event_generator.hpp` produces a synthetic
stream with the mix used throughout this project's testing (70% submit /
20% cancel / 10% replace; among submits, 55% Limit / 15% Market / 15% IOC
/ 15% FOK). Order prices cluster around a midprice that random-walks over
the course of the run (small Gaussian steps per event, offsets from it
drawn from a geometric distribution so most orders land close to the
touch) rather than being drawn uniformly across a fixed range -- a uniform
distribution would make the book far wider and flatter than any real book
ever is, which would bias the comparison in a way that doesn't reflect
realistic usage.

**What's compared.** The Phase 1 engine (`std::map<Price, PriceLevel>`
with `std::list<Order>` per level) against a second implementation built
specifically for this comparison: `FlatVectorOrderBook` / `FlatMatchingEngine`
under `bench/`, implementing Phase 0 §2 Option B -- a sorted
`std::vector<PriceLevel>` with binary search, keeping the per-level
container (`std::list`) identical to isolate exactly one variable (the
top-level structure) rather than conflating two changes. **Before
benchmarking it, its correctness was verified**
(`bench/check_flat_correctness.cpp`): 200 randomized streams x 500 events,
run through both engines with identical inputs, asserting every
`ExecutionReport` matches exactly. Zero divergence. A benchmark comparing
a buggy implementation against a correct one produces numbers, not
evidence -- this had to pass before any of the numbers below meant
anything.

**Measurement.** Each (scale, engine) pair runs as a **separate process**
(`bench/run_benchmarks.sh` orchestrates this), specifically because
`VmHWM` (peak resident set size) is a running high-water mark for the
whole process -- running both engines in one process would attribute
whichever engine used more memory to both, not to the one that actually
did. Per-event latency is `std::chrono::steady_clock` wall time around
each `submit`/`cancel`/`replace` call. Scales: 100k, 1M, 10M events, seed
42 (same seed across engines, so both see an identical stream).

**What this machine is.** A single-vCPU container (Intel Xeon @ 2.10GHz,
no exclusive access, no guaranteed cache topology). These numbers are
**relative and directional, not absolute production figures** -- they are
suitable for comparing the two implementations against each other on
identical inputs, not for claiming "this engine does N events/sec in
production."

**Cache miss rate: not measured, disclosed honestly.** `perf` is not
installed in this sandbox, and `/proc/sys/kernel/perf_event_paranoid` is
set to `2`, which blocks unprivileged hardware-counter access even if it
were. This is a real gap against the Phase 2 scope, not a number I'm
omitting because it was inconvenient. **To fill it in:** on a machine with
`perf` installed and accessible (`perf_event_paranoid <= 1`, or run as
root in CI):
```sh
perf stat -e cache-references,cache-misses,instructions,cycles \
    ./run_benchmark 1000000 42 map
perf stat -e cache-references,cache-misses,instructions,cycles \
    ./run_benchmark 1000000 42 flat
```

## Results

| Scale | Engine | Throughput (events/sec) | p50 | p95 | p99 | max | mean | Peak RSS |
|---|---|---|---|---|---|---|---|---|
| 100k | map+list (Phase 1) | 4,385,810 | 110ns | 395ns | 652ns | 1.21ms | 185ns | 15.9MB |
| 100k | flat_vector (Option B) | 3,789,431 | 105ns | 391ns | 619ns | 2.10ms | 200ns | 15.7MB |
| 1M | map+list (Phase 1) | 3,854,833 | 116ns | 731ns | 1,352ns | 2.07ms | 212ns | 124.4MB |
| 1M | flat_vector (Option B) | 4,472,201 | 107ns | 543ns | 910ns | 394µs | 180ns | 124.6MB |
| 10M | map+list (Phase 1) | 1,369,374 | 171ns | 2,748ns | 9,021ns | 8.23ms | 602ns | 1176.8MB |
| 10M | flat_vector (Option B) | 2,126,330 | 152ns | 1,523ns | 3,676ns | 4.15ms | 372ns | 1176.9MB |

Peak memory is essentially identical between the two implementations at
every scale (expected -- the per-level `std::list<Order>` storage, which
dominates memory, is unchanged between them; only the small top-level
level-indexing structure differs).

At 100k, the two are indistinguishable (within noise of each other,
flat_vector's mean is actually marginally worse). The gap opens up and
grows with scale: by 10M events, flat_vector is ~55% higher throughput
and its p99 is ~2.5x better. **The obvious story is "tree traversal cost
caught up with std::map at scale." That story turned out to be wrong, or
at least incomplete** -- see below.

## The surprising result, and what actually explains it

The naive explanation would be: more price levels means a deeper
`std::map` tree, means more cache-unfriendly pointer chasing on every
lookup, means the flat, contiguous `std::vector` wins at scale. That is
directionally true but understates how small the actual level-count
growth is: diagnostic instrumentation (`eng.book().levelCount(...)`)
shows the book ends a 1M-event run with ~260 total distinct price levels,
and a 10M-event run with ~1,131 -- a ~4.3x growth. `log2(1131) ≈ 10.1`
versus `log2(260) ≈ 8.0`: two extra tree-depth comparisons does not
explain a 55% throughput gap on its own.

Breaking down latency **by event type** (`fok_lat` vs `other_lat` in
`run_benchmark.cpp`) shows what's actually going on:

| Scale | Engine | FOK submits: mean | Other submits: mean | Ratio |
|---|---|---|---|---|
| 1M | map+list | 817ns | 167ns | 4.9x |
| 1M | flat_vector | 403ns | 191ns | 2.1x |
| 10M | map+list | 3,953ns | 227ns | 17.4x |
| 10M | flat_vector | 978ns | 376ns | 2.6x |

**FOK orders are dramatically more expensive than everything else, and
that cost grows with the number of distinct price levels** -- because
`aggregateFillable()`, the dry-run pass that makes FOK atomic (Phase 0
§4.5, §3.11), walks `depth(opp_side, levelCount(opp_side))`: every price
level on the opposite side within the order's price limit, in the worst
case. This is `O(L)` in the number of distinct levels, not `O(log L)`.
From 1M to 10M events, level count grew ~4.3x and map+list's FOK-submit
mean grew ~4.8x -- a close match, and strong evidence the dry-run scan,
not the top-level container's asymptotic lookup complexity, is the
dominant cost at scale.

This also explains *why* flat_vector pulls ahead specifically at scale
despite both implementations sharing the identical `O(L)` dry-run logic
(it's literally the same loop, calling `depth()` on whichever book type):
`std::vector`'s `depth()` walks contiguous memory; `std::map`'s iterates a
red-black tree in sorted order, which is pointer-chasing between
scattered nodes. Same big-O complexity class, very different constant
factor -- exactly the cache-behavior distinction Phase 0 §2 predicted, but
showing up in a different operation (`aggregateFillable`'s per-level
walk) than the one that was expected to matter (`best price` lookup,
which is `O(1)`/`O(log L)` either way and barely shows up in the
other-submits column).

Why did level count grow at all, rather than staying roughly bounded?
The midprice random-walks over the length of a single run (Gaussian
steps compound: over 10M events, standard deviation of total drift is
large), and the generator's cancel/replace events pick target ids from
an ever-growing pool that includes orders resting far from the *current*
midprice, near wherever the midprice was earlier in the run. Those
far-from-touch orders never get randomly cancelled proportionally to how
far they've drifted, so the book accumulates a long tail of stale,
distant price levels. This is a property of the synthetic generator over
a long single run, not necessarily of a real market (where stale resting
orders are more aggressively managed by their owners) -- worth noting as
a limitation of the benchmark's realism, not just a finding about the
engine.

## Post-fix results

The `aggregateFillable` fix described above was implemented immediately
after this analysis, rather than deferred, specifically because it was a
small, well-understood, evidence-backed change with no design ambiguity
left to resolve -- there was no reason to carry a known, already-diagnosed
inefficiency forward into Phase 3. The fix: `OrderBook` gained
`forEachLevelWhile(side, fn)`, which stops pulling levels from the
map/vector the instant `fn` returns `false`, instead of
`aggregateFillable` calling `depth(opp, levelCount(opp))` -- which
unconditionally built a vector of **every** level on the opposite side
before any early-exit logic ever ran on top of it. The early-exit
(`break` once enough quantity is found) was always correct; it just sat
downstream of a pass that had already paid the full `O(L)` cost
regardless.

Full regression suite re-run after the fix: all 42 unit tests, 300x1000
property-test streams, 300x300 differential streams, all clean (including
under ASan+UBSan). The fix did not change any `ExecutionReport` the
engine produces -- only how cheaply it gets there -- which the
differential test against the untouched reference engine confirms
directly.

| Scale | Metric | Before fix | After fix | Change |
|---|---|---|---|---|
| 1M | throughput | 3,854,833 ev/s | 5,447,378 ev/s | +41% |
| 1M | FOK-submit mean latency | 817ns | 105ns | **7.8x faster** |
| 1M | overall p99 | 1,352ns | 461ns | 2.9x better |
| 10M | throughput | 1,369,374 ev/s | 3,299,478 ev/s | **+141%** |
| 10M | FOK-submit mean latency | 3,953ns | 139ns | **28.4x faster** |
| 10M | FOK-submit p99 | 15,904ns | 594ns | **26.8x better** |
| 10M | overall p99 | 9,021ns | 645ns | 14x better |

**The fixed map+list engine now beats the (untouched) flat_vector
alternative at every scale** -- including 10M events, where flat_vector
had a 55% throughput lead over the *unfixed* map engine:

| Scale | map+list (fixed) | flat_vector (Option B, unfixed) |
|---|---|---|
| 100k | 5,846,278 ev/s | 3,789,431 ev/s |
| 1M | 5,447,378 ev/s | 4,472,201 ev/s |
| 10M | 3,299,478 ev/s | 2,126,330 ev/s |

This directly confirms the Phase 2 conclusion below, with before/after
evidence rather than just reasoning about complexity classes: the
container swap would have improved a constant factor on a redundant O(L)
pass; fixing the redundant pass itself was the actual lever, and it was
bigger than the container swap would have been even if taken on its own
terms. `bench/flat_vector_order_book.hpp` / `bench/flat_matching_engine.hpp`
are left exactly as they were when benchmarked -- they already served
their purpose as the comparison point this conclusion is based on, and
re-optimizing them now would just be re-deriving the same answer with
extra steps.

## Conclusion: what I'd actually change, and what I wouldn't

**I would not switch the core data structure to the flat vector**, despite
it winning the throughput comparison, for two reasons. First, per Phase
0's ground rule, the simple, provably-correct structure should only be
replaced on specific, demonstrated evidence that the *thing actually
causing the slowdown* is the structure itself -- and the breakdown above
shows the top-level container's own complexity class (`O(log L)` lookup)
is not the dominant cost; the `O(L)` FOK dry-run is. Switching containers
without fixing that loop would still leave `aggregateFillable` scaling
linearly with level count, just with a better constant factor -- treating
a symptom, not the cause. Second, `std::vector`'s O(n)-shift cost on level
insertion/removal (Phase 0 §2 Option B's named weakness) was not
exercised heavily by this benchmark's mostly-near-the-touch order
placement, so this comparison doesn't actually have evidence about that
cost; a workload with more level churn across the whole price range could
look different.

**What I changed: fixed `aggregateFillable`'s redundant O(L) materialization
directly** (see "Post-fix results" above), independent of which container
holds the levels. It turned out not to even require a genuinely new data
structure (a running cumulative-quantity figure, which was the first
thing I considered) -- the existing early-exit logic was already correct,
it was just sitting downstream of a `depth()` call that threw its benefit
away by building the full list of levels first. Removing that one
unnecessary intermediate pass (`OrderBook::forEachLevelWhile`, which
pulls levels lazily and stops the moment the caller says it has enough)
was sufficient, and the results above show it was a considerably bigger
win than the container swap would have been -- exactly the kind of fix
that only comes from profiling and breaking latency down by event type,
not from intuiting which data structure "should" be faster.

**What I'd want before trusting these numbers for anything beyond this
comparison:** real hardware-counter data (`perf stat`, once available on
a machine that supports it) to confirm the cache-miss-rate story directly
rather than inferring it from the shape of the timing data, and a second
benchmark run with a flatter, wider price distribution specifically
designed to exercise `std::vector`'s O(n)-shift weakness, which this run's
near-the-touch-clustered order placement did not stress.
