# Design Decisions

A running log: considered X, chose Y, because Z, revisit if [condition].
Phase 0's alternatives analysis and invariant list are carried forward
below; Phase 1 entries are what was actually decided once implementation
started, including a few things that only became visible once code existed.

## Core data structure: order book

**Considered:** (A) `std::map<Price, PriceLevel>` with `PriceLevel` holding
either `std::deque<Order>` or `std::list<Order>`; (B) a flat sorted
`std::vector<PriceLevel>` with binary search; (C) a tick-indexed flat array
plus intrusive linked list.

**Chose:** (A), specifically `std::map<Price, PriceLevel>` with
`PriceLevel::orders` as `std::list<Order>`.

**Because:** Tree overhead is a function of the number of distinct price
*levels*, which is small (tens to low hundreds) relative to order count at
any realistic scale -- there is no evidence yet that it matters. Within
`std::map`'s correctness guarantees (no tree-balancing bugs to write),
`std::list` was chosen over `std::deque` for the per-level container
specifically because `std::deque::erase` invalidates iterators/references
at or after the erase point, which silently breaks the O(1)-cancel-by-
stored-iterator design the moment a non-tail order at the same price level
is cancelled. `std::list` guarantees erasing one node never invalidates
iterators to others, at a real but acceptable cost in cache locality.

**Revisit if:** Phase 2 benchmarking shows tree-traversal cost on the hot
matching path is a measured bottleneck at target scale (100k/1M/10M
events) -- at which point (B) is the planned first experiment, with (C) as
the stretch target if (B) is still insufficient. The naive-vs-optimized
comparison in `BENCHMARKS.md` (Phase 2) is specifically built to produce
the evidence this decision would need to be revisited on.

## `frontOfBook()` / `get()` return a copy, not a pointer or reference

**Considered:** returning `Order*` into the book's internal `std::list`
node, to avoid a copy on every matching-loop iteration.

**Chose:** return `std::optional<Order>` (a copy).

**Because:** The matching loop reads a resting order's quantity and then,
in the same iteration, mutates the book (via `reduceQuantity`/`remove`) --
if that read were through a live pointer into the node being mutated, its
safety would depend on an unenforced "read before you mutate" convention
rather than on anything checked by the type system. `Order` is small
(a handful of scalars); the copy cost is not expected to matter at target
scale, and if it ever does, that's a Phase 2 profiling question, not an
assumption to design around now.

**Revisit if:** Phase 2 profiling shows the copy itself (not the
container access pattern) is a measured hot spot -- unlikely, but the
mechanism to check for it exists once benchmarks do.

## Replace semantics: what preserves time priority

**Decided (Phase 0, confirmed):** a strict quantity *decrease* at an
unchanged price preserves time priority (in-place mutation, same sequence
number). Any price change, or any quantity *increase*, forfeits priority
and is handled as cancel + resubmission under a fresh sequence number.
This matches real-exchange convention and avoids inventing a partial-
priority tie-breaking rule that would need its own justification.

**Surfaced during implementation (not anticipated in Phase 0):** what
about a replace request that re-specifies the *same* price and *same*
quantity explicitly, rather than omitting those fields? The implementation
only special-cases the fully-omitted case (both fields `nullopt`) as a
true no-op; an explicitly-restated-but-unchanged value still fails the
strict-decrease test and falls into the forfeit-priority branch. Decided
to leave this as-is (priority is forfeited) rather than add a second
special case for "explicitly unchanged," since (a) it is simpler, and
(b) the crossed-book invariant guarantees this can never produce a
spurious trade -- anything that could have crossed with this order while
it sat resting would already have traded against it before the replace
request arrived. Pinned by an adversarial test (`test_adversarial.cpp`).

## FOK atomicity: two-pass, not walk-and-mutate

**Decided (Phase 0, confirmed):** `aggregateFillable()` sums resting
liquidity within the order's price limit *without mutating the book* first;
only if that sum is sufficient does `matchAggressive()` run and actually
commit trades. A single-pass walk-and-mutate-as-you-go implementation
would leave a partial fill committed if insufficient liquidity were
discovered only after consuming part of the book.

**Surfaced during implementation:** the dry-run pass only needs to prove
*aggregate* liquidity is sufficient -- it does not need to (and does not)
reason about which individual resting orders will end up partially
consumed. That reasoning happens only in the second (committing) pass. An
adversarial test specifically checks that the committing pass stops at a
*quantity* boundary partway through a price level, not a *level* boundary
(`test_adversarial.cpp`, the two-price-level FOK case).

## Order id reuse after cancellation

**Surfaced during implementation, not an explicit Phase 0 item:** the
duplicate-id guard in `submit()` checks only currently-resting orders
(`OrderBook::contains`) -- there is no all-time-ever-used id ledger. This
means an id freed by cancellation or a full fill can be legitimately
reused by an unrelated later order. Decided this is correct MVP behavior
(an ever-growing id ledger is unneeded complexity with no demonstrated
need) and pinned it with an adversarial test rather than leaving it as an
unexamined side effect of how the index happens to be checked.

## Self-trade prevention: out of MVP scope

**Decided (Phase 0, confirmed):** no `participant_id` field exists on
`Order`. Two orders that would cross are matched exactly like any other
pair, regardless of who submitted them. This is named explicitly (both in
code comments and a dedicated test) as a scope decision, not a gap nobody
noticed -- adding a `participant_id` and a self-trade-prevention check is
listed as future work in `README.md`.

## Phase 2: core data structure stays `std::map` + `std::list` (not swapped to the flat vector)

**Considered:** switching to Phase 0 §2 Option B (`std::vector<PriceLevel>`,
sorted, binary search), based on `BENCHMARKS.md` showing it ~55% faster at
10M events.

**Chose:** keep the Phase 1 `std::map`/`std::list` implementation as the
real one; Option B stays a bench-only artifact under `bench/`, not
promoted into `include/lob/`.

**Because:** benchmark instrumentation (per-event-type latency breakdown,
not just aggregate throughput) showed the actual dominant cost at scale is
`MatchingEngine::aggregateFillable()`'s `O(L)` dry-run scan for FOK
orders -- not the top-level container's `O(log L)` lookup complexity,
which is what switching to a flat vector would address. Level count only
grew ~4.3x from 1M to 10M events, which doesn't explain a 55% throughput
gap on `O(log L)` grounds alone; the FOK-submit-specific latency breakdown
(4.9x -> 17.4x mean-latency ratio vs. other submits, map+list, 1M -> 10M)
tracks the level-count growth closely and points at the per-level scan,
not the container. Switching containers would improve the *constant
factor* of that same `O(L)` scan (contiguous vs. pointer-chased traversal)
without fixing its complexity class -- treating the symptom the benchmark
happened to measure, not the cause.

**Revisit if:** a future workload benchmark deliberately exercises
`std::vector`'s O(n)-shift weakness (wide price-range churn, not just
near-the-touch clustering, which is what this benchmark's generator
produced) -- this round of benchmarking does not have evidence about
that cost either way.

**Update -- fix implemented, not just planned:** `aggregateFillable`'s
redundant O(L) materialization (see next entry) was fixed immediately
rather than deferred, since it was small, well-understood, and had no
remaining design ambiguity. Post-fix, the map+list engine outperforms the
*unmodified* flat_vector alternative at every benchmarked scale,
including 10M events where flat_vector previously had a 55% lead. This
is strong retrospective confirmation of the decision above: the container
swap would have been a smaller, less-targeted win than fixing the actual
redundant pass, and is now also simply unnecessary. See `BENCHMARKS.md`
"Post-fix results" for before/after numbers.

## `OrderBook::forEachLevelWhile` -- removing a redundant full-side materialization

**Found:** `MatchingEngine::aggregateFillable` (the FOK atomicity dry-run)
called `book_.depth(opp, book_.levelCount(opp))`, which unconditionally
builds a `std::vector` of **every** level on the opposite side, before
the early-exit summation loop beneath it ever runs. The early-exit itself
was always correct (`break` once a non-crossing price is hit, or once
enough quantity is found) -- but it sat downstream of a pass that had
already paid the full O(L) cost of walking the whole side, on every
single FOK order, regardless of how few levels were actually needed. This
is not an inherent property of "checking fillability within a price
limit" -- it was a wasted intermediate step.

**Fixed:** added `OrderBook::forEachLevelWhile(side, fn)` -- a templated
method (defined inline in the header, since the callback type must be
visible at the call site) that iterates the map directly and stops
calling `++iterator` the moment `fn` returns `false`. `aggregateFillable`
now calls this instead of `depth()`, so the walk is bounded by however
many levels this specific order actually needs to either find enough
liquidity or hit a non-crossing price -- not by the total number of
levels resting on that side.

**Why not implemented as a maintained cumulative-quantity structure
instead** (the first option considered): that would have meant keeping
an additional running aggregate in sync with every insert/remove/reduce
across both the map-based and (if ever promoted) any alternative
implementation -- more state to keep correct, for a problem that turned
out not to need it. The actual defect was a wasted full-materialization
pass sitting in front of logic that already did the right thing; removing
the wasted pass was sufficient and is simpler to audit than a second
piece of synchronized state.

**Verified:** full regression (42 unit tests, 300x1000-event property
streams, 300x300-event differential streams, all under ASan+UBSan) passed
identically before and after -- this change alters cost, not behavior,
and the differential test against the untouched reference engine is what
makes that claim checkable rather than assumed.

## Phase 3: Trade is its own event kind, not a NewOrder that happens to match

**Considered:** modeling every replay event uniformly as something fed
through `MatchingEngine::submit()`, including recorded executions.

**Chose:** a separate `EventKind::Trade`, applied via a new
`MatchingEngine::applyExternalTrade(resting_id, qty, price)` that
directly reduces a named resting order and records a `Trade`, without
running `submit()`'s crossing logic at all.

**Because:** a real market-data feed (LOBSTER being the canonical named
example) reports an execution as "resting order X was hit for quantity
Y" -- it does not hand you a resubmittable aggressor order, because the
aggressor's identity is typically anonymized or simply absent from the
public feed. There is no honest way to replay that kind of record through
`submit()`, since `submit()` requires an aggressor order that the data
doesn't contain. Synthetic `NewOrder` events (ones we generate ourselves,
with no "real" aggressor to be faithful to) still go through `submit()`
and get matched by the engine's own logic -- the two paths exist for two
genuinely different kinds of input, not as a stylistic choice.

`Trade.aggressor_id` is set to the reserved sentinel `0` for every trade
produced this way (0 is never a valid caller-assigned `OrderId`
elsewhere in the engine, so this is safe to treat as "no aggressor order
exists for this trade").

## Phase 3: `SimulationLoop` defaults to `strict_mode = true`, applied uniformly across every event kind

**Considered:** defaulting to "skip and log" (continue past a rejected
event, recording it in the report) versus "fail fast" (stop the run at
the first rejection).

**Chose:** fail-fast by default (`strict_mode = true`), with
`strict_mode = false` as an explicit opt-in for exploratory work on a
known-messy feed.

**Because:** this project's whole posture, stated at the outset, is
correctness-first. A replay loop that silently reconstructs a *partial*
book state and calls it done is the one place that principle would be
violated by default behavior, not just risked by an edge case -- a
silently-wrong reconstruction looks like a complete, trustworthy result
right up until someone notices the numbers don't add up. "Skip and log"
earns its place as an opt-in for a specific, nameable use case (a known-
messy real-world dump, where best-effort reconstruction with a visible
list of what was dropped is genuinely what's wanted), not as the default
for a project built around correctness.

This is applied **uniformly** across every event kind -- a rejected
`NEW` (bad price/qty/duplicate id) or a `CANCEL`/`REPLACE` against an
unknown id stops the run exactly like a bad `TRADE` row would, rather
than only `applyExternalTrade` failures being treated as stoppable.
Special-casing only one event kind would have been an arbitrary
inconsistency, not a principled distinction -- a rejected `NEW` is just
as much a data-quality signal from the feed as a bad `TRADE` row.

**A separate, narrower decision in the same area:** file-format/parse
errors (wrong column count, unparseable number, unrecognized event kind)
are **always** fatal, regardless of `strict_mode` -- thrown by
`parseCsvEvents()` before any event reaches `SimulationLoop` at all.
`strict_mode` governs what to do when an otherwise well-formed event
can't be cleanly *applied*; it does not govern whether the file itself is
readable. Conflating the two would let a flag meant for row-level
data-quality issues silently paper over a whole-file problem (wrong file,
corrupted download, mismatched header).

## Phase 3: a real bug the end-to-end property test caught before it shipped

While writing `tests/property/replay_property_test.cpp` (random events ->
serialized to the actual CSV text format -> parsed back via
`parseCsvEvents()` -> replayed through `SimulationLoop` -> invariants
checked after every dispatched event), the test's own CSV serializer had
an off-by-one bug: the `Cancel` and `ClockTick` row templates had one
extra trailing comma each, producing 9 columns instead of 8. Every single
one of the first 200 random streams failed immediately on the CSV
round-trip step, since nearly every stream contains cancels and the
generator always appends a few `ClockTick` rows.

This is recorded here deliberately, not edited out of the history, for
two reasons. First, it's honest: catching a bug is normal and expected,
and a design-decisions log that only ever shows clean first-try
implementations would be less credible, not more. Second, it's a good
illustration of *why* the end-to-end property test (CSV text round-trip,
not just in-memory `OrderFlowEvent` structs handed directly to
`SimulationLoop`) was worth building as specified in the Phase 3 brief --
a narrower test that skipped CSV serialization entirely would never have
exercised the actual text format and would have missed this class of bug
completely. The fix was two one-character comma-count corrections;
verified against a small Python script computing exact field counts
rather than re-counting by eye a second time, after hand-counting commas
turned out to be the root cause of the bug in the first place.

## `ExecutionReport` gained a `sequence` field, after initial implementation

**Surfaced closing out Phase 1:** `ExecutionReport` had no way to expose
the sequence number an event consumed, which made sequence-monotonicity
untestable as a standalone property (it was only checkable indirectly,
by inference from trade ordering). Added `Sequence sequence = 0;`, with
`0` meaning "this report didn't consume a new sequence number" (`cancel()`
and an in-place quantity-decrease `replace()` don't mint one; every
`submit()` call does, including the internal resubmit inside a
priority-forfeiting `replace()`). Chosen over, e.g., exposing the engine's
internal counter directly, since the report is already the public
per-event result type and this keeps the "what sequence did THIS event
get" question answerable from the return value alone rather than from
engine internals.

## Testing: custom harness over a property-testing library

**Decided (Phase 0, confirmed):** a small, auditable custom
randomized-event-stream harness (`tests/property/property_test.cpp`),
rather than adopting RapidCheck, for Phase 1. Gives full control over
event-shape generation and invariant checking, and is itself a portfolio
signal (demonstrates understanding of what property testing does, rather
than depending on a library to do it). RapidCheck (specifically for
automatic shrinking of failing cases) remains named future work, to be
adopted only if manually narrowing a failing seed down by hand proves
genuinely painful in practice -- which has not yet happened.

## Differential testing: kept in MVP scope

**Decided (Phase 0, confirmed on review):** a deliberately naive, O(n)
linear-scan reference engine (`tests/reference/reference_engine.hpp`),
differentially tested against the real engine on identical randomized
event streams (`tests/reference/test_differential.cpp`). This caught
nothing wrong in this implementation (all streams matched from first
correct implementation), but it remains the strongest correctness
artifact in the repository precisely because it checks the real engine
against an independently-reasoned-about model rather than against its own
stated invariants, which could in principle all hold on a wrong answer.
