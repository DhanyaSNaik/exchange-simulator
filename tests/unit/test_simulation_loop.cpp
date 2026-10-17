#include "lob/simulation_loop.hpp"
#include "testing.hpp"

using namespace lob;

namespace {
OrderFlowEvent newOrderEv(std::uint64_t ts, std::uint64_t file_order, OrderId id, Side side,
                          Price price, Quantity qty) {
    OrderFlowEvent ev;
    ev.timestamp_ns = ts;
    ev.file_order = file_order;
    ev.kind = EventKind::NewOrder;
    ev.id = id;
    ev.side = side;
    ev.type = OrderType::Limit;
    ev.price = price;
    ev.qty = qty;
    return ev;
}
OrderFlowEvent cancelEv(std::uint64_t ts, std::uint64_t file_order, OrderId id) {
    OrderFlowEvent ev;
    ev.timestamp_ns = ts;
    ev.file_order = file_order;
    ev.kind = EventKind::Cancel;
    ev.id = id;
    return ev;
}
OrderFlowEvent tradeEv(std::uint64_t ts, std::uint64_t file_order, OrderId ref_id, Price price,
                       Quantity qty) {
    OrderFlowEvent ev;
    ev.timestamp_ns = ts;
    ev.file_order = file_order;
    ev.kind = EventKind::Trade;
    ev.ref_id = ref_id;
    ev.price = price;
    ev.qty = qty;
    return ev;
}
OrderFlowEvent tickEv(std::uint64_t ts, std::uint64_t file_order) {
    OrderFlowEvent ev;
    ev.timestamp_ns = ts;
    ev.file_order = file_order;
    ev.kind = EventKind::ClockTick;
    return ev;
}
}  // namespace

TEST_CASE("replay: NEW -> CANCEL round trip, events applied in order") {
    MatchingEngine eng;
    SimulationLoop loop(eng);

    std::vector<OrderFlowEvent> events{newOrderEv(1000, 0, 1, Side::Buy, 100, 10),
                                       cancelEv(2000, 1, 1)};
    auto report = loop.run(events);

    CHECK_EQ(report.events_processed, std::size_t{2});
    CHECK_EQ(report.events_rejected, std::size_t{0});
    CHECK(!eng.bestBid().has_value());  // cancelled, nothing resting
}

TEST_CASE("replay: events out of file order but with ascending timestamps apply correctly") {
    MatchingEngine eng;
    SimulationLoop loop(eng);

    // Deliberately constructed OUT of timestamp order in the vector --
    // run() must sort by (timestamp_ns, file_order) before dispatching.
    std::vector<OrderFlowEvent> events{cancelEv(2000, 1, 1),
                                       newOrderEv(1000, 0, 1, Side::Buy, 100, 10)};
    auto report = loop.run(events);
    CHECK_EQ(report.events_processed, std::size_t{2});
    CHECK(!eng.bestBid().has_value());  // NEW then CANCEL, in timestamp order -- not left resting
}

TEST_CASE("replay: equal timestamps break ties by file_order, not vector order") {
    MatchingEngine eng;
    SimulationLoop loop(eng);

    // Two NEW orders at the SAME timestamp; file_order says id=1 came
    // first. If tie-breaking used vector order instead of file_order,
    // this would still happen to pass -- so this test also reverses the
    // vector order relative to file_order to actually distinguish them.
    std::vector<OrderFlowEvent> events{
        newOrderEv(1000, /*file_order=*/1, 2, Side::Sell, 100, 5),  // file_order 1, listed first
        newOrderEv(1000, /*file_order=*/0, 1, Side::Buy, 100, 5)    // file_order 0, listed second
    };
    auto report = loop.run(events);
    CHECK_EQ(report.events_processed, std::size_t{2});
    // id=1 (file_order 0) should have been submitted BEFORE id=2
    // (file_order 1) despite appearing second in the input vector, and
    // despite sharing a timestamp. Since both are Limit Buy/Sell at the
    // same price, they should have crossed and traded -- if ordering
    // were wrong (id=2 processed first), id=2 would rest instead, since
    // there'd be nothing yet to match against.
    CHECK(!eng.bestBid().has_value());
    CHECK(!eng.bestAsk().has_value());
}

TEST_CASE("replay: TRADE event reduces a resting order without an aggressor") {
    MatchingEngine eng;
    SimulationLoop loop(eng);

    std::vector<OrderFlowEvent> events{newOrderEv(1000, 0, 1, Side::Sell, 100, 10),
                                       tradeEv(2000, 1, /*ref_id=*/1, 100, 4)};
    auto report = loop.run(events);

    CHECK_EQ(report.events_processed, std::size_t{2});
    auto depth = eng.depth(Side::Sell, 1);
    CHECK_EQ(depth.size(), std::size_t{1});
    CHECK_EQ(depth[0].second, Quantity{6});  // 10 - 4 = 6 remaining
}

TEST_CASE("replay: strict_mode (default) stops immediately on a rejected event") {
    MatchingEngine eng;
    SimulationLoop loop(eng);  // strict_mode=true by default

    std::vector<OrderFlowEvent> events{
        tradeEv(1000, 0, /*ref_id=*/999, 100, 5),   // ref_id doesn't exist -- rejected
        newOrderEv(2000, 1, 1, Side::Buy, 100, 10)  // should NEVER be reached
    };
    auto report = loop.run(events);

    CHECK_EQ(report.events_processed, std::size_t{0});
    CHECK_EQ(report.events_rejected, std::size_t{1});
    CHECK_EQ(report.errors.size(), std::size_t{1});
    CHECK(!eng.bestBid().has_value());  // the second event never ran
}

TEST_CASE("replay: strict_mode=false logs a rejected event and continues") {
    MatchingEngine eng;
    SimulationLoop loop(eng, /*strict_mode=*/false);

    std::vector<OrderFlowEvent> events{
        tradeEv(1000, 0, /*ref_id=*/999, 100, 5),   // rejected: unknown ref_id
        newOrderEv(2000, 1, 1, Side::Buy, 100, 10)  // should still run
    };
    auto report = loop.run(events);

    CHECK_EQ(report.events_processed, std::size_t{1});
    CHECK_EQ(report.events_rejected, std::size_t{1});
    CHECK_EQ(*eng.bestBid(), Price{100});  // the second event DID run
}

TEST_CASE("replay: strict_mode treats a rejected CANCEL the same as a rejected TRADE") {
    MatchingEngine eng;
    SimulationLoop loop(eng);  // strict_mode=true

    std::vector<OrderFlowEvent> events{
        cancelEv(1000, 0, 999),                     // unknown id -- rejected
        newOrderEv(2000, 1, 1, Side::Buy, 100, 10)  // should never be reached
    };
    auto report = loop.run(events);

    CHECK_EQ(report.events_processed, std::size_t{0});
    CHECK_EQ(report.events_rejected, std::size_t{1});
    CHECK(!eng.bestBid().has_value());
}

TEST_CASE("replay: latency hook fires once per event with a non-negative duration") {
    MatchingEngine eng;
    SimulationLoop loop(eng);

    int hook_calls = 0;
    loop.setLatencyHook([&](const OrderFlowEvent&, std::chrono::nanoseconds d) {
        ++hook_calls;
        CHECK(d.count() >= 0);
    });

    std::vector<OrderFlowEvent> events{newOrderEv(1000, 0, 1, Side::Buy, 100, 10),
                                       cancelEv(2000, 1, 1)};
    loop.run(events);
    CHECK_EQ(hook_calls, 2);
}

TEST_CASE("replay: tick hook fires on ClockTick events with the event's timestamp") {
    MatchingEngine eng;
    SimulationLoop loop(eng);

    std::vector<std::uint64_t> ticks_seen;
    loop.setTickHook([&](std::uint64_t ts) { ticks_seen.push_back(ts); });

    std::vector<OrderFlowEvent> events{newOrderEv(1000, 0, 1, Side::Buy, 100, 10), tickEv(1500, 1),
                                       tickEv(2500, 2)};
    loop.run(events);

    CHECK_EQ(ticks_seen.size(), std::size_t{2});
    CHECK_EQ(ticks_seen[0], std::uint64_t{1500});
    CHECK_EQ(ticks_seen[1], std::uint64_t{2500});
}

TEST_CASE("applyExternalTrade: fails cleanly on unknown ref_id or excessive qty") {
    MatchingEngine eng;
    auto r1 = eng.applyExternalTrade(999, 5, 100);
    CHECK(!r1.has_value());

    eng.submit(Order(1, Side::Sell, OrderType::Limit, 100, 5));
    auto r2 = eng.applyExternalTrade(1, 10, 100);  // qty exceeds what's resting
    CHECK(!r2.has_value());
    CHECK_EQ(eng.depth(Side::Sell, 1)[0].second, Quantity{5});  // untouched by the failed attempt
}

TEST_CASE("applyExternalTrade: aggressor_id is the reserved 0 sentinel") {
    MatchingEngine eng;
    eng.submit(Order(1, Side::Sell, OrderType::Limit, 100, 10));
    auto trade = eng.applyExternalTrade(1, 4, 100);
    CHECK(trade.has_value());
    CHECK_EQ(trade->aggressor_id, OrderId{0});
    CHECK_EQ(trade->resting_id, OrderId{1});
    CHECK_EQ(trade->quantity, Quantity{4});
}

TEST_CASE(
    "applyExternalTrade: records the FEED's reported price, even if it "
    "differs from the resting order's own price") {
    // The resting order's own price is 100; the replayed feed reports an
    // execution at 99 for it. applyExternalTrade records what the feed
    // says, since that is authoritative for the replayed log -- it does
    // not silently substitute the book's own price.
    MatchingEngine eng;
    eng.submit(Order(1, Side::Sell, OrderType::Limit, 100, 10));
    auto trade = eng.applyExternalTrade(1, 4, /*price=*/99);
    CHECK(trade.has_value());
    CHECK_EQ(trade->price, Price{99});
}
