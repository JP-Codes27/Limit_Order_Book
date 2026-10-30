// Benchmark harness v2 for the limit order book.
//
// What it fixes compared with bench_replay.cpp (see the review, Part B):
//   * rdtscp-based timer, calibrated against steady_clock, overhead subtracted (no 100 ns ticks)
//   * throughput measured in batches of 64 operations (timer cost amortized to <1 ns/op)
//   * 21 runs by default, reports min / median / MAD (no "p95 of 5 runs")
//   * interleaved A/B across engines (alternating order each round)
//   * realistic workloads with a seeded, self-implemented PRNG; random-order cancels; many levels
//   * engine-agnostic canonical checksum (trade stream + final book), compared across runs AND engines
//   * per-operation latency for "first replay" and "steady" state, with a noise-floor (hiccup) meter
//   * environment header, optional core pinning, UTF-8 output file (--out), no PowerShell UTF-16 trap
//
// Usage: bench_v2 [--runs N] [--quick] [--only SUBSTR] [--no-tail] [--no-pin] [--core N]
//                 [--hiccup-seconds S] [--out FILE]
// Exit code: 0 ok, 2 checksum mismatch between runs or engines.
//
// To compare engines: write an adapter (see tests/support/engine_adapter_v1.hpp) and add one
// line to makeEngines() below.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "support/canonical.hpp"
#include "support/check.hpp"
#include "support/engine_adapter_v1.hpp"
#include "support/rng.hpp"
#include "support/types.hpp"

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
  #define LOB_X86 1
  #if defined(_MSC_VER)
    #include <intrin.h>
  #else
    #include <x86intrin.h>
    #include <cpuid.h>
  #endif
#endif
#if defined(_WIN32)
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
#elif defined(__linux__)
  #include <sched.h>
#endif

#ifndef LOB_GIT_COMMIT
  #define LOB_GIT_COMMIT "unknown"
#endif
#ifndef LOB_BUILD_TYPE
  #define LOB_BUILD_TYPE "unknown"
#endif

using namespace lobtest;

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Warray-bounds"  // false positive on the single-engine vector in makeEngines()
#endif

namespace {

// ----------------------------------------------------------------------------- output
std::FILE* g_file = nullptr;
void emit(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void emit(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    std::fputs(buf, stdout);
    if (g_file) std::fputs(buf, g_file);
}

// ----------------------------------------------------------------------------- timer
struct Timer {
    double        nsPerTick = 1.0;
    std::uint64_t overhead = 0;      // ticks consumed by begin()+end() back to back
    double        granularityNs = 0; // smallest non-zero observed tick delta
    bool          tsc = false;

    static inline std::uint64_t begin() {
#ifdef LOB_X86
        _mm_lfence();
        return __rdtsc();
#else
        return static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
    }
    static inline std::uint64_t end() {
#ifdef LOB_X86
        unsigned aux;
        const std::uint64_t t = __rdtscp(&aux);
        _mm_lfence();
        return t;
#else
        return static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
    }
    inline double ns(std::uint64_t ticks) const {
        return static_cast<double>(ticks > overhead ? ticks - overhead : 0) * nsPerTick;
    }

    void calibrate() {
        using C = std::chrono::steady_clock;
#ifdef LOB_X86
        tsc = true;
        const auto t0 = C::now();
        const std::uint64_t c0 = begin();
        while (C::now() - t0 < std::chrono::milliseconds(300)) {}
        const std::uint64_t c1 = end();
        const auto t1 = C::now();
        nsPerTick = std::chrono::duration<double, std::nano>(t1 - t0).count() / static_cast<double>(c1 - c0);
#else
        nsPerTick = static_cast<double>(C::period::num) * 1e9 / static_cast<double>(C::period::den);
#endif
        std::vector<std::uint64_t> d;
        for (int i = 0; i < 20000; ++i) { const std::uint64_t a = begin(); const std::uint64_t b = end(); d.push_back(b - a); }
        std::sort(d.begin(), d.end());
        overhead = d[d.size() / 2];
        std::uint64_t minNonZero = 0;
        for (int i = 0; i < 20000; ++i) {
            const std::uint64_t a = end(); const std::uint64_t b = end();
            if (b > a && (minNonZero == 0 || b - a < minNonZero)) minNonZero = b - a;
        }
        granularityNs = static_cast<double>(minNonZero) * nsPerTick;
    }
};

// ----------------------------------------------------------------------------- environment
std::string cpuBrand() {
#ifdef LOB_X86
    char brand[49] = {};
    unsigned r[4];
  #if defined(_MSC_VER)
    int q[4];
    for (unsigned i = 0; i < 3; ++i) { __cpuid(q, static_cast<int>(0x80000002u + i)); std::memcpy(brand + 16 * i, q, 16); }
  #else
    for (unsigned i = 0; i < 3; ++i) {
        if (!__get_cpuid(0x80000002u + i, &r[0], &r[1], &r[2], &r[3])) return "unknown";
        std::memcpy(brand + 16 * i, r, 16);
    }
  #endif
    std::string s(brand);
    while (!s.empty() && s.front() == ' ') s.erase(s.begin());
    return s;
#else
    return "non-x86";
#endif
}

bool invariantTsc() {
#if defined(LOB_X86) && !defined(_MSC_VER)
    unsigned a, b, c, d;
    if (!__get_cpuid(0x80000007u, &a, &b, &c, &d)) return false;
    return (d >> 8) & 1u;
#elif defined(LOB_X86)
    int q[4]; __cpuid(q, 0x80000007); return (q[3] >> 8) & 1;
#else
    return false;
#endif
}

int pinToCore(int requested) {  // returns core used, or -1
#if defined(_WIN32)
    DWORD_PTR proc = 0, sys = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys)) return -1;
    int pick = -1, seen = 0;
    for (int i = 0; i < static_cast<int>(8 * sizeof(DWORD_PTR)); ++i)
        if (proc & (static_cast<DWORD_PTR>(1) << i)) {
            if (requested >= 0 ? i == requested : seen == 1 || pick < 0) pick = i;
            ++seen;
        }
    if (pick < 0) return -1;
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    return SetThreadAffinityMask(GetCurrentThread(), static_cast<DWORD_PTR>(1) << pick) ? pick : -1;
#elif defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof set, &set) != 0) return -1;
    int pick = -1, seen = 0;
    for (int i = 0; i < CPU_SETSIZE; ++i)
        if (CPU_ISSET(i, &set)) {
            if (requested >= 0 ? i == requested : (seen == 1 || pick < 0)) pick = i;  // prefer 2nd core (core 0 handles interrupts)
            ++seen;
        }
    if (pick < 0) return -1;
    CPU_ZERO(&set);
    CPU_SET(pick, &set);
    return sched_setaffinity(0, sizeof set, &set) == 0 ? pick : -1;
#else
    (void)requested;
    return -1;
#endif
}

void printEnvironment(const Timer& tm, int core, bool pinRequested) {
    emit("=== Environment ===\n");
#if defined(__clang__)
    emit("compiler      : clang %s\n", __clang_version__);
#elif defined(__GNUC__)
    emit("compiler      : gcc %s\n", __VERSION__);
#elif defined(_MSC_VER)
    emit("compiler      : msvc %d\n", _MSC_VER);
#endif
    emit("c++ standard  : %ld\n", static_cast<long>(__cplusplus));
    emit("build type    : %s   git: %s\n", LOB_BUILD_TYPE, LOB_GIT_COMMIT);
#if defined(NDEBUG)
    emit("NDEBUG        : defined\n");
#else
    emit("NDEBUG        : NOT defined (asserts active)\n");
#endif
#if defined(_WIN32)
    emit("os            : windows\n");
#elif defined(__linux__)
    emit("os            : linux\n");
#else
    emit("os            : other\n");
#endif
    emit("cpu           : %s (%u logical)\n", cpuBrand().c_str(), std::thread::hardware_concurrency());
    emit("invariant TSC : %s\n", invariantTsc() ? "yes" : "NO/unknown (rdtsc may drift)");
    emit("timer         : %s, %.4f ns/tick, call overhead %.1f ns, granularity %.1f ns\n",
         tm.tsc ? "rdtscp+lfence" : "steady_clock", tm.nsPerTick,
         static_cast<double>(tm.overhead) * tm.nsPerTick, tm.granularityNs);
    if (core >= 0) emit("pinning       : core %d\n", core);
    else emit("pinning       : %s\n", pinRequested ? "FAILED (results will be noisier)" : "disabled");
    bool optimized = true;
#if defined(__GNUC__) && !defined(__OPTIMIZE__)
    optimized = false;
#endif
#if defined(_MSC_VER) && defined(_DEBUG)
    optimized = false;
#endif
    if (!optimized) emit("WARNING       : unoptimized build - these numbers are meaningless. Use the release preset.\n");
    if (tm.granularityNs > 25.0) emit("WARNING       : timer granularity %.0f ns is coarse; per-op percentiles below ~%.0f ns are unreliable.\n",
                                      tm.granularityNs, tm.granularityNs * 10);
    emit("\n");
}

// ----------------------------------------------------------------------------- statistics
struct Stats { double min, median, mad, max; };

double medianOf(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    const std::size_t n = v.size();
    return n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}
Stats summarize(const std::vector<double>& v) {
    Stats s{};
    s.min = *std::min_element(v.begin(), v.end());
    s.max = *std::max_element(v.begin(), v.end());
    s.median = medianOf(v);
    std::vector<double> dev;
    for (double x : v) dev.push_back(std::fabs(x - s.median));
    s.mad = medianOf(dev);
    return s;
}

// ----------------------------------------------------------------------------- workloads
struct Workload {
    std::string     name;
    std::string     description;
    std::vector<Op> prefill;  // applied outside the timed region
    std::vector<Op> timed;
};

constexpr std::int32_t kMid = 100000;

std::vector<Op> makePassiveBook(Rng& rng, std::uint32_t& nextId, int orders, int spreadTicks) {
    std::vector<Op> ops;
    ops.reserve(static_cast<std::size_t>(orders));
    for (int i = 0; i < orders; ++i) {
        const bool buy = rng.below(2) != 0;
        const std::int32_t off = static_cast<std::int32_t>(rng.geometric(13000, 400));
        ops.push_back(makeAdd(buy ? Side::Buy : Side::Sell, nextId++,
                              buy ? kMid - spreadTicks - off : kMid + spreadTicks + off, rng.range(1, 10)));
    }
    return ops;
}

// Steady-state mixed flow: mostly passive adds near the touch, cancels of random LIVE orders,
// some marketable orders and rare sweeps. A shadow V1 engine (the oracle) is run while generating,
// so the generator knows which orders are really still resting (fills are tracked per id) and can
// steer the book size towards `targetLive`. Without this, fills silently drain the book and the
// workload stops being "steady state".
Workload makeRealistic(std::size_t timedOps, int targetLive) {
    Workload w{"realistic-mixed", "~54% add / ~34% cancel of a live order / ~10% marketable / 2% sweep; steady ~targetLive resting orders", {}, {}};
    Rng rng(0xA11CE);
    std::uint32_t nextId = 1;
    const std::size_t maxIds = timedOps + static_cast<std::size_t>(targetLive) + 16;
    std::vector<std::int32_t> remaining(maxIds + 1, 0);   // unfilled quantity per id (0 = gone)
    std::vector<std::int32_t> posOf(maxIds + 1, -1);       // index in `live` or -1
    std::vector<std::uint32_t> live;
    V1Adapter shadow(maxIds);
    std::size_t tradesSeen = 0;

    auto erase = [&](std::uint32_t id) {
        const std::int32_t p = posOf[id];
        if (p < 0) return;
        live[static_cast<std::size_t>(p)] = live.back();
        posOf[live.back()] = p;
        live.pop_back();
        posOf[id] = -1;
    };
    // Apply an op to the shadow engine, then update fill bookkeeping from the trades it emitted.
    auto commit = [&](const Op& op, std::vector<Op>& out) {
        out.push_back(op);
        if (op.kind == Op::Cancel) { shadow.cancel(op.id); remaining[op.id] = 0; erase(op.id); return; }
        remaining[op.id] = op.qty;
        shadow.add(op);
        for (; tradesSeen < shadow.tradeCount(); ++tradesSeen) {
            const TradeRec t = shadow.trade(tradesSeen);
            remaining[t.buyId] -= t.qty;
            remaining[t.sellId] -= t.qty;
            if (remaining[t.buyId] == 0) erase(t.buyId);
            if (remaining[t.sellId] == 0) erase(t.sellId);
        }
        if (remaining[op.id] > 0) { posOf[op.id] = static_cast<std::int32_t>(live.size()); live.push_back(op.id); }
    };

    for (const Op& o : makePassiveBook(rng, nextId, targetLive, 1)) commit(o, w.prefill);
    std::int32_t mid = kMid;
    w.timed.reserve(timedOps);
    while (w.timed.size() < timedOps) {
        if (w.timed.size() % 256 == 0) mid += rng.range(-1, 1);
        std::uint32_t r = rng.below(100);
        const std::size_t n = live.size();
        if (n > static_cast<std::size_t>(targetLive) * 12 / 10) r = 60;          // too big: cancel
        else if (n < static_cast<std::size_t>(targetLive) * 9 / 10) r = rng.percent(90) ? 10 : 95;  // too small: add
        const bool buy = rng.below(2) != 0;
        if (r < 54) {
            std::int32_t off = static_cast<std::int32_t>(rng.geometric(13000, 400));
            if (rng.percent(5)) off = rng.range(0, 300);
            std::int32_t qty = rng.range(1, 9);
            if (rng.percent(10)) qty *= 10;
            commit(makeAdd(buy ? Side::Buy : Side::Sell, nextId++, buy ? mid - 1 - off : mid + 1 + off, qty), w.timed);
        } else if (r < 88) {
            if (live.empty()) continue;
            commit(makeCancel(live[rng.below(static_cast<std::uint32_t>(live.size()))]), w.timed);
        } else if (r < 98) {
            commit(makeAdd(buy ? Side::Buy : Side::Sell, nextId++,
                           buy ? mid + 1 + rng.range(0, 7) : mid - 1 - rng.range(0, 7), rng.range(1, 3)), w.timed);
        } else {
            commit(makeAdd(buy ? Side::Buy : Side::Sell, nextId++, buy ? mid + 30 : mid - 30, rng.range(10, 50)), w.timed);
        }
    }
    return w;
}

// N resting orders spread uniformly over `levels` price levels (buy side), timed = inserting them.
Workload makeInsertRandom(int levels, int n) {
    Workload w{"insert-random-" + std::to_string(levels), "insert into an empty book, uniformly random price over L levels", {}, {}};
    Rng rng(0xBEEF + static_cast<std::uint64_t>(levels));
    for (int i = 0; i < n; ++i)
        w.timed.push_back(makeAdd(Side::Buy, static_cast<std::uint32_t>(i + 1), kMid - rng.range(0, levels - 1), rng.range(1, 10)));
    return w;
}

// Same book, cancelled in a random permutation (the cache-hostile case V1's FIFO cancel benchmark hides).
Workload makeCancelRandom(int levels, int n) {
    Workload w{"cancel-random-" + std::to_string(levels), "cancel all orders in random order (book prebuilt, L levels)", {}, {}};
    Rng rng(0xCAFE + static_cast<std::uint64_t>(levels));
    std::vector<std::uint32_t> ids;
    for (int i = 0; i < n; ++i) {
        w.prefill.push_back(makeAdd(Side::Buy, static_cast<std::uint32_t>(i + 1), kMid - rng.range(0, levels - 1), rng.range(1, 10)));
        ids.push_back(static_cast<std::uint32_t>(i + 1));
    }
    for (std::size_t i = ids.size(); i > 1; --i) std::swap(ids[i - 1], ids[rng.below(static_cast<std::uint32_t>(i))]);
    for (std::uint32_t id : ids) w.timed.push_back(makeCancel(id));
    return w;
}

// ~1000 live ask levels (3 one-lot orders each); every cycle adds 150 orders and sweeps 150 lots (50 levels).
Workload makeSweep() {
    Workload w{"deep-sweep", "taker crossing 50 levels (150 fills) per sweep; ~1000 live levels; level erase heavy", {}, {}};
    std::uint32_t id = 1;
    std::int32_t topPrice = kMid + 1;
    for (int lvl = 0; lvl < 1000; ++lvl, ++topPrice)
        for (int k = 0; k < 3; ++k) w.prefill.push_back(makeAdd(Side::Sell, id++, topPrice, 1));
    for (int cycle = 0; cycle < 1500; ++cycle) {
        for (int lvl = 0; lvl < 50; ++lvl, ++topPrice)
            for (int k = 0; k < 3; ++k) w.timed.push_back(makeAdd(Side::Sell, id++, topPrice, 1));
        w.timed.push_back(makeAdd(Side::Buy, id++, kMid + 100000, 150));
    }
    return w;
}

// A new price level is created and destroyed every cycle, plus two marketable one-lots.
Workload makeChurn(int cycles) {
    Workload w{"level-churn", "per cycle: add at new level, cancel it, 2 marketable one-lots (3 live levels)", {}, {}};
    std::uint32_t id = 1;
    for (int i = 0; i < cycles; ++i) {
        w.prefill.push_back(makeAdd(Side::Buy, id++, kMid - 1, 1));
        w.prefill.push_back(makeAdd(Side::Sell, id++, kMid + 1, 1));
    }
    std::uint32_t tmp = 1u << 24;
    for (int i = 0; i < cycles; ++i) {
        w.timed.push_back(makeAdd(Side::Buy, tmp, kMid - 2, 1));
        w.timed.push_back(makeCancel(tmp));
        ++tmp;
        w.timed.push_back(makeAdd(Side::Buy, id++, kMid + 1, 1));
        w.timed.push_back(makeAdd(Side::Sell, id++, kMid - 1, 1));
    }
    return w;
}

// ----------------------------------------------------------------------------- replay
template <class E>
inline void apply(E& e, const Op& op) {
    if (op.kind == Op::Add) e.add(op); else e.cancel(op.id);
}

struct RunResult {
    double        nsPerOp;
    std::uint64_t checksum;
    std::size_t   restingAtEnd;
    std::size_t   trades;
};

constexpr std::size_t kBatch = 64;

template <class E>
RunResult runBatched(const Workload& w, const Timer& tm) {
    E e(w.prefill.size() + w.timed.size() + 16);
    for (const Op& op : w.prefill) apply(e, op);
    double totalNs = 0;
    const std::size_t n = w.timed.size();
    for (std::size_t i = 0; i < n;) {
        const std::size_t stop = std::min(n, i + kBatch);
        const std::uint64_t a = Timer::begin();
        for (; i < stop; ++i) apply(e, w.timed[i]);
        const std::uint64_t b = Timer::end();
        totalNs += tm.ns(b - a);
    }
    return {totalNs / static_cast<double>(n), canonicalHash(e), e.orderCount(), e.tradeCount()};
}

template <class E>
void runPerOp(const Workload& w, const Timer& tm, std::vector<float>& samples) {
    E e(w.prefill.size() + w.timed.size() + 16);
    for (const Op& op : w.prefill) apply(e, op);
    samples.clear();
    samples.reserve(w.timed.size());
    for (const Op& op : w.timed) {
        const std::uint64_t a = Timer::begin();
        apply(e, op);
        const std::uint64_t b = Timer::end();
        samples.push_back(static_cast<float>(tm.ns(b - a)));
    }
}

struct EngineEntry {
    std::string name;
    RunResult (*batched)(const Workload&, const Timer&);
    void (*perOp)(const Workload&, const Timer&, std::vector<float>&);
};

// ----- add new engines here (one line each) -----
std::vector<EngineEntry> makeEngines() {
    return {
        {V1Adapter::kName, &runBatched<V1Adapter>, &runPerOp<V1Adapter>},
        // {V2Adapter::kName, &runBatched<V2Adapter>, &runPerOp<V2Adapter>},
    };
}

// ----------------------------------------------------------------------------- reporting
void printTail(const char* label, std::vector<float>& s, double noiseFloorNs) {
    std::sort(s.begin(), s.end());
    const std::size_t n = s.size();
    auto q = [&](double p) { return static_cast<double>(s[std::min(n - 1, static_cast<std::size_t>(p * static_cast<double>(n)))]); };
    auto ok = [&](double p) { return static_cast<double>(n) * (1.0 - p) >= 10.0; };  // >=10 samples beyond the quantile
    double mean = 0;
    for (float x : s) mean += static_cast<double>(x);
    mean /= static_cast<double>(n);
    emit("    %-22s n=%llu mean=%.0f  p50=%.0f  p90=%.0f  p95=%.0f  p99=%.0f", label, static_cast<unsigned long long>(n), mean, q(0.5), q(0.9), q(0.95), q(0.99));
    if (ok(0.999)) emit("  p99.9=%.0f", q(0.999)); else emit("  p99.9=n/a");
    if (ok(0.9999)) emit("  p99.99=%.0f", q(0.9999)); else emit("  p99.99=n/a");
    emit("  max=%.0f ns%s\n", static_cast<double>(s.back()), static_cast<double>(s.back()) <= noiseFloorNs ? "  (<= OS noise floor)" : "");
    // log2 histogram
    static const double edges[] = {32, 64, 128, 256, 512, 1024, 4096, 16384, 65536, 262144, 1048576};
    std::size_t counts[12] = {};
    for (float x : s) { std::size_t b = 0; while (b < 11 && static_cast<double>(x) >= edges[b]) ++b; ++counts[b]; }
    emit("      histogram (ns):");
    const char* names[] = {"<32", "32-64", "64-128", "128-256", "256-512", "512-1k", "1k-4k", "4k-16k", "16k-64k", "64k-256k", "256k-1M", ">=1M"};
    for (int b = 0; b < 12; ++b) if (counts[b]) emit("  %s:%llu", names[b], static_cast<unsigned long long>(counts[b]));
    emit("\n");
}

struct Noise { double maxGapNs; std::size_t over1us, over10us, over100us; };

Noise measureNoise(const Timer& tm, double seconds) {
    Noise nz{};
    const std::uint64_t spanTicks = static_cast<std::uint64_t>(seconds * 1e9 / tm.nsPerTick);
    std::uint64_t start = Timer::begin(), prev = start;
    for (;;) {
        const std::uint64_t now = Timer::end();
        const double gap = static_cast<double>(now - prev) * tm.nsPerTick;
        if (gap > 1000) { ++nz.over1us; if (gap > 10000) ++nz.over10us; if (gap > 100000) ++nz.over100us; }
        if (gap > nz.maxGapNs) nz.maxGapNs = gap;
        prev = now;
        if (now - start > spanTicks) break;
    }
    return nz;
}

} // namespace

int main(int argc, char** argv) {
    int runs = 21, core = -1;
    bool quick = false, tail = true, pin = true;
    double hiccupSeconds = 2.0;
    std::string only, outPath;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--runs") runs = std::atoi(next());
        else if (a == "--quick") quick = true;
        else if (a == "--only") only = next();
        else if (a == "--no-tail") tail = false;
        else if (a == "--no-pin") pin = false;
        else if (a == "--core") core = std::atoi(next());
        else if (a == "--hiccup-seconds") hiccupSeconds = std::atof(next());
        else if (a == "--out") outPath = next();
        else { std::fprintf(stderr, "unknown argument: %s\n", a.c_str()); return 2; }
    }
    if (quick) runs = 5;
    if (runs < 3) runs = 3;
    if (!outPath.empty()) { g_file = std::fopen(outPath.c_str(), "wb"); if (!g_file) { std::fprintf(stderr, "cannot open %s\n", outPath.c_str()); return 2; } }

    const int usedCore = pin ? pinToCore(core) : -1;
    Timer tm;
    tm.calibrate();
    printEnvironment(tm, usedCore, pin);

    const Noise nz = measureNoise(tm, quick ? 0.5 : hiccupSeconds);
    emit("=== OS noise floor (idle spin, %.1f s) ===\nlargest gap %.1f us; gaps >1us: %llu, >10us: %llu, >100us: %llu\n"
         "Tail latencies at or below the largest gap are not a property of the engine.\n\n",
         quick ? 0.5 : hiccupSeconds, nz.maxGapNs / 1000.0, static_cast<unsigned long long>(nz.over1us), static_cast<unsigned long long>(nz.over10us), static_cast<unsigned long long>(nz.over100us));

    const std::size_t scale = quick ? 4 : 1;
    std::vector<Workload> all;
    all.push_back(makeRealistic(400000 / scale, 20000));
    all.push_back(makeInsertRandom(256, 200000 / static_cast<int>(scale)));
    all.push_back(makeInsertRandom(65536, 200000 / static_cast<int>(scale)));
    all.push_back(makeCancelRandom(256, 200000 / static_cast<int>(scale)));
    all.push_back(makeCancelRandom(65536, 200000 / static_cast<int>(scale)));
    all.push_back(makeSweep());
    all.push_back(makeChurn(50000 / static_cast<int>(scale)));

    const std::vector<EngineEntry> engines = makeEngines();
    emit("=== Throughput: %d interleaved runs, batches of %llu ops, 2 warm-up rounds ===\n", runs, static_cast<unsigned long long>(kBatch));
    emit("ns/op: min / median / MAD.  Lower is better.  checksum = canonical hash of trades + final book\n\n");

    int exitCode = 0;
    for (const Workload& w : all) {
        if (!only.empty() && w.name.find(only) == std::string::npos) continue;
        emit("[%s] %s\n    timed ops=%llu  prefill ops=%llu\n", w.name.c_str(), w.description.c_str(), static_cast<unsigned long long>(w.timed.size()), static_cast<unsigned long long>(w.prefill.size()));

        // Cold-ish: first replay of this workload, per-op timing (see README note on true cold start).
        std::vector<std::vector<float>> first(engines.size());
        if (tail) for (std::size_t e = 0; e < engines.size(); ++e) engines[e].perOp(w, tm, first[e]);

        std::vector<std::vector<double>> times(engines.size());
        std::vector<std::uint64_t> checksum(engines.size(), 0);
        RunResult last{};
        for (int round = -2; round < runs; ++round) {  // negative rounds = warm-up
            for (std::size_t k = 0; k < engines.size(); ++k) {
                const std::size_t e = (round % 2 == 0) ? k : engines.size() - 1 - k;  // alternate order
                const RunResult r = engines[e].batched(w, tm);
                if (round < 0) { checksum[e] = r.checksum; continue; }
                if (r.checksum != checksum[e]) {
                    emit("    !! %s: checksum changed between runs (non-deterministic engine)\n", engines[e].name.c_str());
                    exitCode = 2;
                }
                times[e].push_back(r.nsPerOp);
                last = r;
            }
        }
        for (std::size_t e = 0; e < engines.size(); ++e) {
            const Stats s = summarize(times[e]);
            emit("    %-30s %8.1f / %8.1f / %6.1f ns/op   (%.2f Mops/s at median)   checksum=0x%016llx\n",
                 engines[e].name.c_str(), s.min, s.median, s.mad, 1e3 / s.median, static_cast<unsigned long long>(checksum[e]));
        }
        if (engines.size() > 1) {
            bool same = true;
            for (std::size_t e = 1; e < engines.size(); ++e) same = same && checksum[e] == checksum[0];
            if (!same) { emit("    !! CHECKSUM MISMATCH BETWEEN ENGINES - an engine is not behaving identically\n"); exitCode = 2; }
            for (std::size_t e = 1; e < engines.size(); ++e)
                emit("    speedup %s vs %s (median): %.2fx\n", engines[e].name.c_str(), engines[0].name.c_str(),
                     summarize(times[0]).median / summarize(times[e]).median);
        }
        emit("    book at end: %llu resting orders, %llu trades\n", static_cast<unsigned long long>(last.restingAtEnd), static_cast<unsigned long long>(last.trades));

        if (tail) {
            for (std::size_t e = 0; e < engines.size(); ++e) {
                emit("  per-operation latency, %s (timer overhead subtracted, resolution ~%.0f ns):\n", engines[e].name.c_str(), tm.granularityNs);
                printTail("first replay", first[e], nz.maxGapNs);
                for (int i = 0; i < 3; ++i) (void)engines[e].batched(w, tm);  // warm heap/caches
                std::vector<float> steady;
                engines[e].perOp(w, tm, steady);
                printTail("steady (warmed)", steady, nz.maxGapNs);
            }
        }
        emit("\n");
    }
    emit("Notes: 'first replay' = fresh book, allocator warmed only by earlier workloads in this process;\n"
         "for a true cold start run a fresh process with --only <workload>.\n");
    if (g_file) std::fclose(g_file);
    return exitCode;
}
