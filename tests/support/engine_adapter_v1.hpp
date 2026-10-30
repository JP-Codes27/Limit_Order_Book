// Adapter that wraps the V1 OrderBook (std::map + std::list) behind the engine-agnostic
// interface described in canonical.hpp. To benchmark/fuzz a new engine, write a sibling
// adapter (e.g. engine_adapter_v2.hpp) with the same member functions.
#pragma once
#include <cstddef>
#include "order_book.hpp"
#include "types.hpp"

namespace lobtest {

class V1Adapter {
public:
    static constexpr const char* kName = "V1 (map+list+unordered_map)";

    explicit V1Adapter(std::size_t /*maxOrders*/ = 0) {}

    // Production path: match against the opposite side, rest the remainder.
    void add(const Op& op) {
        book_.match(Order{static_cast<int>(op.id),
                          op.side == Side::Buy ? orderType::buy : orderType::sell,
                          op.price, op.qty});
    }
    void cancel(std::uint32_t id) { book_.remove(static_cast<int>(id)); }

    std::size_t orderCount() const { return book_.orderCount(); }
    bool bestBid(int& px) const { auto b = book_.getBestBid(); if (b) px = *b; return b.has_value(); }
    bool bestAsk(int& px) const { auto a = book_.getBestAsk(); if (a) px = *a; return a.has_value(); }
    std::size_t tradeCount() const { return book_.getTrades().size(); }
    TradeRec trade(std::size_t i) const {
        const Trade& t = book_.getTrades()[i];
        return TradeRec{static_cast<std::uint32_t>(t.buyId), static_cast<std::uint32_t>(t.sellId),
                        t.price, t.shares};
    }

    template <class F>
    void forEachOrder(F&& f) const {
        for (auto it = book_.getBids().rbegin(); it != book_.getBids().rend(); ++it)
            for (const Order& o : it->second)
                f(true, it->first, static_cast<std::uint32_t>(o.orderId), o.shares);
        for (const auto& level : book_.getAsks())
            for (const Order& o : level.second)
                f(false, level.first, static_cast<std::uint32_t>(o.orderId), o.shares);
    }

private:
    OrderBook book_;
};

} // namespace lobtest
