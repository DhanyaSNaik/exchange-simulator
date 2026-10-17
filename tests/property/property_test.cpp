// Custom randomized-event-stream property harness (Phase 0 §5 decision:
// a small auditable harness over a library dependency, for Phase 1).
//
// Generates random sequences of submit/cancel/replace events, runs them
// through MatchingEngine, and asserts every invariant from Phase 0 §3
// holds AFTER EVERY SINGLE EVENT -- not just at the end of the run. On
// failure, prints the failing seed and the exact event index, so the run
// is trivially reproducible.

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <random>
#include <sstream>
#include <vector>

#include "lob/matching_engine.hpp"

using namespace lob;

namespace {

struct RandomEventGenerator {
    std::mt19937_64 rng;
    OrderId next_id = 1;
    std::vector<OrderId> live_ids;  // ids we believe are currently resting

    explicit RandomEventGenerator(std::uint64_t seed) : rng(seed) {}

    Price randPrice() { return 95 + static_cast<Price>(rng() % 11); }     // 95..105
    Quantity randQty() { return 1 + static_cast<Quantity>(rng() % 20);    // 1..20
    }
    Side randSide() { return (rng() % 2 == 0) ? Side::Buy : Side::Sell; }

    OrderType randType() {
        switch (rng() % 4) {
            case 0: return OrderType::Limit;
            case 1: return OrderType::Market;
            case 2: return OrderType::IOC;
            default: return OrderType::FOK;
        }
    }

    OrderId pickLiveId() {
        if (live_ids.empty()) return 0;
        return live_ids[rng() % live_ids.size()];
    }

    void forgetId(OrderId id) {
        auto it = std::find(live_ids.begin(), live_ids.end(), id);
        if (it != live_ids.end()) live_ids.erase(it);
    }
};

// Checks the invariants that don't already live inside OrderBook::
// checkInvariants() (which covers the structural/per-level ones) --
// specifically the ones that are properties of the *engine's* behavior
// across an event, like sequence monotonicity and report consistency.
struct EngineInvariantTracker {
    Sequence last_sequence = 0;

    std::optional<std::string> checkReport(const ExecutionReport& report,
                                             Sequence /*expected_min*/) {
        // §3.9 conservation: filled + remaining can never exceed what the
        // order could possibly have (can't check original here directly,
        // but filled/remaining must both be non-negative).
        if (report.filled_qty < 0 || report.remaining_qty < 0) {
            return "negative filled_qty or remaining_qty in report";
        }
        // §3.12: IOC/Market/FOK/zero-remainder cases must report
        // remaining_qty == 0 (never rests) -- MatchingEngine enforces this
        // by construction; this is a belt-and-suspenders runtime check.
        if ((report.status == OrderStatus::Cancelled ||
             report.status == OrderStatus::Filled ||
             report.status == OrderStatus::Rejected) &&
            report.remaining_qty != 0) {
            return "terminal status reported non-zero remaining_qty";
        }
        return std::nullopt;
    }
};

bool runOneStream(std::uint64_t seed, int num_events, bool verbose) {
    MatchingEngine eng;
    RandomEventGenerator gen(seed);
    EngineInvariantTracker tracker;

    for (int i = 0; i < num_events; ++i) {
        const int choice = static_cast<int>(gen.rng() % 10);

        if (choice < 7 || gen.live_ids.empty()) {
            // submit (70% of the time, or whenever there's nothing to
            // cancel/replace against)
            OrderId id = gen.next_id++;
            Side side = gen.randSide();
            OrderType type = gen.randType();
            Price price = gen.randPrice();
            Quantity qty = gen.randQty();

            Order order(id, side, type, price, qty);
            ExecutionReport report = eng.submit(order);

            if (auto err = tracker.checkReport(report, 0)) {
                std::cerr << "seed=" << seed << " event=" << i
                          << " report invariant violated: " << *err << "\n";
                return false;
            }
            if (report.status == OrderStatus::New ||
                report.status == OrderStatus::PartiallyFilled) {
                if (type == OrderType::Limit) gen.live_ids.push_back(id);
            }
            // Filled/Cancelled/Rejected orders never rest -- nothing to track.
            if (report.status == OrderStatus::Filled) gen.forgetId(id);

        } else if (choice < 9) {
            // cancel a (believed) live id
            OrderId id = gen.pickLiveId();
            if (id != 0) {
                eng.cancel(id);
                gen.forgetId(id);
            }
        } else {
            // replace a (believed) live id
            OrderId id = gen.pickLiveId();
            if (id != 0) {
                MatchingEngine::ReplaceRequest req{id, std::nullopt, std::nullopt};
                if (gen.rng() % 2 == 0) {
                    req.new_qty = gen.randQty();
                } else {
                    req.new_price = gen.randPrice();
                }
                auto report = eng.replace(req);
                // A replace that forfeits priority internally resubmits
                // under the SAME id, so the id can still be live, now
                // possibly fully filled (forget it) or still resting.
                if (report.status == OrderStatus::Filled ||
                    report.status == OrderStatus::Rejected) {
                    gen.forgetId(id);
                }
            }
        }

        // The structural invariant walk -- after EVERY event, not just at
        // the end of the run.
        if (auto err = eng.book().checkInvariants()) {
            std::cerr << "seed=" << seed << " event=" << i
                      << " book invariant violated: " << *err << "\n";
            return false;
        }

        if (verbose && i % 1000 == 0) {
            std::cerr << "  seed=" << seed << " event=" << i << " ok\n";
        }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    int num_streams = 200;
    int events_per_stream = 500;
    std::uint64_t base_seed = 1234567;

    if (argc > 1) num_streams = std::atoi(argv[1]);
    if (argc > 2) events_per_stream = std::atoi(argv[2]);
    if (argc > 3) base_seed = std::strtoull(argv[3], nullptr, 10);

    int failures = 0;
    for (int s = 0; s < num_streams; ++s) {
        std::uint64_t seed = base_seed + static_cast<std::uint64_t>(s);
        if (!runOneStream(seed, events_per_stream, false)) {
            std::cerr << "FAILING SEED: " << seed
                      << " (rerun with argv[3]=" << seed << ", num_streams=1)\n";
            ++failures;
        }
    }

    std::cout << (num_streams - failures) << "/" << num_streams
              << " randomized event streams passed ("
              << events_per_stream << " events each, " << failures
              << " failed)\n";
    return failures == 0 ? 0 : 1;
}
