#pragma once

#include <cstddef>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "lob/order.hpp"

namespace lob {

// One resting price level. std::list (not std::deque) for `orders`: see
// DESIGN_DECISIONS.md — list iterators survive erasure of *other* elements,
// which is required for the O(1)-average cancel-by-stored-iterator design.
// deque erase invalidates iterators/references at or after the erase point,
// which would silently corrupt the index below.
struct PriceLevel {
    Price            price = 0;
    std::list<Order> orders;
    Quantity         total_qty = 0;
};

// Pure storage: price-time-ordered resting orders. Knows nothing about
// order *types* (Market/IOC/FOK) or matching — that is MatchingEngine's job.
// Keeping this boundary strict is what lets OrderBook be unit-tested (and
// later re-implemented, per Phase 0 §2 Option B/C) in complete isolation
// from matching semantics.
class OrderBook {
public:
    // Inserts a resting order. False (no-op) if `order.id` already present.
    bool insert(const Order& order);

    // Removes by id. Returns the removed order (its state at removal time),
    // or nullopt if not found.
    std::optional<Order> remove(OrderId id);

    // Reduces a resting order's remaining_qty by `qty`, used mid-match by
    // the matching engine. If qty consumes the order fully, it is erased
    // (and the level is cleaned up if now empty). Returns false if the id
    // is not found or qty > current remaining_qty (a caller bug, not a
    // normal runtime path — the matching loop must never call this with
    // more than it just observed via frontOfBook()).
    bool reduceQuantity(OrderId id, Quantity qty);

    // In-place quantity DECREASE preserving time priority (replace,
    // Phase 0 decision). False if not found, or new_qty is not a strict
    // decrease of the current remaining_qty (0 < new_qty < current).
    bool decreaseQuantityInPlace(OrderId id, Quantity new_qty);

    bool contains(OrderId id) const;

    // Read-only lookup by id -- a copy, same rationale as frontOfBook().
    // Used by replace() to inspect current price/qty before deciding
    // whether priority is preserved, without mutating anything.
    std::optional<Order> get(OrderId id) const;

    std::optional<Price> bestBid() const;
    std::optional<Price> bestAsk() const;

    // A COPY of the oldest resting order at the best price on `side`.
    // Deliberately a copy, not a pointer/reference into the list node:
    // the matching loop reads quantities off this copy and then mutates
    // the book by id (reduceQuantity/remove), so there is never a window
    // where a dangling or stale reference could be read. See
    // DESIGN_DECISIONS.md for the rejected pointer-returning alternative.
    std::optional<Order> frontOfBook(Side side) const;

    // Aggregated (price, total_qty) for up to `levels` price levels,
    // best-price-first.
    std::vector<std::pair<Price, Quantity>> depth(Side side, std::size_t levels) const;

    // Walks levels on `side`, best-price-first, calling
    // fn(price, total_qty) for each -- STOPPING as soon as fn returns
    // false, without ever materializing levels beyond that point into a
    // vector. This exists specifically because `depth(side,
    // levelCount(side))` -- which MatchingEngine::aggregateFillable used
    // to call -- unconditionally builds a vector of EVERY level before
    // any early-exit logic runs on top of it, paying the full O(L) cost
    // of walking the whole side regardless of how few levels an order
    // actually needs. See DESIGN_DECISIONS.md, Phase 2 fix. Templated and
    // defined here (not in the .cpp) because the callback type has to be
    // visible at the call site.
    template <typename Fn>
    void forEachLevelWhile(Side side, Fn&& fn) const {
        if (side == Side::Buy) {
            for (const auto& [price, level] : bids_) {
                if (!fn(price, level.total_qty)) return;
            }
        } else {
            for (const auto& [price, level] : asks_) {
                if (!fn(price, level.total_qty)) return;
            }
        }
    }

    // 0-indexed position of order `id` within its own price level's FIFO
    // queue (0 = next to trade at that level). O(position): a linear walk
    // of std::list. This is a query path, not the matching hot path, so
    // per Phase 0's correctness-first rule it is not optimized pre-emptively.
    std::optional<std::size_t> queuePosition(OrderId id) const;

    std::size_t orderCount() const { return index_.size(); }
    std::size_t levelCount(Side side) const;

    // Full-book invariant walk, used by unit/property tests. Returns the
    // first violation found as a human-readable string, or nullopt if
    // every invariant in DESIGN_DECISIONS.md / Phase 0 §3 holds.
    std::optional<std::string> checkInvariants() const;

private:
    struct Locator {
        Side                        side;
        Price                       price;
        std::list<Order>::iterator it;
    };

    // Bids are stored high-to-low, asks low-to-high, via separate map
    // instantiations with different comparators — both expose begin() as
    // "best price", which is the only property the matching/query code
    // actually relies on.
    std::map<Price, PriceLevel, std::greater<Price>> bids_;
    std::map<Price, PriceLevel, std::less<Price>>    asks_;
    std::unordered_map<OrderId, Locator>              index_;
};

}  // namespace lob
