# Architecture

## Module boundary

`OrderBook` (storage: price levels, FIFO, lookup-by-id) knows nothing about
order-type semantics. `MatchingEngine` (Market/Limit/IOC/FOK, cancel,
replace, trade generation) owns all of that and treats `OrderBook` as a
correct, dumb storage layer it calls into. This boundary is what let
`OrderBook` be unit-tested in complete isolation (`test_order_book.cpp`)
from matching logic, and is the seam along which Phase 2's naive-vs-
optimized data structure comparison will happen without touching
`MatchingEngine` at all.

```
orders/            -> include/lob/order.hpp            (pure data, no behavior)
order_book/        -> include/lob/order_book.hpp + src/order_book.cpp
matching_engine/   -> include/lob/matching_engine.hpp + src/matching_engine.cpp
tests/             -> unit/, property/, reference/ (differential naive engine)
```

## Core types

- `Order` -- id, side, type, price, original/remaining quantity, engine-
  assigned sequence, status.
- `Trade` -- trade id, aggressor id, resting id, price (always the resting
  side's), quantity, sequence.
- `ExecutionReport` -- what `submit`/`cancel`/`replace` return: status,
  filled/remaining quantity, trades, reject reason.
- `PriceLevel` -- a price and a `std::list<Order>` (not `std::deque`; see
  DESIGN_DECISIONS.md for why) plus a cached `total_qty`.
- `OrderBook` -- `std::map<Price, PriceLevel>` for each side (opposite
  comparators so `begin()` is always "best price" on both), plus
  `std::unordered_map<OrderId, Locator>` for O(1)-average lookup/cancel.
- `MatchingEngine` -- owns one `OrderBook`, a sequence counter, a trade-id
  counter, and all order-type semantics.

## Order submission -> match -> trade generation

1. `submit(order)` assigns the next sequence number -- every event
   consumes one, including rejected ones (monotonicity, not "only valid
   events count").
2. Validates (non-positive quantity -> `ZeroQuantity`; non-Market order
   with non-positive price -> `InvalidPrice`; id already resting ->
   `DuplicateId`).
3. If the order is FOK: a dry-run (`aggregateFillable`) sums resting
   liquidity within the order's price limit across however many levels are
   needed, *without mutating the book*. If insufficient, reject
   (`FOKUnfillable`) with zero book mutation -- this is what makes FOK
   atomic across multiple price levels rather than leaving a partial fill
   committed if insufficient liquidity is discovered partway through a
   sweep.
4. `matchAggressive(order)` walks the opposite side's best-price-first
   orders, generating one `Trade` per resting order consumed (fully or
   partially), mutating the book by **id** via `OrderBook::reduceQuantity`
   (never through a pointer/reference into the book -- see
   DESIGN_DECISIONS.md).
5. Any quantity left over: a `Limit` order rests the remainder in the
   book; `Market`/`IOC`/`FOK` orders never rest -- the remainder (if any)
   is discarded.

## Cancel and replace

- **Cancel**: look up by id (`OrderBook::remove`), which does FIFO-
  preserving erase-from-list plus empty-level cleanup internally. Unknown
  id returns a clean `Rejected`/`UnknownOrder` report rather than throwing.
- **Replace**: read the current order (`OrderBook::get`, a copy). If the
  request is a *strict quantity decrease at an unchanged price*, mutate in
  place (`decreaseQuantityInPlace`) -- time priority preserved, no new
  sequence number. Any price change, or any quantity increase (or an
  explicitly-restated-but-unchanged value -- see the adversarial test for
  this), forfeits priority: the order is removed and resubmitted through
  the normal `submit()` path under a fresh sequence number. Resubmitting
  through `submit()` (rather than re-inserting directly) is deliberate --
  it means a replace whose new price now crosses the book trades
  immediately instead of silently re-resting at a crossing price.

## Why `std::list`, not `std::deque`, for a price level

`std::deque::erase` invalidates iterators and references to *all* elements
at or after the erase point (only `push_back`/`pop_front` at the very ends
leave other iterators stable). `OrderBook` stores a
`std::list<Order>::iterator` per resting order specifically to get O(1)-
average cancel-by-id. With `std::deque`, that stored iterator would become
silently invalid the moment any *other*, earlier order at the same level
was cancelled -- a correctness bug, not a performance one, and exactly the
kind that would not show up until a property test hammered cancels in a
specific order. `std::list` gives the needed guarantee (erasing one node
never invalidates iterators to others) at the cost of worse cache locality
than `deque`. Correctness first, per the project's stated ground rule.

## Phase 3: market data replay (`replay/` -> `include/lob/replay_event.hpp`, `csv_event_reader.hpp`, `simulation_loop.hpp`)

One-directional dependency: `SimulationLoop` depends on `MatchingEngine`;
nothing in Phase 1/2 depends on replay. The module can be deleted without
touching the engine.

- `OrderFlowEvent` / `EventKind` -- the five event kinds (`NewOrder`,
  `Cancel`, `Replace`, `Trade`, `ClockTick`). `Trade` is deliberately
  **not** a `NewOrder` that happens to match -- see
  `DESIGN_DECISIONS.md` for why a real feed's reported executions can't
  be faithfully replayed through `submit()` at all.
- `parseCsvEvents()` -- parses the synthetic CSV format documented at the
  top of `csv_event_reader.hpp`. Any file-format problem (wrong header,
  wrong column count, unparseable value) throws `CsvParseError`
  **unconditionally** -- this is never governed by `SimulationLoop`'s
  `strict_mode`, which is about a different kind of problem (see below).
- `SimulationLoop::run()` sorts events by `(timestamp_ns, file_order)` --
  the latter breaking ties for events sharing a timestamp -- then
  dispatches each in that order. `strict_mode` (default `true`) decides
  whether a *rejected* event (bad `NEW`, unknown-id `CANCEL`/`REPLACE`,
  or an unresolvable `TRADE`) stops the run or is logged and skipped;
  applied uniformly across every event kind, not just `Trade`.
- `MatchingEngine::applyExternalTrade(resting_id, qty, price)` -- the one
  growth to the Phase 1/2 engine's own public interface this phase
  required. Directly reduces a named resting order and records a `Trade`
  with `aggressor_id = 0` (a reserved sentinel), bypassing
  `submit()`'s crossing logic entirely, because there is no aggressor
  order to match against in a replayed execution record.
- Latency instrumentation (`setLatencyHook`) and a tick hook
  (`setTickHook`) are measurement/subscription points only -- no delay is
  injected, and no hooks are registered by Phase 3 itself. They exist for
  Phase 4's agents to consume.

## Why `frontOfBook`/`get` return copies, not pointers

Early in Phase 1 API design, `frontOfBook` was going to return `Order*`
into the book's internal list node, to avoid a copy on every match
iteration. Rejected before implementation: the matching loop would be
holding a pointer into a container it mutates (via `reduceQuantity`) in
the same loop iteration, and the safety of that pointer would depend on an
unenforced call-order convention ("read the quantity before you mutate the
same node") rather than on anything the type system checks. `Order` is a
handful of scalars, so the copy is not costly, and all book mutation after
reading a copy happens strictly by id (`reduceQuantity(id, qty)`,
`remove(id)`), never through the copy itself.
