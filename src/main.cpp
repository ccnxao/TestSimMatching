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

    void put(sim::Order order) override {
        orders_.push_back(std::move(order));
    }

private:
    std::vector<sim::Order> orders_;
};

}  // namespace

int main() {
    sim::VectorTickSource tick_source({
        {1000, 99.0, 100.0, 99.5},
        {2000, 100.0, 101.0, 100.5},
        {3000, 103.0, 104.0, 103.5},
        {4000, 106.0, 107.0, 106.5},
    });

    sim::Order normal;
    normal.id = 1;
    normal.price = 101.0;
    normal.submit_ts = 1000;
    normal.time_to_live = 5000.0;
    normal.space_lifecycle_price = 98.0;
    normal.target_profit = -1.0;

    sim::Order forward;
    forward.id = 2;
    forward.price = 101.0;
    forward.submit_ts = 1000;
    forward.time_to_live = 5000.0;
    forward.market_offset = 4.0;
    forward.target_profit = 3.0;
    forward.acceptable_risk = 2.0;
    forward.forward_reprice_offset = 0.5;
    forward.reverse_reprice_offset = 0.5;

    DemoOrderQueue queue({normal, forward});

    sim::SimMatcher matcher(tick_source, queue);
    const sim::MatchResult result = matcher.run();

    std::cout << "fills: " << result.fills.size() << '\n';
    for (const auto& fill : result.fills) {
        std::cout << "order=" << fill.order_id
                  << " side=" << sim::to_string(fill.side)
                  << " kind=" << sim::to_string(fill.kind)
                  << " price=" << fill.price
                  << " ts=" << fill.fill_ts
                  << " slippage=" << fill.accumulated_slippage
                  << " pnl=" << fill.pnl << '\n';
        if (fill.spawned_reverse_order.has_value()) {
            const auto& reverse = *fill.spawned_reverse_order;
            std::cout << "  spawned reverse order=" << reverse.id
                      << " side=" << sim::to_string(reverse.price > 0.0 ? sim::Side::Buy : sim::Side::Sell)
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
