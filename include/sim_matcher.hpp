#pragma once

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace sim {

using Timestamp = std::int64_t;
using OrderId = std::uint64_t;

enum class Side {
    Buy,
    Sell
};

enum class OrderKind {
    Normal,
    Forward,
    Reverse
};

enum class OrderStatus {
    Pending,
    Filled,
    Cancelled,
    ExpiredTime,
    ExpiredSpace,
    Replaced
};

struct FuturesTick {
    // 期货 Tick 快照。买单严格穿过 ask 才成交，卖单严格穿过 bid 才成交。
    Timestamp ts {0};
    double bid_price {0.0};
    double ask_price {0.0};
    double last_price {0.0};
};

struct Order {
    OrderId id {0};
    // kind 会根据 target_profit 重新推导，保留该字段便于撮合结果直接表达订单类型。
    OrderKind kind {OrderKind::Normal};
    // 带方向的挂单价：> 0 买入，< 0 卖出，== 0 非法。
    double price {0.0};
    // 最初报单价格，用于计算累计冲击成本。入队时若为 0，由撮合器用 price 初始化。
    double initial_price {0.0};
    // 订单第一次进入撮合系统的时间戳；反向单沿用正向单的报单时间。
    Timestamp submit_ts {0};
    // 由撮合器在成交时填写。
    Timestamp fill_ts {0};
    // 时间生命周期。反向单不按该字段过期，该字段用于存储正向单冲击成本。
    double time_to_live {0.0};
    // 空间生命周期阈值。反向单不按该字段过期。
    double space_lifecycle_price {0.0};
    // 正向单市场偏移：买单看 ask >= price + offset，卖单看 bid <= price - offset。
    double market_offset {0.0};
    // 最终成交价和最初报单价的方向归一价差，正数表示成交更差。
    double accumulated_slippage {0.0};
    // < 0 常规单，= 0 反向单，> 0 正向单并用于生成反向单目标盈利。
    double target_profit {0.0};
    // 反向单使用的市场偏移。正向单成交生成反向单时，反向单 market_offset 使用该值。
    double acceptable_risk {0.0};
    // 正向单撤单重委时，买单用 ask + offset，卖单用 bid - offset。
    double forward_reprice_offset {0.0};
    // 反向单撤单重委时，买单用 ask + offset，卖单用 bid - offset。
    double reverse_reprice_offset {0.0};
    OrderStatus status {OrderStatus::Pending};
    // 反向单会记录它是由哪张父单生成的。
    std::optional<OrderId> parent_order_id;
    // 反向单记录正向单成交价，用于计算组合盈亏。
    double parent_fill_price {0.0};
    // 反向单成交时填写。正向买入后卖出：reverse - forward；正向卖出后买入：forward - reverse。
    double pnl {0.0};
    // 仅统计被市场偏移触发的重挂次数。
    std::uint32_t replace_count {0};
};

struct Fill {
    OrderId order_id {0};
    double price {0.0};
    Timestamp fill_ts {0};
    Side side {Side::Buy};
    OrderKind kind {OrderKind::Normal};
    double accumulated_slippage {0.0};
    double pnl {0.0};
    std::optional<Order> spawned_reverse_order;
};

class IOrderQueue {
public:
    virtual ~IOrderQueue() = default;
    // 拉取当前待处理的新订单。这里约定队列本身支持并发，撮合器只负责消费结果。
    virtual std::vector<Order> get() = 0;
    // 向外部队列放回订单，供需要把撮合器生成订单交还给队列的场景使用。
    virtual void put(Order order) = 0;
};

class ITickSource {
public:
    virtual ~ITickSource() = default;
    // 返回按时间升序排列的期货 Tick 序列。
    virtual const std::vector<FuturesTick>& data() const = 0;
};

struct MatchResult {
    // 本轮 run() 新产生的成交。
    std::vector<Fill> fills;
    // 本轮 run() 内状态已经终结的订单，例如成交、过期。
    std::vector<Order> terminal_orders;
    // 本轮 run() 结束后仍继续留在撮合器里的活跃订单。
    std::vector<Order> active_orders;
};

class SimMatcher {
public:
    SimMatcher(ITickSource& tick_source, IOrderQueue& order_queue);

    MatchResult run();

private:
    // 从外部队列拉取新订单，只接纳此前没见过的订单 id。
    void pull_new_orders();
    bool tick_can_match_order(const Order& order, const FuturesTick& tick) const;
    bool try_fill(Order& order, const FuturesTick& tick, Fill& fill);
    bool should_expire_time(const Order& order, const FuturesTick& tick) const;
    bool should_expire_space(const Order& order, const FuturesTick& tick) const;
    bool should_replace(const Order& order, const FuturesTick& tick) const;
    void replace_order(Order& order, const FuturesTick& tick);
    Order spawn_reverse_order(const Order& filled_order, const Fill& fill);
    double order_abs_price(const Order& order) const;
    double signed_price(Side side, double abs_price) const;
    Side order_side(const Order& order) const;
    double opponent_price(const Order& order, const FuturesTick& tick) const;
    double reprice_reference(const Order& order, const FuturesTick& tick) const;
    double current_reprice(const Order& order, const FuturesTick& tick) const;
    double slippage(const Order& order, double fill_price) const;
    double reverse_pnl(const Order& order, double fill_price) const;

private:
    ITickSource& tick_source_;
    IOrderQueue& order_queue_;
    // 当前仍然挂在系统里的订单。
    std::deque<Order> active_orders_;
    // 只需要记录 id 是否出现过，set 比 map 少存一个无用 value，内存更小。
    std::unordered_set<OrderId> known_orders_;
    // 正向单成交后，反向单 id 由撮合器内部生成。
    OrderId next_generated_order_id_ {1'000'000};
    // 增量消费 Tick，避免 run() 被重复调用时把老 Tick 重新撮合一遍。
    std::size_t next_tick_index_ {0};
};

class VectorTickSource final : public ITickSource {
public:
    explicit VectorTickSource(std::vector<FuturesTick> ticks) : ticks_(std::move(ticks)) {}

    const std::vector<FuturesTick>& data() const override {
        return ticks_;
    }

private:
    std::vector<FuturesTick> ticks_;
};

std::string to_string(OrderStatus status);
std::string to_string(Side side);
std::string to_string(OrderKind kind);

}  // namespace sim
