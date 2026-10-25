#include "lob/analytics.hpp"

#include <cmath>

namespace lob {

TradeSign classifyTradeSign(const Trade& trade, std::optional<Price> prevailing_bid,
                            std::optional<Price> prevailing_ask) {
    if (trade.aggressor_id != 0) {
        return (trade.aggressor_side == Side::Buy) ? TradeSign::Buy : TradeSign::Sell;
    }
    // No known aggressor (an externally-replayed trade) -- the quote rule.
    if (!prevailing_bid || !prevailing_ask) return TradeSign::Ambiguous;
    if (trade.price >= *prevailing_ask) return TradeSign::Buy;
    if (trade.price <= *prevailing_bid) return TradeSign::Sell;
    return TradeSign::Ambiguous;  // strictly between the quotes: genuinely ambiguous
}

namespace {

// Bid-side OFI contribution (Cont/Kukanov/Stoikov): a price improvement
// contributes the full new size; a price decline removes the full old
// size; an unchanged price contributes the net size change. A missing
// bid is treated as an infinitely bad price (-infinity), so a bid
// appearing from nothing is a price improvement, and a bid vanishing is
// a price decline -- consistent with "no resting interest" being the
// worst possible price on that side.
double bidContribution(std::optional<Price> px_before, Quantity sz_before,
                       std::optional<Price> px_after, Quantity sz_after) {
    if (!px_before && !px_after) return 0.0;
    if (!px_before) return static_cast<double>(sz_after);
    if (!px_after) return -static_cast<double>(sz_before);
    if (*px_after > *px_before) return static_cast<double>(sz_after);
    if (*px_after == *px_before)
        return static_cast<double>(sz_after) - static_cast<double>(sz_before);
    return -static_cast<double>(sz_before);
}

// Ask-side contribution, mirrored: a LOWER ask price is the
// improvement (more aggressive sell interest), a HIGHER ask is the
// decline. A missing ask is treated as +infinity.
double askContribution(std::optional<Price> px_before, Quantity sz_before,
                       std::optional<Price> px_after, Quantity sz_after) {
    if (!px_before && !px_after) return 0.0;
    if (!px_before) return static_cast<double>(sz_after);
    if (!px_after) return -static_cast<double>(sz_before);
    if (*px_after < *px_before) return static_cast<double>(sz_after);
    if (*px_after == *px_before)
        return static_cast<double>(sz_after) - static_cast<double>(sz_before);
    return -static_cast<double>(sz_before);
}

}  // namespace

OrderFlowImbalanceTracker::OrderFlowImbalanceTracker(std::size_t window_events)
    : window_events_(window_events) {}

void OrderFlowImbalanceTracker::update(std::optional<Price> bid_px_before, Quantity bid_sz_before,
                                       std::optional<Price> ask_px_before, Quantity ask_sz_before,
                                       std::optional<Price> bid_px_after, Quantity bid_sz_after,
                                       std::optional<Price> ask_px_after, Quantity ask_sz_after) {
    const double e_bid = bidContribution(bid_px_before, bid_sz_before, bid_px_after, bid_sz_after);
    const double e_ask = askContribution(ask_px_before, ask_sz_before, ask_px_after, ask_sz_after);
    const double e_n = e_bid - e_ask;

    contributions_.push_back(e_n);
    running_sum_ += e_n;
    while (contributions_.size() > window_events_) {
        running_sum_ -= contributions_.front();
        contributions_.pop_front();
    }
}

double OrderFlowImbalanceTracker::currentOFI() const {
    return running_sum_;
}

RealizedVolatilityTracker::RealizedVolatilityTracker(std::uint64_t window_ns)
    : window_ns_(window_ns) {}

void RealizedVolatilityTracker::sample(std::uint64_t timestamp_ns, double midprice) {
    if (has_last_sample_ && last_midprice_ > 0.0 && midprice > 0.0) {
        const double log_ret = std::log(midprice / last_midprice_);
        const double sq = log_ret * log_ret;
        windowed_returns_.push_back({timestamp_ns, sq});
        running_sum_ += sq;
    }
    has_last_sample_ = true;
    last_midprice_ = midprice;

    while (!windowed_returns_.empty() &&
           timestamp_ns - windowed_returns_.front().first > window_ns_) {
        running_sum_ -= windowed_returns_.front().second;
        windowed_returns_.pop_front();
    }
}

double RealizedVolatilityTracker::currentRealizedVol() const {
    return running_sum_;
}

}  // namespace lob
