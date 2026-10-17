#pragma once
// Phase 3: market data replay event model. See DESIGN_DECISIONS.md for
// why Trade is its own event kind rather than a NewOrder that happens to
// match -- a real feed reports "resting order X was hit for qty Y," not
// an aggressor order you could resubmit through MatchingEngine::submit().

#include <cstdint>
#include <optional>
#include <string>

#include "lob/order.hpp"

namespace lob {

enum class EventKind { NewOrder, Cancel, Replace, Trade, ClockTick };

struct OrderFlowEvent {
    std::uint64_t timestamp_ns = 0;
    // Tie-break for events sharing a timestamp: assigned by the reader
    // as the event's position in the source file/stream, monotonically.
    // Same discipline as MatchingEngine's own `sequence` field, applied
    // one layer up -- deterministic ordering requires SOME total order,
    // and timestamp alone doesn't guarantee one.
    std::uint64_t file_order = 0;
    EventKind kind = EventKind::ClockTick;

    // NewOrder fields
    OrderId id = 0;
    Side side = Side::Buy;
    OrderType type = OrderType::Limit;
    Price price = 0;
    Quantity qty = 0;

    // Cancel: uses `id` above (the order to cancel).

    // Replace: uses `id` above (the order to replace), plus:
    std::optional<Price> replace_price;
    std::optional<Quantity> replace_qty;

    // Trade: `ref_id` is the RESTING order being hit (not `id` -- kept
    // separate so a malformed row that fills in the wrong column is a
    // clear data-quality error rather than a silent misread), `price`
    // and `qty` above double as the execution's price/quantity.
    OrderId ref_id = 0;

    // ClockTick: no payload; only timestamp_ns matters.
};

// A human-readable label, used in error messages and logs -- kept
// separate from the enum itself so adding a new kind later doesn't
// require updating two switch statements in lockstep by hand everywhere
// this is needed.
inline std::string toString(EventKind kind) {
    switch (kind) {
        case EventKind::NewOrder:
            return "NEW";
        case EventKind::Cancel:
            return "CANCEL";
        case EventKind::Replace:
            return "REPLACE";
        case EventKind::Trade:
            return "TRADE";
        case EventKind::ClockTick:
            return "TICK";
    }
    return "UNKNOWN";
}

}  // namespace lob
