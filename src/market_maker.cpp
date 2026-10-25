#include "lob/market_maker.hpp"

#include <algorithm>
#include <cmath>

namespace lob {

MarketMakerAgent::MarketMakerAgent(MatchingEngine& engine, MarketMakerConfig config,
                                   OrderId first_order_id)
    : engine_(engine), config_(std::move(config)), next_order_id_(first_order_id) {}

double MarketMakerAgent::unrealizedPnl(double midprice) const {
    // inventory * (midprice - avg_cost) is correct for BOTH signs of
    // inventory without a branch: a short position (inventory < 0)
    // profits when midprice falls below avg_cost, and
    // inventory * (midprice - avg_cost) with inventory negative and
    // (midprice - avg_cost) negative is already positive in that case.
    if (state_.inventory == 0) return 0.0;
    return static_cast<double>(state_.inventory) * (midprice - state_.avg_cost);
}

double MarketMakerAgent::totalPnl(double midprice) const {
    return state_.realized_pnl + unrealizedPnl(midprice);
}

void MarketMakerAgent::applyPnlUpdate(Side side_filled, Price price, Quantity qty) {
    // signed_qty: the signed change to inventory this fill represents.
    // Buying (side_filled == Buy) increases inventory; selling decreases it.
    const Quantity signed_qty = (side_filled == Side::Buy) ? qty : -qty;
    const double fill_price = static_cast<double>(price);

    const bool opening_or_adding =
        state_.inventory == 0 ||
        (state_.inventory > 0) == (signed_qty > 0);  // same sign -> adding, not reducing

    if (opening_or_adding) {
        // Weighted-average cost basis update.
        const double total_cost_before =
            state_.avg_cost * static_cast<double>(std::llabs(state_.inventory));
        const double total_cost_after = total_cost_before + fill_price * static_cast<double>(qty);
        state_.inventory += signed_qty;
        state_.avg_cost = (state_.inventory != 0)
                              ? total_cost_after / static_cast<double>(std::llabs(state_.inventory))
                              : 0.0;
    } else {
        // Reducing (and possibly flipping through zero) an existing position.
        const Quantity closing_qty =
            std::min(qty, static_cast<Quantity>(std::llabs(state_.inventory)));
        const double pnl_per_unit = (state_.inventory > 0) ? (fill_price - state_.avg_cost)
                                                           : (state_.avg_cost - fill_price);
        state_.realized_pnl += pnl_per_unit * static_cast<double>(closing_qty);

        const Quantity flip_through_qty = qty - closing_qty;
        state_.inventory += signed_qty;

        if (flip_through_qty > 0) {
            // Flipped through zero: the portion beyond what closed the
            // old position opens a brand-new position at this fill's price.
            state_.avg_cost = fill_price;
        } else if (state_.inventory == 0) {
            state_.avg_cost = 0.0;
        }
        // else: still the same sign, just smaller -- avg_cost unchanged,
        // which is exactly the point of the average-cost method (a
        // partial close doesn't change the cost basis of what remains).
    }
}

void MarketMakerAgent::updateAdverseSelection(Side side_filled, std::uint64_t timestamp_ns) {
    recent_fills_.push_back({side_filled, timestamp_ns});
    // Trim anything outside the run-over window relative to the newest fill.
    while (!recent_fills_.empty() &&
           timestamp_ns - recent_fills_.front().second > config_.run_over_window_ns) {
        recent_fills_.pop_front();
    }
    if (static_cast<int>(recent_fills_.size()) >= config_.run_over_fill_threshold) {
        bool all_same_side = true;
        // Check the most recent `run_over_fill_threshold` fills specifically
        // -- not the whole (already window-trimmed) deque, which could be
        // longer than the threshold if fills arrived in rapid succession.
        auto it = recent_fills_.rbegin();
        Side first_side = it->first;
        for (int i = 0; i < config_.run_over_fill_threshold; ++i, ++it) {
            if (it->first != first_side) {
                all_same_side = false;
                break;
            }
        }
        if (all_same_side) {
            state_.widened = true;
            state_.widen_until_ns = timestamp_ns + config_.widen_duration_ns;
        }
    }
}

void MarketMakerAgent::recordFill(Side side_filled, Price price, Quantity qty,
                                  std::uint64_t timestamp_ns) {
    applyPnlUpdate(side_filled, price, qty);
    updateAdverseSelection(side_filled, timestamp_ns);
}

void MarketMakerAgent::onFill(Side side_filled, Price price, Quantity qty,
                              std::uint64_t timestamp_ns) {
    recordFill(side_filled, price, qty, timestamp_ns);
}

void MarketMakerAgent::onTick(std::uint64_t timestamp_ns) {
    if (state_.widened && timestamp_ns >= state_.widen_until_ns) {
        state_.widened = false;
    }

    auto mid = engine_.midprice();
    if (!mid) return;  // no reference price available -- leave existing quotes untouched

    const double skew = static_cast<double>(state_.inventory) * config_.inventory_skew_factor;
    const double reference = *mid - skew;
    const double half_spread_d = static_cast<double>(config_.base_half_spread) *
                                 (state_.widened ? config_.adverse_selection_widen_factor : 1.0);

    const Price reference_ticks = static_cast<Price>(std::llround(reference));
    const Price half_spread_ticks = static_cast<Price>(std::llround(half_spread_d));
    const Price desired_bid = reference_ticks - half_spread_ticks;
    const Price desired_ask = reference_ticks + half_spread_ticks;

    const bool bid_allowed = state_.inventory < config_.max_inventory;
    if (bid_allowed) {
        if (state_.bid_order_id != 0) {
            engine_.cancel(state_.bid_order_id);  // ignore result: already-gone is fine
            state_.bid_order_id = 0;
        }
        OrderId id = next_order_id_++;
        Order order(id, Side::Buy, OrderType::Limit, desired_bid, config_.quote_size);
        ExecutionReport report = engine_.submit(order);
        if (report.status != OrderStatus::Rejected) {
            state_.bid_order_id = id;
            for (const auto& t : report.trades) {
                recordFill(Side::Buy, t.price, t.quantity, timestamp_ns);
            }
        }
    } else if (state_.bid_order_id != 0) {
        engine_.cancel(state_.bid_order_id);
        state_.bid_order_id = 0;
    }

    const bool ask_allowed = state_.inventory > -config_.max_inventory;
    if (ask_allowed) {
        if (state_.ask_order_id != 0) {
            engine_.cancel(state_.ask_order_id);
            state_.ask_order_id = 0;
        }
        OrderId id = next_order_id_++;
        Order order(id, Side::Sell, OrderType::Limit, desired_ask, config_.quote_size);
        ExecutionReport report = engine_.submit(order);
        if (report.status != OrderStatus::Rejected) {
            state_.ask_order_id = id;
            for (const auto& t : report.trades) {
                recordFill(Side::Sell, t.price, t.quantity, timestamp_ns);
            }
        }
    } else if (state_.ask_order_id != 0) {
        engine_.cancel(state_.ask_order_id);
        state_.ask_order_id = 0;
    }
}

}  // namespace lob
