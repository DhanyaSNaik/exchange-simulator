# Limit Order Book & Matching Engine

A price-time-priority limit order book and matching engine, written in
modern C++. Built as a portfolio project targeting quant trading firms --
the point is not raw throughput (this will never see production load), but
the signals C++ is uniquely suited to demonstrate: RAII and object
lifetime discipline, deliberate STL container choice backed by reasoning
about iterator invalidation and cache behavior, templates, and
profiling-driven (not assumed) optimization.

## What's here (Phase 1 / MVP)

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

See `ARCHITECTURE.md` for module/class/sequence diagrams,
`DESIGN_DECISIONS.md` for the alternatives considered and why each choice
was made, `TESTING.md` for the test strategy and how to reproduce a
property-test failure, and `BENCHMARKS.md` (Phase 2) for throughput/latency
numbers at 100k/1M/10M events.

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

## Future work (explicitly deferred, not forgotten)

Deferred by deliberate MVP scope decision (see `DESIGN_DECISIONS.md`), not
by oversight:

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
