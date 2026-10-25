# Testing

## Coverage map

| Invariant (Phase 0 §3) | Checked where |
|---|---|
| Crossed-book (`best_bid < best_ask`) | `OrderBook::checkInvariants()`, called after every event in the property harness; also exercised directly in unit tests (exact-price-boundary case) |
| FIFO within a price level | `test_order_book.cpp` (direct queue-position checks); property harness implicitly, since any violation would show up as a differential-test mismatch |
| `level.total_qty == sum(order.remaining_qty)` | `OrderBook::checkInvariants()`, every event |
| No duplicate ids / index-book consistency | `OrderBook::checkInvariants()`, every event; `test_matching_engine.cpp` duplicate-id case |
| Cancelled/filled orders never resurface | `test_matching_engine.cpp` ("cancel of an already-fully-filled order"); `test_adversarial.cpp` (id reuse case, the adjacent and easily-confused scenario) |
| Liquidity conservation per trade | `test_matching_engine.cpp` fill-quantity assertions; `test_differential.cpp` (trade-by-trade equality against the reference engine) |
| Quantity conservation across a multi-level sweep | `test_matching_engine.cpp` ("multiple fills per incoming order"); `test_adversarial.cpp` (FOK two-level partial-consumption case) |
| Determinism | Implicit in every differential-test pass: identical input streams must produce byte-identical `ExecutionReport`s from two independently-written engines |
| Non-negativity | `OrderBook::checkInvariants()`, every event |
| Sequence monotonicity | Assigned once per `submit()` call, including rejected ones; directly asserted by a dedicated unit test (`test_sequence_monotonicity.cpp`) over a 5,000-event mixed random stream, checking every sequence-bearing report is strictly greater than the last |
| FOK atomicity | `test_matching_engine.cpp` (reject-with-zero-mutation case); `test_adversarial.cpp` (two-level partial-consumption case) |
| IOC/Market never rest | `test_matching_engine.cpp`; `EngineInvariantTracker::checkReport` in the property harness |
| Empty-level cleanup | `test_order_book.cpp`; `OrderBook::checkInvariants()` every event |
| Self-trade policy | Explicitly out of scope -- `test_matching_engine.cpp` documents the resulting (unprevented) behavior as intended |

## Unit tests vs. property tests vs. differential tests

- **Unit tests** (`tests/unit/`, 41 cases) are named, discrete,
  known-input/known-output: one behavior per test, including 4 adversarial
  cases surfaced by reasoning about *interactions* between features
  (replace + crossed-book invariant, cancel + id reuse, FOK + partial-level
  consumption, Market + garbage price field).
- **Property tests** (`tests/property/property_test.cpp`) generate random
  submit/cancel/replace event streams and call
  `OrderBook::checkInvariants()` after *every single event*, not just at
  the end of a run. Default CI run: 300 streams x 1000 events. Verified
  locally at larger scale (500 streams x 1000 events = 500,000 checked
  events; separately, 100 streams x 20,000 events = 2,000,000 checked
  events) with zero violations.
- **Differential tests** (`tests/reference/`) run identical random event
  streams through the real `MatchingEngine` and a deliberately naive,
  O(n)-linear-scan `ReferenceEngine`, asserting every `ExecutionReport`
  (status, filled/remaining quantity, and every trade's resting id, price,
  and quantity) matches exactly. This catches "internally consistent but
  wrong" bugs that invariant checks cannot, by construction. Default CI
  run: 300 streams x 300 events. Verified locally at 500 streams x 500
  events = 250,000 events with zero divergence.

## Reproducing a property or differential test failure

Both harnesses take `(num_streams, events_per_stream, base_seed)` as
positional CLI args and print the exact failing seed on any violation:

```sh
# re-run just the one failing stream
./property_test 1 <events_per_stream> <failing_seed>
./differential_test 1 <events_per_stream> <failing_seed>
```

Each stream's generator is seeded independently
(`base_seed + stream_index`), so the printed failing seed reproduces that
exact sequence of events deterministically.

## Sanitizers

Local verification (CI runs the same via `-DSANITIZE=address,undefined`):

```sh
g++ -std=c++17 -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
    -Iinclude -Itests src/*.cpp tests/unit/*.cpp -o unit_tests_san
./unit_tests_san
```

All 41 unit tests, 100 x 3,000-event property streams (150,000 events),
and 50 x 300-event differential streams were run under this build with
zero ASan or UBSan reports.

## Phase 3: market data replay

- **Unit tests** (`test_csv_event_reader.cpp`, 10 cases): one per event
  kind parsed correctly, file_order assigned by row position (not
  timestamp), every named parse-error condition from the file-level
  comment in `csv_event_reader.hpp` (missing/wrong header, wrong column
  count, unrecognized event kind, unparseable number, a value present
  where the kind requires it blank, invalid side/type) -- each confirmed
  fatal regardless of anything else, per the "parse errors are always
  fatal" decision in `DESIGN_DECISIONS.md`.
- **Unit tests** (`test_simulation_loop.cpp`, 12 cases): NEW/CANCEL/
  TRADE application, timestamp-then-file_order sort ordering (including a
  case specifically constructed so file_order and vector order disagree,
  to actually distinguish correct tie-breaking from an accidental
  pass), `strict_mode` true (stops immediately, applied uniformly across
  NEW/CANCEL/TRADE rejections) and false (logs and continues),
  `applyExternalTrade`'s failure modes and its aggressor-id-0 sentinel,
  and the "feed's reported price is authoritative, not the book's own
  price" behavior.
- **End-to-end property test** (`replay_property_test.cpp`): satisfies
  the Phase 3 stop condition directly. Random event streams are
  serialized to the actual CSV **text** format (not handed to
  `SimulationLoop` as in-memory structs), parsed back via
  `parseCsvEvents()`, replayed through `SimulationLoop(strict_mode=false)`
  against a real `MatchingEngine`, with every Phase 0 invariant checked
  (via `OrderBook::checkInvariants()`) after **every single dispatched
  event**. `strict_mode=false` is used here specifically so a long
  randomized stream keeps running past the random ids/quantities'
  frequent (expected) rejections, rather than stopping at the first one.
  Verified clean at 500 streams x 1,000 events (500,000 events) and,
  separately, under ASan+UBSan at 100 streams x 500 events (50,000
  events). This test's own CSV serializer had a real bug (see
  `DESIGN_DECISIONS.md`'s "a real bug the end-to-end property test
  caught" entry) caught on its very first run -- left in the history
  rather than quietly fixed and forgotten, since it's a good illustration
  of why the round-trip-through-real-text-format design was worth the
  extra effort over a narrower in-memory-only test.

## Phase 4: market maker agent + analytics

- **Unit tests** (`test_market_maker.cpp`, 7 cases): symmetric quoting
  around the reference, inventory-limit withdrawal, inventory-skew
  direction (with the indexing subtlety -- a skewed quote can become the
  *best* level rather than staying behind a wider seeded book, which an
  earlier draft of this exact test got wrong and a build-and-run caught
  immediately; left as a worked example in the test's own comments),
  adverse-selection triggering and its time-window boundary condition, a
  mixed-side run NOT triggering it, and the midprice-unavailable no-op case.
- **Unit tests** (`test_market_maker_pnl.cpp`, 7 cases): the average-cost
  method in isolation via direct `onFill()` calls -- opening, adding at a
  different price (weighted average), partial close (realized PnL,
  unchanged avg_cost), full close (avg_cost zeroed), a fill that flips
  through zero (closes the old position, opens a new one at the fill
  price), a short position's `unrealizedPnl` sign, and `totalPnl`
  combining both correctly.
- **Unit tests** (`test_market_session.cpp`, 4 cases): an external order
  crossing the agent's resting bid triggers `onFill`; a trade between two
  OTHER participants does not touch agent state; `applyExternalTrade`
  against the agent's resting ask triggers `onFill`; `cancel` is a pure
  pass-through with no fill side effects.
- **Unit tests** (`test_analytics.cpp`, 12 cases): `classifyTradeSign`'s
  ground-truth path (internal trades) and quote-rule fallback (at/above
  ask, at/below bid, strictly-between, no-quote-available); `OFI`'s
  per-event contribution formula for a size-only change, a bid price
  improvement, an ask price improvement, and window forgetting; realized
  volatility's zero/one-sample base case, a hand-matched squared-log-
  return sum, and the time-window aging-out behavior.
- **The gate scenario** (`test_pnl_hand_calculation.cpp`): the Phase 4
  stop condition. A short sequence -- seed a reference book, the agent
  quotes, three external orders cross its resting bid/ask in turn -- run
  through the REAL `MatchingEngine` + `MarketSession` + `MarketMakerAgent`
  pipeline end to end, with every intermediate state (inventory, avg_cost,
  realized_pnl) checked against a hand calculation written out in the
  test file's own comments, matching exactly what's presented in the
  conversation turn that closed this phase. Not an isolated formula
  check (that's `test_market_maker_pnl.cpp`) -- this exercises real
  price-time matching, fill detection through `MarketSession`, and the
  PnL update together, which is what the brief's stop condition actually
  asked to be validated.

## Known gaps (Phase 1, honest accounting)

- `clang-format`/`clang-tidy` are specified (`.clang-format`, `.clang-tidy`)
  and wired into CI, but could not be run in this sandbox (no network
  access to install either tool locally). They will run for real on the
  first CI push; this is flagged rather than silently assumed clean.
- The property harness's invariant walk is itself O(n) per event, so a
  very long single stream (not yet attempted beyond ~2M events total
  across streams) would start to dominate wall-clock time; this is a
  correctness-testing tool, not the Phase 2 benchmark harness, and is not
  intended to scale to 10M events in one run.
