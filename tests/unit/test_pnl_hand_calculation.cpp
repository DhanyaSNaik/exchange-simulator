// Phase 4 STOP CONDITION: a short, hand-calculable scenario, run through
// the REAL MatchingEngine + MarketSession + MarketMakerAgent pipeline end
// to end (not just the isolated PnL formula, which test_market_maker_pnl.cpp
// already covers) -- the by-hand arithmetic is written out in full below,
// matching exactly what appears in the conversation response presenting
// this gate, so the two can be checked against each other independently.

#include "lob/market_session.hpp"
#include "testing.hpp"

using namespace lob;

TEST_CASE("HAND-CALCULATED SCENARIO: quote -> three external fills -> PnL matches by hand") {
    MatchingEngine eng;

    // Reference book: bid 95 / ask 105, qty 1000 each -- wide enough
    // that the agent's own tighter quotes never cross them.
    eng.submit(Order(900001, Side::Buy, OrderType::Limit, 95, 1000));
    eng.submit(Order(900002, Side::Sell, OrderType::Limit, 105, 1000));
    // midprice = (95 + 105) / 2 = 100

    MarketMakerConfig cfg;
    cfg.base_half_spread = 2;
    cfg.quote_size = 10;
    cfg.inventory_skew_factor = 0.0;    // no skew -- keep this scenario's
    cfg.run_over_fill_threshold = 100;  // arithmetic simple; widening
                                        // and skew are tested separately
                                        // in test_market_maker.cpp.
    MarketMakerAgent mm(eng, cfg, /*first_order_id=*/1);
    MarketSession session(eng, mm);

    // t=1000: agent quotes. desired_bid = 100-2=98, desired_ask = 100+2=102.
    // Neither crosses the 95/105 reference book, so both simply rest.
    session.onTick(1000);
    CHECK_EQ(mm.state().bid_order_id, OrderId{1});
    CHECK_EQ(mm.state().ask_order_id, OrderId{2});
    CHECK_EQ(mm.state().inventory, Quantity{0});

    // t=2000: external SELL id=501, price=90, qty=6.
    // Best bid is the agent's own (98 > 95 seeded), and 90 <= 98 crosses.
    // Trade: qty=min(6,10)=6 @ price=98 (the RESTING side's price).
    // Agent's bid has 4 remaining, still resting. Order 501 fully
    // consumed (qty 6 exactly), so exactly one trade.
    //   -> agent BOUGHT 6 @ 98. inventory 0->6, avg_cost=98, realized=0.
    auto r1 = session.submit(Order(501, Side::Sell, OrderType::Limit, 90, 6), 2000);
    CHECK_EQ(r1.trades.size(), std::size_t{1});
    CHECK_EQ(mm.state().inventory, Quantity{6});
    CHECK_EQ(mm.state().avg_cost, 98.0);
    CHECK_EQ(mm.state().realized_pnl, 0.0);

    // t=3000: external BUY id=502, price=115, qty=4.
    // Best ask is the agent's own (102 < 105 seeded), and 115 >= 102 crosses.
    // Trade: qty=min(4,10)=4 @ price=102. Agent's ask has 6 remaining.
    //   -> agent SOLD 4 @ 102. Reducing a long(6): closing_qty=min(4,6)=4.
    //      realized += 4 * (102 - 98) = 4 * 4 = 16.
    //      inventory 6 -> 2 (avg_cost unchanged at 98 -- a partial close
    //      never changes the remaining position's cost basis).
    auto r2 = session.submit(Order(502, Side::Buy, OrderType::Limit, 115, 4), 3000);
    CHECK_EQ(r2.trades.size(), std::size_t{1});
    CHECK_EQ(mm.state().inventory, Quantity{2});
    CHECK_EQ(mm.state().avg_cost, 98.0);
    CHECK_EQ(mm.state().realized_pnl, 16.0);

    // t=4000: external SELL id=503, price=90, qty=4.
    // Agent's bid still rests at 98 with exactly 4 remaining (from the
    // first fill). 90 <= 98 crosses; qty=min(4,4)=4 exactly consumes
    // BOTH sides -- agent's bid is now fully filled and removed.
    //   -> agent BOUGHT 4 more @ 98. inventory(2, long) + 4 (same sign,
    //      adding): total_cost_before = 98*2=196, total_cost_after =
    //      196 + 98*4 = 588. inventory 2->6. avg_cost = 588/6 = 98.0
    //      (unchanged, since every fill so far happened at exactly 98
    //      or realized its delta separately -- this is not a
    //      coincidence-free check, it's worth double-checking BY HAND
    //      that 98 is actually still correct here, not just assumed).
    auto r3 = session.submit(Order(503, Side::Sell, OrderType::Limit, 90, 4), 4000);
    CHECK_EQ(r3.trades.size(), std::size_t{1});
    CHECK_EQ(mm.state().inventory, Quantity{6});
    CHECK_EQ(mm.state().avg_cost, 98.0);
    CHECK_EQ(mm.state().realized_pnl, 16.0);  // unchanged -- fill 3 only added, didn't close

    // FINAL: mark to midprice=100 (unchanged throughout this scenario).
    //   unrealizedPnl = inventory * (mid - avg_cost) = 6 * (100 - 98) = 12
    //   totalPnl = realized(16) + unrealized(12) = 28
    CHECK_EQ(mm.unrealizedPnl(100.0), 12.0);
    CHECK_EQ(mm.totalPnl(100.0), 28.0);
}
