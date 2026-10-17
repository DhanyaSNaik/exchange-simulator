#include "lob/matching_engine.hpp"
#include "testing.hpp"

using namespace lob;

namespace {
Order lim(OrderId id, Side side, Price price, Quantity qty) {
    return Order(id, side, OrderType::Limit, price, qty);
}
Order mkt(OrderId id, Side side, Quantity qty) {
    return Order(id, side, OrderType::Market, 0, qty);
}
Order ioc(OrderId id, Side side, Price price, Quantity qty) {
    return Order(id, side, OrderType::IOC, price, qty);
}
Order fok(OrderId id, Side side, Price price, Quantity qty) {
    return Order(id, side, OrderType::FOK, price, qty);
}
}  // namespace

TEST_CASE("non-crossing limit order simply rests") {
    MatchingEngine eng;
    auto r = eng.submit(lim(1, Side::Buy, 100, 10));
    CHECK(r.status == OrderStatus::New);
    CHECK(r.trades.empty());
    CHECK_EQ(*eng.bestBid(), Price{100});
}

TEST_CASE("exact full fill: incoming matches resting exactly") {
    MatchingEngine eng;
    eng.submit(lim(1, Side::Sell, 100, 10));
    auto r = eng.submit(lim(2, Side::Buy, 100, 10));

    CHECK(r.status == OrderStatus::Filled);
    CHECK_EQ(r.trades.size(), std::size_t{1});
    CHECK_EQ(r.trades[0].quantity, Quantity{10});
    CHECK_EQ(r.trades[0].price, Price{100});
    CHECK(!eng.bestAsk().has_value());  // resting side fully consumed too
}

TEST_CASE("partial fill: aggressor larger than one resting order rests the remainder") {
    MatchingEngine eng;
    eng.submit(lim(1, Side::Sell, 100, 4));
    auto r = eng.submit(lim(2, Side::Buy, 100, 10));

    CHECK(r.status == OrderStatus::PartiallyFilled);
    CHECK_EQ(r.filled_qty, Quantity{4});
    CHECK_EQ(r.remaining_qty, Quantity{6});
    CHECK_EQ(*eng.bestBid(), Price{100});
}

TEST_CASE("multiple fills per incoming order, walking several resting orders") {
    MatchingEngine eng;
    eng.submit(lim(1, Side::Sell, 100, 3));
    eng.submit(lim(2, Side::Sell, 100, 4));
    eng.submit(lim(3, Side::Sell, 101, 5));

    auto r = eng.submit(lim(4, Side::Buy, 101, 12));

    CHECK_EQ(r.trades.size(), std::size_t{3});
    CHECK_EQ(r.trades[0].resting_id, OrderId{1});
    CHECK_EQ(r.trades[1].resting_id, OrderId{2});
    CHECK_EQ(r.trades[2].resting_id, OrderId{3});
    CHECK_EQ(r.filled_qty, Quantity{12});
    CHECK(r.status == OrderStatus::Filled);
}

TEST_CASE("FIFO priority: earlier resting order at same price fills first") {
    MatchingEngine eng;
    eng.submit(lim(1, Side::Sell, 100, 5));  // arrives first
    eng.submit(lim(2, Side::Sell, 100, 5));  // arrives second, same price

    auto r = eng.submit(lim(3, Side::Buy, 100, 5));
    CHECK_EQ(r.trades.size(), std::size_t{1});
    CHECK_EQ(r.trades[0].resting_id, OrderId{1});  // not 2
}

TEST_CASE("price-time priority: better price fills before earlier-arriving worse price") {
    MatchingEngine eng;
    eng.submit(lim(1, Side::Sell, 101, 5));  // arrives first, worse price
    eng.submit(lim(2, Side::Sell, 100, 5));  // arrives second, better price

    auto r = eng.submit(lim(3, Side::Buy, 101, 5));
    CHECK_EQ(r.trades[0].resting_id, OrderId{2});  // better price wins despite later arrival
}

TEST_CASE("exact-price boundary matches, does not merely approach") {
    MatchingEngine eng;
    eng.submit(lim(1, Side::Sell, 100, 5));
    auto r = eng.submit(lim(2, Side::Buy, 100, 5));  // bid == ask exactly
    CHECK_EQ(r.trades.size(), std::size_t{1});
    CHECK(r.status == OrderStatus::Filled);
}

TEST_CASE("cancel of a resting order removes it and prevents future fills") {
    MatchingEngine eng;
    eng.submit(lim(1, Side::Sell, 100, 5));
    auto c = eng.cancel(1);
    CHECK(c.status == OrderStatus::Cancelled);

    auto r = eng.submit(lim(2, Side::Buy, 100, 5));
    CHECK(r.trades.empty());        // nothing to match against
    CHECK(r.status == OrderStatus::New);
}

TEST_CASE("cancel of unknown order id is rejected cleanly") {
    MatchingEngine eng;
    auto c = eng.cancel(999);
    CHECK(c.status == OrderStatus::Rejected);
    CHECK(c.reject_reason == RejectReason::UnknownOrder);
}

TEST_CASE("cancel of an already-fully-filled order is rejected, not a crash") {
    MatchingEngine eng;
    eng.submit(lim(1, Side::Sell, 100, 5));
    eng.submit(lim(2, Side::Buy, 100, 5));  // fully fills and removes order 1
    auto c = eng.cancel(1);
    CHECK(c.status == OrderStatus::Rejected);
    CHECK(c.reject_reason == RejectReason::UnknownOrder);
}

TEST_CASE("replace: quantity decrease only preserves queue position") {
    MatchingEngine eng;
    eng.submit(lim(1, Side::Buy, 100, 10));
    eng.submit(lim(2, Side::Buy, 100, 5));

    MatchingEngine::ReplaceRequest req{1, std::nullopt, Quantity{3}};
    auto r = eng.replace(req);

    CHECK(r.status == OrderStatus::PartiallyFilled);
    CHECK_EQ(*eng.queuePosition(1), std::size_t{0});  // still ahead of order 2
}

TEST_CASE("replace: price change forfeits priority (moves to back / new sequence)") {
    MatchingEngine eng;
    eng.submit(lim(1, Side::Buy, 100, 10));
    eng.submit(lim(2, Side::Buy, 100, 5));

    // Replace order 1's price but keep it at the same level (100 -> 100 is
    // a no-op price; instead bump to a still-non-crossing but different
    // price then back, OR directly test with a genuine price move that
    // keeps it on the same side without crossing the (nonexistent) ask.
    MatchingEngine::ReplaceRequest req{1, Price{99}, std::nullopt};
    auto r = eng.replace(req);
    CHECK(r.status != OrderStatus::Rejected);

    // Move it back to 100 -- this forfeits priority again, so order 2
    // (never touched) should now be ahead of order 1 at price 100.
    MatchingEngine::ReplaceRequest back{1, Price{100}, std::nullopt};
    eng.replace(back);
    CHECK_EQ(*eng.queuePosition(2), std::size_t{0});
    CHECK_EQ(*eng.queuePosition(1), std::size_t{1});
}

TEST_CASE("replace: quantity increase forfeits priority") {
    MatchingEngine eng;
    eng.submit(lim(1, Side::Buy, 100, 5));
    eng.submit(lim(2, Side::Buy, 100, 5));

    MatchingEngine::ReplaceRequest req{1, std::nullopt, Quantity{20}};
    eng.replace(req);

    CHECK_EQ(*eng.queuePosition(2), std::size_t{0});  // order 2 now ahead
    CHECK_EQ(*eng.queuePosition(1), std::size_t{1});
}

TEST_CASE("replace: price change that now crosses the book trades immediately") {
    MatchingEngine eng;
    eng.submit(lim(1, Side::Sell, 100, 5));
    eng.submit(lim(2, Side::Buy, 95, 5));

    MatchingEngine::ReplaceRequest req{2, Price{100}, std::nullopt};  // now crosses
    auto r = eng.replace(req);
    CHECK_EQ(r.trades.size(), std::size_t{1});
    CHECK(r.status == OrderStatus::Filled);
}

TEST_CASE("replace: unknown order id is rejected") {
    MatchingEngine eng;
    MatchingEngine::ReplaceRequest req{999, Price{100}, std::nullopt};
    auto r = eng.replace(req);
    CHECK(r.status == OrderStatus::Rejected);
    CHECK(r.reject_reason == RejectReason::UnknownOrder);
}

TEST_CASE("FOK rejects entirely when insufficient liquidity, mutating nothing") {
    MatchingEngine eng;
    eng.submit(lim(1, Side::Sell, 100, 5));
    auto r = eng.submit(fok(2, Side::Buy, 100, 10));  // needs 10, only 5 available

    CHECK(r.status == OrderStatus::Rejected);
    CHECK(r.reject_reason == RejectReason::FOKUnfillable);
    CHECK(r.trades.empty());
    CHECK_EQ(*eng.bestAsk(), Price{100});  // order 1 untouched
    auto depth = eng.depth(Side::Sell, 1);
    CHECK_EQ(depth[0].second, Quantity{5});  // qty unchanged, nothing partially consumed
}

TEST_CASE("FOK fills completely when liquidity across multiple levels suffices") {
    MatchingEngine eng;
    eng.submit(lim(1, Side::Sell, 100, 4));
    eng.submit(lim(2, Side::Sell, 101, 6));

    auto r = eng.submit(fok(3, Side::Buy, 101, 10));
    CHECK(r.status == OrderStatus::Filled);
    CHECK_EQ(r.trades.size(), std::size_t{2});
    CHECK_EQ(r.filled_qty, Quantity{10});
}

TEST_CASE("FOK respects its own price limit when aggregating fillable qty") {
    MatchingEngine eng;
    eng.submit(lim(1, Side::Sell, 100, 4));
    eng.submit(lim(2, Side::Sell, 105, 6));  // outside the FOK's price limit

    auto r = eng.submit(fok(3, Side::Buy, 100, 10));  // limit 100, needs 10
    CHECK(r.status == OrderStatus::Rejected);  // only 4 is within reach at price <= 100
    CHECK(r.reject_reason == RejectReason::FOKUnfillable);
}

TEST_CASE("IOC partially fills then discards the remainder without resting") {
    MatchingEngine eng;
    eng.submit(lim(1, Side::Sell, 100, 3));

    auto r = eng.submit(ioc(2, Side::Buy, 100, 10));
    CHECK(r.status == OrderStatus::PartiallyFilled);
    CHECK_EQ(r.filled_qty, Quantity{3});
    CHECK_EQ(r.remaining_qty, Quantity{0});  // remainder killed, not resting
    CHECK(!eng.bestBid().has_value());       // confirm nothing rests on the bid side
}

TEST_CASE("IOC with zero available liquidity is fully cancelled, never rests") {
    MatchingEngine eng;
    auto r = eng.submit(ioc(1, Side::Buy, 100, 10));
    CHECK(r.status == OrderStatus::Cancelled);
    CHECK(r.trades.empty());
    CHECK(!eng.bestBid().has_value());
}

TEST_CASE("market order walks the book and discards any unfilled remainder") {
    MatchingEngine eng;
    eng.submit(lim(1, Side::Sell, 100, 5));
    auto r = eng.submit(mkt(2, Side::Buy, 10));  // more than available

    CHECK_EQ(r.filled_qty, Quantity{5});
    CHECK_EQ(r.remaining_qty, Quantity{0});  // never rests at "last traded price" or otherwise
    CHECK(!eng.bestAsk().has_value());
}

TEST_CASE("zero-quantity order is rejected before reaching the matching loop") {
    MatchingEngine eng;
    auto r = eng.submit(lim(1, Side::Buy, 100, 0));
    CHECK(r.status == OrderStatus::Rejected);
    CHECK(r.reject_reason == RejectReason::ZeroQuantity);
}

TEST_CASE("invalid (non-positive) price on a Limit order is rejected") {
    MatchingEngine eng;
    auto r = eng.submit(lim(1, Side::Buy, 0, 10));
    CHECK(r.status == OrderStatus::Rejected);
    CHECK(r.reject_reason == RejectReason::InvalidPrice);
}

TEST_CASE("duplicate order id while original is still resting is rejected") {
    MatchingEngine eng;
    eng.submit(lim(1, Side::Buy, 100, 10));
    auto r = eng.submit(lim(1, Side::Buy, 99, 5));
    CHECK(r.status == OrderStatus::Rejected);
    CHECK(r.reject_reason == RejectReason::DuplicateId);
}

TEST_CASE("self-crossing orders match normally (self-trade prevention is out of MVP scope)") {
    // Phase 0 decision: no participant_id exists on Order in this MVP, so
    // the engine cannot distinguish "self" from any other counterparty.
    // Two orders that would cross are matched exactly as any other pair
    // would be -- this test documents that as the intended behavior
    // rather than an oversight, per the explicit scope decision.
    MatchingEngine eng;
    eng.submit(lim(1, Side::Sell, 100, 5));
    auto r = eng.submit(lim(2, Side::Buy, 100, 5));
    CHECK_EQ(r.trades.size(), std::size_t{1});
    CHECK(r.status == OrderStatus::Filled);
}

TEST_CASE("spread, midprice, and microprice basic correctness") {
    MatchingEngine eng;
    eng.submit(lim(1, Side::Buy, 98, 10));   // bid size 10
    eng.submit(lim(2, Side::Sell, 102, 30)); // ask size 30

    CHECK_EQ(*eng.spread(), Price{4});
    CHECK_EQ(*eng.midprice(), 100.0);

    // microprice is cross-weighted: ASK size weights the BID price and
    // BID size weights the ASK price. A large ask size (heavy resting sell
    // interest) pulls microprice TOWARD the bid, below the simple midpoint
    // -- the economic intuition being that heavy size on one side signals
    // price pressure toward the other side. Here bid=98 (size 10),
    // ask=102 (size 30): micro = (98*30 + 102*10)/40 = 99.0, below the
    // midprice of 100.0.
    double micro = *eng.microprice();
    CHECK(micro < 100.0);
    CHECK(micro > 98.0);
}
