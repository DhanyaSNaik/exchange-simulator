#pragma once
// A deliberately naive, obviously-correct reference matching engine.
// O(n) linear scan over a flat vector, no indexing, no price levels, no
// cleverness of any kind -- the entire point is that its correctness is
// easy to eyeball, so that cross-checking the "real" engine's trade output
// against this one's catches logic bugs that invariant checks alone
// cannot (an internally-consistent-but-wrong fill is possible; invariants
// describe properties of A correct state, not whether it's THE SAME state
// a correct implementation reaches). Phase 0 §5 / this project's testing
// plan.
//
// Not intended to be fast, used only in tests, scales are kept small
// deliberately (O(n) per event is fine at the differential-test scale;
// this is never run at benchmark scale).

#include <algorithm>
#include <optional>
#include <vector>

#include "lob/order.hpp"

namespace lob_ref {

using namespace lob;

class ReferenceEngine {
public:
    ExecutionReport submit(Order order) {
        order.sequence = ++sequence_counter_;

        if (order.remaining_qty <= 0) {
            return ExecutionReport{order.id, OrderStatus::Rejected,     0, 0,
                                   {},       RejectReason::ZeroQuantity};
        }
        if (order.type != OrderType::Market && order.price <= 0) {
            return ExecutionReport{order.id, OrderStatus::Rejected,     0, 0,
                                   {},       RejectReason::InvalidPrice};
        }
        if (findIndex(order.id) != -1) {
            return ExecutionReport{order.id, OrderStatus::Rejected,    0, 0,
                                   {},       RejectReason::DuplicateId};
        }

        if (order.type == OrderType::FOK) {
            if (aggregateFillable(order) < order.remaining_qty) {
                return ExecutionReport{order.id, OrderStatus::Rejected,      0, 0,
                                       {},       RejectReason::FOKUnfillable};
            }
        }

        std::vector<Trade> trades = matchAgainstAll(order);

        ExecutionReport report;
        report.order_id = order.id;
        report.filled_qty = order.original_qty - order.remaining_qty;
        report.trades = std::move(trades);

        if (order.remaining_qty == 0) {
            report.status = OrderStatus::Filled;
            report.remaining_qty = 0;
        } else if (order.type == OrderType::Limit) {
            order.status =
                (report.filled_qty > 0) ? OrderStatus::PartiallyFilled : OrderStatus::New;
            orders_.push_back(order);
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
        int idx = findIndex(id);
        if (idx == -1) {
            return ExecutionReport{id, OrderStatus::Rejected, 0, 0, {}, RejectReason::UnknownOrder};
        }
        Order removed = orders_[static_cast<std::size_t>(idx)];
        orders_.erase(orders_.begin() + idx);

        ExecutionReport report;
        report.order_id = id;
        report.status = OrderStatus::Cancelled;
        report.filled_qty = removed.original_qty - removed.remaining_qty;
        report.remaining_qty = 0;
        return report;
    }

    struct ReplaceRequest {
        OrderId id;
        std::optional<Price> new_price;
        std::optional<Quantity> new_qty;
    };

    ExecutionReport replace(const ReplaceRequest& req) {
        int idx = findIndex(req.id);
        if (idx == -1) {
            return ExecutionReport{req.id, OrderStatus::Rejected,     0, 0,
                                   {},     RejectReason::UnknownOrder};
        }
        if (!req.new_price && !req.new_qty) {
            const Order& cur = orders_[static_cast<std::size_t>(idx)];
            return ExecutionReport{req.id, cur.status,        0, cur.remaining_qty,
                                   {},     RejectReason::None};
        }

        Order cur = orders_[static_cast<std::size_t>(idx)];
        bool price_changed = req.new_price && *req.new_price != cur.price;
        bool strict_decrease = req.new_qty && *req.new_qty > 0 && *req.new_qty < cur.remaining_qty;

        if (!price_changed && strict_decrease) {
            orders_[static_cast<std::size_t>(idx)].remaining_qty = *req.new_qty;
            orders_[static_cast<std::size_t>(idx)].status = OrderStatus::PartiallyFilled;
            ExecutionReport report;
            report.order_id = req.id;
            report.status = OrderStatus::PartiallyFilled;
            report.filled_qty = cur.original_qty - *req.new_qty;
            report.remaining_qty = *req.new_qty;
            return report;
        }

        orders_.erase(orders_.begin() + idx);
        Order new_order(req.id, cur.side, cur.type, req.new_price.value_or(cur.price),
                        req.new_qty.value_or(cur.remaining_qty));
        return submit(new_order);
    }

private:
    std::vector<Order> orders_;
    Sequence sequence_counter_ = 0;
    std::uint64_t trade_id_counter_ = 0;

    int findIndex(OrderId id) const {
        for (std::size_t i = 0; i < orders_.size(); ++i) {
            if (orders_[i].id == id) return static_cast<int>(i);
        }
        return -1;
    }

    static bool crosses(const Order& aggressor, Price resting_price) {
        if (aggressor.type == OrderType::Market) return true;
        return aggressor.side == Side::Buy ? aggressor.price >= resting_price
                                           : aggressor.price <= resting_price;
    }

    // Finds the single best (price-time priority) resting order on the
    // opposite side that the aggressor crosses with, by brute-force
    // linear scan of the ENTIRE order list every single call. This is
    // exactly the inefficiency a real implementation avoids -- which is
    // the point: it is trivially, obviously correct.
    int findBestOpposing(const Order& aggressor) const {
        const Side opp = (aggressor.side == Side::Buy) ? Side::Sell : Side::Buy;
        int best_idx = -1;
        for (std::size_t i = 0; i < orders_.size(); ++i) {
            const Order& o = orders_[i];
            if (o.side != opp) continue;
            if (!crosses(aggressor, o.price)) continue;
            if (best_idx == -1) {
                best_idx = static_cast<int>(i);
                continue;
            }
            const Order& best = orders_[static_cast<std::size_t>(best_idx)];
            const bool better_price =
                (opp == Side::Sell) ? (o.price < best.price) : (o.price > best.price);
            const bool same_price_earlier = (o.price == best.price) && (o.sequence < best.sequence);
            if (better_price || same_price_earlier) best_idx = static_cast<int>(i);
        }
        return best_idx;
    }

    Quantity aggregateFillable(const Order& order) const {
        const Side opp = (order.side == Side::Buy) ? Side::Sell : Side::Buy;
        Quantity total = 0;
        for (const auto& o : orders_) {
            if (o.side != opp) continue;
            if (!crosses(order, o.price)) continue;
            total += o.remaining_qty;
        }
        return total;
    }

    std::vector<Trade> matchAggressiveStep(Order& order) {
        std::vector<Trade> trades;
        while (order.remaining_qty > 0) {
            int idx = findBestOpposing(order);
            if (idx == -1) break;
            Order& resting = orders_[static_cast<std::size_t>(idx)];
            Quantity trade_qty = std::min(order.remaining_qty, resting.remaining_qty);

            trades.push_back(Trade{++trade_id_counter_, order.id, resting.id, resting.price,
                                   trade_qty, order.sequence, order.side});
            order.remaining_qty -= trade_qty;
            resting.remaining_qty -= trade_qty;
            if (resting.remaining_qty == 0) {
                orders_.erase(orders_.begin() + idx);
            }
        }
        return trades;
    }

    std::vector<Trade> matchAgainstAll(Order& order) { return matchAggressiveStep(order); }
};

}  // namespace lob_ref
