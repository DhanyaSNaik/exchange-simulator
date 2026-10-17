// Before benchmarking FlatMatchingEngine against MatchingEngine, confirm
// they are actually both correct and agree on outcomes -- a benchmark
// comparing a buggy implementation against a correct one produces
// numbers, but not evidence. Same differential-testing approach as
// tests/reference/test_differential.cpp, reused here for the bench-only
// alternative instead of the naive reference engine.

#include <algorithm>
#include <iostream>
#include <random>
#include <sstream>

#include "flat_matching_engine.hpp"
#include "lob/matching_engine.hpp"

using namespace lob;

namespace {

std::string describe(const ExecutionReport& r) {
    std::ostringstream os;
    os << "status=" << static_cast<int>(r.status) << " filled=" << r.filled_qty
       << " remaining=" << r.remaining_qty << " reject=" << static_cast<int>(r.reject_reason)
       << " trades=" << r.trades.size();
    for (auto& t : r.trades) os << " (rest=" << t.resting_id << " qty=" << t.quantity << ")";
    return os.str();
}

bool match(const ExecutionReport& a, const ExecutionReport& b) {
    if (a.status != b.status || a.filled_qty != b.filled_qty ||
        a.remaining_qty != b.remaining_qty || a.reject_reason != b.reject_reason ||
        a.trades.size() != b.trades.size())
        return false;
    for (std::size_t i = 0; i < a.trades.size(); ++i) {
        if (a.trades[i].resting_id != b.trades[i].resting_id) return false;
        if (a.trades[i].price != b.trades[i].price) return false;
        if (a.trades[i].quantity != b.trades[i].quantity) return false;
    }
    return true;
}

bool runStream(std::uint64_t seed, int n) {
    MatchingEngine real_eng;
    lob_bench::FlatMatchingEngine flat_eng;
    std::mt19937_64 rng(seed);
    OrderId next_id = 1;
    std::vector<OrderId> live;

    auto price = [&] { return 95 + static_cast<Price>(rng() % 11); };
    auto qty = [&] { return 1 + static_cast<Quantity>(rng() % 20); };
    auto side = [&] { return (rng() % 2 == 0) ? Side::Buy : Side::Sell; };
    auto type = [&]() -> OrderType {
        switch (rng() % 4) {
            case 0: return OrderType::Limit;
            case 1: return OrderType::Market;
            case 2: return OrderType::IOC;
            default: return OrderType::FOK;
        }
    };

    for (int i = 0; i < n; ++i) {
        int choice = static_cast<int>(rng() % 10);
        if (choice < 7 || live.empty()) {
            OrderId id = next_id++;
            Order o(id, side(), type(), price(), qty());
            auto r1 = real_eng.submit(o);
            auto r2 = flat_eng.submit(o);
            if (!match(r1, r2)) {
                std::cerr << "seed=" << seed << " event=" << i << " DIVERGE submit\n  real: "
                          << describe(r1) << "\n  flat: " << describe(r2) << "\n";
                return false;
            }
            if (r1.status == OrderStatus::New || r1.status == OrderStatus::PartiallyFilled) {
                live.push_back(id);
            }
        } else if (choice < 9) {
            OrderId id = live[rng() % live.size()];
            auto r1 = real_eng.cancel(id);
            auto r2 = flat_eng.cancel(id);
            if (!match(r1, r2)) {
                std::cerr << "seed=" << seed << " event=" << i << " DIVERGE cancel\n";
                return false;
            }
            auto it = std::find(live.begin(), live.end(), id);
            if (it != live.end()) live.erase(it);
        } else {
            OrderId id = live[rng() % live.size()];
            MatchingEngine::ReplaceRequest req1{id, std::nullopt, std::nullopt};
            lob_bench::FlatMatchingEngine::ReplaceRequest req2{id, std::nullopt, std::nullopt};
            if (rng() % 2 == 0) {
                Quantity q = qty();
                req1.new_qty = q;
                req2.new_qty = q;
            } else {
                Price p = price();
                req1.new_price = p;
                req2.new_price = p;
            }
            auto r1 = real_eng.replace(req1);
            auto r2 = flat_eng.replace(req2);
            if (!match(r1, r2)) {
                std::cerr << "seed=" << seed << " event=" << i << " DIVERGE replace\n";
                return false;
            }
        }
    }
    return true;
}

}  // namespace

int main() {
    int failures = 0;
    constexpr int kStreams = 200;
    constexpr int kEvents = 500;
    for (int s = 0; s < kStreams; ++s) {
        if (!runStream(10000 + s, kEvents)) ++failures;
    }
    std::cout << (kStreams - failures) << "/" << kStreams << " streams matched between "
              << "MatchingEngine and FlatMatchingEngine (" << kEvents << " events each)\n";
    return failures == 0 ? 0 : 1;
}
