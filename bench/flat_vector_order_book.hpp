#pragma once
// Phase 0 §2 Option B: a flat, sorted std::vector<PriceLevel> with binary
// search, instead of std::map<Price, PriceLevel>. Lives under bench/, not
// include/lob/ -- this is explicitly NOT a Phase 1 deliverable or a
// supported alternative implementation; it exists solely to produce the
// naive-vs-optimized evidence Phase 2 asks for. If benchmarking justifies
// adopting it, THAT is the point where it gets promoted into include/lob/
// and the core library switches to it -- not before.
//
// Deliberately isolates ONE variable against the Phase 1 OrderBook: the
// top-level price-level container (map vs. sorted vector). The per-level
// order container stays std::list, same as Phase 1, same rationale
// (iterator stability under erase-from-elsewhere). Changing two things at
// once would make it impossible to attribute any performance delta to
// either change specifically.

#include <algorithm>
#include <cstddef>
#include <list>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "lob/order.hpp"

namespace lob_bench {

using namespace lob;

struct FlatPriceLevel {
    Price price = 0;
    std::list<Order> orders;
    Quantity total_qty = 0;
};

class FlatVectorOrderBook {
public:
    bool insert(const Order& order) {
        if (index_.count(order.id)) return false;

        auto& levels = (order.side == Side::Buy) ? bids_ : asks_;
        auto it = locate(levels, order.side, order.price);

        if (it == levels.end() || it->price != order.price) {
            FlatPriceLevel lvl;
            lvl.price = order.price;
            it = levels.insert(it, std::move(lvl));  // O(n) shift -- the
                                                     // cost this design
                                                     // trades away.
        }
        it->orders.push_back(order);
        it->total_qty += order.remaining_qty;
        auto list_it = std::prev(it->orders.end());
        index_[order.id] = Locator{order.side, order.price, list_it};
        return true;
    }

    std::optional<Order> remove(OrderId id) {
        auto idx_it = index_.find(id);
        if (idx_it == index_.end()) return std::nullopt;
        const Locator loc = idx_it->second;

        auto& levels = (loc.side == Side::Buy) ? bids_ : asks_;
        auto lvl_it = locate(levels, loc.side, loc.price);
        // lvl_it is guaranteed to point at the matching level -- this
        // order's own presence in index_ guarantees the level exists.

        Order removed = *loc.it;
        lvl_it->total_qty -= loc.it->remaining_qty;
        lvl_it->orders.erase(loc.it);
        if (lvl_it->orders.empty()) {
            levels.erase(lvl_it);  // O(n) shift
        }
        index_.erase(idx_it);
        return removed;
    }

    bool reduceQuantity(OrderId id, Quantity qty) {
        auto idx_it = index_.find(id);
        if (idx_it == index_.end()) return false;
        const Locator loc = idx_it->second;
        Order& o = *loc.it;
        if (qty <= 0 || qty > o.remaining_qty) return false;

        o.remaining_qty -= qty;
        auto& levels = (loc.side == Side::Buy) ? bids_ : asks_;
        auto lvl_it = locate(levels, loc.side, loc.price);

        if (o.remaining_qty == 0) {
            o.status = OrderStatus::Filled;
            lvl_it->total_qty -= qty;
            lvl_it->orders.erase(loc.it);
            if (lvl_it->orders.empty()) levels.erase(lvl_it);
            index_.erase(idx_it);
        } else {
            o.status = OrderStatus::PartiallyFilled;
            lvl_it->total_qty -= qty;
        }
        return true;
    }

    bool decreaseQuantityInPlace(OrderId id, Quantity new_qty) {
        auto idx_it = index_.find(id);
        if (idx_it == index_.end()) return false;
        const Locator loc = idx_it->second;
        Order& o = *loc.it;
        if (new_qty <= 0 || new_qty >= o.remaining_qty) return false;

        const Quantity delta = o.remaining_qty - new_qty;
        o.remaining_qty = new_qty;
        o.status = OrderStatus::PartiallyFilled;

        auto& levels = (loc.side == Side::Buy) ? bids_ : asks_;
        auto lvl_it = locate(levels, loc.side, loc.price);
        lvl_it->total_qty -= delta;
        return true;
    }

    bool contains(OrderId id) const { return index_.count(id) != 0; }

    std::optional<Order> get(OrderId id) const {
        auto it = index_.find(id);
        if (it == index_.end()) return std::nullopt;
        return *it->second.it;
    }

    std::optional<Price> bestBid() const {
        if (bids_.empty()) return std::nullopt;
        return bids_.front().price;
    }
    std::optional<Price> bestAsk() const {
        if (asks_.empty()) return std::nullopt;
        return asks_.front().price;
    }

    std::optional<Order> frontOfBook(Side side) const {
        const auto& levels = (side == Side::Buy) ? bids_ : asks_;
        if (levels.empty()) return std::nullopt;
        return levels.front().orders.front();
    }

    std::vector<std::pair<Price, Quantity>> depth(Side side, std::size_t levels_n) const {
        const auto& levels = (side == Side::Buy) ? bids_ : asks_;
        std::vector<std::pair<Price, Quantity>> out;
        out.reserve(levels_n);
        for (std::size_t i = 0; i < levels.size() && out.size() < levels_n; ++i) {
            out.emplace_back(levels[i].price, levels[i].total_qty);
        }
        return out;
    }

    std::optional<std::size_t> queuePosition(OrderId id) const {
        auto idx_it = index_.find(id);
        if (idx_it == index_.end()) return std::nullopt;
        const Locator& loc = idx_it->second;
        const auto& levels = (loc.side == Side::Buy) ? bids_ : asks_;
        auto lvl_it = locate(levels, loc.side, loc.price);

        std::size_t pos = 0;
        for (auto it = lvl_it->orders.begin(); it != lvl_it->orders.end(); ++it, ++pos) {
            if (it->id == id) return pos;
        }
        return std::nullopt;
    }

    std::size_t orderCount() const { return index_.size(); }
    std::size_t levelCount(Side side) const {
        return (side == Side::Buy) ? bids_.size() : asks_.size();
    }

private:
    struct Locator {
        Side side;
        Price price;
        std::list<Order>::iterator it;
    };

    // Binary search by price. Bids are sorted descending (best = highest,
    // at index 0); asks ascending (best = lowest, at index 0). Returns an
    // iterator to the matching level, or the correct insertion point if
    // none exists at that exact price.
    static std::vector<FlatPriceLevel>::iterator locate(std::vector<FlatPriceLevel>& levels,
                                                        Side side, Price price) {
        if (side == Side::Buy) {
            return std::lower_bound(
                levels.begin(), levels.end(), price,
                [](const FlatPriceLevel& lvl, Price p) { return lvl.price > p; });
        } else {
            return std::lower_bound(
                levels.begin(), levels.end(), price,
                [](const FlatPriceLevel& lvl, Price p) { return lvl.price < p; });
        }
    }
    static std::vector<FlatPriceLevel>::const_iterator locate(
        const std::vector<FlatPriceLevel>& levels, Side side, Price price) {
        if (side == Side::Buy) {
            return std::lower_bound(
                levels.begin(), levels.end(), price,
                [](const FlatPriceLevel& lvl, Price p) { return lvl.price > p; });
        } else {
            return std::lower_bound(
                levels.begin(), levels.end(), price,
                [](const FlatPriceLevel& lvl, Price p) { return lvl.price < p; });
        }
    }

    std::vector<FlatPriceLevel> bids_;  // descending by price
    std::vector<FlatPriceLevel> asks_;  // ascending by price
    std::unordered_map<OrderId, Locator> index_;
};

}  // namespace lob_bench
