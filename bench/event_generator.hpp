#pragma once
// Synthetic event stream generator for benchmarking (Phase 0 §6 plan):
// prices cluster around a random-walk midprice rather than being drawn
// uniformly, to approximate a realistic book shape instead of a
// pathological one (uniform-random prices would make the book far wider
// and flatter than any real order book ever is, which would bias cache
// and tree-depth behavior in a way that doesn't reflect realistic usage).

#include <cstdint>
#include <optional>
#include <random>
#include <vector>

#include "lob/order.hpp"

namespace lob_bench {

using namespace lob;

enum class EventKind { Submit, Cancel, Replace };

struct SyntheticEvent {
    EventKind kind;
    Order order{0, Side::Buy, OrderType::Limit, 0, 0};  // used when kind == Submit
    OrderId target_id = 0;                               // used for Cancel/Replace
    std::optional<Price> replace_price;
    std::optional<Quantity> replace_qty;
};

// Mix: 70% submit / 20% cancel / 10% replace, matching the property-test
// harness's mix so results are comparable across the two kinds of test.
// Order-type mix among submits: 55% Limit, 15% Market, 15% IOC, 15% FOK --
// Limit-heavy because that's what actually builds up a resting book to
// benchmark against; an all-Market/IOC stream would never let the book
// grow past a handful of resting orders.
inline std::vector<SyntheticEvent> generateEventStream(std::uint64_t seed, std::size_t n) {
    std::vector<SyntheticEvent> events;
    events.reserve(n);

    std::mt19937_64 rng(seed);
    double midprice = 10000.0;  // integer ticks; starts at 10000
    OrderId next_id = 1;
    std::vector<OrderId> live_ids;
    live_ids.reserve(n / 4);

    auto randSide = [&] { return (rng() % 2 == 0) ? Side::Buy : Side::Sell; };
    auto randQty = [&] { return 1 + static_cast<Quantity>(rng() % 50); };

    auto randPriceNear = [&](Side side) -> Price {
        // Random walk the midprice slowly; cluster order prices within a
        // small offset of it, biased to the correct side of the book
        // (bids below midprice, asks above) so the stream actually
        // produces a two-sided, crossable book rather than two disjoint
        // piles that never interact.
        std::normal_distribution<double> step(0.0, 0.5);
        midprice += step(rng);
        if (midprice < 100.0) midprice = 100.0;

        std::geometric_distribution<int> offset_dist(0.3);  // small offsets most likely
        int offset = offset_dist(rng) + 1;                  // at least 1 tick away

        Price base = static_cast<Price>(midprice);
        return (side == Side::Buy) ? (base - offset) : (base + offset);
    };

    auto randType = [&]() -> OrderType {
        const auto r = rng() % 100;
        if (r < 55) return OrderType::Limit;
        if (r < 70) return OrderType::Market;
        if (r < 85) return OrderType::IOC;
        return OrderType::FOK;
    };

    for (std::size_t i = 0; i < n; ++i) {
        const auto choice = rng() % 10;

        if (choice < 7 || live_ids.empty()) {
            SyntheticEvent ev;
            ev.kind = EventKind::Submit;
            Side side = randSide();
            OrderId id = next_id++;
            ev.order = Order(id, side, randType(), randPriceNear(side), randQty());
            events.push_back(ev);
            if (ev.order.type == OrderType::Limit) live_ids.push_back(id);
        } else if (choice < 9) {
            SyntheticEvent ev;
            ev.kind = EventKind::Cancel;
            ev.target_id = live_ids[rng() % live_ids.size()];
            events.push_back(ev);
        } else {
            SyntheticEvent ev;
            ev.kind = EventKind::Replace;
            ev.target_id = live_ids[rng() % live_ids.size()];
            if (rng() % 2 == 0) {
                ev.replace_qty = randQty();
            } else {
                // Replace price relative to the CURRENT midprice, same
                // helper as a fresh submit would use, picking an
                // arbitrary side for the offset direction.
                ev.replace_price = randPriceNear(randSide());
            }
            events.push_back(ev);
        }
    }
    return events;
}

}  // namespace lob_bench
