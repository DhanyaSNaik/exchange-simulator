#pragma once
// Exact algorithmic duplicate of lob::MatchingEngine, except it drives
// FlatVectorOrderBook instead of lob::OrderBook. Deliberately NOT done by
// templatizing the production MatchingEngine on a book type -- that would
// add template complexity to the Phase 1 library headers to serve a
// benchmark-only comparison, before there's any evidence the swap is even
// worth making permanent. Duplicating ~150 lines into a bench-only header
// has a smaller blast radius and is easy to delete outright if Option B
// loses the comparison. See DESIGN_DECISIONS.md.

#include <algorithm>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "flat_vector_order_book.hpp"
#include "lob/order.hpp"

namespace lob_bench {

class FlatMatchingEngine {
public:
    ExecutionReport submit(Order order) {
        order.sequence = ++sequence_counter_;

        RejectReason reason = validate(order);
        if (reason == RejectReason::None && book_.contains(order.id)) {
            reason = RejectReason::DuplicateId;
        }
        if (reason != RejectReason::None) {
            return ExecutionReport{order.id, OrderStatus::Rejected, 0, 0, {}, reason,
                                    order.sequence};
        }

        if (order.type == OrderType::FOK) {
            if (aggregateFillable(order) < order.remaining_qty) {
                return ExecutionReport{order.id, OrderStatus::Rejected, 0, 0, {},
                                        RejectReason::FOKUnfillable, order.sequence};
            }
        }

        std::vector<Trade> trades = matchAggressive(order);

        ExecutionReport report;
        report.order_id = order.id;
        report.sequence = order.sequence;
        report.filled_qty = order.original_qty - order.remaining_qty;
        report.trades = std::move(trades);

        if (order.remaining_qty == 0) {
            report.status = OrderStatus::Filled;
            report.remaining_qty = 0;
        } else if (order.type == OrderType::Limit) {
            order.status =
                (report.filled_qty > 0) ? OrderStatus::PartiallyFilled : OrderStatus::New;
            book_.insert(order);
            report.status = order.status;
            report.remaining_qty = order.remaining_qty;
        } else {
            report.status =
                (report.filled_qty > 0) ? OrderStatus::PartiallyFilled : OrderStatus::Cancelled;
            report.remaining_qty = 0;
        }
        return report;
    }

    ExecutionReport cancel(OrderId id) {
        auto removed = book_.remove(id);
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

    struct ReplaceRequest {
        OrderId id;
        std::optional<Price> new_price;
        std::optional<Quantity> new_qty;
    };

    ExecutionReport replace(const ReplaceRequest& req) {
        if (!req.new_price && !req.new_qty) {
            auto current = book_.get(req.id);
            if (!current) {
                return ExecutionReport{req.id, OrderStatus::Rejected, 0, 0, {},
                                        RejectReason::UnknownOrder};
            }
            return ExecutionReport{req.id, current->status, 0, current->remaining_qty, {},
                                    RejectReason::None};
        }

        auto current = book_.get(req.id);
        if (!current) {
            return ExecutionReport{req.id, OrderStatus::Rejected, 0, 0, {},
                                    RejectReason::UnknownOrder};
        }

        const bool price_changed = req.new_price && *req.new_price != current->price;
        const bool strict_decrease =
            req.new_qty && *req.new_qty > 0 && *req.new_qty < current->remaining_qty;

        if (!price_changed && strict_decrease) {
            book_.decreaseQuantityInPlace(req.id, *req.new_qty);
            ExecutionReport report;
            report.order_id = req.id;
            report.status = OrderStatus::PartiallyFilled;
            report.filled_qty = current->original_qty - *req.new_qty;
            report.remaining_qty = *req.new_qty;
            return report;
        }

        auto removed = book_.remove(req.id);
        Order new_order(req.id, removed->side, removed->type,
                         req.new_price.value_or(removed->price),
                         req.new_qty.value_or(removed->remaining_qty));
        return submit(new_order);
    }

    std::optional<Price> bestBid() const { return book_.bestBid(); }
    std::optional<Price> bestAsk() const { return book_.bestAsk(); }
    const FlatVectorOrderBook& book() const { return book_; }

private:
    FlatVectorOrderBook book_;
    Sequence sequence_counter_ = 0;
    std::uint64_t trade_id_counter_ = 0;

    static RejectReason validate(const Order& order) {
        if (order.remaining_qty <= 0) return RejectReason::ZeroQuantity;
        if (order.type != OrderType::Market && order.price <= 0) return RejectReason::InvalidPrice;
        return RejectReason::None;
    }

    static bool crosses(const Order& aggressor, Price resting_price) {
        if (aggressor.type == OrderType::Market) return true;
        return aggressor.side == Side::Buy ? aggressor.price >= resting_price
                                            : aggressor.price <= resting_price;
    }

    Quantity aggregateFillable(const Order& order) const {
        const Side opp = (order.side == Side::Buy) ? Side::Sell : Side::Buy;
        const auto levels = book_.depth(opp, book_.levelCount(opp));
        Quantity total = 0;
        for (const auto& [price, qty] : levels) {
            if (!crosses(order, price)) break;
            total += qty;
            if (total >= order.remaining_qty) break;
        }
        return total;
    }

    std::vector<Trade> matchAggressive(Order& order) {
        std::vector<Trade> trades;
        const Side opp = (order.side == Side::Buy) ? Side::Sell : Side::Buy;

        while (order.remaining_qty > 0) {
            std::optional<Order> front = book_.frontOfBook(opp);
            if (!front) break;
            if (!crosses(order, front->price)) break;

            const Quantity trade_qty = std::min(order.remaining_qty, front->remaining_qty);
            trades.push_back(Trade{++trade_id_counter_, order.id, front->id, front->price,
                                    trade_qty, order.sequence});
            order.remaining_qty -= trade_qty;
            book_.reduceQuantity(front->id, trade_qty);
        }
        return trades;
    }
};

}  // namespace lob_bench
