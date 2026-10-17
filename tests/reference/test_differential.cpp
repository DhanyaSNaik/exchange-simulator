// Differential testing: the real MatchingEngine and the deliberately naive
// ReferenceEngine are driven with IDENTICAL randomized event streams, and
// every ExecutionReport they produce is compared for exact equality --
// same status, same filled/remaining quantities, same trades (same ids,
// prices, quantities, in the same order). Any divergence means the
// "optimized" engine disagrees with the obviously-correct one, which is a
// real bug regardless of whether any invariant was technically preserved.

#include <cstdint>
#include <iostream>
#include <random>
#include <sstream>

#include "lob/matching_engine.hpp"
#include "reference_engine.hpp"

using namespace lob;

namespace {

std::string describeReport(const ExecutionReport& r) {
    std::ostringstream os;
    os << "order_id=" << r.order_id << " status=" << static_cast<int>(r.status)
       << " filled=" << r.filled_qty << " remaining=" << r.remaining_qty
       << " reject=" << static_cast<int>(r.reject_reason) << " trades=[";
    for (auto& t : r.trades) {
        os << "(agg=" << t.aggressor_id << " rest=" << t.resting_id << " px=" << t.price
           << " qty=" << t.quantity << ") ";
    }
    os << "]";
    return os.str();
}

bool reportsMatch(const ExecutionReport& a, const ExecutionReport& b) {
    if (a.order_id != b.order_id) return false;
    if (a.status != b.status) return false;
    if (a.filled_qty != b.filled_qty) return false;
    if (a.remaining_qty != b.remaining_qty) return false;
    if (a.reject_reason != b.reject_reason) return false;
    if (a.trades.size() != b.trades.size()) return false;
    for (std::size_t i = 0; i < a.trades.size(); ++i) {
        if (a.trades[i].resting_id != b.trades[i].resting_id) return false;
        if (a.trades[i].price != b.trades[i].price) return false;
        if (a.trades[i].quantity != b.trades[i].quantity) return false;
    }
    return true;
}

bool runDifferentialStream(std::uint64_t seed, int num_events) {
    MatchingEngine real_eng;
    lob_ref::ReferenceEngine ref_eng;
    std::mt19937_64 rng(seed);
    OrderId next_id = 1;
    std::vector<OrderId> live_ids;

    auto randPrice = [&] { return 95 + static_cast<Price>(rng() % 11); };
    auto randQty = [&] { return 1 + static_cast<Quantity>(rng() % 20); };
    auto randSide = [&] { return (rng() % 2 == 0) ? Side::Buy : Side::Sell; };
    auto randType = [&]() -> OrderType {
        switch (rng() % 4) {
            case 0:
                return OrderType::Limit;
            case 1:
                return OrderType::Market;
            case 2:
                return OrderType::IOC;
            default:
                return OrderType::FOK;
        }
    };

    for (int i = 0; i < num_events; ++i) {
        const int choice = static_cast<int>(rng() % 10);

        if (choice < 7 || live_ids.empty()) {
            OrderId id = next_id++;
            Order order(id, randSide(), randType(), randPrice(), randQty());

            auto real_report = real_eng.submit(order);
            auto ref_report = ref_eng.submit(order);

            if (!reportsMatch(real_report, ref_report)) {
                std::cerr << "seed=" << seed << " event=" << i
                          << " DIVERGENCE on submit:\n  real: " << describeReport(real_report)
                          << "\n  ref:  " << describeReport(ref_report) << "\n";
                return false;
            }
            if (real_report.status == OrderStatus::New ||
                real_report.status == OrderStatus::PartiallyFilled) {
                live_ids.push_back(id);
            }
        } else if (choice < 9) {
            if (live_ids.empty()) continue;
            OrderId id = live_ids[rng() % live_ids.size()];
            auto real_report = real_eng.cancel(id);
            auto ref_report = ref_eng.cancel(id);
            if (!reportsMatch(real_report, ref_report)) {
                std::cerr << "seed=" << seed << " event=" << i
                          << " DIVERGENCE on cancel:\n  real: " << describeReport(real_report)
                          << "\n  ref:  " << describeReport(ref_report) << "\n";
                return false;
            }
            auto it = std::find(live_ids.begin(), live_ids.end(), id);
            if (it != live_ids.end()) live_ids.erase(it);
        } else {
            if (live_ids.empty()) continue;
            OrderId id = live_ids[rng() % live_ids.size()];
            MatchingEngine::ReplaceRequest real_req{id, std::nullopt, std::nullopt};
            lob_ref::ReferenceEngine::ReplaceRequest ref_req{id, std::nullopt, std::nullopt};
            if (rng() % 2 == 0) {
                Quantity q = randQty();
                real_req.new_qty = q;
                ref_req.new_qty = q;
            } else {
                Price p = randPrice();
                real_req.new_price = p;
                ref_req.new_price = p;
            }
            auto real_report = real_eng.replace(real_req);
            auto ref_report = ref_eng.replace(ref_req);
            if (!reportsMatch(real_report, ref_report)) {
                std::cerr << "seed=" << seed << " event=" << i
                          << " DIVERGENCE on replace:\n  real: " << describeReport(real_report)
                          << "\n  ref:  " << describeReport(ref_report) << "\n";
                return false;
            }
        }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    int num_streams = 300;
    int events_per_stream = 300;
    std::uint64_t base_seed = 2024;

    if (argc > 1) num_streams = std::atoi(argv[1]);
    if (argc > 2) events_per_stream = std::atoi(argv[2]);
    if (argc > 3) base_seed = std::strtoull(argv[3], nullptr, 10);

    int failures = 0;
    for (int s = 0; s < num_streams; ++s) {
        std::uint64_t seed = base_seed + static_cast<std::uint64_t>(s);
        if (!runDifferentialStream(seed, events_per_stream)) {
            std::cerr << "FAILING SEED: " << seed << "\n";
            ++failures;
        }
    }

    std::cout << (num_streams - failures) << "/" << num_streams << " differential streams matched ("
              << events_per_stream << " events each, " << failures << " failed)\n";
    return failures == 0 ? 0 : 1;
}
