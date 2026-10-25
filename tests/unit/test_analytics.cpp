#include <cmath>

#include "lob/analytics.hpp"
#include "testing.hpp"

using namespace lob;

namespace {
Trade mkInternalTrade(OrderId aggressor_id, Side aggressor_side, Price price) {
    return Trade{1, aggressor_id, 2, price, 10, 1, aggressor_side};
}
Trade mkExternalTrade(Price price) {
    Trade t{1, /*aggressor_id=*/0, 2, price, 10, 1, Side::Buy};
    return t;
}
}  // namespace

TEST_CASE("trade sign: internal trade uses ground-truth aggressor_side, ignoring quotes") {
    auto t = mkInternalTrade(5, Side::Sell, 100);
    // Even if the quote arguments would suggest Buy, ground truth wins.
    auto sign = classifyTradeSign(t, /*bid=*/90, /*ask=*/95);
    CHECK(sign == TradeSign::Sell);
}

TEST_CASE("trade sign: external trade at or above the ask classifies as Buy") {
    auto t = mkExternalTrade(105);
    CHECK(classifyTradeSign(t, 95, 100) == TradeSign::Buy);
    auto t2 = mkExternalTrade(100);  // exactly at the ask
    CHECK(classifyTradeSign(t2, 95, 100) == TradeSign::Buy);
}

TEST_CASE("trade sign: external trade at or below the bid classifies as Sell") {
    auto t = mkExternalTrade(90);
    CHECK(classifyTradeSign(t, 95, 100) == TradeSign::Sell);
    auto t2 = mkExternalTrade(95);  // exactly at the bid
    CHECK(classifyTradeSign(t2, 95, 100) == TradeSign::Sell);
}

TEST_CASE("trade sign: external trade strictly between the quotes is Ambiguous") {
    auto t = mkExternalTrade(97);
    CHECK(classifyTradeSign(t, 95, 100) == TradeSign::Ambiguous);
}

TEST_CASE("trade sign: external trade with no prevailing quote is Ambiguous") {
    auto t = mkExternalTrade(100);
    CHECK(classifyTradeSign(t, std::nullopt, 100) == TradeSign::Ambiguous);
    CHECK(classifyTradeSign(t, 95, std::nullopt) == TradeSign::Ambiguous);
}

TEST_CASE("OFI: a pure bid-size increase at an unchanged price contributes positively") {
    OrderFlowImbalanceTracker ofi(10);
    // bid unchanged at 100, size 5 -> 8; ask unchanged at 105, size 5 -> 5 (no change)
    ofi.update(/*bid_px_before=*/100, 5, /*ask_px_before=*/105, 5,
               /*bid_px_after=*/100, 8, /*ask_px_after=*/105, 5);
    // e_bid = 8 - 5 = 3 (unchanged price -> net size delta)
    // e_ask = 5 - 5 = 0
    // e_n = e_bid - e_ask = 3
    CHECK_EQ(ofi.currentOFI(), 3.0);
}

TEST_CASE("OFI: a bid price IMPROVEMENT contributes the full new size, not a delta") {
    OrderFlowImbalanceTracker ofi(10);
    ofi.update(100, 5, 105, 5, /*bid_px_after=*/101, 7, 105, 5);
    // e_bid = 7 (full new size, since price improved)
    // e_ask = 0
    CHECK_EQ(ofi.currentOFI(), 7.0);
}

TEST_CASE("OFI: an ask price IMPROVEMENT (lower ask) contributes negatively to e_n") {
    OrderFlowImbalanceTracker ofi(10);
    ofi.update(100, 5, 105, 5, 100, 5, /*ask_px_after=*/103, 6);
    // e_bid = 0 (unchanged price, unchanged size)
    // e_ask = 6 (full new size, since ask price improved/dropped)
    // e_n = e_bid - e_ask = 0 - 6 = -6
    CHECK_EQ(ofi.currentOFI(), -6.0);
}

TEST_CASE("OFI: window forgets contributions older than window_events") {
    OrderFlowImbalanceTracker ofi(2);              // tiny window
    ofi.update(100, 5, 105, 5, 100, 10, 105, 5);   // e_n = 5
    ofi.update(100, 10, 105, 5, 100, 10, 105, 5);  // e_n = 0 (no change)
    CHECK_EQ(ofi.currentOFI(), 5.0);
    ofi.update(100, 10, 105, 5, 100, 10, 105, 5);  // e_n = 0 again -- window now [0, 0]
    // The first update's e_n=5 should have aged out of the window-of-2.
    CHECK_EQ(ofi.currentOFI(), 0.0);
}

TEST_CASE("realized vol: zero samples or one sample contribute nothing") {
    RealizedVolatilityTracker vol(1000);
    CHECK_EQ(vol.currentRealizedVol(), 0.0);
    vol.sample(0, 100.0);
    CHECK_EQ(vol.currentRealizedVol(), 0.0);  // no return yet -- only one price observed
}

TEST_CASE("realized vol: sum of squared log returns matches hand calculation") {
    RealizedVolatilityTracker vol(1'000'000);  // wide window, nothing ages out
    vol.sample(0, 100.0);
    vol.sample(100, 110.0);  // log(110/100)
    vol.sample(200, 100.0);  // log(100/110)

    double r1 = std::log(110.0 / 100.0);
    double r2 = std::log(100.0 / 110.0);
    double expected = r1 * r1 + r2 * r2;
    double actual = vol.currentRealizedVol();
    CHECK(std::abs(actual - expected) < 1e-12);
}

TEST_CASE("realized vol: returns outside the time window age out") {
    RealizedVolatilityTracker vol(50);  // narrow window
    vol.sample(0, 100.0);
    vol.sample(10, 110.0);    // return at t=10, within window of itself
    vol.sample(1000, 100.0);  // far later -- the t=10 return should have aged out by now
    double r_last = std::log(100.0 / 110.0);
    double expected = r_last * r_last;  // only the most recent return remains in window
    CHECK(std::abs(vol.currentRealizedVol() - expected) < 1e-12);
}
