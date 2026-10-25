# Exchange Simulator

A simulated exchange, built in modern C++: a price-time-priority limit
order book and matching engine, deterministic market-data replay, a
market-maker agent with inventory-aware quoting and PnL tracking, and
core microstructure analytics. Built as a portfolio project targeting
quant trading firms.

The point of the project is not raw throughput -- this will never see
production load. It's the signals C++ is uniquely suited to demonstrate:
RAII and object lifetime discipline, deliberate STL container choice
backed by reasoning about iterator invalidation and cache behavior, and
profiling-driven (not assumed) optimization. The system was built in five
gated phases, each reviewed before the next started; `DESIGN_DECISIONS.md`
is the record of what was actually decided at each one, including the
things that only became visible once code existed.

## Repository layout

```
include/lob/        Public headers -- the library's actual interface
src/                 Implementation
tests/
  unit/              Discrete, named test cases -- one behavior each
  property/          Randomized event streams, invariants checked after
                      every single event, not just at the end of a run
  reference/          A deliberately naive, obviously-correct reference
                      engine, differentially tested against the real one
bench/               Benchmark harness + the Phase 0 "Option B" flat-
                      vector alternative, built specifically to produce
                      BENCHMARKS.md's naive-vs-optimized comparison
examples/            Runnable, end-to-end demos (see below) + sample
                      order-flow files
```

## What's here

- **Matching engine** (Phase 1): price-time priority for Market, Limit,
  IOC, and FOK orders, plus cancel and replace with correct
  priority-preservation semantics. A strict module boundary between pure
  storage (`OrderBook`, which knows nothing about order-type semantics)
  and matching logic (`MatchingEngine`, which owns all of it).
- **Benchmarking** (Phase 2): throughput/latency/memory at 100k/1M/10M
  events, a real naive-vs-optimized comparison against an alternative
  data structure, and a profiling-driven fix to an actual bug the
  comparison surfaced -- not a container swap, a fixed redundant scan.
  See `BENCHMARKS.md`.
- **Market-data replay** (Phase 3): a synthetic CSV order-flow format
  (new order / cancel / replace / trade / clock-tick), ingested and
  replayed in strict timestamp-then-file-order against the real engine.
  Trade events apply directly to a named resting order with no aggressor
  to resubmit -- a real market-data feed doesn't give you one.
- **Market-maker agent + analytics** (Phase 4): inventory-aware
  two-sided quoting (hard inventory limit, linear inventory skew),
  adverse-selection-aware spread widening, average-cost PnL (realized +
  unrealized, marked to midprice), order flow imbalance, realized
  volatility over a rolling time window, and trade-sign classification.
- **95 unit tests**, a custom randomized-event-stream property-testing
  harness, differential testing against a naive reference engine, and
  CI (GitHub Actions) running the full build + test suite under both
  AddressSanitizer and UndefinedBehaviorSanitizer, plus `clang-format`
  and `clang-tidy`, on every commit.

See `ARCHITECTURE.md` for module/class/sequence diagrams,
`DESIGN_DECISIONS.md` for the full decision log (start with its Index if
you have five minutes, not fifty), `TESTING.md` for the test strategy and
how to reproduce a property-test failure, and `BENCHMARKS.md` for the
full benchmark methodology and results.

## Building

```sh
cmake -B build
cmake --build build
cd build && ctest --output-on-failure
```

Sanitizer build:

```sh
cmake -B build-san -DSANITIZE=address,undefined -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-san
cd build-san && ctest --output-on-failure
```

## Running the examples

Two runnable demos under `examples/`, each reading a CSV order-flow file
and printing the result -- this is the fastest way to see the whole
system work end to end rather than reading test assertions.

**Plain replay** (matching engine + replay, no agent):

```sh
cmake --build build --target replay_demo
./build/replay_demo examples/basic_order_flow.csv
```

```
Parsed 10 events from examples/basic_order_flow.csv

Replay report: 10 processed, 0 rejected.

Final book state:
  Bids (best first): [9800 x50]
  Asks (best first): [9850 x15] [10200 x25]

  Spread:     50
  Midprice:   9825
  Microprice: 9838.46
  Order imbalance (top level): 0.538462
```

**Market maker** (matching engine + replay + agent + PnL):

```sh
cmake --build build --target market_maker_demo
./build/market_maker_demo examples/market_maker_scenario.csv
```

```
Replaying 6 events from examples/market_maker_scenario.csv

Final agent state:
  inventory:     6
  avg_cost:      98
  realized_pnl:  16

Marked to current midprice (98.5):
  unrealized_pnl: 3
  total_pnl:      19
```

`examples/market_maker_scenario.csv` reproduces, as an actual replayable
file, the exact scenario hand-calculated by hand in
`tests/unit/test_pnl_hand_calculation.cpp` (Phase 4's stop condition) --
`inventory`, `avg_cost`, and `realized_pnl` above match that hand
calculation exactly. The final `unrealized`/`total` numbers differ from
that test's because this run marks to the engine's *actual* final
midprice (98.5, since the agent's own resting bid was fully consumed by
the last fill and the book's best bid reverted to the wider seeded order)
rather than the test's arbitrarily-chosen mark of 100.0 -- both are
correct; they're marking to different reference prices, by design, and
`market_maker_demo.cpp`'s own comments walk through why.

## Benchmarking

```sh
cmake --build build --target run_benchmark
./build/run_benchmark 1000000 42 map     # single run
bench/run_benchmarks.sh ./build/run_benchmark 42   # full 100k/1M/10M sweep, both engines
```

`bench/` also contains `FlatVectorOrderBook` / `FlatMatchingEngine` -- the
Phase 0 Option B alternative, built and differentially-tested against the
real engine specifically to produce `BENCHMARKS.md`'s comparison. It is
deliberately not part of `include/lob/`; see `DESIGN_DECISIONS.md`'s
Phase 2 entry for why it wasn't promoted despite winning the initial
throughput comparison.

## Known Limitations / Future Work

Everything below is a deliberate scope decision, named explicitly here
(and, in most cases, argued for in `DESIGN_DECISIONS.md`) -- not a gap
nobody noticed. This is meant to read as evidence of scope discipline,
not as an apology:

- **Self-trade prevention** -- no `participant_id` exists on `Order`;
  two crossing orders match like any other pair regardless of who
  submitted them. The natural next step once agent identity matters
  beyond a single agent.
- **LOBSTER-format ingestion** -- no real LOBSTER sample data was
  available to build or test a reader against. The CSV reader is built
  so a second reader producing the same `OrderFlowEvent` struct is a
  bounded, additive piece of work once sample data exists.
- **Full latency simulation** (vs. the measurement-only hooks that
  exist) -- injected network/exchange/agent delay, not just timing
  instrumentation.
- **Full Avellaneda-Stoikov inventory skew** -- the current linear proxy
  is explicitly not this; real A-S derives the optimal skew from risk
  aversion, volatility, and time-to-horizon via a closed-form solution.
- **FIFO lot-accounting PnL** (vs. the current average-cost method).
- **Soft inventory tapering** (vs. the current hard stop at the limit).
- **Continuous/exponentially-decaying adverse-selection signal** (vs.
  the current fixed trigger-and-decay window).
- **Full Lee-Ready trade-sign classification** (the current quote rule
  has no tick-rule fallback for exactly-at-midpoint trades).
- **Iceberg / hidden orders.**
- **Full agent roster** beyond the single market-maker agent.
- **Multi-venue matching / routing.**
- **Visualization dashboard.**
- **Research / analytics module** (Python, layered on top -- deliberately
  not mixed with the C++ engine; see the project's original scope
  decision in `DESIGN_DECISIONS.md`).
- **Reinforcement learning agents** -- a materially different approach
  from the rule-based market maker here (learned policy vs. hand-specified
  quoting logic), not an extension of it.
- **DuckDB/Polars research infrastructure** -- deliberately a separate
  project, not something to bolt onto this one; this repo is the C++
  exchange simulator, not a research platform.
- **GPU / Rust stretch goals.**

None of the above will be built unless a specific, demonstrated need
arises -- the same evidence-before-action discipline applied to every
decision in `DESIGN_DECISIONS.md`.
