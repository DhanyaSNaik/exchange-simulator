#include "lob/order_book.hpp"

#include <algorithm>
#include <sstream>

namespace lob {

// bids_ and asks_ are different map *types* (different comparator template
// arguments), so there is no single reference type that could alias both.
// Each method below branches on `side` and repeats the logic for each map
// explicitly rather than fighting the type system with a templated/variant
// accessor -- more lines, but each branch is trivially auditable, which is
// the priority at this stage per Phase 0's correctness-first rule.

bool OrderBook::insert(const Order& order) {
    if (index_.count(order.id)) return false;

    if (order.side == Side::Buy) {
        auto& level = bids_[order.price];
        level.price = order.price;
        level.orders.push_back(order);
        level.total_qty += order.remaining_qty;
        auto it = std::prev(level.orders.end());
        index_[order.id] = Locator{Side::Buy, order.price, it};
    } else {
        auto& level = asks_[order.price];
        level.price = order.price;
        level.orders.push_back(order);
        level.total_qty += order.remaining_qty;
        auto it = std::prev(level.orders.end());
        index_[order.id] = Locator{Side::Sell, order.price, it};
    }
    return true;
}

std::optional<Order> OrderBook::remove(OrderId id) {
    auto idx_it = index_.find(id);
    if (idx_it == index_.end()) return std::nullopt;

    const Locator loc = idx_it->second;
    std::optional<Order> removed;

    if (loc.side == Side::Buy) {
        auto level_it = bids_.find(loc.price);
        removed = *loc.it;
        level_it->second.total_qty -= loc.it->remaining_qty;
        level_it->second.orders.erase(loc.it);
        if (level_it->second.orders.empty()) bids_.erase(level_it);
    } else {
        auto level_it = asks_.find(loc.price);
        removed = *loc.it;
        level_it->second.total_qty -= loc.it->remaining_qty;
        level_it->second.orders.erase(loc.it);
        if (level_it->second.orders.empty()) asks_.erase(level_it);
    }
    index_.erase(idx_it);
    return removed;
}

bool OrderBook::reduceQuantity(OrderId id, Quantity qty) {
    auto idx_it = index_.find(id);
    if (idx_it == index_.end()) return false;
    const Locator loc = idx_it->second;

    Order& o = *loc.it;
    if (qty <= 0 || qty > o.remaining_qty) return false;  // caller bug guard

    o.remaining_qty -= qty;

    if (o.remaining_qty == 0) {
        o.status = OrderStatus::Filled;
        if (loc.side == Side::Buy) {
            auto level_it = bids_.find(loc.price);
            level_it->second.total_qty -= qty;
            level_it->second.orders.erase(loc.it);
            if (level_it->second.orders.empty()) bids_.erase(level_it);
        } else {
            auto level_it = asks_.find(loc.price);
            level_it->second.total_qty -= qty;
            level_it->second.orders.erase(loc.it);
            if (level_it->second.orders.empty()) asks_.erase(level_it);
        }
        index_.erase(idx_it);
    } else {
        o.status = OrderStatus::PartiallyFilled;
        if (loc.side == Side::Buy) {
            bids_.find(loc.price)->second.total_qty -= qty;
        } else {
            asks_.find(loc.price)->second.total_qty -= qty;
        }
    }
    return true;
}

bool OrderBook::decreaseQuantityInPlace(OrderId id, Quantity new_qty) {
    auto idx_it = index_.find(id);
    if (idx_it == index_.end()) return false;
    const Locator loc = idx_it->second;
    Order& o = *loc.it;

    if (new_qty <= 0 || new_qty >= o.remaining_qty) return false;  // must be a strict decrease

    const Quantity delta = o.remaining_qty - new_qty;
    o.remaining_qty = new_qty;
    o.status = OrderStatus::PartiallyFilled;

    if (loc.side == Side::Buy) {
        bids_.find(loc.price)->second.total_qty -= delta;
    } else {
        asks_.find(loc.price)->second.total_qty -= delta;
    }
    return true;
}

bool OrderBook::contains(OrderId id) const { return index_.count(id) != 0; }

std::optional<Order> OrderBook::get(OrderId id) const {
    auto idx_it = index_.find(id);
    if (idx_it == index_.end()) return std::nullopt;
    return *idx_it->second.it;
}

std::optional<Price> OrderBook::bestBid() const {
    if (bids_.empty()) return std::nullopt;
    return bids_.begin()->first;
}

std::optional<Price> OrderBook::bestAsk() const {
    if (asks_.empty()) return std::nullopt;
    return asks_.begin()->first;
}

std::optional<Order> OrderBook::frontOfBook(Side side) const {
    if (side == Side::Buy) {
        if (bids_.empty()) return std::nullopt;
        return bids_.begin()->second.orders.front();
    } else {
        if (asks_.empty()) return std::nullopt;
        return asks_.begin()->second.orders.front();
    }
}

std::vector<std::pair<Price, Quantity>> OrderBook::depth(Side side, std::size_t levels) const {
    std::vector<std::pair<Price, Quantity>> out;
    out.reserve(levels);
    if (side == Side::Buy) {
        for (auto it = bids_.begin(); it != bids_.end() && out.size() < levels; ++it) {
            out.emplace_back(it->first, it->second.total_qty);
        }
    } else {
        for (auto it = asks_.begin(); it != asks_.end() && out.size() < levels; ++it) {
            out.emplace_back(it->first, it->second.total_qty);
        }
    }
    return out;
}

std::optional<std::size_t> OrderBook::queuePosition(OrderId id) const {
    auto idx_it = index_.find(id);
    if (idx_it == index_.end()) return std::nullopt;
    const Locator& loc = idx_it->second;

    const PriceLevel* level = nullptr;
    if (loc.side == Side::Buy) {
        auto it = bids_.find(loc.price);
        level = &it->second;
    } else {
        auto it = asks_.find(loc.price);
        level = &it->second;
    }

    std::size_t pos = 0;
    for (auto it = level->orders.begin(); it != level->orders.end(); ++it, ++pos) {
        if (it->id == id) return pos;
    }
    return std::nullopt;  // should be unreachable if index_ is consistent
}

std::size_t OrderBook::levelCount(Side side) const {
    return side == Side::Buy ? bids_.size() : asks_.size();
}

std::optional<std::string> OrderBook::checkInvariants() const {
    std::ostringstream err;

    // Crossed-book invariant (Phase 0 §3.1)
    if (!bids_.empty() && !asks_.empty() && bids_.begin()->first >= asks_.begin()->first) {
        err << "crossed book: best_bid=" << bids_.begin()->first
            << " best_ask=" << asks_.begin()->first;
        return err.str();
    }

    // Per-level sum + empty-level + FIFO-local-consistency checks (§3.3, §3.13)
    auto checkSide = [&](const auto& levels, Side expected_side) -> std::optional<std::string> {
        for (const auto& [price, level] : levels) {
            if (level.orders.empty()) {
                err << "empty level left in map at price " << price;
                return err.str();
            }
            if (level.price != price) {
                err << "level.price mismatch at key " << price << " vs stored " << level.price;
                return err.str();
            }
            Quantity sum = 0;
            for (const auto& o : level.orders) {
                if (o.remaining_qty <= 0) {
                    err << "non-positive remaining_qty for order " << o.id;
                    return err.str();
                }
                if (o.side != expected_side) {
                    err << "order " << o.id << " on wrong side of book";
                    return err.str();
                }
                if (o.price != price) {
                    err << "order " << o.id << " resting at wrong price level";
                    return err.str();
                }
                sum += o.remaining_qty;
            }
            if (sum != level.total_qty) {
                err << "total_qty mismatch at price " << price << ": cached=" << level.total_qty
                    << " actual=" << sum;
                return err.str();
            }
        }
        return std::nullopt;
    };

    if (auto e = checkSide(bids_, Side::Buy)) return e;
    if (auto e = checkSide(asks_, Side::Sell)) return e;

    // Index/book 1:1 consistency (§3.4): every index entry resolves, and
    // order_count matches total resting orders across both sides.
    std::size_t total_resting = 0;
    for (const auto& [p, lvl] : bids_) total_resting += lvl.orders.size();
    for (const auto& [p, lvl] : asks_) total_resting += lvl.orders.size();
    if (total_resting != index_.size()) {
        err << "index size " << index_.size() << " != resting order count " << total_resting;
        return err.str();
    }
    for (const auto& [id, loc] : index_) {
        bool found = false;
        if (loc.side == Side::Buy) {
            auto it = bids_.find(loc.price);
            found = (it != bids_.end()) && (std::find_if(it->second.orders.begin(),
                       it->second.orders.end(),
                       [id](const Order& o) { return o.id == id; }) != it->second.orders.end());
        } else {
            auto it = asks_.find(loc.price);
            found = (it != asks_.end()) && (std::find_if(it->second.orders.begin(),
                       it->second.orders.end(),
                       [id](const Order& o) { return o.id == id; }) != it->second.orders.end());
        }
        if (!found) {
            err << "index entry for order " << id << " does not resolve into the book";
            return err.str();
        }
    }

    return std::nullopt;
}

}  // namespace lob
