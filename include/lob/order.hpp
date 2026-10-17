#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace lob {

using Price = std::int64_t;  // integer ticks. Never a floating type:
                             // FP equality is unsafe for price-level
                             // keys and tie-breaking (Phase 0 note).
using Quantity = std::int64_t;
using OrderId = std::uint64_t;   // caller-assigned, validated for duplicates.
using Sequence = std::uint64_t;  // engine-assigned on submit(), monotonic.

enum class Side { Buy, Sell };

enum class OrderType { Market, Limit, IOC, FOK };

enum class OrderStatus { New, PartiallyFilled, Filled, Cancelled, Rejected };

enum class RejectReason {
    None,
    ZeroQuantity,
    InvalidPrice,
    DuplicateId,
    FOKUnfillable,
    UnknownOrder,
    AlreadyFinal
};

struct Order {
    OrderId id;
    Side side;
    OrderType type;
    Price price;  // meaningless for Market; validate() ignores it.
    Quantity original_qty;
    Quantity remaining_qty;
    Sequence sequence = 0;  // 0 = "not yet assigned by the engine".
    OrderStatus status = OrderStatus::New;

    Order(OrderId id_, Side side_, OrderType type_, Price price_, Quantity qty_)
        : id(id_),
          side(side_),
          type(type_),
          price(price_),
          original_qty(qty_),
          remaining_qty(qty_) {}
};

struct Trade {
    std::uint64_t trade_id;
    OrderId aggressor_id;
    OrderId resting_id;
    Price price;  // always the resting order's price.
    Quantity quantity;
    Sequence sequence;  // sequence of the aggressing event.
};

struct ExecutionReport {
    OrderId order_id;
    OrderStatus status;
    Quantity filled_qty = 0;
    Quantity remaining_qty = 0;
    std::vector<Trade> trades;
    RejectReason reject_reason = RejectReason::None;

    // The sequence number assigned by submit() to the event that produced
    // this report. 0 means "no new sequence was consumed" -- cancel()
    // never consumes one, and an in-place quantity-decrease replace()
    // doesn't either (same resting order, same sequence it already had).
    // Only sequence-bearing reports (every submit() call, including the
    // internal resubmit inside a priority-forfeiting replace) are
    // meaningful for the strict-monotonicity check in TESTING.md.
    Sequence sequence = 0;
};

}  // namespace lob
