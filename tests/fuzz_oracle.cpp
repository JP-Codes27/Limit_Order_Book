// Differential fuzzer: random operation streams are fed to the engine under test and to a
// brute-force reference model; any divergence (trades, order count, best bid/ask, full book
// contents including FIFO order) is reported with the seed and the last operations.
//
// Usage: fuzz_oracle [--seeds N] [--steps N] [--seed-start N] [--mode 0..5] [--verbose]
// Exit code 0 = no divergence found, 1 = divergence, 2 = bad arguments.
//
// Generator modes (default: seed % 6):
//   0 wide band 90..110      1 single price           2 two adjacent prices
//   3 big sweeping takers    4 add-burst / cancel-burst phases
//   5 tiny id pool, ids are reused after fill/cancel
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "support/check.hpp"
#include "support/engine_adapter_v1.hpp"
#include "support/rng.hpp"
#include "support/types.hpp"

using namespace lobtest;

namespace {

using Engine = V1Adapter;  // <- change this one alias to fuzz another engine

struct Row {
    bool          isBuy;
    int           price;
    std::uint32_t id;
    int           qty;
    bool operator==(const Row& o) const {
        return isBuy == o.isBuy && price == o.price && id == o.id && qty == o.qty;
    }
};

// Obviously-correct O(n) model: scan every resting order, pick best by (price, arrival).
class Reference {
public:
    struct Resting { std::uint32_t id; bool isBuy; int price; int qty; std::uint64_t seq; };

    bool live(std::uint32_t id) const {
        for (const Resting& r : v_) if (r.id == id) return true;
        return false;
    }
    void add(const Op& op) {
        const bool isBuy = op.side == Side::Buy;
        int qty = op.qty;
        while (qty > 0) {
            int best = -1;
            for (std::size_t i = 0; i < v_.size(); ++i) {
                const Resting& r = v_[i];
                if (r.isBuy == isBuy) continue;
                if (isBuy ? r.price > op.price : r.price < op.price) continue;  // does not cross
                if (best < 0) { best = static_cast<int>(i); continue; }
                const Resting& b = v_[static_cast<std::size_t>(best)];
                const bool better = isBuy ? (r.price < b.price || (r.price == b.price && r.seq < b.seq))
                                          : (r.price > b.price || (r.price == b.price && r.seq < b.seq));
                if (better) best = static_cast<int>(i);
            }
            if (best < 0) break;
            Resting& m = v_[static_cast<std::size_t>(best)];
            const int fill = std::min(qty, m.qty);
            trades_.push_back(isBuy ? TradeRec{op.id, m.id, m.price, fill}
                                    : TradeRec{m.id, op.id, m.price, fill});
            qty -= fill;
            m.qty -= fill;
            if (m.qty == 0) v_.erase(v_.begin() + best);
        }
        if (qty > 0) v_.push_back({op.id, isBuy, op.price, qty, seq_++});
    }
    void cancel(std::uint32_t id) {
        for (std::size_t i = 0; i < v_.size(); ++i)
            if (v_[i].id == id) { v_.erase(v_.begin() + static_cast<std::ptrdiff_t>(i)); return; }
    }
    std::size_t orderCount() const { return v_.size(); }
    bool bestBid(int& px) const {
        bool any = false;
        for (const Resting& r : v_) if (r.isBuy && (!any || r.price > px)) { px = r.price; any = true; }
        return any;
    }
    bool bestAsk(int& px) const {
        bool any = false;
        for (const Resting& r : v_) if (!r.isBuy && (!any || r.price < px)) { px = r.price; any = true; }
        return any;
    }
    const std::vector<TradeRec>& trades() const { return trades_; }
    std::vector<Row> dump() const {
        std::vector<Resting> bids, asks;
        for (const Resting& r : v_) (r.isBuy ? bids : asks).push_back(r);
        std::sort(bids.begin(), bids.end(), [](const Resting& a, const Resting& b) {
            return a.price != b.price ? a.price > b.price : a.seq < b.seq; });
        std::sort(asks.begin(), asks.end(), [](const Resting& a, const Resting& b) {
            return a.price != b.price ? a.price < b.price : a.seq < b.seq; });
        std::vector<Row> out;
        for (const Resting& r : bids) out.push_back({true, r.price, r.id, r.qty});
        for (const Resting& r : asks) out.push_back({false, r.price, r.id, r.qty});
        return out;
    }
    const std::vector<Resting>& resting() const { return v_; }
private:
    std::vector<Resting>     v_;
    std::vector<TradeRec>    trades_;
    std::uint64_t            seq_ = 0;
};

std::vector<Row> dumpEngine(const Engine& e) {
    std::vector<Row> out;
    e.forEachOrder([&](bool isBuy, int price, std::uint32_t id, int qty) {
        out.push_back({isBuy, price, id, qty});
    });
    return out;
}

struct Generator {
    Rng           rng;
    int           mode;
    std::uint32_t nextId = 1;
    std::vector<std::uint32_t> issued;  // every id ever used (some are stale)

    Generator(std::uint64_t seed, int m) : rng(seed), mode(m) {}

    Side randSide() { return rng.below(2) ? Side::Buy : Side::Sell; }

    Op cancelOp(const Reference& ref) {
        const auto& rest = ref.resting();
        if (!rest.empty() && rng.percent(75))
            return makeCancel(rest[rng.below(static_cast<std::uint32_t>(rest.size()))].id);
        if (!issued.empty())  // stale or unknown id: must be a harmless no-op
            return makeCancel(issued[rng.below(static_cast<std::uint32_t>(issued.size()))]);
        return makeCancel(999999);
    }

    Op next(int step, const Reference& ref) {
        std::uint32_t cancelPct = 35;
        if (mode == 3) cancelPct = 15;
        if (mode == 4) cancelPct = ((step / 60) % 2 == 0) ? 0 : 100;
        const bool wantCancel = rng.percent(cancelPct);

        if (mode == 5) {  // tiny id pool with reuse
            std::vector<std::uint32_t> freeIds;
            for (std::uint32_t id = 1; id <= 30; ++id) if (!ref.live(id)) freeIds.push_back(id);
            if (wantCancel || freeIds.empty())
                return makeCancel(static_cast<std::uint32_t>(rng.range(1, 30)));
            const std::uint32_t id = freeIds[rng.below(static_cast<std::uint32_t>(freeIds.size()))];
            return makeAdd(randSide(), id, rng.range(97, 103), rng.range(1, 9));
        }
        if (wantCancel && !(mode == 4 && ref.resting().empty())) return cancelOp(ref);

        const std::uint32_t id = nextId++;
        issued.push_back(id);
        const Side side = randSide();
        switch (mode) {
            case 1:  return makeAdd(side, id, 100, rng.range(1, 9));
            case 2:  return makeAdd(side, id, 100 + static_cast<int>(rng.below(2)), rng.range(1, 9));
            case 3:
                if (rng.percent(6))  // big taker through everything on the other side
                    return makeAdd(side, id, side == Side::Buy ? 130 : 70, rng.range(40, 120));
                return makeAdd(side, id, rng.range(80, 120), rng.range(1, 9));
            default: return makeAdd(side, id, rng.range(90, 110), rng.range(1, 9));
        }
    }
};

std::string describe(const Op& op) {
    char buf[96];
    if (op.kind == Op::Cancel) std::snprintf(buf, sizeof buf, "cancel id=%u", op.id);
    else std::snprintf(buf, sizeof buf, "add    id=%u %s px=%d qty=%d", op.id,
                       op.side == Side::Buy ? "BUY " : "SELL", op.price, op.qty);
    return buf;
}

// Returns an empty string when engine and reference agree, else a description.
std::string compare(const Engine& e, const Reference& r, std::size_t& tradesChecked, bool full) {
    char buf[200];
    if (e.orderCount() != r.orderCount()) {
        std::snprintf(buf, sizeof buf, "orderCount engine=%llu reference=%llu", static_cast<unsigned long long>(e.orderCount()), static_cast<unsigned long long>(r.orderCount()));
        return buf;
    }
    if (e.tradeCount() != r.trades().size()) {
        std::snprintf(buf, sizeof buf, "tradeCount engine=%llu reference=%llu", static_cast<unsigned long long>(e.tradeCount()), static_cast<unsigned long long>(r.trades().size()));
        return buf;
    }
    for (; tradesChecked < r.trades().size(); ++tradesChecked) {
        const TradeRec a = e.trade(tradesChecked);
        const TradeRec& b = r.trades()[tradesChecked];
        if (!(a == b)) {
            std::snprintf(buf, sizeof buf,
                "trade #%llu engine{buy=%u sell=%u px=%d qty=%d} reference{buy=%u sell=%u px=%d qty=%d}",
                static_cast<unsigned long long>(tradesChecked), a.buyId, a.sellId, a.price, a.qty, b.buyId, b.sellId, b.price, b.qty);
            return buf;
        }
    }
    int pe = 0, pr = 0;
    const bool eb = e.bestBid(pe), rb = r.bestBid(pr);
    if (eb != rb || (eb && pe != pr)) {
        std::snprintf(buf, sizeof buf, "best bid engine=%s%d reference=%s%d", eb ? "" : "none/", pe, rb ? "" : "none/", pr);
        return buf;
    }
    const bool ea = e.bestAsk(pe), ra = r.bestAsk(pr);
    if (ea != ra || (ea && pe != pr)) {
        std::snprintf(buf, sizeof buf, "best ask engine=%s%d reference=%s%d", ea ? "" : "none/", pe, ra ? "" : "none/", pr);
        return buf;
    }
    if (full) {
        const std::vector<Row> a = dumpEngine(e), b = r.dump();
        if (a.size() != b.size()) return "book dump size differs";
        for (std::size_t i = 0; i < a.size(); ++i)
            if (!(a[i] == b[i])) {
                std::snprintf(buf, sizeof buf,
                    "book row %llu engine{%s px=%d id=%u qty=%d} reference{%s px=%d id=%u qty=%d}", static_cast<unsigned long long>(i),
                    a[i].isBuy ? "bid" : "ask", a[i].price, a[i].id, a[i].qty,
                    b[i].isBuy ? "bid" : "ask", b[i].price, b[i].id, b[i].qty);
                return buf;
            }
    }
    return {};
}

} // namespace

int main(int argc, char** argv) {
    long seeds = 1000, steps = 1000, seedStart = 1;
    int forcedMode = -1;
    bool verbose = false;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        auto val = [&](long& dst) { if (i + 1 < argc) dst = std::atol(argv[++i]); };
        if (!std::strcmp(a, "--seeds")) val(seeds);
        else if (!std::strcmp(a, "--steps")) val(steps);
        else if (!std::strcmp(a, "--seed-start")) val(seedStart);
        else if (!std::strcmp(a, "--mode")) { long m = -1; val(m); forcedMode = static_cast<int>(m); }
        else if (!std::strcmp(a, "--verbose")) verbose = true;
        else { std::fprintf(stderr, "unknown argument: %s\n", a); return 2; }
    }
    if (forcedMode > 5) { std::fprintf(stderr, "--mode must be 0..5\n"); return 2; }

    std::size_t totalTrades = 0, totalOps = 0;
    for (long s = seedStart; s < seedStart + seeds; ++s) {
        const int mode = forcedMode >= 0 ? forcedMode : static_cast<int>(s % 6);
        Generator gen(static_cast<std::uint64_t>(s), mode);
        Engine engine(4096);
        Reference ref;
        std::size_t tradesChecked = 0;
        std::vector<Op> history;
        history.reserve(static_cast<std::size_t>(steps));

        for (long step = 0; step < steps; ++step) {
            const Op op = gen.next(static_cast<int>(step), ref);
            history.push_back(op);
            if (op.kind == Op::Add) { engine.add(op); ref.add(op); }
            else { engine.cancel(op.id); ref.cancel(op.id); }

            const bool full = (step % 25 == 0) || step == steps - 1;
            const std::string err = compare(engine, ref, tradesChecked, full);
            if (!err.empty()) {
                std::printf("DIVERGENCE: seed=%ld mode=%d step=%ld\n  %s\n  last operations:\n", s, mode, step, err.c_str());
                const std::size_t from = history.size() > 15 ? history.size() - 15 : 0;
                for (std::size_t k = from; k < history.size(); ++k)
                    std::printf("    [%llu] %s\n", static_cast<unsigned long long>(k), describe(history[k]).c_str());
                std::printf("  reproduce: fuzz_oracle --seed-start %ld --seeds 1 --steps %ld --mode %d\n", s, steps, mode);
                return 1;
            }
        }
        totalTrades += ref.trades().size();
        totalOps += history.size();
        if (verbose)
            std::printf("seed %ld mode %d ok (%llu trades, %llu resting)\n", s, mode, static_cast<unsigned long long>(ref.trades().size()), static_cast<unsigned long long>(ref.orderCount()));
    }
    std::printf("OK: %ld seeds x %ld steps (%llu operations, %llu trades) - engine matches reference\n",
                seeds, steps, static_cast<unsigned long long>(totalOps), static_cast<unsigned long long>(totalTrades));
    return 0;
}
