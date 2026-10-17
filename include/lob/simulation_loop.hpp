#pragma once
// Phase 3: deterministic event-driven replay against a MatchingEngine.
// This module depends on MatchingEngine; nothing in Phase 1/2 depends on
// this -- one-directional, so replay can be deleted without touching the
// engine (DESIGN_DECISIONS.md).

#include <chrono>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "lob/matching_engine.hpp"
#include "lob/replay_event.hpp"

namespace lob {

struct ReplayReport {
    std::size_t events_processed = 0;
    std::size_t events_rejected = 0;
    std::vector<std::string> errors;  // one human-readable entry per rejected event
};

class SimulationLoop {
public:
    // strict_mode (default true): an event whose outcome is a REJECTION
    // -- a NEW order rejected (bad price/qty/duplicate id), a CANCEL or
    // REPLACE targeting an unknown order, or a TRADE whose ref_id isn't
    // resting or whose qty exceeds what's resting -- stops the run
    // immediately, with the report reflecting everything processed up
    // to that point. Applied uniformly across every event kind, not
    // just applyExternalTrade, because a rejected NEW or CANCEL is just
    // as much a data-quality signal from the feed as a bad TRADE row --
    // special-casing only one event kind would be an arbitrary
    // inconsistency, not a principled distinction.
    //
    // strict_mode=false instead records the rejection in
    // ReplayReport.errors and continues with the rest of the stream --
    // an explicit opt-in for exploratory work on a known-messy feed, not
    // the default, because a silently-partial book reconstruction is a
    // worse failure mode than a loud stop for a project whose whole
    // premise is correctness-first (DESIGN_DECISIONS.md).
    //
    // This flag does NOT govern file-format/parse errors (wrong column
    // count, unparseable number, unknown event kind) -- those are always
    // fatal, thrown by parseCsvEvents() before any event reaches this
    // class at all. strict_mode is about data-quality problems in an
    // otherwise well-formed event, not about whether the file itself is
    // readable.
    explicit SimulationLoop(MatchingEngine& engine, bool strict_mode = true);

    // Sorts by (timestamp_ns, file_order) -- the latter being the
    // deterministic tie-break for events sharing a timestamp, assigned
    // by whatever produced the events (e.g. the CSV reader, as file
    // position) -- then dispatches each in that order.
    ReplayReport run(std::vector<OrderFlowEvent> events);

    // Latency instrumentation: called around EVERY event's dispatch,
    // given the event and the wall-clock duration its processing took.
    // Measurement only -- no injected delay, no simulated network/
    // exchange/agent latency (explicitly future work per the Phase 3
    // brief). One slot, replacing a previous hook if set again -- kept
    // simple since no agents consume this yet.
    using LatencyHook = std::function<void(const OrderFlowEvent&, std::chrono::nanoseconds)>;
    void setLatencyHook(LatencyHook hook);

    // Fired on every ClockTick event, given the event's timestamp_ns.
    // No hooks registered by Phase 3 itself; exists for Phase 4's agents
    // to subscribe to simulated time advancing.
    using TickHook = std::function<void(std::uint64_t timestamp_ns)>;
    void setTickHook(TickHook hook);

private:
    MatchingEngine& engine_;
    bool strict_mode_;
    LatencyHook latency_hook_;
    TickHook tick_hook_;

    // Returns an error message if the event's outcome was a rejection
    // (see strict_mode above for what counts), or an empty string on a
    // clean apply. ClockTick never produces an error.
    std::string dispatch(const OrderFlowEvent& ev);
};

}  // namespace lob
