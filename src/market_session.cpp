#include "lob/market_session.hpp"

namespace lob {

MarketSession::MarketSession(MatchingEngine& engine, MarketMakerAgent& agent)
    : engine_(engine), agent_(agent) {}

void MarketSession::checkForFills(const std::vector<Trade>& trades, std::uint64_t timestamp_ns) {
    const auto& st = agent_.state();
    for (const auto& t : trades) {
        if (st.bid_order_id != 0 && t.resting_id == st.bid_order_id) {
            agent_.onFill(Side::Buy, t.price, t.quantity, timestamp_ns);
        } else if (st.ask_order_id != 0 && t.resting_id == st.ask_order_id) {
            agent_.onFill(Side::Sell, t.price, t.quantity, timestamp_ns);
        }
        // A trade whose resting_id matches neither is between two OTHER
        // participants (or an external replayed trade against a
        // non-agent order) and is correctly ignored here.
    }
}

ExecutionReport MarketSession::submit(Order order, std::uint64_t timestamp_ns) {
    ExecutionReport report = engine_.submit(std::move(order));
    checkForFills(report.trades, timestamp_ns);
    return report;
}

ExecutionReport MarketSession::cancel(OrderId id, std::uint64_t timestamp_ns) {
    // A cancel never generates a trade, so there is nothing to check for
    // fills here -- included for API symmetry and so a caller can route
    // ALL external order flow through this one class uniformly.
    (void)timestamp_ns;
    return engine_.cancel(id);
}

ExecutionReport MarketSession::replace(const MatchingEngine::ReplaceRequest& req,
                                       std::uint64_t timestamp_ns) {
    ExecutionReport report = engine_.replace(req);
    checkForFills(report.trades, timestamp_ns);
    return report;
}

std::optional<Trade> MarketSession::applyExternalTrade(OrderId resting_id, Quantity qty,
                                                       Price price, std::uint64_t timestamp_ns) {
    auto trade = engine_.applyExternalTrade(resting_id, qty, price);
    if (trade) checkForFills({*trade}, timestamp_ns);
    return trade;
}

void MarketSession::onTick(std::uint64_t timestamp_ns) {
    agent_.onTick(timestamp_ns);
}

}  // namespace lob
