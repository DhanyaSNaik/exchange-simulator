#include "lob/csv_event_reader.hpp"
#include "testing.hpp"

using namespace lob;

namespace {
const char* kHeader = "ts_ns,event,order_id,side,type,price,qty,ref_id\n";
}

TEST_CASE("CSV: parses one of each event kind correctly") {
    std::string csv = std::string(kHeader) +
                      "1000,NEW,1,BUY,LIMIT,10000,50,\n"
                      "1500,NEW,2,SELL,LIMIT,10005,30,\n"
                      "2200,CANCEL,1,,,,,\n"
                      "3000,REPLACE,2,,,10002,20,\n"
                      "4000,TRADE,,,,10000,15,1\n"
                      "5000,TICK,,,,,,\n";

    auto events = parseCsvEvents(csv);
    CHECK_EQ(events.size(), std::size_t{6});

    CHECK(events[0].kind == EventKind::NewOrder);
    CHECK_EQ(events[0].timestamp_ns, std::uint64_t{1000});
    CHECK_EQ(events[0].id, OrderId{1});
    CHECK(events[0].side == Side::Buy);
    CHECK(events[0].type == OrderType::Limit);
    CHECK_EQ(events[0].price, Price{10000});
    CHECK_EQ(events[0].qty, Quantity{50});

    CHECK(events[2].kind == EventKind::Cancel);
    CHECK_EQ(events[2].id, OrderId{1});

    CHECK(events[3].kind == EventKind::Replace);
    CHECK_EQ(events[3].id, OrderId{2});
    CHECK_EQ(*events[3].replace_price, Price{10002});  // both price and qty set in this row
    CHECK_EQ(*events[3].replace_qty, Quantity{20});

    CHECK(events[4].kind == EventKind::Trade);
    CHECK_EQ(events[4].ref_id, OrderId{1});
    CHECK_EQ(events[4].price, Price{10000});
    CHECK_EQ(events[4].qty, Quantity{15});

    CHECK(events[5].kind == EventKind::ClockTick);
    CHECK_EQ(events[5].timestamp_ns, std::uint64_t{5000});
}

TEST_CASE("CSV: REPLACE row with a blank price OR blank qty maps that field to nullopt") {
    std::string csv = std::string(kHeader) + "1000,REPLACE,1,,,,20,\n" +  // price blank, qty=20
                      "2000,REPLACE,1,,,10050,,\n";                       // price=10050, qty blank
    auto events = parseCsvEvents(csv);

    CHECK(!events[0].replace_price.has_value());
    CHECK_EQ(*events[0].replace_qty, Quantity{20});

    CHECK_EQ(*events[1].replace_price, Price{10050});
    CHECK(!events[1].replace_qty.has_value());
}

TEST_CASE("CSV: file_order is assigned by row position, not timestamp") {
    std::string csv = std::string(kHeader) +
                      "5000,TICK,,,,,,\n"
                      "1000,TICK,,,,,,\n";  // out-of-order timestamps in the file
    auto events = parseCsvEvents(csv);
    CHECK_EQ(events[0].timestamp_ns, std::uint64_t{5000});
    CHECK_EQ(events[0].file_order, std::uint64_t{0});
    CHECK_EQ(events[1].timestamp_ns, std::uint64_t{1000});
    CHECK_EQ(events[1].file_order, std::uint64_t{1});
}

TEST_CASE("CSV: missing or wrong header is a fatal parse error") {
    std::string csv = "not,the,right,header\n1000,TICK,,,,,,\n";
    bool threw = false;
    try {
        parseCsvEvents(csv);
    } catch (const CsvParseError&) {
        threw = true;
    }
    CHECK(threw);
}

TEST_CASE("CSV: wrong column count is a fatal parse error") {
    std::string csv = std::string(kHeader) + "1000,TICK,,,,,\n";  // one column short
    bool threw = false;
    try {
        parseCsvEvents(csv);
    } catch (const CsvParseError& e) {
        threw = true;
        CHECK(e.line_number == 2);
    }
    CHECK(threw);
}

TEST_CASE("CSV: unrecognized event kind is a fatal parse error") {
    std::string csv = std::string(kHeader) + "1000,BOGUS,,,,,,,\n";
    bool threw = false;
    try {
        parseCsvEvents(csv);
    } catch (const CsvParseError&) {
        threw = true;
    }
    CHECK(threw);
}

TEST_CASE("CSV: unparseable numeric field is a fatal parse error") {
    std::string csv = std::string(kHeader) + "not_a_number,TICK,,,,,,\n";
    bool threw = false;
    try {
        parseCsvEvents(csv);
    } catch (const CsvParseError&) {
        threw = true;
    }
    CHECK(threw);
}

TEST_CASE("CSV: a value present where the event kind requires it blank is fatal") {
    // TICK requires every field but ts_ns blank; this one has a stray
    // order_id, which must fail rather than be silently ignored.
    std::string csv = std::string(kHeader) + "1000,TICK,5,,,,,\n";
    bool threw = false;
    try {
        parseCsvEvents(csv);
    } catch (const CsvParseError&) {
        threw = true;
    }
    CHECK(threw);
}

TEST_CASE("CSV: invalid side or type value is a fatal parse error") {
    std::string csv1 = std::string(kHeader) + "1000,NEW,1,SIDEWAYS,LIMIT,100,5,\n";
    bool threw1 = false;
    try {
        parseCsvEvents(csv1);
    } catch (const CsvParseError&) {
        threw1 = true;
    }
    CHECK(threw1);

    std::string csv2 = std::string(kHeader) + "1000,NEW,1,BUY,WEIRD,100,5,\n";
    bool threw2 = false;
    try {
        parseCsvEvents(csv2);
    } catch (const CsvParseError&) {
        threw2 = true;
    }
    CHECK(threw2);
}

TEST_CASE("CSV: blank lines between data rows are skipped silently") {
    std::string csv = std::string(kHeader) + "1000,TICK,,,,,,\n\n2000,TICK,,,,,,\n";
    auto events = parseCsvEvents(csv);
    CHECK_EQ(events.size(), std::size_t{2});
}
