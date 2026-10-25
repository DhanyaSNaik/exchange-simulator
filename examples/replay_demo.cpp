// Example: replays a synthetic CSV order-flow file against a plain
// MatchingEngine (no agent) via SimulationLoop, then prints the final
// book state and core microstructure queries. Run with:
//
//   ./replay_demo examples/basic_order_flow.csv
//
// This demonstrates Phase 1 (matching) + Phase 3 (replay) together,
// end to end, against a real file -- not just unit-tested fragments.

#include <fstream>
#include <iostream>
#include <sstream>

#include "lob/csv_event_reader.hpp"
#include "lob/simulation_loop.hpp"

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

    std::cout << "Parsed " << events.size() << " events from " << argv[1] << "\n\n";

    MatchingEngine engine;
    SimulationLoop loop(engine);  // strict_mode=true (default): any
                                  // rejected event stops the run and is
                                  // reported, rather than silently
                                  // producing a partial reconstruction.

    ReplayReport report = loop.run(events);

    std::cout << "Replay report: " << report.events_processed << " processed, "
              << report.events_rejected << " rejected.\n";
    for (const auto& err : report.errors) {
        std::cout << "  ERROR: " << err << "\n";
    }

    std::cout << "\nFinal book state:\n";
    auto bid_depth = engine.depth(Side::Buy, 5);
    auto ask_depth = engine.depth(Side::Sell, 5);
    std::cout << "  Bids (best first): ";
    for (auto& [px, qty] : bid_depth) std::cout << "[" << px << " x" << qty << "] ";
    std::cout << "\n  Asks (best first): ";
    for (auto& [px, qty] : ask_depth) std::cout << "[" << px << " x" << qty << "] ";
    std::cout << "\n\n";

    if (auto spread = engine.spread()) std::cout << "  Spread:     " << *spread << "\n";
    if (auto mid = engine.midprice()) std::cout << "  Midprice:   " << *mid << "\n";
    if (auto micro = engine.microprice()) std::cout << "  Microprice: " << *micro << "\n";
    std::cout << "  Order imbalance (top level): " << engine.orderImbalance(1) << "\n";

    return (report.events_rejected == 0) ? 0 : 1;
}
