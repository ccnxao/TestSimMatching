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
    double price {0.0};
    Timestamp submit_ts {0};
    Timestamp fill_ts {0};
    Timestamp time_to_live {0};
    double space_lifecycle_price {0.0};
    double market_offset {0.0};
    double target_profit {0.0};
    double acceptable_risk {0.0};
    OrderStatus status {OrderStatus::Pending};
    std::optional<OrderId> parent_order_id;
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
    virtual std::vector<Order> get() = 0;
};

class IKLineSource {
public:
    virtual ~IKLineSource() = default;
    virtual const std::vector<KLine>& data() const = 0;
};

struct MatchResult {
    std::vector<Fill> fills;
    std::vector<Order> terminal_orders;
    std::vector<Order> active_orders;
};

class SimMatcher {
public:
    SimMatcher(IKLineSource& kline_source, IOrderQueue& order_queue);

    MatchResult run();

private:
    void pull_new_orders();
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
    std::deque<Order> active_orders_;
    std::unordered_map<OrderId, std::size_t> known_orders_;
    OrderId next_generated_order_id_ {1'000'000};
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
