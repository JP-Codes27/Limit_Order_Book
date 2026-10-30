// Engine-agnostic canonical state hash.
//
// An engine adapter E must provide:
//   size_t orderCount() const;
//   bool   bestBid(int& px) const;   bool bestAsk(int& px) const;
//   size_t tradeCount() const;       TradeRec trade(size_t i) const;
//   template<class F> void forEachOrder(F&& f) const;
//        // calls f(bool isBuy, int price, uint32_t id, int qty)
//        // order: all bids best-price-first (FIFO inside a level), then all asks best-first.
//
// Two engines that behave identically MUST produce identical hashes. This is the
// correctness proof used to compare V1 against every later optimized version.
#pragma once
#include <cstdint>
#include "types.hpp"

namespace lobtest {

inline void mixIn(std::uint64_t& h, std::uint64_t x) {
    h ^= x + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
    h *= 0xFF51AFD7ED558CCDULL;
    h ^= h >> 32;
}

template <class E>
std::uint64_t canonicalHash(const E& e) {
    std::uint64_t h = 0xCBF29CE484222325ULL;
    mixIn(h, e.orderCount());
    mixIn(h, e.tradeCount());
    for (std::size_t i = 0; i < e.tradeCount(); ++i) {
        const TradeRec t = e.trade(i);
        mixIn(h, t.buyId);
        mixIn(h, t.sellId);
        mixIn(h, static_cast<std::uint32_t>(t.price));
        mixIn(h, static_cast<std::uint32_t>(t.qty));
    }
    int px = 0;
    mixIn(h, e.bestBid(px) ? static_cast<std::uint64_t>(static_cast<std::uint32_t>(px)) + 1 : 0);
    mixIn(h, e.bestAsk(px) ? static_cast<std::uint64_t>(static_cast<std::uint32_t>(px)) + 1 : 0);
    e.forEachOrder([&](bool isBuy, int price, std::uint32_t id, int qty) {
        mixIn(h, isBuy ? 1 : 2);
        mixIn(h, static_cast<std::uint32_t>(price));
        mixIn(h, id);
        mixIn(h, static_cast<std::uint32_t>(qty));
    });
    return h;
}

} // namespace lobtest
