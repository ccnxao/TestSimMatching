#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
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

struct KLine {
    // K 线覆盖的时间窗口，撮合时会把订单放到对应窗口里判断是否触价。
    Timestamp start_ts {0};
    Timestamp end_ts {0};
    double open {0.0};
    double high {0.0};
    double low {0.0};
    double close {0.0};
};

struct Order {
    OrderId id {0};
    Side side {Side::Buy};
    OrderKind kind {OrderKind::Normal};
    // 用户的挂单价。仿真成交时默认按该价格成交，而不是按 bar 的 high/low 成交。
    double price {0.0};
    // 订单第一次进入撮合系统，或者被重挂后的生效时间。
    Timestamp submit_ts {0};
    // 由撮合器在成交时填写。
    Timestamp fill_ts {0};
    // 时间生命周期，单位与 Timestamp 保持一致。<= 0 视为不启用。
    Timestamp time_to_live {0};
    // 空间生命周期阈值。对买单来说，市场下探到该价位仍未成交则废单。
    double space_lifecycle_price {0.0};
    // 市场向不利方向偏离该距离后，触发撤单重挂。
    double market_offset {0.0};
    // > 0 时表示这是一个正向单，成交后会生成反向单。
    double target_profit {0.0};
    // 重挂时允许相对原挂单价追价的最大幅度。
    double acceptable_risk {0.0};
    OrderStatus status {OrderStatus::Pending};
    // 反向单会记录它是由哪张父单生成的。
    std::optional<OrderId> parent_order_id;
    // 仅统计被市场偏移触发的重挂次数。
    std::uint32_t replace_count {0};
};

struct Fill {
    OrderId order_id {0};
    double price {0.0};
    Timestamp fill_ts {0};
    Side side {Side::Buy};
    OrderKind kind {OrderKind::Normal};
    std::optional<Order> spawned_reverse_order;
};

class IOrderQueue {
public:
    virtual ~IOrderQueue() = default;
    // 拉取当前待处理的新订单。这里约定队列本身支持并发，撮合器只负责消费结果。
    virtual std::vector<Order> get() = 0;
};

class IKLineSource {
public:
    virtual ~IKLineSource() = default;
    // 返回按时间升序排列的 K 线序列。
    virtual const std::vector<KLine>& data() const = 0;
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
    SimMatcher(IKLineSource& kline_source, IOrderQueue& order_queue);

    MatchResult run();

private:
    // 从外部队列拉取新订单，只接纳此前没见过的订单 id。
    void pull_new_orders();
    // 某根 bar 只有覆盖到订单生效时间之后，才能拿来撮合该订单。
    bool bar_can_match_order(const Order& order, const KLine& bar) const;
    bool try_fill(Order& order, const KLine& bar, Fill& fill);
    bool should_expire_time(const Order& order, const KLine& bar) const;
    bool should_expire_space(const Order& order, const KLine& bar) const;
    bool should_replace(const Order& order, const KLine& bar) const;
    void replace_order(Order& order, const KLine& bar);
    Order spawn_reverse_order(const Order& filled_order, const Fill& fill);
    double favorable_touch_price(const Order& order, const KLine& bar) const;
    double unfavorable_touch_price(const Order& order, const KLine& bar) const;
    double current_reprice(const Order& order, const KLine& bar) const;

private:
    IKLineSource& kline_source_;
    IOrderQueue& order_queue_;
    // 当前仍然挂在系统里的订单。
    std::deque<Order> active_orders_;
    // 用于去重，避免队列重复吐出同一订单时被重复入场。
    std::unordered_map<OrderId, std::size_t> known_orders_;
    // 正向单成交后，反向单 id 由撮合器内部生成。
    OrderId next_generated_order_id_ {1'000'000};
    // 增量消费 K 线，避免 run() 被重复调用时把老 bar 重新撮合一遍。
    std::size_t next_bar_index_ {0};
};

class VectorKLineSource final : public IKLineSource {
public:
    explicit VectorKLineSource(std::vector<KLine> klines) : klines_(std::move(klines)) {}

    const std::vector<KLine>& data() const override {
        return klines_;
    }

private:
    std::vector<KLine> klines_;
};

std::string to_string(OrderStatus status);
std::string to_string(Side side);
std::string to_string(OrderKind kind);

}  // namespace sim
