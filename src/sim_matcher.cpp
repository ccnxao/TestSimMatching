#include "sim_matcher.hpp"

#include <cmath>
#include <stdexcept>
#include <utility>

namespace sim {

namespace {

Side opposite(Side side) {
    return side == Side::Buy ? Side::Sell : Side::Buy;
}

OrderKind infer_kind(double target_profit) {
    if (target_profit > 0.0) {
        return OrderKind::Forward;
    }
    if (target_profit == 0.0) {
        return OrderKind::Reverse;
    }
    return OrderKind::Normal;
}

}  // namespace

SimMatcher::SimMatcher(ITickSource& tick_source, IOrderQueue& order_queue)
    : tick_source_(tick_source), order_queue_(order_queue) {}

MatchResult SimMatcher::run() {
    MatchResult result;

    // 每次运行都先吃掉外部队列里新进来的订单，再拿最新的 Tick 去撮合。
    pull_new_orders();

    const auto& ticks = tick_source_.data();
    result.fills.reserve(active_orders_.size());
    result.terminal_orders.reserve(active_orders_.size());
    if (next_tick_index_ >= ticks.size()) {
        result.active_orders.assign(active_orders_.begin(), active_orders_.end());
        return result;
    }

    for (std::size_t tick_index = next_tick_index_; tick_index < ticks.size(); ++tick_index) {
        const FuturesTick& tick = ticks[tick_index];

        scratch_orders_.clear();
        // current_count 固定住这一轮要处理的订单数。
        // 这样即使中途生成了新的反向单，也要等下一笔 Tick 再参与撮合，避免同一 Tick 内无限连锁。
        const std::size_t current_count = active_orders_.size();
        scratch_orders_.reserve(current_count);
        for (std::size_t i = 0; i < current_count; ++i) {
            Order order = std::move(active_orders_[i]);

            if (order.status != OrderStatus::Pending) {
                result.terminal_orders.push_back(std::move(order));
                continue;
            }

            if (!tick_can_match_order(order, tick)) {
                // 当前 Tick 早于订单报单时间，订单已入活动队列但尚未生效，暂不参与撮合。
                scratch_orders_.push_back(std::move(order));
                continue;
            }

            Fill fill;
            if (try_fill(order, tick, fill)) {
                result.fills.push_back(fill);
                order.status = OrderStatus::Filled;
                order.fill_ts = fill.fill_ts;
                order.accumulated_slippage = fill.accumulated_slippage;
                order.pnl = fill.pnl;
                result.terminal_orders.push_back(std::move(order));

                if (fill.spawned_reverse_order.has_value()) {
                    Order spawned = std::move(*fill.spawned_reverse_order);
                    known_orders_.insert(spawned.id);
                    // 新生成的反向单进入活动队列，但不会在当前 Tick 立即撮合。
                    scratch_orders_.push_back(std::move(spawned));
                }
                continue;
            }

            if (should_expire_time(order, tick)) {
                order.status = OrderStatus::ExpiredTime;
                result.terminal_orders.push_back(std::move(order));
                continue;
            }

            if (should_expire_space(order, tick)) {
                order.status = OrderStatus::ExpiredSpace;
                result.terminal_orders.push_back(std::move(order));
                continue;
            }

            if (should_replace(order, tick)) {
                replace_order(order, tick);
            }

            scratch_orders_.push_back(std::move(order));
        }

        active_orders_.clear();
        active_orders_.swap(scratch_orders_);
    }

    next_tick_index_ = ticks.size();
    result.active_orders.assign(active_orders_.begin(), active_orders_.end());
    return result;
}

void SimMatcher::pull_new_orders() {
    std::vector<Order> orders = order_queue_.get();
    if (!orders.empty()) {
        known_orders_.reserve(known_orders_.size() + orders.size());
    }

    for (Order& order : orders) {
        if (order.price == 0.0) {
            order.status = OrderStatus::Cancelled;
            continue;
        }

        const auto insert_result = known_orders_.insert(order.id);
        if (!insert_result.second) {
            // 外部队列可能重复返回同一订单，这里直接去重。
            continue;
        }

        if (order.initial_price == 0.0) {
            order.initial_price = order.price;
        }
        // 用户约定 target_profit 同时承担订单类型编码，入场时统一归一化一次。
        order.kind = infer_kind(order.target_profit);
        active_orders_.push_back(std::move(order));
    }
}

bool SimMatcher::tick_can_match_order(const Order& order, const FuturesTick& tick) const {
    return order.submit_ts <= tick.ts;
}

bool SimMatcher::try_fill(Order& order, const FuturesTick& tick, Fill& fill) {
    const Side side = order_side(order);
    const double order_price = order_abs_price(order);
    const double opponent = opponent_price(order, tick);
    // 严格撮合：买单必须穿过 ask，卖单必须穿过 bid；等价不成交。
    const bool matched = side == Side::Buy ? (order_price > opponent) : (order_price < opponent);
    if (!matched) {
        return false;
    }

    fill.order_id = order.id;
    fill.price = opponent;
    fill.fill_ts = tick.ts;
    fill.side = side;
    fill.kind = order.kind;
    fill.accumulated_slippage = slippage(order, fill.price);
    fill.pnl = order.kind == OrderKind::Reverse ? reverse_pnl(order, fill.price) : 0.0;

    if (order.kind == OrderKind::Forward && order.target_profit > 0.0) {
        // 正向单成交后，立即构造一张反向单交回活动队列。
        fill.spawned_reverse_order = spawn_reverse_order(order, fill);
    }

    return true;
}

bool SimMatcher::should_expire_time(const Order& order, const FuturesTick& tick) const {
    // 反向单必须成交，不允许被时间生命周期淘汰。
    if (order.kind == OrderKind::Reverse || order.time_to_live <= 0) {
        return false;
    }

    return static_cast<double>(tick.ts) >= static_cast<double>(order.submit_ts) + order.time_to_live;
}

bool SimMatcher::should_expire_space(const Order& order, const FuturesTick& tick) const {
    // 反向单必须成交，不允许被空间生命周期淘汰。
    if (order.kind == OrderKind::Reverse || order.space_lifecycle_price <= 0.0) {
        return false;
    }

    const Side side = order_side(order);
    const double opponent = opponent_price(order, tick);
    return side == Side::Buy ? (opponent <= order.space_lifecycle_price)
                             : (opponent >= order.space_lifecycle_price);
}

bool SimMatcher::should_replace(const Order& order, const FuturesTick& tick) const {
    // 正向单用 market_offset，反向单用 acceptable_risk 作为重委触发阈值。
    const double trigger_offset = order.kind == OrderKind::Reverse ? order.acceptable_risk
                                                                   : order.market_offset;
    if (trigger_offset <= 0.0 || order.kind == OrderKind::Normal) {
        return false;
    }

    const Side side = order_side(order);
    const double opponent = opponent_price(order, tick);
    const double order_price = order_abs_price(order);
    return side == Side::Buy ? (opponent >= order_price + trigger_offset)
                             : (opponent <= order_price - trigger_offset);
}

void SimMatcher::replace_order(Order& order, const FuturesTick& tick) {
    // 重委只改价格和次数，不改 submit_ts；反向单的报单时间仍沿用正向单。
    order.price = current_reprice(order, tick);
    ++order.replace_count;
}

Order SimMatcher::spawn_reverse_order(const Order& filled_order, const Fill& fill) {
    Order reverse;
    reverse.id = next_generated_order_id_++;
    reverse.kind = OrderKind::Reverse;
    const Side forward_side = order_side(filled_order);
    const Side reverse_side = opposite(forward_side);
    const double reverse_price = forward_side == Side::Buy
        ? (fill.price + filled_order.target_profit)
        : (fill.price - filled_order.target_profit);
    reverse.price = signed_price(reverse_side, reverse_price);
    reverse.initial_price = reverse.price;
    reverse.submit_ts = filled_order.submit_ts;
    reverse.fill_ts = 0;
    // 按需求复用 time_to_live 存储正向单冲击成本；反向单不会用它做过期判断。
    reverse.time_to_live = fill.accumulated_slippage;
    reverse.space_lifecycle_price = 0.0;
    reverse.market_offset = filled_order.acceptable_risk;
    reverse.accumulated_slippage = 0.0;
    reverse.target_profit = 0.0;
    reverse.acceptable_risk = filled_order.acceptable_risk;
    reverse.forward_reprice_offset = filled_order.forward_reprice_offset;
    reverse.reverse_reprice_offset = filled_order.reverse_reprice_offset;
    reverse.parent_order_id = filled_order.id;
    reverse.parent_fill_price = fill.price;
    return reverse;
}

double SimMatcher::order_abs_price(const Order& order) const {
    return std::abs(order.price);
}

double SimMatcher::signed_price(Side side, double abs_price) const {
    return side == Side::Buy ? std::abs(abs_price) : -std::abs(abs_price);
}

Side SimMatcher::order_side(const Order& order) const {
    return order.price > 0.0 ? Side::Buy : Side::Sell;
}

double SimMatcher::opponent_price(const Order& order, const FuturesTick& tick) const {
    // 买单对手盘是 ask，卖单对手盘是 bid。
    return order_side(order) == Side::Buy ? tick.ask_price : tick.bid_price;
}

double SimMatcher::reprice_reference(const Order& order, const FuturesTick& tick) const {
    return opponent_price(order, tick);
}

double SimMatcher::current_reprice(const Order& order, const FuturesTick& tick) const {
    const Side side = order_side(order);
    const double offset = order.kind == OrderKind::Reverse ? order.reverse_reprice_offset
                                                           : order.forward_reprice_offset;
    const double reference = reprice_reference(order, tick);
    const double abs_price = side == Side::Buy ? (reference + offset) : (reference - offset);
    return signed_price(side, abs_price);
}

double SimMatcher::slippage(const Order& order, double fill_price) const {
    const double initial = std::abs(order.initial_price == 0.0 ? order.price : order.initial_price);
    // 方向归一化后，正数代表更差成交，负数代表更优成交。
    return order_side(order) == Side::Buy ? (fill_price - initial) : (initial - fill_price);
}

double SimMatcher::reverse_pnl(const Order& order, double fill_price) const {
    if (order.parent_fill_price <= 0.0) {
        return 0.0;
    }

    return order_side(order) == Side::Sell ? (fill_price - order.parent_fill_price)
                                           : (order.parent_fill_price - fill_price);
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
