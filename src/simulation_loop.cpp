#include "lob/simulation_loop.hpp"

#include <algorithm>
#include <sstream>

namespace lob {

SimulationLoop::SimulationLoop(MatchingEngine& engine, bool strict_mode)
    : engine_(engine), strict_mode_(strict_mode) {}

void SimulationLoop::setLatencyHook(LatencyHook hook) {
    latency_hook_ = std::move(hook);
}
void SimulationLoop::setTickHook(TickHook hook) {
    tick_hook_ = std::move(hook);
}

std::string SimulationLoop::dispatch(const OrderFlowEvent& ev) {
    switch (ev.kind) {
        case EventKind::NewOrder: {
            Order order(ev.id, ev.side, ev.type, ev.price, ev.qty);
            ExecutionReport r = engine_.submit(order);
            if (r.status == OrderStatus::Rejected) {
                std::ostringstream os;
                os << "NEW order " << ev.id
                   << " rejected: reason=" << static_cast<int>(r.reject_reason);
                return os.str();
            }
            return "";
        }
        case EventKind::Cancel: {
            ExecutionReport r = engine_.cancel(ev.id);
            if (r.status == OrderStatus::Rejected) {
                std::ostringstream os;
                os << "CANCEL for order " << ev.id
                   << " rejected: reason=" << static_cast<int>(r.reject_reason);
                return os.str();
            }
            return "";
        }
        case EventKind::Replace: {
            MatchingEngine::ReplaceRequest req{ev.id, ev.replace_price, ev.replace_qty};
            ExecutionReport r = engine_.replace(req);
            if (r.status == OrderStatus::Rejected) {
                std::ostringstream os;
                os << "REPLACE for order " << ev.id
                   << " rejected: reason=" << static_cast<int>(r.reject_reason);
                return os.str();
            }
            return "";
        }
        case EventKind::Trade: {
            auto trade = engine_.applyExternalTrade(ev.ref_id, ev.qty, ev.price);
            if (!trade) {
                std::ostringstream os;
                os << "TRADE against ref_id " << ev.ref_id << " qty=" << ev.qty
                   << " could not be applied (not resting, or qty exceeds remaining)";
                return os.str();
            }
            return "";
        }
        case EventKind::ClockTick: {
            if (tick_hook_) tick_hook_(ev.timestamp_ns);
            return "";
        }
    }
    return "unrecognized event kind";  // unreachable given the enum's fixed cases
}

ReplayReport SimulationLoop::run(std::vector<OrderFlowEvent> events) {
    std::stable_sort(events.begin(), events.end(),
                     [](const OrderFlowEvent& a, const OrderFlowEvent& b) {
                         if (a.timestamp_ns != b.timestamp_ns)
                             return a.timestamp_ns < b.timestamp_ns;
                         return a.file_order < b.file_order;
                     });

    ReplayReport report;
    for (const auto& ev : events) {
        auto t0 = std::chrono::steady_clock::now();
        std::string err = dispatch(ev);
        auto t1 = std::chrono::steady_clock::now();

        if (latency_hook_) {
            latency_hook_(ev, std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0));
        }

        if (!err.empty()) {
            report.events_rejected++;
            report.errors.push_back(err);
            if (strict_mode_) {
                return report;  // stop immediately; report reflects progress so far
            }
            continue;  // strict_mode_ == false: log and keep going
        }
        report.events_processed++;
    }
    return report;
}

}  // namespace lob
