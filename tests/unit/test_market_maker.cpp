#include "lob/market_maker.hpp"
#include "testing.hpp"

using namespace lob;

namespace {
// Seeds a book with a resting bid/ask so MatchingEngine::midprice()
// is available for the agent's onTick() to use as a reference.
void seedBook(MatchingEngine& eng, Price bid, Price ask, Quantity size = 1000) {
    eng.submit(Order(900001, Side::Buy, OrderType::Limit, bid, size));
    eng.submit(Order(900002, Side::Sell, OrderType::Limit, ask, size));
}
}  // namespace

TEST_CASE("market maker: onTick posts symmetric quotes around the reference with no skew") {
    MatchingEngine eng;
    seedBook(eng, 98, 102);  // midprice = 100

    MarketMakerConfig cfg;
    cfg.base_half_spread = 3;
    cfg.quote_size = 5;
    MarketMakerAgent mm(eng, cfg, /*first_order_id=*/1);

    mm.onTick(1000);
    CHECK_EQ(mm.state().bid_order_id, OrderId{1});
    CHECK_EQ(mm.state().ask_order_id, OrderId{2});

    // Agent's own bid/ask should rest BEHIND the seeded book's tighter
    // quotes (98/102), not cross them, since 100-3=97 < 98 and
    // 100+3=103 > 102.
    auto bid_depth = eng.depth(Side::Buy, 2);
    auto ask_depth = eng.depth(Side::Sell, 2);
    CHECK_EQ(bid_depth[1].first, Price{97});   // agent's own bid, behind the seeded 98
    CHECK_EQ(ask_depth[1].first, Price{103});  // agent's own ask, behind the seeded 102
}

TEST_CASE("market maker: bid is withdrawn once inventory hits max_inventory") {
    MatchingEngine eng;
    seedBook(eng, 98, 102);

    MarketMakerConfig cfg;
    cfg.max_inventory = 5;
    MarketMakerAgent mm(eng, cfg, 1);

    mm.onTick(1000);
    CHECK(mm.state().bid_order_id != 0);

    // Force inventory to the cap directly via onFill (simulating fills
    // that already happened), rather than constructing a real crossing
    // scenario -- this test is specifically about the WITHDRAWAL logic
    // in onTick, not about how inventory got there.
    mm.onFill(Side::Buy, 100, 5, 1500);  // inventory = 5 == max_inventory

    mm.onTick(2000);
    CHECK_EQ(mm.state().bid_order_id, OrderId{0});  // withdrawn, not reposted
    CHECK(mm.state().ask_order_id != 0);            // ask side unaffected
}

TEST_CASE("market maker: positive inventory skews BOTH quotes down") {
    MatchingEngine eng;
    seedBook(eng, 50, 150);  // wide seeded book so the agent's own quotes never cross it

    MarketMakerConfig cfg;
    cfg.base_half_spread = 2;
    cfg.inventory_skew_factor = 1.0;  // 1 tick of shift per unit of inventory
    MarketMakerAgent mm(eng, cfg, 1);

    mm.onFill(Side::Buy, 100, 10, 500);  // inventory = +10 BEFORE the first onTick
    mm.onTick(1000);

    // reference = midprice(100) - inventory(10)*factor(1.0) = 90
    // desired_bid = 90 - 2 = 88, desired_ask = 90 + 2 = 92.
    // Both are BETTER than the wide seeded book (50/150) -- 88 > 50 is
    // a better bid, 92 < 150 is a better ask -- so the agent's own
    // quotes are now the BEST level (index 0), not behind the seeded
    // one (unlike the no-skew test above, where the agent's unshifted
    // quotes were deliberately worse than the tighter seeded book).
    auto depth_b = eng.depth(Side::Buy, 2);
    auto depth_a = eng.depth(Side::Sell, 2);
    CHECK_EQ(depth_b[0].first, Price{88});
    CHECK_EQ(depth_a[0].first, Price{92});
}

TEST_CASE("market maker: N consecutive same-side fills within the window trigger widening") {
    MatchingEngine eng;
    seedBook(eng, 50, 150);

    MarketMakerConfig cfg;
    cfg.base_half_spread = 2;
    cfg.adverse_selection_widen_factor = 3.0;
    cfg.run_over_fill_threshold = 3;
    cfg.run_over_window_ns = 1000;
    cfg.widen_duration_ns = 5000;
    MarketMakerAgent mm(eng, cfg, 1);

    CHECK(!mm.state().widened);
    mm.onFill(Side::Buy, 100, 1, 100);
    mm.onFill(Side::Buy, 100, 1, 200);
    CHECK(!mm.state().widened);         // only 2 so far, threshold is 3
    mm.onFill(Side::Buy, 100, 1, 300);  // 3rd consecutive same-side fill, within window
    CHECK(mm.state().widened);

    mm.onTick(400);  // half_spread should now be 2*3=6, not 2
    auto depth_b = eng.depth(Side::Buy, 2);
    // reference = midprice(100, from seeded 50/150 book), desired_bid =
    // 100 - 6 = 94. 94 > 50 (seeded), so the agent's own quote is now
    // the best level, index 0 -- same indexing note as the skew test above.
    CHECK_EQ(depth_b[0].first, Price{94});

    mm.onTick(400 + cfg.widen_duration_ns);  // past the widen window -- should decay
    CHECK(!mm.state().widened);
}

TEST_CASE(
    "market maker: adverse-selection trigger requires fills WITHIN the time window, "
    "not just N consecutive same-side fills ever") {
    MatchingEngine eng;
    MarketMakerConfig cfg;
    cfg.run_over_fill_threshold = 3;
    cfg.run_over_window_ns = 100;  // narrow window
    MarketMakerAgent mm(eng, cfg, 1);

    mm.onFill(Side::Buy, 100, 1, 0);
    mm.onFill(Side::Buy, 100, 1, 1000);  // far outside the 100ns window from the first
    mm.onFill(Side::Buy, 100, 1, 1050);  // still only within 50ns of the PREVIOUS one
    // The oldest of these three is now far outside the window relative
    // to the newest, so trimming should leave fewer than 3 relevant
    // fills and NOT trigger widening.
    CHECK(!mm.state().widened);
}

TEST_CASE("market maker: a mixed-side run does not trigger widening") {
    MatchingEngine eng;
    MarketMakerConfig cfg;
    cfg.run_over_fill_threshold = 3;
    cfg.run_over_window_ns = 1000;
    MarketMakerAgent mm(eng, cfg, 1);

    mm.onFill(Side::Buy, 100, 1, 100);
    mm.onFill(Side::Sell, 100, 1, 200);  // breaks the run
    mm.onFill(Side::Buy, 100, 1, 300);
    CHECK(!mm.state().widened);
}

TEST_CASE("market maker: onTick is a no-op when midprice is unavailable") {
    MatchingEngine eng;  // empty book -- no bid, no ask, no midprice
    MarketMakerConfig cfg;
    MarketMakerAgent mm(eng, cfg, 1);

    mm.onTick(1000);
    CHECK_EQ(mm.state().bid_order_id, OrderId{0});
    CHECK_EQ(mm.state().ask_order_id, OrderId{0});
}
