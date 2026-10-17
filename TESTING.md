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
