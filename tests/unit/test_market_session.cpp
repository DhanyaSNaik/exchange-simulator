#include "lob/market_session.hpp"
#include "testing.hpp"

using namespace lob;

TEST_CASE("session: an external order crossing the agent's resting bid triggers onFill") {
    MatchingEngine eng;
    MarketMakerConfig cfg;
    cfg.base_half_spread = 1;
    cfg.quote_size = 10;
    MarketMakerAgent mm(eng, cfg, /*first_order_id=*/1);
    MarketSession session(eng, mm);

    // Seed a wide reference book first so midprice() exists, then let
    // the agent post its own quote via onTick through the session.
    eng.submit(Order(900001, Side::Buy, OrderType::Limit, 50, 1000));
    eng.submit(Order(900002, Side::Sell, OrderType::Limit, 150, 1000));
    session.onTick(1000);  // agent posts bid ~99, ask ~101 (best of book now)

    OrderId agent_bid_id = mm.state().bid_order_id;
    CHECK(agent_bid_id != 0);
    CHECK_EQ(mm.state().inventory, Quantity{0});  // nothing filled yet

    // An external participant sells INTO the agent's bid.
    Order external_sell(500, Side::Sell, OrderType::Limit, 90, 5);  // crosses the agent's bid
    auto report = session.submit(external_sell, 2000);

    CHECK_EQ(report.trades.size(), std::size_t{1});
    CHECK_EQ(mm.state().inventory, Quantity{5});  // agent bought 5 via its resting bid
}

TEST_CASE("session: a trade between two OTHER participants does not touch agent state") {
    MatchingEngine eng;
    MarketMakerAgent mm(eng, MarketMakerConfig{}, 1);
    MarketSession session(eng, mm);

    // Two external participants trade with each other; the agent has no
    // resting quotes in the book at all.
    session.submit(Order(500, Side::Sell, OrderType::Limit, 100, 10), 1000);
    session.submit(Order(501, Side::Buy, OrderType::Limit, 100, 10), 2000);

    CHECK_EQ(mm.state().inventory, Quantity{0});
    CHECK_EQ(mm.state().realized_pnl, 0.0);
}

TEST_CASE("session: applyExternalTrade against the agent's resting ask triggers onFill") {
    MatchingEngine eng;
    MarketMakerConfig cfg;
    cfg.base_half_spread = 1;
    MarketMakerAgent mm(eng, cfg, 1);
    MarketSession session(eng, mm);

    eng.submit(Order(900001, Side::Buy, OrderType::Limit, 50, 1000));
    eng.submit(Order(900002, Side::Sell, OrderType::Limit, 150, 1000));
    session.onTick(1000);
    OrderId agent_ask_id = mm.state().ask_order_id;
    CHECK(agent_ask_id != 0);

    // A replayed TRADE event hits the agent's resting ask directly.
    auto trade = session.applyExternalTrade(agent_ask_id, 3, 101, 2000);
    CHECK(trade.has_value());
    CHECK_EQ(mm.state().inventory, Quantity{-3});  // agent sold 3
}

TEST_CASE("session: cancel is a pure pass-through with no fill side effects") {
    MatchingEngine eng;
    MarketMakerAgent mm(eng, MarketMakerConfig{}, 1);
    MarketSession session(eng, mm);

    session.submit(Order(500, Side::Buy, OrderType::Limit, 100, 10), 1000);
    auto report = session.cancel(500, 2000);
    CHECK(report.status == OrderStatus::Cancelled);
    CHECK_EQ(mm.state().inventory, Quantity{0});
}
