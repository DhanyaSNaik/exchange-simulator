# Limit Order Book & Matching Engine

A price-time-priority limit order book and matching engine, written in
modern C++. Built as a portfolio project targeting quant trading firms --
the point is not raw throughput (this will never see production load), but
the signals C++ is uniquely suited to demonstrate: RAII and object
lifetime discipline, deliberate STL container choice backed by reasoning
about iterator invalidation and cache behavior, templates, and
profiling-driven (not assumed) optimization.

## What's here (Phase 1-3 / MVP)

- Price-time priority matching for Market, Limit, IOC, and FOK orders,
  plus cancel and replace (with correct priority-preservation semantics).
- A strict module boundary between pure storage (`OrderBook`, which knows
  nothing about order-type semantics) and matching logic (`MatchingEngine`,
  which owns all of it).
- 41 unit tests, including 4 adversarial cases surfaced by reasoning about
  feature *interactions* rather than features in isolation.
- A custom randomized-event-stream property-testing harness that checks
  every structural invariant after every single event, not just at the
  end of a run.
- A deliberately naive, obviously-correct reference engine, differentially
  tested against the real engine on identical randomized event streams.
- Basic microstructure queries: spread, midprice, microprice, order
  imbalance, depth, queue position.
- CI (GitHub Actions) running the full build + test suite under both
  AddressSanitizer and UndefinedBehaviorSanitizer, plus `clang-format` and
  `clang-tidy`, on every commit.
- Deterministic market-data replay: a synthetic CSV order-flow format
  (new order / cancel / replace / trade / clock-tick), ingested and
  replayed in strict timestamp+file-order, against the same
  `MatchingEngine`. Trade events apply directly to a named resting order
  (no aggressor to resubmit -- see `DESIGN_DECISIONS.md`), rather than
  being forced through normal matching.

See `ARCHITECTURE.md` for module/class/sequence diagrams,
`DESIGN_DECISIONS.md` for the alternatives considered and why each choice
was made, `TESTING.md` for the test strategy and how to reproduce a
property-test failure, and `BENCHMARKS.md` for throughput/latency/memory
numbers at 100k/1M/10M events, including a naive-vs-optimized comparison
against Phase 0's flat-vector alternative and the (initially
counter-intuitive) explanation for why it wins at scale.

## Benchmarking

```sh
cmake --build build --target run_benchmark
./build/run_benchmark 1000000 42 map     # single run
bench/run_benchmarks.sh ./build/run_benchmark 42   # full 100k/1M/10M sweep, both engines
```

`bench/` also contains `FlatVectorOrderBook` / `FlatMatchingEngine` -- the
Phase 0 Option B alternative, built and differentially-tested against the
real engine specifically to produce `BENCHMARKS.md`'s comparison. It is
deliberately not part of the `include/lob/` library; see
`DESIGN_DECISIONS.md`'s Phase 2 entry for why it wasn't promoted despite
winning the throughput comparison.

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

## Replaying a synthetic order-flow file

```sh
cmake --build build --target unit_tests   # includes CSV reader + replay tests
```

See `tests/unit/test_csv_event_reader.cpp` for the exact CSV column
format with worked examples of every event kind, and
`tests/property/replay_property_test.cpp` for the end-to-end harness
(random events -> real CSV text -> parsed back -> replayed -> invariants
checked after every event).

## Future work (explicitly deferred, not forgotten)

Deferred by deliberate MVP scope decision (see `DESIGN_DECISIONS.md`), not
by oversight:

- **LOBSTER-format ingestion** -- no real LOBSTER sample data was
  available to build or test a reader against; the synthetic CSV reader
  is built so a second reader producing the same `OrderFlowEvent` struct
  is a bounded, additive piece of work once sample data exists.
- **Full latency simulation** (vs. the measurement-only hooks here) --
  injected network/exchange/agent delay, not just timing instrumentation.
- **Self-trade prevention** -- no `participant_id` exists on `Order` yet;
  adding one is the natural next step once agent identity matters (e.g.
  studying the market-maker agent's own self-trade behavior).
- **Iceberg / hidden orders**
- **Full agent roster** beyond the single market-maker agent
- **Multi-venue** matching / routing
- **Visualization dashboard**
- **Research / analytics module** (Python, layered on top -- see project
  plan: C++ is the engine, Python is for research, deliberately not mixed)
- **GPU / Rust** stretch goals

None of the above will be built unless the MVP checklist above is airtight
with real time to spare.
