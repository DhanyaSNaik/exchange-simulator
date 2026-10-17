// Phase 3 end-to-end property test, satisfying the stop condition
// directly: this is "the replay loop tested end-to-end against the
// Phase 1 engine" -- not in-memory OrderFlowEvent structs handed
// straight to SimulationLoop, but round-tripped through the actual CSV
// TEXT format first, exactly as a real ingestion would receive it.
//
// Invariants are checked after EVERY dispatched event (via the latency
// hook, which fires right after dispatch() commits its effect), not
// just at the end of a run -- same discipline as tests/property/property_test.cpp.
// strict_mode=false is used here specifically so a long randomized
// stream keeps running past the (expected, frequent) rejections random
// ids/quantities produce, rather than stopping at the first one.

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <random>
#include <sstream>
#include <vector>

#include "lob/csv_event_reader.hpp"
#include "lob/simulation_loop.hpp"

using namespace lob;

namespace {

// Serializes one OrderFlowEvent to a CSV row, in the exact column order
// csv_event_reader.cpp expects. This is the inverse of parsing -- used
// here so the property test exercises the real text format, not a
// shortcut around it.
std::string toCsvRow(const OrderFlowEvent& ev) {
    std::ostringstream os;
    os << ev.timestamp_ns << "," << toString(ev.kind) << ",";

    switch (ev.kind) {
        case EventKind::NewOrder:
            os << ev.id << "," << (ev.side == Side::Buy ? "BUY" : "SELL") << ","
               << (ev.type == OrderType::Limit    ? "LIMIT"
                   : ev.type == OrderType::Market ? "MARKET"
                   : ev.type == OrderType::IOC    ? "IOC"
                                                  : "FOK")
               << "," << ev.price << "," << ev.qty << ",";
            break;
        case EventKind::Cancel:
            os << ev.id << ",,,,,";
            break;
        case EventKind::Replace:
            os << ev.id << ",,," << (ev.replace_price ? std::to_string(*ev.replace_price) : "")
               << "," << (ev.replace_qty ? std::to_string(*ev.replace_qty) : "") << ",";
            break;
        case EventKind::Trade:
            os << ",,," << ev.price << "," << ev.qty << "," << ev.ref_id;
            break;
        case EventKind::ClockTick:
            os << ",,,,,";
            break;
    }
    return os.str();
}

std::string buildCsv(const std::vector<OrderFlowEvent>& events) {
    std::ostringstream os;
    os << "ts_ns,event,order_id,side,type,price,qty,ref_id\n";
    for (const auto& ev : events) os << toCsvRow(ev) << "\n";
    return os.str();
}

struct Generator {
    std::mt19937_64 rng;
    OrderId next_id = 1;
    std::uint64_t ts = 1000;

    explicit Generator(std::uint64_t seed) : rng(seed) {}

    Price price() { return 95 + static_cast<Price>(rng() % 11); }
    Quantity qty() { return 1 + static_cast<Quantity>(rng() % 20); }
    Side side() { return (rng() % 2 == 0) ? Side::Buy : Side::Sell; }
    OrderType type() {
        switch (rng() % 4) {
            case 0:
                return OrderType::Limit;
            case 1:
                return OrderType::Market;
            case 2:
                return OrderType::IOC;
            default:
                return OrderType::FOK;
        }
    }

    // Occasionally reuse a recently-issued id for CANCEL/REPLACE/TRADE
    // targets (sometimes landing on something still resting, sometimes
    // not -- both are useful: the first exercises normal application,
    // the second exercises the rejection path under strict_mode=false).
    OrderId plausibleTargetId() {
        if (next_id <= 1) return 1;
        return 1 + static_cast<OrderId>(rng() % (next_id - 1));
    }

    std::vector<OrderFlowEvent> generate(std::size_t n) {
        std::vector<OrderFlowEvent> out;
        out.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            // Timestamps mostly advance, but occasionally repeat --
            // deliberately exercising the file_order tie-break.
            if (rng() % 5 != 0) ts += 1 + (rng() % 50);

            OrderFlowEvent ev;
            ev.timestamp_ns = ts;
            ev.file_order = i;

            const auto choice = rng() % 10;
            if (choice < 6) {
                ev.kind = EventKind::NewOrder;
                ev.id = next_id++;
                ev.side = side();
                ev.type = type();
                ev.price = price();
                ev.qty = qty();
            } else if (choice < 8) {
                ev.kind = EventKind::Cancel;
                ev.id = plausibleTargetId();
            } else if (choice == 8) {
                ev.kind = EventKind::Replace;
                ev.id = plausibleTargetId();
                if (rng() % 2 == 0)
                    ev.replace_qty = qty();
                else
                    ev.replace_price = price();
            } else {
                ev.kind = EventKind::Trade;
                ev.ref_id = plausibleTargetId();
                ev.price = price();
                ev.qty = qty();
            }
            out.push_back(ev);
        }
        // A handful of explicit ClockTicks interspersed, matching the
        // brief's event model even though Phase 3 registers no tick
        // hooks of its own consequence here beyond the test's own check.
        for (int k = 0; k < 5; ++k) {
            OrderFlowEvent tick;
            tick.timestamp_ns = ts + 1000 + k;
            tick.file_order = n + static_cast<std::size_t>(k);
            tick.kind = EventKind::ClockTick;
            out.push_back(tick);
        }
        return out;
    }
};

bool runOneStream(std::uint64_t seed, std::size_t n) {
    Generator gen(seed);
    std::vector<OrderFlowEvent> original = gen.generate(n);

    // Round-trip through the REAL CSV text format.
    std::string csv = buildCsv(original);
    std::vector<OrderFlowEvent> parsed;
    try {
        parsed = parseCsvEvents(csv);
    } catch (const CsvParseError& e) {
        std::cerr << "seed=" << seed << " CSV round-trip failed to parse: " << e.what() << "\n";
        return false;
    }

    if (parsed.size() != original.size()) {
        std::cerr << "seed=" << seed << " round-trip event count mismatch: generated "
                  << original.size() << " parsed " << parsed.size() << "\n";
        return false;
    }

    MatchingEngine eng;
    SimulationLoop loop(eng, /*strict_mode=*/false);

    bool invariant_ok = true;
    std::string failure_detail;
    loop.setLatencyHook([&](const OrderFlowEvent& ev, std::chrono::nanoseconds) {
        if (!invariant_ok) return;  // already failed; stop re-checking
        if (auto err = eng.book().checkInvariants()) {
            invariant_ok = false;
            std::ostringstream os;
            os << "invariant violated after event kind=" << toString(ev.kind)
               << " ts=" << ev.timestamp_ns << ": " << *err;
            failure_detail = os.str();
        }
    });

    ReplayReport report = loop.run(parsed);
    (void)report;

    if (!invariant_ok) {
        std::cerr << "seed=" << seed << " " << failure_detail << "\n";
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    int num_streams = 200;
    std::size_t events_per_stream = 300;
    std::uint64_t base_seed = 90000;

    if (argc > 1) num_streams = std::atoi(argv[1]);
    if (argc > 2) events_per_stream = static_cast<std::size_t>(std::strtoull(argv[2], nullptr, 10));
    if (argc > 3) base_seed = std::strtoull(argv[3], nullptr, 10);

    int failures = 0;
    for (int s = 0; s < num_streams; ++s) {
        std::uint64_t seed = base_seed + static_cast<std::uint64_t>(s);
        if (!runOneStream(seed, events_per_stream)) {
            std::cerr << "FAILING SEED: " << seed << "\n";
            ++failures;
        }
    }

    std::cout << (num_streams - failures) << "/" << num_streams
              << " replay streams passed end-to-end (CSV round-trip + SimulationLoop + "
              << "per-event invariant checks, " << events_per_stream << " events each, " << failures
              << " failed)\n";
    return failures == 0 ? 0 : 1;
}
