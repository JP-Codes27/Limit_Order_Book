# Limit Order Book & Matching Engine (C++20)

A high-performance, deterministic Limit Order Book (LOB) matching engine written in modern **C++20**. Designed for ultra-low latency, strict price-time priority (FIFO), formal correctness verification via differential fuzzing, and cycle-accurate microbenchmarking.

---

## 🏛️ Architecture & Design (V1 Baseline)

The V1 engine implements standard exchange matching semantics with the following internal structures:

- **Price-Time Priority (FIFO):**
  - **Bids:** Highest prices take priority (`bid.rbegin()`).
  - **Asks:** Lowest prices take priority (`ask.begin()`).
  - **Queue:** Multiple orders at the same price are matched strictly in order of arrival ($\mathcal{O}(1)$ FIFO queue via `std::list`).
- **Fast Order Cancellation ($\mathcal{O}(1)$):**
  - An index table (`std::unordered_map`) maps `orderId` directly to iterators for both the price level and the order in the list, enabling $\mathcal{O}(1)$ cancellation and removal without scanning.
- **Maker-Taker Price Execution:**
  - Aggressive crossing orders execute at the resting maker's price.
  - Partial fills decrement volume in place; fully executed orders purge empty price levels cleanly.

---

## 📁 Repository Layout

```text
LimitOrderBook/
├── include/
│   ├── order.hpp                  # Order entity and orderType (Buy/Sell)
│   ├── trade.hpp                  # Trade execution record
│   └── order_book.hpp             # OrderBook interface, state encapsulation, getters
├── src/
│   ├── order_book.cpp             # Matching engine logic & DRY fill execution
│   └── main.cpp                   # Interactive demonstration driver
├── tests/
│   ├── order_book_tests.cpp       # 14 unit test scenarios (FIFO, levels, cancellations)
│   ├── fuzz_oracle.cpp            # Differential fuzzer (engine vs O(N) reference model)
│   └── support/                   # Engine-agnostic testing & benchmarking infrastructure
│       ├── types.hpp              # Op, Side, TradeRec primitives
│       ├── check.hpp              # LOB_CHECK (assert that survives -DNDEBUG)
│       ├── rng.hpp                # Deterministic seeded xorshift64* PRNG
│       ├── canonical.hpp          # Engine-agnostic canonical state hash
│       └── engine_adapter_v1.hpp  # V1 adapter wrapping OrderBook for fuzzer & bench
├── benchmarks/
│   ├── bench_v2.cpp               # Cycle-accurate benchmark suite (rdtscp, core pinning)
│   ├── BASELINE_V1_11.txt         # Official V1 baseline (Core 11 pinned, Core Ultra 5)
│   └── BASELINE_V1_NOPIN.txt      # Unpinned reference run
├── CMakeLists.txt                 # Modern CMake build configuration (C++20, Release default)
├── CMakePresets.json              # Release (-O3), Debug, and Sanitizer presets
└── README.md
```

---

## 🛠️ Building & Running

### Requirements
- **C++ Compiler:** GCC $\ge 11$, Clang $\ge 13$, or MSVC with C++20 support.
- **Build System:** CMake $\ge 3.20$ and Ninja.

### 1. Build in Optimized Release Mode
```powershell
cmake --preset release
cmake --build build/release --config Release
```

### 2. Run the Unit Test Suite
```powershell
ctest --test-dir build/release --output-on-failure
# Or run directly:
.\build\release\order_book_tests.exe
```

### 3. Run the Differential Fuzzer
Validates every operation, trade, and resting queue against an $\mathcal{O}(N)$ brute-force reference model across 6 generation modes:
```powershell
# Quick test (300 seeds x 500 steps):
.\build\release\fuzz_oracle.exe --seeds 300 --steps 500

# Heavy soak test:
.\build\release\fuzz_oracle.exe --seeds 5000 --steps 2000
```

### 4. Run Cycle-Accurate Benchmarks
```powershell
# Standard run (21 interleaved runs, core-pinned):
.\build\release\bench_v2.exe --out benchmarks/BASELINE_V1_11.txt

# Quick benchmark (~1 min):
.\build\release\bench_v2.exe --quick

# Filter specific workload:
.\build\release\bench_v2.exe --only realistic-mixed
```

---

## 📊 V1 Performance Baseline

*Environment: Intel Core Ultra 5 125H (18 logical cores), Windows 11, GCC 16.1.0 (`-O3 -DNDEBUG`), pinned to Core 11. Hardware timer: `rdtscp` + `lfence` ($0.3339\text{ ns/tick}$, overhead $\approx 12\text{ ns}$, granularity $\approx 14\text{ ns}$). Measured across 21 interleaved runs in batches of 64 operations.*

### 1. Throughput & Statistical Latency

| Workload | Timed Ops | Median Latency | MAD | Throughput | Canonical Checksum |
| :--- | :---: | :---: | :---: | :---: | :---: |
| **`realistic-mixed`** (~54% add, 34% cancel, 12% match) | 400,000 | 111.0 ns/op | 2.7 ns | **9.01 Mops/s** | `0x6168c0740d9856cb` |
| **`level-churn`** (Add/cancel cycle + marketable fills) | 200,000 | 65.6 ns/op | 9.5 ns | **15.24 Mops/s** | `0xcdb3bc7ab26d818b` |
| **`deep-sweep`** (Taker sweeps 50 levels per cross) | 226,500 | 119.0 ns/op | 1.6 ns | **8.40 Mops/s** | `0x1e176129aab58bb5` |
| **`insert-random-256`** (256 price levels) | 200,000 | 164.0 ns/op | 7.0 ns | **6.10 Mops/s** | `0xb8e2f0d2a008422d` |
| **`cancel-random-256`** (Random order deallocation) | 200,000 | 200.8 ns/op | 27.9 ns | **4.98 Mops/s** | `0xdb8ae71da94e7d90` |
| **`insert-random-65536`** (Deep tree insertion) | 200,000 | 360.8 ns/op | 22.6 ns | **2.77 Mops/s** | `0xb27d1b2219c6eaaa` |
| **`cancel-random-65536`** (Deep tree deallocation) | 200,000 | 383.1 ns/op | 34.2 ns | **2.61 Mops/s** | `0xdb8ae71da94e7d90` |

### 2. High-Resolution Per-Operation Tail Latency (Steady Warmed State)

| Workload | $p50$ (Median) | $p90$ | $p99$ (Tail 1%) | $p99.9$ (Tail 0.1%) |
| :--- | :---: | :---: | :---: | :---: |
| **`realistic-mixed`** | 98 ns | 172 ns | 401 ns | 1,104 ns |
| **`level-churn`** | 44 ns | 67 ns | 197 ns | 471 ns |
| **`deep-sweep`** | 67 ns | 107 ns | 178 ns | 5,532 ns |
| **`insert-random-256`** | 88 ns | 109 ns | 194 ns | 20,656 ns |
| **`insert-random-65536`** | 234 ns | 340 ns | 621 ns | 25,355 ns |

> [!NOTE]
> **V1 Bottleneck Analysis:**
> - In shallow or steady books (`level-churn`, `realistic-mixed`), median latency is under **100 ns** ($\ge 9 - 15\text{ Mops/s}$).
> - As book depth grows (`65,536` levels), throughput drops by $\approx 3.5\times$ down to **$2.6 - 2.8\text{ Mops/s}$** with tail latency spikes exceeding **$20\ \mu\text{s}$**. This degradation is driven by pointer-chasing in the Red-Black tree (`std::map`), individual heap allocations (`std::list`), and dynamic growth of the trade container.

---

## 🔮 Roadmap: V2 Optimizations

V2 will be developed in parallel as `OrderBookV2` and plugged directly into `engine_adapter_v2.hpp` to benchmark head-to-head against this V1 baseline:

1. **Zero-Allocation Object Pool:** Pre-allocate contiguous order memory arenas upfront; eliminate dynamic `malloc`/`free` on the critical path.
2. **Intrusive Doubly-Linked Lists:** Store order queues contiguously using array indices (`prev`/`next`) to eliminate cache misses.
3. **Trade Emission Without Reallocation:** Replace dynamic `trades.push_back()` with pre-allocated circular ring buffers or event listener callbacks.
4. **Flat Price Array & Radix/B-Tree Indexing:** Replace pointer-chasing tree traversals with direct tick indexing for dense price levels.
