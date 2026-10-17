#pragma once

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "lob/order.hpp"
#include "lob/order_book.hpp"

namespace lob {

// All order-type semantics (Market/Limit/IOC/FOK), trade generation, and
// cancel/replace live here. OrderBook itself has no notion of these -- see
// order_book.hpp. The engine is single-threaded and processes events
// strictly sequentially; "concurrent" access from multiple callers is out
// of scope for Phase 1 (Phase 0 §4.1).
class MatchingEngine {
public:
    ExecutionReport submit(Order order);

    ExecutionReport cancel(OrderId id);

    struct ReplaceRequest {
        OrderId id;
        std::optional<Price> new_price;   // nullopt = unchanged
        std::optional<Quantity> new_qty;  // nullopt = unchanged
    };
    ExecutionReport replace(const ReplaceRequest& req);

    // Directly reduces a RESTING order (by id) by `qty`, producing a
    // Trade record, WITHOUT running submit()'s crossing/matching logic --
    // there is no aggressor order to match against. This exists for
    // market-data replay (Phase 3): a real feed reports "resting order X
    // was hit for qty Y," not an aggressor order you could resubmit (the
    // aggressor is typically anonymized or absent from the public feed
    // entirely). `price` is the execution price to record on the Trade;
    // it is NOT required to equal the resting order's own price (a
    // replayed feed's reported execution price is authoritative for the
    // log, even if it differs from this book's reconstructed state --
    // that mismatch is itself a signal worth being able to observe, not
    // something to silently override).
    //
    // Fails (returns nullopt) if `resting_id` isn't currently resting, or
    // if `qty` is non-positive or exceeds its remaining_qty -- both are
    // data-quality problems in the replay source, never silently
    // clamped. Trade::aggressor_id is 0 for every trade produced this
    // way: a reserved sentinel meaning "no aggressor order exists in
    // this replay" (0 is never a valid caller-assigned OrderId
    // elsewhere in this engine).
    std::optional<Trade> applyExternalTrade(OrderId resting_id, Quantity qty, Price price);

    // --- read-only analytics, delegating to the OrderBook ---
    std::optional<Price> bestBid() const { return book_.bestBid(); }
    std::optional<Price> bestAsk() const { return book_.bestAsk(); }
    std::optional<Price> spread() const;
    std::optional<double> midprice() const;
    std::optional<double> microprice() const;
    double orderImbalance(std::size_t levels = 1) const;
    std::vector<std::pair<Price, Quantity>> depth(Side side, std::size_t levels) const {
        return book_.depth(side, levels);
    }
    std::optional<std::size_t> queuePosition(OrderId id) const { return book_.queuePosition(id); }

    const OrderBook& book() const { return book_; }  // for test/invariant inspection

private:
    OrderBook book_;
    Sequence sequence_counter_ = 0;
    std::uint64_t trade_id_counter_ = 0;

    // Walks the opposing side consuming liquidity for an aggressive order.
    // Mutates book_ via OrderBook::reduceQuantity/remove (id-based, never
    // via a raw pointer into the book -- see DESIGN_DECISIONS.md). Returns
    // the trades generated; mutates `order.remaining_qty` in place.
    std::vector<Trade> matchAggressive(Order& order);

    // Dry-run only: total quantity fillable within order's price limit,
    // without mutating the book. Used by FOK to decide fill-or-kill before
    // committing to any state change (Phase 0 §4.5 atomicity requirement).
    Quantity aggregateFillable(const Order& order) const;

    static RejectReason validate(const Order& order);

    static bool crosses(const Order& aggressor, Price resting_price);
};

}  // namespace lob
