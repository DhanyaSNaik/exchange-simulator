#include "lob/csv_event_reader.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace lob {

namespace {

constexpr const char* kExpectedHeader = "ts_ns,event,order_id,side,type,price,qty,ref_id";
constexpr std::size_t kColumnCount = 8;

std::string trim(const std::string& s) {
    std::size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    std::size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::vector<std::string> splitLine(const std::string& line) {
    std::vector<std::string> out;
    std::string field;
    std::istringstream iss(line);
    while (std::getline(iss, field, ',')) {
        out.push_back(trim(field));
    }
    // A trailing comma means a final empty field, which getline above
    // drops silently -- add it back so column counts stay honest.
    if (!line.empty() && line.back() == ',') out.push_back("");
    return out;
}

[[noreturn]] void fail(std::size_t line_num, const std::string& msg) {
    throw CsvParseError(line_num, msg);
}

std::uint64_t parseU64(const std::string& s, std::size_t line_num, const std::string& field) {
    if (s.empty()) fail(line_num, "expected a value for '" + field + "', got blank");
    try {
        std::size_t pos = 0;
        unsigned long long v = std::stoull(s, &pos);
        if (pos != s.size()) fail(line_num, "'" + field + "' is not a valid integer: " + s);
        return static_cast<std::uint64_t>(v);
    } catch (const std::invalid_argument&) {
        fail(line_num, "'" + field + "' is not a valid integer: " + s);
    } catch (const std::out_of_range&) {
        fail(line_num, "'" + field + "' is out of range: " + s);
    }
    throw std::logic_error("unreachable");  // silence -Wreturn-type; fail() never returns
}

std::int64_t parseI64(const std::string& s, std::size_t line_num, const std::string& field) {
    if (s.empty()) fail(line_num, "expected a value for '" + field + "', got blank");
    try {
        std::size_t pos = 0;
        long long v = std::stoll(s, &pos);
        if (pos != s.size()) fail(line_num, "'" + field + "' is not a valid integer: " + s);
        return static_cast<std::int64_t>(v);
    } catch (const std::invalid_argument&) {
        fail(line_num, "'" + field + "' is not a valid integer: " + s);
    } catch (const std::out_of_range&) {
        fail(line_num, "'" + field + "' is out of range: " + s);
    }
    throw std::logic_error("unreachable");
}

void requireBlank(const std::string& s, std::size_t line_num, const std::string& field,
                  const std::string& kind) {
    if (!s.empty()) {
        fail(line_num, "'" + field + "' must be blank for " + kind + " events, got: " + s);
    }
}

Side parseSide(const std::string& s, std::size_t line_num) {
    if (s == "BUY") return Side::Buy;
    if (s == "SELL") return Side::Sell;
    fail(line_num, "'side' must be BUY or SELL, got: " + s);
}

OrderType parseType(const std::string& s, std::size_t line_num) {
    if (s == "LIMIT") return OrderType::Limit;
    if (s == "MARKET") return OrderType::Market;
    if (s == "IOC") return OrderType::IOC;
    if (s == "FOK") return OrderType::FOK;
    fail(line_num, "'type' must be LIMIT/MARKET/IOC/FOK, got: " + s);
}

}  // namespace

std::vector<OrderFlowEvent> parseCsvEvents(const std::string& csv_content) {
    std::vector<OrderFlowEvent> events;
    std::istringstream stream(csv_content);
    std::string line;
    std::size_t line_num = 0;
    bool header_seen = false;
    std::uint64_t file_order = 0;

    while (std::getline(stream, line)) {
        ++line_num;
        if (trim(line).empty()) continue;  // skip blank lines silently

        if (!header_seen) {
            if (trim(line) != kExpectedHeader) {
                fail(line_num,
                     "expected header '" + std::string(kExpectedHeader) + "', got: " + line);
            }
            header_seen = true;
            continue;
        }

        std::vector<std::string> cols = splitLine(line);
        if (cols.size() != kColumnCount) {
            fail(line_num, "expected " + std::to_string(kColumnCount) + " columns, got " +
                               std::to_string(cols.size()));
        }
        const std::string& ts_ns_s = cols[0];
        const std::string& event_s = cols[1];
        const std::string& order_id_s = cols[2];
        const std::string& side_s = cols[3];
        const std::string& type_s = cols[4];
        const std::string& price_s = cols[5];
        const std::string& qty_s = cols[6];
        const std::string& ref_id_s = cols[7];

        OrderFlowEvent ev;
        ev.timestamp_ns = parseU64(ts_ns_s, line_num, "ts_ns");
        ev.file_order = file_order++;

        if (event_s == "NEW") {
            ev.kind = EventKind::NewOrder;
            ev.id = parseU64(order_id_s, line_num, "order_id");
            ev.side = parseSide(side_s, line_num);
            ev.type = parseType(type_s, line_num);
            ev.price = parseI64(price_s, line_num, "price");
            ev.qty = parseI64(qty_s, line_num, "qty");
            requireBlank(ref_id_s, line_num, "ref_id", "NEW");
        } else if (event_s == "CANCEL") {
            ev.kind = EventKind::Cancel;
            ev.id = parseU64(order_id_s, line_num, "order_id");
            requireBlank(side_s, line_num, "side", "CANCEL");
            requireBlank(type_s, line_num, "type", "CANCEL");
            requireBlank(price_s, line_num, "price", "CANCEL");
            requireBlank(qty_s, line_num, "qty", "CANCEL");
            requireBlank(ref_id_s, line_num, "ref_id", "CANCEL");
        } else if (event_s == "REPLACE") {
            ev.kind = EventKind::Replace;
            ev.id = parseU64(order_id_s, line_num, "order_id");
            requireBlank(side_s, line_num, "side", "REPLACE");
            requireBlank(type_s, line_num, "type", "REPLACE");
            if (!price_s.empty()) ev.replace_price = parseI64(price_s, line_num, "price");
            if (!qty_s.empty()) ev.replace_qty = parseI64(qty_s, line_num, "qty");
            requireBlank(ref_id_s, line_num, "ref_id", "REPLACE");
        } else if (event_s == "TRADE") {
            ev.kind = EventKind::Trade;
            requireBlank(order_id_s, line_num, "order_id", "TRADE");
            requireBlank(side_s, line_num, "side", "TRADE");
            requireBlank(type_s, line_num, "type", "TRADE");
            ev.price = parseI64(price_s, line_num, "price");
            ev.qty = parseI64(qty_s, line_num, "qty");
            ev.ref_id = parseU64(ref_id_s, line_num, "ref_id");
        } else if (event_s == "TICK") {
            ev.kind = EventKind::ClockTick;
            requireBlank(order_id_s, line_num, "order_id", "TICK");
            requireBlank(side_s, line_num, "side", "TICK");
            requireBlank(type_s, line_num, "type", "TICK");
            requireBlank(price_s, line_num, "price", "TICK");
            requireBlank(qty_s, line_num, "qty", "TICK");
            requireBlank(ref_id_s, line_num, "ref_id", "TICK");
        } else {
            fail(line_num, "unrecognized event kind: " + event_s);
        }

        events.push_back(ev);
    }

    if (!header_seen) {
        fail(line_num == 0 ? 1 : line_num, "file is empty or missing the required header row");
    }

    return events;
}

}  // namespace lob
