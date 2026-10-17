// Closes the gap flagged in TESTING.md: sequence monotonicity was
// enforced by construction (one ++sequence_counter_ per submit() call)
// but had no standalone test asserting it directly. This drives a long
// random stream of submit/cancel/replace events and checks that every
// SEQUENCE-BEARING report (ExecutionReport::sequence != 0 -- i.e. every
// submit() call, including the internal resubmit inside a
// priority-forfeiting replace) produces a strictly increasing sequence
// number, with no repeats and no regressions, even though cancels and
// in-place replace-decreases are interleaved and consume no sequence of
// their own.

#include <algorithm>
#include <optional>
#include <random>

#include "lob/matching_engine.hpp"
#include "testing.hpp"

using namespace lob;

TEST_CASE(
    "sequence numbers are strictly increasing across a long mixed "
    "random stream, counting only sequence-bearing events") {
    MatchingEngine eng;
    std::mt19937_64 rng(424242);
    std::vector<OrderId> live_ids;
    OrderId next_id = 1;

    Sequence last_sequence = 0;
    int sequence_bearing_events = 0;

    auto randPrice = [&] { return 95 + static_cast<Price>(rng() % 11); };
    auto randQty = [&] { return 1 + static_cast<Quantity>(rng() % 20); };
    auto randSide = [&] { return (rng() % 2 == 0) ? Side::Buy : Side::Sell; };
    auto randType = [&]() -> OrderType {
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
    };

    constexpr int kNumEvents = 5000;
    for (int i = 0; i < kNumEvents; ++i) {
        const int choice = static_cast<int>(rng() % 10);
        std::optional<ExecutionReport> report;

        if (choice < 7 || live_ids.empty()) {
            OrderId id = next_id++;
            Order order(id, randSide(), randType(), randPrice(), randQty());
            report = eng.submit(order);
            if (report->status == OrderStatus::New ||
                report->status == OrderStatus::PartiallyFilled) {
                if (order.type == OrderType::Limit) live_ids.push_back(id);
            }
        } else if (choice < 9) {
            OrderId id = live_ids[rng() % live_ids.size()];
            report = eng.cancel(id);
            auto it = std::find(live_ids.begin(), live_ids.end(), id);
            if (it != live_ids.end()) live_ids.erase(it);
        } else {
            OrderId id = live_ids[rng() % live_ids.size()];
            MatchingEngine::ReplaceRequest req{id, std::nullopt, std::nullopt};
            if (rng() % 2 == 0) {
                req.new_qty = randQty();
            } else {
                req.new_price = randPrice();
            }
            report = eng.replace(req);
        }

        if (report->sequence != 0) {
            // A sequence-bearing event: must be strictly greater than the
            // last one seen, with no gaps that would indicate reuse, and
            // no regression that would indicate two events racing for the
            // same number.
            CHECK(report->sequence > last_sequence);
            last_sequence = report->sequence;
            ++sequence_bearing_events;
        }
    }

    // Sanity: a 5000-event stream with ~90% submit-shaped events should
    // have produced a substantial number of sequence-bearing reports --
    // this guards against the test accidentally checking nothing (e.g.
    // if every report's sequence were silently left at the default 0).
    CHECK(sequence_bearing_events > 1000);
}
