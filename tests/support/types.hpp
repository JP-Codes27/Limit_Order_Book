// Engine-agnostic types shared by the fuzzer and the benchmark harness.
#pragma once
#include <cstdint>

namespace lobtest {

enum class Side : std::uint8_t { Buy = 0, Sell = 1 };

struct Op {
    enum Kind : std::uint8_t { Add = 0, Cancel = 1 };
    Kind          kind;
    Side          side;
    std::uint32_t id;
    std::int32_t  price;
    std::int32_t  qty;
};

inline Op makeAdd(Side s, std::uint32_t id, std::int32_t price, std::int32_t qty) {
    return Op{Op::Add, s, id, price, qty};
}
inline Op makeCancel(std::uint32_t id) { return Op{Op::Cancel, Side::Buy, id, 0, 0}; }

struct TradeRec {
    std::uint32_t buyId;
    std::uint32_t sellId;
    std::int32_t  price;
    std::int32_t  qty;
};
inline bool operator==(const TradeRec& a, const TradeRec& b) {
    return a.buyId == b.buyId && a.sellId == b.sellId && a.price == b.price && a.qty == b.qty;
}

} // namespace lobtest
