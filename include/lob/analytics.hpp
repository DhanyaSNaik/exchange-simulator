#pragma once
// Phase 4 microstructure analytics. Spread/midprice/microprice are NOT
// here -- they already exist on MatchingEngine (Phase 1) and are called
// directly, per the brief's instruction not to duplicate them.

#include <cstdint>
#include <deque>
#include <optional>

#include "lob/order.hpp"

namespace lob {

enum class TradeSign { Buy, Sell, Ambiguous };

// Classifies which side was the AGGRESSOR for a trade.
//
// Ground truth when it's available: if trade.aggressor_id != 0 (an
// internally-matched trade, which always knows its aggressor), this
// returns Buy/Sell directly from trade.aggressor_side -- no inference
// needed at all.
//
// Otherwise (trade.aggressor_id == 0: an externally-replayed Trade event
// with no known aggressor -- see Phase 3's sentinel convention), this
// falls back to the QUOTE RULE: compare the trade price to the
// prevailing bid/ask immediately BEFORE the trade. Above the ask (or at
// it) -> Buy-initiated; below the bid (or at it) -> Sell-initiated;
// strictly between them, or when no prevailing quote is available at
// all, returns Ambiguous rather than guessing. This is a deliberately
// narrower rule than the full Lee-Ready algorithm (which adds a tick-
// rule fallback for exactly-at-midpoint trades using the previous
// trade's price direction) -- named here as a known simplification, not
// implemented, since it would need trade-history state this function
// doesn't otherwise require.
TradeSign classifyTradeSign(const Trade& trade, std::optional<Price> prevailing_bid,
                            std::optional<Price> prevailing_ask);

// Standard per-event OFI contribution (Cont, Kukanov, Stoikov), summed
// over a window of the last `window_events` calls to update(). This is
// an EVENT-COUNT window, not a time window -- see DESIGN_DECISIONS.md
// for why that's the right choice here specifically, in contrast to
// RealizedVolatilityTracker's time window below.
//
// Call update() with the best bid/ask price and size immediately before
// and immediately after every book-mutating event (NewOrder, Cancel,
// Replace, Trade -- NOT ClockTick, which doesn't change the book).
class OrderFlowImbalanceTracker {
public:
    explicit OrderFlowImbalanceTracker(std::size_t window_events);

    void update(std::optional<Price> bid_px_before, Quantity bid_sz_before,
                std::optional<Price> ask_px_before, Quantity ask_sz_before,
                std::optional<Price> bid_px_after, Quantity bid_sz_after,
                std::optional<Price> ask_px_after, Quantity ask_sz_after);

    double currentOFI() const;

private:
    std::size_t window_events_;
    std::deque<double> contributions_;
    double running_sum_ = 0.0;
};

// Sum-of-squared-log-returns over a TIME window (not an event count).
// Volatility is inherently a per-unit-time quantity; an event-count
// window would make the resulting number depend on how busy the market
// happened to be during that stretch, which is a real distortion
// specifically for this metric (unlike OFI, where an event-count window
// is the standard and correct choice -- see DESIGN_DECISIONS.md).
class RealizedVolatilityTracker {
public:
    explicit RealizedVolatilityTracker(std::uint64_t window_ns);

    // Call once per observed midprice change (e.g. from a tick hook, or
    // after any book-mutating event). timestamp_ns must be
    // non-decreasing across calls -- this is simulated/event time, not
    // wall-clock time, same discipline as the rest of this project.
    void sample(std::uint64_t timestamp_ns, double midprice);

    // Sum of squared log-returns within the window -- NOT annualized
    // and NOT a standard deviation; callers wanting an annualized vol
    // figure apply that scaling themselves, since it depends on the
    // window's time unit in a way this class has no opinion about.
    double currentRealizedVol() const;

private:
    std::uint64_t window_ns_;
    // (timestamp_ns, squared log-return) for each return WITHIN the
    // window -- NOT raw price samples; the running sum below is these
    // entries' total, maintained incrementally as entries enter and age
    // out, so currentRealizedVol() is O(1) rather than re-summing.
    std::deque<std::pair<std::uint64_t, double>> windowed_returns_;
    double running_sum_ = 0.0;

    // The single most recent midprice observed, needed to compute the
    // NEXT log-return regardless of whether it's still inside the
    // window -- this is tracked separately from windowed_returns_
    // because computing a return always needs the immediately-prior
    // price, even once that prior observation itself has aged out of
    // the reported window.
    bool has_last_sample_ = false;
    double last_midprice_ = 0.0;
};

}  // namespace lob
