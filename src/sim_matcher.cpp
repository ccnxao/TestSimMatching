#include "sim_matcher.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace sim {

namespace {

bool is_buy(Side side) {
    return side == Side::Buy;
}

Side opposite(Side side) {
    return is_buy(side) ? Side::Sell : Side::Buy;
}

}  // namespace

SimMatcher::SimMatcher(IKLineSource& kline_source, IOrderQueue& order_queue)
    : kline_source_(kline_source), order_queue_(order_queue) {}

MatchResult SimMatcher::run() {
    MatchResult result;
    pull_new_orders();

    const auto& bars = kline_source_.data();
    if (next_bar_index_ >= bars.size()) {
        result.active_orders.assign(active_orders_.begin(), active_orders_.end());
        return result;
    }

    for (std::size_t bar_index = next_bar_index_; bar_index < bars.size(); ++bar_index) {
        const KLine& bar = bars[bar_index];
        std::size_t current_count = active_orders_.size();
        for (std::size_t i = 0; i < current_count; ++i) {
            Order order = active_orders_.front();
            active_orders_.pop_front();

            if (order.status != OrderStatus::Pending) {
                result.terminal_orders.push_back(std::move(order));
                continue;
            }

            if (!bar_can_match_order(order, bar)) {
                active_orders_.push_back(std::move(order));
                continue;
            }

            Fill fill;
            if (try_fill(order, bar, fill)) {
                result.fills.push_back(fill);
                order.status = OrderStatus::Filled;
                order.fill_ts = fill.fill_ts;
                result.terminal_orders.push_back(order);

                if (fill.spawned_reverse_order.has_value()) {
                    Order spawned = *fill.spawned_reverse_order;
                    known_orders_[spawned.id] = 1;
                    active_orders_.push_back(std::move(spawned));
                }
                continue;
            }

            if (should_expire_time(order, bar)) {
                order.status = OrderStatus::ExpiredTime;
                result.terminal_orders.push_back(std::move(order));
                continue;
            }

            if (should_expire_space(order, bar)) {
                order.status = OrderStatus::ExpiredSpace;
                result.terminal_orders.push_back(std::move(order));
                continue;
            }

            if (should_replace(order, bar)) {
                replace_order(order, bar);
            }

            active_orders_.push_back(std::move(order));
        }
    }

    next_bar_index_ = bars.size();
    result.active_orders.assign(active_orders_.begin(), active_orders_.end());
    return result;
}

void SimMatcher::pull_new_orders() {
    for (Order order : order_queue_.get()) {
        if (known_orders_.count(order.id) != 0) {
            continue;
        }

        known_orders_[order.id] = 1;
        active_orders_.push_back(std::move(order));
    }
}

bool SimMatcher::bar_can_match_order(const Order& order, const KLine& bar) const {
    return order.submit_ts <= bar.end_ts;
}

bool SimMatcher::try_fill(Order& order, const KLine& bar, Fill& fill) {
    const double touch = favorable_touch_price(order, bar);
    const bool matched = is_buy(order.side) ? (touch <= order.price) : (touch >= order.price);
    if (!matched) {
        return false;
    }

    fill.order_id = order.id;
    fill.price = order.price;
    fill.fill_ts = std::max(order.submit_ts, bar.start_ts);
    fill.side = order.side;
    fill.kind = order.kind;

    if (order.kind == OrderKind::Forward && order.target_profit > 0.0) {
        fill.spawned_reverse_order = spawn_reverse_order(order, fill);
    }

    return true;
}

bool SimMatcher::should_expire_time(const Order& order, const KLine& bar) const {
    if (order.time_to_live <= 0) {
        return false;
    }
    return bar.end_ts >= order.submit_ts + order.time_to_live;
}

bool SimMatcher::should_expire_space(const Order& order, const KLine& bar) const {
    if (order.space_lifecycle_price <= 0.0) {
        return false;
    }

    const double touch = favorable_touch_price(order, bar);
    return is_buy(order.side) ? (touch <= order.space_lifecycle_price)
                              : (touch >= order.space_lifecycle_price);
}

bool SimMatcher::should_replace(const Order& order, const KLine& bar) const {
    if (order.market_offset <= 0.0) {
        return false;
    }

    const double unfavorable = unfavorable_touch_price(order, bar);
    const double drift = std::abs(unfavorable - order.price);
    return drift >= order.market_offset;
}

void SimMatcher::replace_order(Order& order, const KLine& bar) {
    order.price = current_reprice(order, bar);
    order.submit_ts = bar.end_ts;
    ++order.replace_count;
}

Order SimMatcher::spawn_reverse_order(const Order& filled_order, const Fill& fill) {
    Order reverse;
    reverse.id = next_generated_order_id_++;
    reverse.side = opposite(filled_order.side);
    reverse.kind = OrderKind::Reverse;
    reverse.price = is_buy(filled_order.side)
        ? (fill.price + filled_order.target_profit)
        : (fill.price - filled_order.target_profit);
    reverse.submit_ts = fill.fill_ts;
    reverse.time_to_live = filled_order.time_to_live;
    reverse.space_lifecycle_price = 0.0;
    reverse.market_offset = filled_order.market_offset;
    reverse.target_profit = 0.0;
    reverse.acceptable_risk = filled_order.acceptable_risk;
    reverse.parent_order_id = filled_order.id;
    return reverse;
}

double SimMatcher::favorable_touch_price(const Order& order, const KLine& bar) const {
    return is_buy(order.side) ? bar.low : bar.high;
}

double SimMatcher::unfavorable_touch_price(const Order& order, const KLine& bar) const {
    return is_buy(order.side) ? bar.high : bar.low;
}

double SimMatcher::current_reprice(const Order& order, const KLine& bar) const {
    const double candidate = bar.close;
    if (order.acceptable_risk <= 0.0) {
        return candidate;
    }

    if (is_buy(order.side)) {
        return std::min(candidate, order.price + order.acceptable_risk);
    }
    return std::max(candidate, order.price - order.acceptable_risk);
}

std::string to_string(OrderStatus status) {
    switch (status) {
        case OrderStatus::Pending: return "Pending";
        case OrderStatus::Filled: return "Filled";
        case OrderStatus::Cancelled: return "Cancelled";
        case OrderStatus::ExpiredTime: return "ExpiredTime";
        case OrderStatus::ExpiredSpace: return "ExpiredSpace";
        case OrderStatus::Replaced: return "Replaced";
    }
    throw std::invalid_argument("unknown OrderStatus");
}

std::string to_string(Side side) {
    switch (side) {
        case Side::Buy: return "Buy";
        case Side::Sell: return "Sell";
    }
    throw std::invalid_argument("unknown Side");
}

std::string to_string(OrderKind kind) {
    switch (kind) {
        case OrderKind::Normal: return "Normal";
        case OrderKind::Forward: return "Forward";
        case OrderKind::Reverse: return "Reverse";
    }
    throw std::invalid_argument("unknown OrderKind");
}

}  // namespace sim
