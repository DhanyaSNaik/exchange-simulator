#pragma once
// Phase 4: the integration point between external order flow (any
// participant OTHER than the agent -- a test driver, or Phase 3's
// SimulationLoop) and a MarketMakerAgent's resting quotes.
//
// MatchingEngine and SimulationLoop stay completely agent-ignorant, the
// same way OrderBook stayed matching-ignorant in Phase 1 -- this class
// exists specifically so that symmetry holds here too. It wraps every
// externally-driven engine call, inspects the resulting ExecutionReport
// (or Trade, for applyExternalTrade) for any trade whose resting_id
// matches the agent's CURRENT bid or ask order id, and routes it to
// agent.onFill(). The agent's OWN quote-posting calls (inside
// MarketMakerAgent::onTick(), which calls engine_.submit()/cancel()
// directly) are NOT routed through this class -- a self-cross from the
// agent's own new quote is already visible in that same submit() call's
// own ExecutionReport, so MarketMakerAgent handles it inline without
// needing this wrapper at all (see market_maker.cpp).
//
// Timestamps are passed explicitly by the caller on every call, rather
// than read from a wall clock -- consistent with Phase 3's simulated-
// time discipline. Call SimulationLoop's hooks (or a test driver's own
// loop) with the event's own timestamp_ns.

#include <cstdint>
#include <optional>

#include "lob/market_maker.hpp"
#include "lob/matching_engine.hpp"

namespace lob {

class MarketSession {
public:
    MarketSession(MatchingEngine& engine, MarketMakerAgent& agent);

    ExecutionReport submit(Order order, std::uint64_t timestamp_ns);
    ExecutionReport cancel(OrderId id, std::uint64_t timestamp_ns);
    ExecutionReport replace(const MatchingEngine::ReplaceRequest& req, std::uint64_t timestamp_ns);
    std::optional<Trade> applyExternalTrade(OrderId resting_id, Quantity qty, Price price,
                                            std::uint64_t timestamp_ns);

    // Forwards directly to agent.onTick(timestamp_ns) -- provided here
    // so a caller can drive the whole session (external flow AND the
    // agent's own quoting) through one object without holding a
    // separate reference to the agent.
    void onTick(std::uint64_t timestamp_ns);

private:
    MatchingEngine& engine_;
    MarketMakerAgent& agent_;

    void checkForFills(const std::vector<Trade>& trades, std::uint64_t timestamp_ns);
};

}  // namespace lob
