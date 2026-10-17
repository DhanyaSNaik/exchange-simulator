#include "lob/matching_engine.hpp"

#include <algorithm>

namespace lob {

RejectReason MatchingEngine::validate(const Order& order) {
    if (order.remaining_qty <= 0) return RejectReason::ZeroQuantity;
    if (order.type != OrderType::Market && order.price <= 0) return RejectReason::InvalidPrice;
    return RejectReason::None;
}

bool MatchingEngine::crosses(const Order& aggressor, Price resting_price) {
    if (aggressor.type == OrderType::Market) return true;
    return aggressor.side == Side::Buy ? aggressor.price >= resting_price
                                        : aggressor.price <= resting_price;
}

Quantity MatchingEngine::aggregateFillable(const Order& order) const {
    const Side opp = (order.side == Side::Buy) ? Side::Sell : Side::Buy;
    const auto levels = book_.depth(opp, book_.levelCount(opp));

    Quantity total = 0;
    for (const auto& [price, qty] : levels) {
        if (!crosses(order, price)) break;  // price-sorted, so we can stop early
        total += qty;
        if (total >= order.remaining_qty) break;  // enough found, no need to keep summing
    }
    return total;
}

std::vector<Trade> MatchingEngine::matchAggressive(Order& order) {
    std::vector<Trade> trades;
    const Side opp = (order.side == Side::Buy) ? Side::Sell : Side::Buy;

    while (order.remaining_qty > 0) {
        std::optional<Order> front = book_.frontOfBook(opp);  // a COPY, see header note
        if (!front) break;
        if (!crosses(order, front->price)) break;

        const Quantity trade_qty = std::min(order.remaining_qty, front->remaining_qty);

        trades.push_back(Trade{++trade_id_counter_, order.id, front->id, front->price,
                                trade_qty, order.sequence});

        order.remaining_qty -= trade_qty;
        // Mutate the book by id, never through `front` (which is a copy
        // and, even if it weren't, would risk the dangling-reference
        // hazard discussed in DESIGN_DECISIONS.md).
        book_.reduceQuantity(front->id, trade_qty);
    }
    return trades;
}

ExecutionReport MatchingEngine::submit(Order order) {
    order.sequence = ++sequence_counter_;  // every event consumes a sequence
                                            // number, even a rejected one --
                                            // monotonicity, not "only valid
                                            // events count" (Phase 0 §3.10).

    RejectReason reason = validate(order);
    if (reason == RejectReason::None && book_.contains(order.id)) {
        reason = RejectReason::DuplicateId;
    }
    if (reason != RejectReason::None) {
        return ExecutionReport{order.id, OrderStatus::Rejected, 0, 0, {}, reason, order.sequence};
    }

    if (order.type == OrderType::FOK) {
        // Two-pass: check fillability WITHOUT mutating, then only commit
        // if the whole order can be filled. This is what makes FOK
        // atomicity (Phase 0 §3.11, §4.5) correct across multiple levels --
        // a walk-and-mutate-as-you-go approach would leave a partial fill
        // committed if insufficient liquidity were discovered partway
        // through a multi-level sweep.
        if (aggregateFillable(order) < order.remaining_qty) {
            return ExecutionReport{order.id, OrderStatus::Rejected, 0, 0, {},
                                    RejectReason::FOKUnfillable, order.sequence};
        }
    }

    std::vector<Trade> trades = matchAggressive(order);

    ExecutionReport report;
    report.order_id = order.id;
    report.sequence = order.sequence;  // every submit() call consumes exactly one
    report.filled_qty = order.original_qty - order.remaining_qty;
    report.trades = std::move(trades);

    if (order.remaining_qty == 0) {
        report.status = OrderStatus::Filled;
        report.remaining_qty = 0;
    } else if (order.type == OrderType::Limit) {
        // Only Limit orders ever rest. Market/IOC/FOK never do (FOK should
        // already be fully filled by the pre-check above; the branch below
        // still defensively discards rather than resting if that ever
        // fails to hold, rather than silently violating invariant §3.12).
        order.status =
            (report.filled_qty > 0) ? OrderStatus::PartiallyFilled : OrderStatus::New;
        book_.insert(order);
        report.status = order.status;
        report.remaining_qty = order.remaining_qty;  // now resting in the book
    } else {
        report.status =
            (report.filled_qty > 0) ? OrderStatus::PartiallyFilled : OrderStatus::Cancelled;
        report.remaining_qty = 0;  // never rests -- any leftover was killed, not queued
    }

    return report;
}

ExecutionReport MatchingEngine::cancel(OrderId id) {
    std::optional<Order> removed = book_.remove(id);
    if (!removed) {
        return ExecutionReport{id, OrderStatus::Rejected, 0, 0, {}, RejectReason::UnknownOrder};
    }
    ExecutionReport report;
    report.order_id = id;
    report.status = OrderStatus::Cancelled;
    report.filled_qty = removed->original_qty - removed->remaining_qty;
    report.remaining_qty = 0;
    return report;
}

ExecutionReport MatchingEngine::replace(const ReplaceRequest& req) {
    if (!req.new_price.has_value() && !req.new_qty.has_value()) {
        // No-op replace request: nothing to do, nothing forfeited.
        std::optional<Order> current = book_.get(req.id);
        if (!current) {
            return ExecutionReport{req.id, OrderStatus::Rejected, 0, 0, {},
                                    RejectReason::UnknownOrder};
        }
        return ExecutionReport{req.id, current->status, 0, current->remaining_qty, {},
                                RejectReason::None};
    }

    std::optional<Order> current = book_.get(req.id);
    if (!current) {
        return ExecutionReport{req.id, OrderStatus::Rejected, 0, 0, {},
                                RejectReason::UnknownOrder};
    }

    const bool price_changed = req.new_price.has_value() && *req.new_price != current->price;
    const bool is_strict_decrease =
        req.new_qty.has_value() && *req.new_qty > 0 && *req.new_qty < current->remaining_qty;

    if (!price_changed && is_strict_decrease) {
        // Quantity-decrease-only at the same price: preserves time
        // priority (Phase 0 decision, confirmed). In-place mutation, no
        // new sequence number, no change in FIFO position.
        book_.decreaseQuantityInPlace(req.id, *req.new_qty);
        ExecutionReport report;
        report.order_id = req.id;
        report.status = OrderStatus::PartiallyFilled;
        report.filled_qty = current->original_qty - *req.new_qty;
        report.remaining_qty = *req.new_qty;
        return report;
    }

    // Any price change, or any quantity INCREASE (or a same-side no-op
    // quantity value, which decreaseQuantityInPlace would reject anyway):
    // forfeits priority. Modeled internally as cancel + a fresh submit,
    // which also correctly handles the case where the new price now
    // crosses the book and the "replaced" order should trade immediately
    // rather than simply re-resting.
    std::optional<Order> removed = book_.remove(req.id);
    // removed is guaranteed present: `current` was read moments earlier
    // and this engine processes events strictly sequentially (no
    // concurrent mutation is possible between the two lookups).
    Order new_order(req.id, removed->side, removed->type,
                     req.new_price.value_or(removed->price),
                     req.new_qty.value_or(removed->remaining_qty));
    return submit(new_order);
}

std::optional<Price> MatchingEngine::spread() const {
    auto b = book_.bestBid();
    auto a = book_.bestAsk();
    if (!b || !a) return std::nullopt;
    return *a - *b;
}

std::optional<double> MatchingEngine::midprice() const {
    auto b = book_.bestBid();
    auto a = book_.bestAsk();
    if (!b || !a) return std::nullopt;
    return (static_cast<double>(*b) + static_cast<double>(*a)) / 2.0;
}

std::optional<double> MatchingEngine::microprice() const {
    auto bidLevel = book_.depth(Side::Buy, 1);
    auto askLevel = book_.depth(Side::Sell, 1);
    if (bidLevel.empty() || askLevel.empty()) return std::nullopt;

    const double bid_price = static_cast<double>(bidLevel[0].first);
    const double bid_size  = static_cast<double>(bidLevel[0].second);
    const double ask_price = static_cast<double>(askLevel[0].first);
    const double ask_size  = static_cast<double>(askLevel[0].second);

    if (bid_size + ask_size == 0.0) return std::nullopt;

    // Cross-weighted: ask SIZE weights the BID price and vice versa. Easy
    // to get backwards -- see DESIGN_DECISIONS.md / Phase-1 API notes.
    return (bid_price * ask_size + ask_price * bid_size) / (bid_size + ask_size);
}

double MatchingEngine::orderImbalance(std::size_t levels) const {
    auto bidLevels = book_.depth(Side::Buy, levels);
    auto askLevels = book_.depth(Side::Sell, levels);

    Quantity bid_qty = 0, ask_qty = 0;
    for (auto& [p, q] : bidLevels) bid_qty += q;
    for (auto& [p, q] : askLevels) ask_qty += q;

    if (bid_qty + ask_qty == 0) return 0.0;
    return static_cast<double>(bid_qty - ask_qty) / static_cast<double>(bid_qty + ask_qty);
}

}  // namespace lob
