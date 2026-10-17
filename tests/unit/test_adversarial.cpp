// Adversarial edge cases -- not "does feature X work" (covered in
// test_matching_engine.cpp) but "what happens where two features interact
// in a way nobody writing each feature in isolation would naturally think
// to check."

#include "lob/matching_engine.hpp"
#include "testing.hpp"

using namespace lob;

namespace {
Order lim(OrderId id, Side side, Price price, Quantity qty) {
    return Order(id, side, OrderType::Limit, price, qty);
}
}  // namespace

TEST_CASE(
    "ADVERSARIAL: replace that re-specifies the SAME price and qty "
    "still forfeits priority, and does not spuriously trade") {
    // Easy to assume "nothing changed" means "nothing happens." But the
    // implementation only special-cases the OMITTED case (both fields
    // nullopt); an explicitly-equal value still fails the strict-decrease
    // test and falls into the forfeit-priority (cancel+resubmit) branch.
    // The interesting question: can resubmitting a price/qty-identical
    // order ever generate a trade it shouldn't? It can't -- because the
    // crossed-book invariant guarantees nothing on the opposite side could
    // have been resting at a crossing price without already having traded
    // against this order while it sat there. This test pins that down
    // rather than assuming it.
    MatchingEngine eng;
    eng.submit(lim(1, Side::Buy, 100, 10));
    eng.submit(lim(2, Side::Buy, 100, 5));

    MatchingEngine::ReplaceRequest req{1, Price{100}, Quantity{10}};  // explicitly unchanged
    auto r = eng.replace(req);

    CHECK(r.trades.empty());  // no spurious match
    CHECK(r.status != OrderStatus::Filled);
    CHECK_EQ(*eng.queuePosition(2), std::size_t{0});  // order 2 now ahead
    CHECK_EQ(*eng.queuePosition(1), std::size_t{1});  // priority WAS forfeited
}

TEST_CASE(
    "ADVERSARIAL: an order id can be legitimately reused after its "
    "original order is fully cancelled") {
    // The duplicate-id guard checks only CURRENTLY-RESTING orders
    // (OrderBook::contains), by design (Phase 1 API note: no
    // all-time-ever-used id ledger is kept). This test pins that as
    // intended behavior: once id=1 is cancelled, a brand new, unrelated
    // order may reuse id=1 and must NOT be rejected as a duplicate.
    MatchingEngine eng;
    eng.submit(lim(1, Side::Buy, 100, 5));
    auto c = eng.cancel(1);
    CHECK(c.status == OrderStatus::Cancelled);

    auto r = eng.submit(lim(1, Side::Sell, 101, 7));  // reuses id 1
    CHECK(r.status != OrderStatus::Rejected);
    CHECK(r.reject_reason == RejectReason::None);
    CHECK_EQ(*eng.bestAsk(), Price{101});
}

TEST_CASE(
    "ADVERSARIAL: FOK that spans two price levels consumes only "
    "PART of the second level, exactly, on commit") {
    // The dry-run (aggregateFillable) only needs to prove TOTAL liquidity
    // within the price limit is sufficient -- it is not required to, and
    // does not, reason about which individual resting orders get
    // (partially) consumed. The real risk is in the SECOND pass
    // (matchAggressive): does it correctly stop partway through the
    // second level's resting order rather than either overshooting
    // (taking more than needed) or leaving the FOK order under-filled
    // because it stopped at a level boundary instead of a quantity
    // boundary?
    MatchingEngine eng;
    eng.submit(lim(1, Side::Sell, 100, 4));   // level 1: fully consumed
    eng.submit(lim(2, Side::Sell, 101, 10));  // level 2: only 3 of 10 needed

    auto r = eng.submit(Order(3, Side::Buy, OrderType::FOK, 101, 7));  // needs exactly 7

    CHECK(r.status == OrderStatus::Filled);
    CHECK_EQ(r.filled_qty, Quantity{7});
    CHECK_EQ(r.trades.size(), std::size_t{2});
    CHECK_EQ(r.trades[0].resting_id, OrderId{1});
    CHECK_EQ(r.trades[0].quantity, Quantity{4});
    CHECK_EQ(r.trades[1].resting_id, OrderId{2});
    CHECK_EQ(r.trades[1].quantity, Quantity{3});  // exactly the remainder, not all 10

    CHECK(eng.bestAsk().has_value());  // sanity: ask side still exists
    auto depth = eng.depth(Side::Sell, 5);
    CHECK_EQ(depth.size(), std::size_t{1});  // level 1 fully gone
    CHECK_EQ(depth[0].first, Price{101});
    CHECK_EQ(depth[0].second, Quantity{7});  // 10 - 3 = 7 left resting at level 2
}

TEST_CASE(
    "ADVERSARIAL: a Market order's price field is garbage and must "
    "be completely ignored by both validation and crossing logic") {
    // validate() only checks price for non-Market types, and crosses()
    // short-circuits true for Market regardless of price -- but nothing
    // stops a caller from constructing a Market order with a nonsensical
    // (even negative) price. This must not leak into behavior anywhere.
    MatchingEngine eng;
    eng.submit(lim(1, Side::Sell, 100, 5));

    Order garbage_market(2, Side::Buy, OrderType::Market, /*price=*/-999999, 5);
    auto r = eng.submit(garbage_market);

    CHECK(r.status == OrderStatus::Filled);
    CHECK_EQ(r.trades.size(), std::size_t{1});
    CHECK_EQ(r.trades[0].price, Price{100});  // trade prices at the RESTING side's price
}
