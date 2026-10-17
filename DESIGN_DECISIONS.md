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
