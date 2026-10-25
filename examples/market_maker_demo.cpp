// Example: replays a CSV order-flow file through MatchingEngine +
// MarketSession + MarketMakerAgent together. Run with:
//
//   ./market_maker_demo examples/market_maker_scenario.csv
//
// NOTE: this does NOT use SimulationLoop. SimulationLoop is deliberately
// agent-ignorant (see DESIGN_DECISIONS.md, Phase 4's MarketSession
// entry) -- it only knows how to drive a plain MatchingEngine. Driving a
// MarketSession instead means repeating SimulationLoop's own
// sort-by-(timestamp,file_order)-then-dispatch logic here, in this
// example program, rather than in the library. This is intentional: it
// is example/glue code demonstrating how EXISTING pieces compose, not a
// new library feature -- Phase 5's brief is explicit that no new
// features are added in this phase.
//
// examples/market_maker_scenario.csv reproduces, as an actual replayable
// file, the exact scenario hand-calculated in
// tests/unit/test_pnl_hand_calculation.cpp: seed a reference book, let
// the agent quote on a ClockTick, then three external orders cross its
// resting bid/ask in turn. Expect inventory=6, avg_cost=98.0,
// realized_pnl=16.0 at the end -- matching that test's by-hand arithmetic
// exactly (unrealized/total PnL below are marked to this run's ACTUAL
// final midprice, which differs from the unit test's arbitrarily-chosen
// mark of 100.0, since the agent's own bid was fully consumed by the
// third fill and the book's best bid reverts to the wider seeded order).

#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>

#include "lob/csv_event_reader.hpp"
#include "lob/market_session.hpp"

using namespace lob;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: " << argv[0] << " <path-to-order-flow.csv>\n";
        return 1;
    }

    std::ifstream file(argv[1]);
    if (!file) {
        std::cerr << "could not open file: " << argv[1] << "\n";
        return 1;
    }
    std::ostringstream buf;
    buf << file.rdbuf();

    std::vector<OrderFlowEvent> events;
    try {
        events = parseCsvEvents(buf.str());
    } catch (const CsvParseError& e) {
        std::cerr << "parse error: " << e.what() << "\n";
        return 1;
    }

    std::stable_sort(events.begin(), events.end(),
                     [](const OrderFlowEvent& a, const OrderFlowEvent& b) {
                         if (a.timestamp_ns != b.timestamp_ns) {
                             return a.timestamp_ns < b.timestamp_ns;
                         }
                         return a.file_order < b.file_order;
                     });

    MatchingEngine engine;
    MarketMakerConfig config;
    config.base_half_spread = 2;
    config.quote_size = 10;
    config.inventory_skew_factor = 0.0;
    config.run_over_fill_threshold = 100;  // high enough that this
                                           // scenario's 3 fills never
                                           // trigger adverse-selection
                                           // widening -- kept simple on
                                           // purpose, see
                                           // DESIGN_DECISIONS.md.
    MarketMakerAgent agent(engine, config, /*first_order_id=*/1);
    MarketSession session(engine, agent);

    std::cout << "Replaying " << events.size() << " events from " << argv[1] << "\n\n";

    for (const auto& ev : events) {
        switch (ev.kind) {
            case EventKind::NewOrder: {
                Order order(ev.id, ev.side, ev.type, ev.price, ev.qty);
                session.submit(order, ev.timestamp_ns);
                break;
            }
            case EventKind::Cancel:
                session.cancel(ev.id, ev.timestamp_ns);
                break;
            case EventKind::Replace: {
                MatchingEngine::ReplaceRequest req{ev.id, ev.replace_price, ev.replace_qty};
                session.replace(req, ev.timestamp_ns);
                break;
            }
            case EventKind::Trade:
                session.applyExternalTrade(ev.ref_id, ev.qty, ev.price, ev.timestamp_ns);
                break;
            case EventKind::ClockTick:
                session.onTick(ev.timestamp_ns);
                break;
        }
    }

    const auto& st = agent.state();
    std::cout << "Final agent state:\n";
    std::cout << "  inventory:     " << st.inventory << "\n";
    std::cout << "  avg_cost:      " << st.avg_cost << "\n";
    std::cout << "  realized_pnl:  " << st.realized_pnl << "\n";

    if (auto mid = engine.midprice()) {
        std::cout << "\nMarked to current midprice (" << *mid << "):\n";
        std::cout << "  unrealized_pnl: " << agent.unrealizedPnl(*mid) << "\n";
        std::cout << "  total_pnl:      " << agent.totalPnl(*mid) << "\n";
    } else {
        std::cout << "\n(no midprice available to mark unrealized PnL)\n";
    }

    return 0;
}
