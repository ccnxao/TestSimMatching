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

    // 每次运行都先吃掉外部队列里新进来的订单，再拿最新的 K 线去撮合。
    pull_new_orders();

    const auto& bars = kline_source_.data();
    if (next_bar_index_ >= bars.size()) {
        result.active_orders.assign(active_orders_.begin(), active_orders_.end());
        return result;
    }

    for (std::size_t bar_index = next_bar_index_; bar_index < bars.size(); ++bar_index) {
        const KLine& bar = bars[bar_index];

        // current_count 固定住这一轮要处理的订单数。
        // 这样即使中途生成了新的反向单，也要等下一根 bar 再参与撮合，避免同一根 bar 内无限连锁。
        std::size_t current_count = active_orders_.size();
        for (std::size_t i = 0; i < current_count; ++i) {
            Order order = active_orders_.front();
            active_orders_.pop_front();

            if (order.status != OrderStatus::Pending) {
                result.terminal_orders.push_back(std::move(order));
                continue;
            }

            if (!bar_can_match_order(order, bar)) {
                // bar 还没覆盖到订单生效时间，说明这张单在这根 K 线期间还不存在。
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
                    // 新生成的反向单进入活动队列，但不会在当前 bar 立即撮合。
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
            // 外部队列可能重复返回同一订单，这里直接去重。
            continue;
        }

        known_orders_[order.id] = 1;
        active_orders_.push_back(std::move(order));
    }
}

bool SimMatcher::bar_can_match_order(const Order& order, const KLine& bar) const {
    // 只要订单生效时间落在 bar 内，或者早于 bar，便允许这根 bar 参与撮合。
    return order.submit_ts <= bar.end_ts;
}

bool SimMatcher::try_fill(Order& order, const KLine& bar, Fill& fill) {
    // 买单看 bar.low 是否打到挂单价，卖单看 bar.high 是否打到挂单价。
    const double touch = favorable_touch_price(order, bar);
    const bool matched = is_buy(order.side) ? (touch <= order.price) : (touch >= order.price);
    if (!matched) {
        return false;
    }

    fill.order_id = order.id;
    fill.price = order.price;
    // 这里取 max 是为了防止订单在 bar 中途才生效，却被记成 bar.start_ts 成交。
    fill.fill_ts = std::max(order.submit_ts, bar.start_ts);
    fill.side = order.side;
    fill.kind = order.kind;

    if (order.kind == OrderKind::Forward && order.target_profit > 0.0) {
        // 正向单成交后，立即构造一张反向单交回活动队列。
        fill.spawned_reverse_order = spawn_reverse_order(order, fill);
    }

    return true;
}

bool SimMatcher::should_expire_time(const Order& order, const KLine& bar) const {
    if (order.time_to_live <= 0) {
        return false;
    }

    // 这里用 submit_ts + TTL 作为绝对过期时刻。
    return bar.end_ts >= order.submit_ts + order.time_to_live;
}

bool SimMatcher::should_expire_space(const Order& order, const KLine& bar) const {
    if (order.space_lifecycle_price <= 0.0) {
        return false;
    }

    const double touch = favorable_touch_price(order, bar);
    // 空间生命周期看的是“市场已经向有利方向走过了多少”。
    // 例如买单场景里，low 都已经跌到某个更低价位还没成交，则视为机会已经错过。
    return is_buy(order.side) ? (touch <= order.space_lifecycle_price)
                              : (touch >= order.space_lifecycle_price);
}

bool SimMatcher::should_replace(const Order& order, const KLine& bar) const {
    if (order.market_offset <= 0.0) {
        return false;
    }

    const double unfavorable = unfavorable_touch_price(order, bar);
    const double drift = std::abs(unfavorable - order.price);
    // 这里衡量的是市场朝不利方向偏离挂单价的程度。
    return drift >= order.market_offset;
}

void SimMatcher::replace_order(Order& order, const KLine& bar) {
    // 仿真里把重挂简化为“撤掉旧单，然后按当前收盘附近重新挂出一张新单”。
    order.price = current_reprice(order, bar);
    order.submit_ts = bar.end_ts;
    ++order.replace_count;
}

Order SimMatcher::spawn_reverse_order(const Order& filled_order, const Fill& fill) {
    Order reverse;
    reverse.id = next_generated_order_id_++;
    reverse.side = opposite(filled_order.side);
    reverse.kind = OrderKind::Reverse;
    // 买入正向单成交后，反向单是卖单，目标价 = 成交价 + 利润目标；
    // 卖出正向单则相反。
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
    // 对买单来说，越低越容易成交；对卖单来说，越高越容易成交。
    return is_buy(order.side) ? bar.low : bar.high;
}

double SimMatcher::unfavorable_touch_price(const Order& order, const KLine& bar) const {
    // 与 favorable 相反，用来衡量市场是否朝不利方向跑远了。
    return is_buy(order.side) ? bar.high : bar.low;
}

double SimMatcher::current_reprice(const Order& order, const KLine& bar) const {
    // 当前实现用 bar.close 作为新的参考价，再用 acceptable_risk 限制追价幅度。
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
