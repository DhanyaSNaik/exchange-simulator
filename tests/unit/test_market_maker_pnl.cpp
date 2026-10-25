// Unit tests isolating the average-cost PnL method itself, via onFill()
// directly -- separate from test_market_maker.cpp's quoting-logic tests,
// and separate from the hand-calculated end-to-end scenario in
// test_pnl_hand_calculation.cpp (this phase's actual stop condition).

#include "lob/market_maker.hpp"
#include "testing.hpp"

using namespace lob;

namespace {
MarketMakerAgent makeAgent(MatchingEngine& eng) {
    return MarketMakerAgent(eng, MarketMakerConfig{}, 1);
}
}  // namespace

TEST_CASE("PnL: opening a long position sets avg_cost to the fill price") {
    MatchingEngine eng;
    auto mm = makeAgent(eng);
    mm.onFill(Side::Buy, 100, 10, 1000);
    CHECK_EQ(mm.state().inventory, Quantity{10});
    CHECK_EQ(mm.state().avg_cost, 100.0);
    CHECK_EQ(mm.state().realized_pnl, 0.0);
}

TEST_CASE("PnL: adding to a long position at a different price updates weighted avg_cost") {
    MatchingEngine eng;
    auto mm = makeAgent(eng);
    mm.onFill(Side::Buy, 100, 10, 1000);  // 10 @ 100
    mm.onFill(Side::Buy, 110, 10, 2000);  // +10 @ 110
    // weighted avg = (10*100 + 10*110) / 20 = 2100/20 = 105
    CHECK_EQ(mm.state().inventory, Quantity{20});
    CHECK_EQ(mm.state().avg_cost, 105.0);
    CHECK_EQ(mm.state().realized_pnl, 0.0);  // nothing closed yet
}

TEST_CASE("PnL: partially closing a long position realizes PnL and leaves avg_cost unchanged") {
    MatchingEngine eng;
    auto mm = makeAgent(eng);
    mm.onFill(Side::Buy, 100, 10, 1000);  // long 10 @ avg_cost 100
    mm.onFill(Side::Sell, 120, 4, 2000);  // close 4 @ 120
    // realized = 4 * (120 - 100) = 80
    CHECK_EQ(mm.state().inventory, Quantity{6});
    CHECK_EQ(mm.state().avg_cost, 100.0);  // unchanged by a partial close
    CHECK_EQ(mm.state().realized_pnl, 80.0);
}

TEST_CASE("PnL: fully closing a long position zeroes avg_cost") {
    MatchingEngine eng;
    auto mm = makeAgent(eng);
    mm.onFill(Side::Buy, 100, 10, 1000);
    mm.onFill(Side::Sell, 90, 10, 2000);  // sold at a loss
    // realized = 10 * (90 - 100) = -100
    CHECK_EQ(mm.state().inventory, Quantity{0});
    CHECK_EQ(mm.state().avg_cost, 0.0);
    CHECK_EQ(mm.state().realized_pnl, -100.0);
}

TEST_CASE(
    "PnL: a fill that flips long to short realizes the closing part and "
    "opens a fresh position at the fill price") {
    MatchingEngine eng;
    auto mm = makeAgent(eng);
    mm.onFill(Side::Buy, 100, 5, 1000);   // long 5 @ 100
    mm.onFill(Side::Sell, 110, 8, 2000);  // sell 8: closes 5, opens short 3 @ 110
    // realized = 5 * (110 - 100) = 50
    CHECK_EQ(mm.state().inventory, Quantity{-3});
    CHECK_EQ(mm.state().avg_cost, 110.0);
    CHECK_EQ(mm.state().realized_pnl, 50.0);
}

TEST_CASE("PnL: short position profits when price falls, via unrealizedPnl") {
    MatchingEngine eng;
    auto mm = makeAgent(eng);
    mm.onFill(Side::Sell, 100, 10, 1000);  // short 10 @ avg_cost 100
    CHECK_EQ(mm.state().inventory, Quantity{-10});
    CHECK_EQ(mm.state().avg_cost, 100.0);

    // Price falls to 90: short position should show a PROFIT of 10*10=100.
    CHECK_EQ(mm.unrealizedPnl(90.0), 100.0);
    // Price rises to 105: short position should show a LOSS of 10*5=50.
    CHECK_EQ(mm.unrealizedPnl(105.0), -50.0);
}

TEST_CASE("PnL: totalPnl combines realized and unrealized correctly") {
    MatchingEngine eng;
    auto mm = makeAgent(eng);
    mm.onFill(Side::Buy, 100, 10, 1000);
    mm.onFill(Side::Sell, 120, 4, 2000);      // realize 4*(120-100)=80, 6 remain long @ 100
    double unreal = mm.unrealizedPnl(130.0);  // 6 * (130-100) = 180
    CHECK_EQ(unreal, 180.0);
    CHECK_EQ(mm.totalPnl(130.0), 80.0 + 180.0);
}
