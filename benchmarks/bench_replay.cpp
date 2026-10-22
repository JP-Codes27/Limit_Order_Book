#include<bits/stdc++.h>
#include "order_book.hpp"

namespace {

// =============================================================================
// Engine Under Test
// To benchmark a new engine (e.g. OrderBookV2), change this single alias:
using Engine = OrderBook;
// =============================================================================

using Clock = std::chrono::steady_clock;
using Nanoseconds = std::chrono::nanoseconds;

// These defaults complete quickly on a development machine while being large
// enough to exercise price-level and order-index allocation paths.  Keep them
// fixed when comparing different OrderBook implementations.
constexpr int kWarmupRuns = 1;
constexpr int kMeasuredRuns = 5;
constexpr int kRestingOrders = 200'000;
constexpr int kMatchingOrders = 100'000;
constexpr int kCancellationOrders = 200'000;
constexpr int kMixedOperations = 200'000; // Must remain divisible by four.
constexpr int kBasePrice = 10'000;

struct Measurement {
    double elapsedNs;
    std::uint64_t checksum;
};

[[noreturn]] void fail(std::string_view message) {
    throw std::runtime_error(std::string(message));
}

void require(bool condition, std::string_view message) {
    if (!condition) {
        fail(message);
    }
}
std::uint64_t mix(std::uint64_t value, std::uint64_t input) {
    // A small deterministic hash combiner. It makes accidental state changes
    // visible without adding work to a timed region.
    value ^= input + 0x9e3779b97f4a7c15ULL + (value << 6U) + (value >> 2U);
    return value;
}

std::uint64_t checksum(const Engine& book) {
    std::uint64_t result = mix(0, book.orderCount());

    const auto addSide = [&result](const auto& side) {
        for (const auto& [price, orders] : side) {
            result = mix(result, static_cast<std::uint64_t>(price));
            result = mix(result, orders.size());
            for (const Order& order : orders) {
                result = mix(result, static_cast<std::uint64_t>(order.orderId));
                result = mix(result, static_cast<std::uint64_t>(order.limit));
                result = mix(result, static_cast<std::uint64_t>(order.shares));
            }
        }
    };

    addSide(book.getBids());
    addSide(book.getAsks());
    for (const Trade& trade : book.getTrades()) {
        result = mix(result, static_cast<std::uint64_t>(trade.tradeId));
        result = mix(result, static_cast<std::uint64_t>(trade.buyId));
        result = mix(result, static_cast<std::uint64_t>(trade.sellId));
        result = mix(result, static_cast<std::uint64_t>(trade.price));
        result = mix(result, static_cast<std::uint64_t>(trade.shares));
    }
    return result;
}

template <typename Setup, typename TimedWork, typename Validate>
std::vector<Measurement> measure(Setup&& setup, TimedWork&& timedWork, Validate&& validate) {
    for (int run = 0; run < kWarmupRuns; ++run) {
        auto state = setup();
        timedWork(state);
        static_cast<void>(validate(state));
    }

    std::vector<Measurement> measurements;
    measurements.reserve(kMeasuredRuns);
    for (int run = 0; run < kMeasuredRuns; ++run) {
        auto state = setup();
        const auto start = Clock::now();
        timedWork(state);
        const auto stop = Clock::now();
        const std::uint64_t result = validate(state);
        measurements.push_back({
            static_cast<double>(std::chrono::duration_cast<Nanoseconds>(stop - start).count()),
            result,
        });
    }
    return measurements;
}

void printSummary(std::string_view name, std::size_t operations,
                  const std::vector<Measurement>& measurements) {
    std::vector<double> times;
    times.reserve(measurements.size());
    std::uint64_t combinedChecksum = 0;
    for (const Measurement& measurement : measurements) {
        times.push_back(measurement.elapsedNs);
        combinedChecksum = mix(combinedChecksum, measurement.checksum);
    }
    std::sort(times.begin(), times.end());

    const double meanNs = std::accumulate(times.begin(), times.end(), 0.0) / times.size();
    const double medianNs = times[times.size() / 2];
    const std::size_t p95Index = static_cast<std::size_t>(
        std::ceil(0.95 * static_cast<double>(times.size())) - 1.0);
    const double p95Ns = times[p95Index];
    const double operationsPerSecond = static_cast<double>(operations) * 1e9 / medianNs;

    std::cout << std::left << std::setw(22) << name
              << " ops=" << std::setw(7) << operations
              << " median=" << std::fixed << std::setprecision(2) << medianNs / 1e6 << " ms"
              << " mean=" << meanNs / 1e6 << " ms"
              << " p95=" << p95Ns / 1e6 << " ms"
              << " latency=" << medianNs / operations << " ns/op"
              << " throughput=" << operationsPerSecond / 1e6 << " Mops/s"
              << " checksum=0x" << std::hex << combinedChecksum << std::dec << '\n';
}

std::vector<Order> makeRestingOrders(int count, int firstId, orderType side) {
    std::vector<Order> orders;
    orders.reserve(count);
    for (int index = 0; index < count; ++index) {
        // A deterministic, non-random price distribution: 256 price levels
        // and varied quantities exercise both map and FIFO-list paths.
        const int price = kBasePrice + (index % 256) - 128;
        const int quantity = 1 + (index % 11);
        orders.push_back({firstId + index, side, price, quantity});
    }
    return orders;
}

const std::vector<Order>& restingOrders() {
    static const std::vector<Order> orders = makeRestingOrders(kRestingOrders, 1, orderType::buy);
    return orders;
}

void restingInsertion(Engine& book) {
    const std::vector<Order>& orders = restingOrders();
    for (const Order& order : orders) {
        book.insert(order);
    }
}

std::uint64_t validateRestingInsertion(const Engine& book) {
    require(book.orderCount() == restingOrders().size(), "resting insertion lost orders");
    require(book.getBestBid() == kBasePrice + 127, "resting insertion best bid is incorrect");
    return checksum(book);
}

Engine matchingSetup() {
    Engine book;
    for (int index = 0; index < kMatchingOrders; ++index) {
        book.insert({1 + index, orderType::sell, kBasePrice, 1});
    }
    return book;
}

void matchingHeavy(Engine& book) {
    for (int index = 0; index < kMatchingOrders; ++index) {
        book.match({1'000'000 + index, orderType::buy, kBasePrice, 1});
    }
}

std::uint64_t validateMatchingHeavy(const Engine& book) {
    require(book.orderCount() == 0, "matching-heavy workload left resting orders");
    require(book.getTrades().size() == static_cast<std::size_t>(kMatchingOrders),
            "matching-heavy workload trade count is incorrect");
    return checksum(book);
}

const std::vector<Order>& cancellationOrders() {
    static const std::vector<Order> orders = makeRestingOrders(kCancellationOrders, 1, orderType::buy);
    return orders;
}

Engine cancellationSetup() {
    Engine book;
    for (const Order& order : cancellationOrders()) {
        book.insert(order);
    }
    return book;
}

void cancellationHeavy(Engine& book) {
    const std::vector<Order>& orders = cancellationOrders();

    for (const Order& order : orders) {
        book.remove(order.orderId);
    }
}

std::uint64_t validateCancellationHeavy(const Engine& book) {
    require(book.orderCount() == 0, "cancellation-heavy workload left orders");
    require(book.getBids().empty(), "cancellation-heavy workload left bid levels");
    return checksum(book);
}

Engine mixedSetup() {
    static_assert(kMixedOperations % 4 == 0);
    const int cycles = kMixedOperations / 4;
    Engine book;

    // This preparation gives the taker operations known liquidity while the
    // add/cancel pair touches the order-id index and price-level lifecycle.
    for (int index = 0; index < cycles; ++index) {
        book.insert({1 + index, orderType::buy, kBasePrice - 1, 1});
        book.insert({1 + cycles + index, orderType::sell, kBasePrice + 1, 1});
    }
    return book;
}

void mixedFlow(Engine& book) {
    const int cycles = kMixedOperations / 4;

    for (int index = 0; index < cycles; ++index) {
        const int transientId = 1'000'000 + index;
        book.insert({transientId, orderType::buy, kBasePrice - 2, 1});
        book.remove(transientId);
        book.match({2'000'000 + index, orderType::buy, kBasePrice + 1, 1});
        book.match({3'000'000 + index, orderType::sell, kBasePrice - 1, 1});
    }
}

std::uint64_t validateMixedFlow(const Engine& book) {
    const int cycles = kMixedOperations / 4;
    require(book.orderCount() == 0, "mixed workload left orders");
    require(book.getTrades().size() == static_cast<std::size_t>(cycles * 2),
            "mixed workload trade count is incorrect");
    return checksum(book);
}
void scaleInsertion(Engine& book, const std::vector<Order>& orders) {
    for (const Order& order : orders) {
        book.insert(order);
    }
}

std::uint64_t validateScaleInsertion(const Engine& book, std::size_t expectedOrders) {
    require(book.orderCount() == expectedOrders, "scale insertion lost orders");
    return checksum(book);
}

constexpr int kLatencySamples = 100'000;

uint32_t estimateTimerResolution() {
    constexpr int kIters = 1'000;
    std::vector<uint32_t> deltas;
    deltas.reserve(kIters);
    for (int i = 0; i < kIters; ++i) {
        const auto t0 = Clock::now();
        const auto t1 = Clock::now();
        deltas.push_back(static_cast<uint32_t>(std::chrono::duration_cast<Nanoseconds>(t1 - t0).count()));
    }
    std::sort(deltas.begin(), deltas.end());
    return deltas[kIters / 2];
}

void printLatencyDistribution(std::string_view operationName, std::vector<uint32_t>& latencies) {
    if (latencies.empty()) return;
    std::sort(latencies.begin(), latencies.end());

    const std::size_t n = latencies.size();
    const double minNs = latencies.front();
    const double p50Ns = latencies[static_cast<std::size_t>(n * 0.50)];
    const double p90Ns = latencies[static_cast<std::size_t>(n * 0.90)];
    const double p99Ns = latencies[static_cast<std::size_t>(n * 0.99)];
    const double p99_9Ns = latencies[static_cast<std::size_t>(n * 0.999)];
    const double maxNs = latencies.back();
    const double meanNs = std::accumulate(latencies.begin(), latencies.end(), 0.0) / n;

    std::cout << "\n======================================================================\n"
              << " Tail Latency Profile: " << operationName << " (" << n << " samples)\n"
              << "======================================================================\n";
    std::cout << std::fixed << std::setprecision(1)
              << "  Min:    " << std::setw(8) << minNs << " ns\n"
              << "  p50:    " << std::setw(8) << p50Ns << " ns  (Median)\n"
              << "  Mean:   " << std::setw(8) << meanNs << " ns\n"
              << "  p90:    " << std::setw(8) << p90Ns << " ns\n"
              << "  p99:    " << std::setw(8) << p99Ns << " ns  (Tail 1%)\n"
              << "  p99.9:  " << std::setw(8) << p99_9Ns << " ns  (Tail 0.1%)\n"
              << "  Max:    " << std::setw(8) << maxNs << " ns\n\n";

    struct Bucket {
        uint32_t limitNs;
        std::string label;
        std::size_t count = 0;
    };
    std::vector<Bucket> buckets = {
        {100,      "       < 100 ns"},
        {200,      "  100 -  200 ns"},
        {400,      "  200 -  400 ns"},
        {800,      "  400 -  800 ns"},
        {1'500,    "  800 - 1500 ns"},
        {3'000,    "  1.5 -  3.0 us"},
        {10'000,   "  3.0 - 10.0 us"},
        {UINT32_MAX, "       > 10.0 us"}
    };

    for (uint32_t lat : latencies) {
        for (auto& b : buckets) {
            if (lat < b.limitNs) {
                b.count++;
                break;
            }
        }
    }

    std::cout << "  Histogram Distribution:\n";
    for (const auto& b : buckets) {
        double pct = (static_cast<double>(b.count) / n) * 100.0;
        int barWidth = static_cast<int>(pct / 2.0); // 50 chars = 100%
        std::string bar(barWidth, '#');
        std::cout << "    " << b.label << " : "
                  << std::setw(7) << b.count << " ("
                  << std::setw(5) << std::setprecision(1) << pct << "%) | "
                  << bar << "\n";
    }
}

void runTailLatencyBenchmarks() {
    std::cout << "\n\n----------------------------------------------------------------------\n"
              << " Running High-Resolution Per-Operation Tail Latency Profiling\n"
              << " Probe timer overhead: ~" << estimateTimerResolution() << " ns\n"
              << "----------------------------------------------------------------------\n";

    // 1. Insertion Tail Latency
    {
        Engine book;
        const auto orders = makeRestingOrders(kLatencySamples, 1, orderType::buy);
        std::vector<uint32_t> latencies;
        latencies.reserve(kLatencySamples);

        for (const auto& ord : orders) {
            const auto t0 = Clock::now();
            book.insert(ord);
            const auto t1 = Clock::now();
            latencies.push_back(static_cast<uint32_t>(std::chrono::duration_cast<Nanoseconds>(t1 - t0).count()));
        }
        printLatencyDistribution("Order Insertion", latencies);
    }

    // 2. Cancellation Tail Latency
    {
        Engine book;
        const auto orders = makeRestingOrders(kLatencySamples, 1, orderType::buy);
        for (const auto& ord : orders) {
            book.insert(ord);
        }

        std::vector<uint32_t> latencies;
        latencies.reserve(kLatencySamples);

        for (const auto& ord : orders) {
            const auto t0 = Clock::now();
            book.remove(ord.orderId);
            const auto t1 = Clock::now();
            latencies.push_back(static_cast<uint32_t>(std::chrono::duration_cast<Nanoseconds>(t1 - t0).count()));
        }
        printLatencyDistribution("Order Cancellation", latencies);
    }

    // 3. Matching (Crossing) Tail Latency
    {
        Engine book;
        for (int i = 0; i < kLatencySamples; ++i) {
            book.insert({1 + i, orderType::sell, kBasePrice, 1});
        }

        std::vector<uint32_t> latencies;
        latencies.reserve(kLatencySamples);

        for (int i = 0; i < kLatencySamples; ++i) {
            Order taker{1'000'000 + i, orderType::buy, kBasePrice, 1};
            const auto t0 = Clock::now();
            book.match(taker);
            const auto t1 = Clock::now();
            latencies.push_back(static_cast<uint32_t>(std::chrono::duration_cast<Nanoseconds>(t1 - t0).count()));
        }
        printLatencyDistribution("Order Matching (Crossing)", latencies);
    }
}

} // namespace

int main() {
    try {
        std::cout << "Limit Order Book benchmark suite\n"
                  << "warmup=" << kWarmupRuns << ", measured runs=" << kMeasuredRuns
                  << ", deterministic input (no RNG)\n\n";

        printSummary("resting-insertion", kRestingOrders,
                     measure([] { return Engine{}; }, restingInsertion, validateRestingInsertion));
        printSummary("matching-heavy", kMatchingOrders,
                     measure(matchingSetup, matchingHeavy, validateMatchingHeavy));
        printSummary("cancellation-heavy", kCancellationOrders,
                     measure(cancellationSetup, cancellationHeavy, validateCancellationHeavy));
        printSummary("mixed-add/cancel/match", kMixedOperations,
                     measure(mixedSetup, mixedFlow, validateMixedFlow));

        for (const int scale : {1'000, 10'000, 50'000, 200'000}) {
            const std::string label = "scale-insert-" + std::to_string(scale);
            const std::vector<Order> orders = makeRestingOrders(scale, 1, orderType::buy);
            printSummary(label, scale, measure(
                [] { return Engine{}; },
                [&orders](Engine& book) { scaleInsertion(book, orders); },
                [&orders](const Engine& book) { return validateScaleInsertion(book, orders.size()); }));
        }

        // High-resolution per-operation tail latency profiling & histogram
        runTailLatencyBenchmarks();

        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Benchmark validation failed: " << error.what() << '\n';
        return 1;
    }
}