// Benchmark harness. Usage: run_benchmark <num_events> <seed>
// Replays an identical synthetic event stream through both MatchingEngine
// (std::map-based, Phase 1) and FlatMatchingEngine (sorted-vector-based,
// Phase 0 Option B), measuring per-event latency and reporting throughput,
// latency percentiles, and peak memory for each.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

#include "event_generator.hpp"
#include "flat_matching_engine.hpp"
#include "lob/matching_engine.hpp"

using namespace lob;
using Clock = std::chrono::steady_clock;

namespace {

long currentRssKb() {
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line)) {
        if (line.compare(0, 6, "VmHWM:") == 0) {
            std::istringstream iss(line.substr(6));
            long kb;
            iss >> kb;
            return kb;  // peak resident set size so far
        }
    }
    return -1;
}

struct LatencyStats {
    double p50, p95, p99, max, mean;
};

LatencyStats computeStats(std::vector<double>& ns) {  // sorts in place
    std::sort(ns.begin(), ns.end());
    auto pct = [&](double p) {
        std::size_t idx = static_cast<std::size_t>(p * (ns.size() - 1));
        return ns[idx];
    };
    double sum = std::accumulate(ns.begin(), ns.end(), 0.0);
    return LatencyStats{pct(0.50), pct(0.95), pct(0.99), ns.back(), sum / ns.size()};
}

template <typename EngineT, typename ReplaceReqT>
void runEventStream(EngineT& eng, const std::vector<lob_bench::SyntheticEvent>& events,
                     std::vector<double>& latencies_ns, std::vector<double>* fok_latencies_ns,
                     std::vector<double>* other_submit_latencies_ns) {
    latencies_ns.reserve(events.size());
    for (const auto& ev : events) {
        auto t0 = Clock::now();
        switch (ev.kind) {
            case lob_bench::EventKind::Submit:
                eng.submit(ev.order);
                break;
            case lob_bench::EventKind::Cancel:
                eng.cancel(ev.target_id);
                break;
            case lob_bench::EventKind::Replace: {
                ReplaceReqT req{ev.target_id, ev.replace_price, ev.replace_qty};
                eng.replace(req);
                break;
            }
        }
        auto t1 = Clock::now();
        double ns = std::chrono::duration<double, std::nano>(t1 - t0).count();
        latencies_ns.push_back(ns);

        // Breakdown by event type -- specifically isolating FOK submits,
        // to test the hypothesis that their O(L)-in-price-levels dry-run
        // (aggregateFillable) is what drives tail latency as the
        // synthetic book's level count grows over a long run.
        if (ev.kind == lob_bench::EventKind::Submit) {
            if (ev.order.type == OrderType::FOK) {
                if (fok_latencies_ns) fok_latencies_ns->push_back(ns);
            } else {
                if (other_submit_latencies_ns) other_submit_latencies_ns->push_back(ns);
            }
        }
    }
}

void report(const std::string& label, std::vector<double> latencies_ns, double total_seconds,
            long rss_kb) {
    auto stats = computeStats(latencies_ns);
    double throughput = static_cast<double>(latencies_ns.size()) / total_seconds;
    std::printf("  %-18s throughput=%.0f events/sec  p50=%.0fns  p95=%.0fns  p99=%.0fns  "
                "max=%.0fns  mean=%.0fns  peak_RSS=%ldKB\n",
                label.c_str(), throughput, stats.p50, stats.p95, stats.p99, stats.max,
                stats.mean, rss_kb);
}

}  // namespace

int main(int argc, char** argv) {
    // argv: <num_events> <seed> <engine: map|flat>
    //
    // Deliberately ONE engine per process invocation. VmHWM ("peak RSS so
    // far") is a running high-water mark for the whole process, not a
    // per-scope figure -- running both engines in one process would mean
    // the second engine's reported "peak" is actually whichever of the
    // two engines used more memory, not necessarily itself. Separate
    // processes (orchestrated by run_benchmarks.sh) is what makes each
    // engine's memory number actually attributable to that engine.
    std::size_t n = (argc > 1) ? static_cast<std::size_t>(std::strtoull(argv[1], nullptr, 10))
                                : 100000;
    std::uint64_t seed = (argc > 2) ? std::strtoull(argv[2], nullptr, 10) : 42;
    std::string engine_kind = (argc > 3) ? argv[3] : "map";

    auto events = lob_bench::generateEventStream(seed, n);

    std::vector<double> fok_lat, other_lat;

    if (engine_kind == "map") {
        MatchingEngine eng;
        std::vector<double> lat;
        auto t0 = Clock::now();
        runEventStream<MatchingEngine, MatchingEngine::ReplaceRequest>(eng, events, lat,
                                                                        &fok_lat, &other_lat);
        auto t1 = Clock::now();
        double secs = std::chrono::duration<double>(t1 - t0).count();
        report("map+list (P1)", std::move(lat), secs, currentRssKb());
        std::printf("    final levels: bid=%zu ask=%zu  final resting orders=%zu\n",
                    eng.book().levelCount(Side::Buy), eng.book().levelCount(Side::Sell),
                    eng.book().orderCount());
    } else if (engine_kind == "flat") {
        lob_bench::FlatMatchingEngine eng;
        std::vector<double> lat;
        auto t0 = Clock::now();
        runEventStream<lob_bench::FlatMatchingEngine, lob_bench::FlatMatchingEngine::ReplaceRequest>(
            eng, events, lat, &fok_lat, &other_lat);
        auto t1 = Clock::now();
        double secs = std::chrono::duration<double>(t1 - t0).count();
        report("flat_vector (B)", std::move(lat), secs, currentRssKb());
    } else {
        std::fprintf(stderr, "unknown engine kind: %s (expected 'map' or 'flat')\n",
                     engine_kind.c_str());
        return 1;
    }

    if (!fok_lat.empty() && !other_lat.empty()) {
        auto fok_stats = computeStats(fok_lat);
        auto other_stats = computeStats(other_lat);
        std::printf("    FOK submits   (n=%6zu): p50=%.0fns p99=%.0fns max=%.0fns mean=%.0fns\n",
                    fok_lat.size(), fok_stats.p50, fok_stats.p99, fok_stats.max,
                    fok_stats.mean);
        std::printf("    other submits (n=%6zu): p50=%.0fns p99=%.0fns max=%.0fns mean=%.0fns\n",
                    other_lat.size(), other_stats.p50, other_stats.p99, other_stats.max,
                    other_stats.mean);
    }
    return 0;
}
