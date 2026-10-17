#include "lob/order_book.hpp"
#include "testing.hpp"

using namespace lob;

namespace {
Order mk(OrderId id, Side side, Price price, Quantity qty) {
    return Order(id, side, OrderType::Limit, price, qty);
}
}  // namespace

TEST_CASE("insert then bestBid/bestAsk reflect top of book") {
    OrderBook book;
    CHECK(!book.bestBid().has_value());
    CHECK(book.insert(mk(1, Side::Buy, 100, 10)));
    CHECK_EQ(*book.bestBid(), 100);
    CHECK(book.insert(mk(2, Side::Sell, 105, 10)));
    CHECK_EQ(*book.bestAsk(), 105);
    CHECK(!book.checkInvariants().has_value());
}

TEST_CASE("duplicate id insert is rejected") {
    OrderBook book;
    CHECK(book.insert(mk(1, Side::Buy, 100, 10)));
    CHECK(!book.insert(mk(1, Side::Buy, 101, 5)));  // same id, different price
    CHECK_EQ(book.orderCount(), std::size_t{1});
}

TEST_CASE("FIFO order within a price level is preserved") {
    OrderBook book;
    book.insert(mk(1, Side::Buy, 100, 10));
    book.insert(mk(2, Side::Buy, 100, 5));
    book.insert(mk(3, Side::Buy, 100, 7));

    CHECK_EQ(*book.queuePosition(1), std::size_t{0});
    CHECK_EQ(*book.queuePosition(2), std::size_t{1});
    CHECK_EQ(*book.queuePosition(3), std::size_t{2});

    auto front = book.frontOfBook(Side::Buy);
    CHECK_EQ(front->id, OrderId{1});
}

TEST_CASE("remove from middle preserves FIFO of remaining orders") {
    OrderBook book;
    book.insert(mk(1, Side::Buy, 100, 10));
    book.insert(mk(2, Side::Buy, 100, 5));
    book.insert(mk(3, Side::Buy, 100, 7));

    CHECK(book.remove(2).has_value());
    CHECK_EQ(*book.queuePosition(1), std::size_t{0});
    CHECK_EQ(*book.queuePosition(3), std::size_t{1});
    CHECK(!book.checkInvariants().has_value());
}

TEST_CASE("empty level is removed from the map once last order leaves") {
    OrderBook book;
    book.insert(mk(1, Side::Buy, 100, 10));
    CHECK_EQ(book.levelCount(Side::Buy), std::size_t{1});
    book.remove(1);
    CHECK_EQ(book.levelCount(Side::Buy), std::size_t{0});
    CHECK(!book.bestBid().has_value());
}

TEST_CASE("reduceQuantity partial leaves order resting, full removes it") {
    OrderBook book;
    book.insert(mk(1, Side::Buy, 100, 10));
    CHECK(book.reduceQuantity(1, 4));
    auto front = book.frontOfBook(Side::Buy);
    CHECK_EQ(front->remaining_qty, Quantity{6});
    CHECK(book.reduceQuantity(1, 6));
    CHECK(!book.contains(1));
    CHECK(!book.bestBid().has_value());
}

TEST_CASE("reduceQuantity rejects qty exceeding remaining (caller-bug guard)") {
    OrderBook book;
    book.insert(mk(1, Side::Buy, 100, 10));
    CHECK(!book.reduceQuantity(1, 11));
    CHECK_EQ(book.get(1)->remaining_qty, Quantity{10});
}

TEST_CASE("decreaseQuantityInPlace preserves queue position (replace semantics)") {
    OrderBook book;
    book.insert(mk(1, Side::Buy, 100, 10));
    book.insert(mk(2, Side::Buy, 100, 5));

    CHECK(book.decreaseQuantityInPlace(1, 3));
    CHECK_EQ(*book.queuePosition(1), std::size_t{0});  // still first
    CHECK_EQ(book.get(1)->remaining_qty, Quantity{3});
}

TEST_CASE("decreaseQuantityInPlace rejects an increase or non-strict value") {
    OrderBook book;
    book.insert(mk(1, Side::Buy, 100, 10));
    CHECK(!book.decreaseQuantityInPlace(1, 15));  // increase
    CHECK(!book.decreaseQuantityInPlace(1, 10));  // not a strict decrease
    CHECK(!book.decreaseQuantityInPlace(1, 0));   // not a valid positive qty
}

TEST_CASE("depth aggregates total_qty per level, best price first") {
    OrderBook book;
    book.insert(mk(1, Side::Buy, 100, 10));
    book.insert(mk(2, Side::Buy, 100, 5));
    book.insert(mk(3, Side::Buy, 99, 20));

    auto d = book.depth(Side::Buy, 5);
    CHECK_EQ(d.size(), std::size_t{2});
    CHECK_EQ(d[0].first, Price{100});
    CHECK_EQ(d[0].second, Quantity{15});
    CHECK_EQ(d[1].first, Price{99});
    CHECK_EQ(d[1].second, Quantity{20});
}

TEST_CASE("bids ordered high-to-low, asks ordered low-to-high") {
    OrderBook book;
    book.insert(mk(1, Side::Buy, 100, 1));
    book.insert(mk(2, Side::Buy, 102, 1));
    book.insert(mk(3, Side::Buy, 101, 1));
    CHECK_EQ(*book.bestBid(), Price{102});

    book.insert(mk(4, Side::Sell, 110, 1));
    book.insert(mk(5, Side::Sell, 108, 1));
    book.insert(mk(6, Side::Sell, 109, 1));
    CHECK_EQ(*book.bestAsk(), Price{108});
}
