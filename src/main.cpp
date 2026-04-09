#include "sim_matcher.hpp"

#include <iostream>
#include <vector>

namespace {

class DemoOrderQueue final : public sim::IOrderQueue {
public:
    explicit DemoOrderQueue(std::vector<sim::Order> orders) : orders_(std::move(orders)) {}

    std::vector<sim::Order> get() override {
        std::vector<sim::Order> out;
        out.swap(orders_);
        return out;
    }

private:
    std::vector<sim::Order> orders_;
};

}  // namespace

int main() {
    sim::VectorKLineSource kline_source({
        {1000, 1999, 100.0, 102.0, 98.0, 101.0},
        {2000, 2999, 101.0, 105.0, 100.0, 104.0},
        {3000, 3999, 104.0, 106.0, 103.0, 105.0},
        {4000, 4999, 105.0, 108.0, 104.0, 107.0},
    });

    DemoOrderQueue queue({
        {1, sim::Side::Buy, sim::OrderKind::Normal, 99.0, 1000, 0, 5000, 97.0, 3.0, 0.0, 2.0},
        {2, sim::Side::Buy, sim::OrderKind::Forward, 100.0, 1000, 0, 5000, 0.0, 4.0, 3.0, 2.0},
    });

    sim::SimMatcher matcher(kline_source, queue);
    const sim::MatchResult result = matcher.run();

    std::cout << "fills: " << result.fills.size() << '\n';
    for (const auto& fill : result.fills) {
        std::cout << "order=" << fill.order_id
                  << " side=" << sim::to_string(fill.side)
                  << " kind=" << sim::to_string(fill.kind)
                  << " price=" << fill.price
                  << " ts=" << fill.fill_ts << '\n';
        if (fill.spawned_reverse_order.has_value()) {
            const auto& reverse = *fill.spawned_reverse_order;
            std::cout << "  spawned reverse order=" << reverse.id
                      << " side=" << sim::to_string(reverse.side)
                      << " price=" << reverse.price << '\n';
        }
    }

    std::cout << "terminal orders: " << result.terminal_orders.size() << '\n';
    for (const auto& order : result.terminal_orders) {
        std::cout << "terminal order=" << order.id
                  << " kind=" << sim::to_string(order.kind)
                  << " status=" << sim::to_string(order.status)
                  << " fill_ts=" << order.fill_ts
                  << " replace_count=" << order.replace_count << '\n';
    }
    std::cout << "active orders: " << result.active_orders.size() << '\n';
    return 0;
}
