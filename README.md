# Limit Order Book & Matching Engine (C++20)

A high-performance, deterministic Limit Order Book (LOB) matching engine written in modern **C++20**. Designed for ultra-low latency, strict price-time priority (FIFO), and comprehensive microbenchmarking.

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
│   ├── order.hpp              # Order entity and orderType (Buy/Sell)
│   ├── trade.hpp              # Trade execution record
│   └── order_book.hpp         # OrderBook interface, state encapsulation, getters
├── src/
│   ├── order_book.cpp         # Matching engine logic & DRY fill execution
│   └── main.cpp               # Interactive demonstration driver
├── tests/
│   └── order_book_tests.cpp   # 14 comprehensive unit tests
├── benchmarks/
│   ├── bench_replay.cpp       # Deterministic benchmark suite & tail latency profiler
│   └── BASELINE_V1.txt        # Raw benchmark output for V1 baseline
├── CMakeLists.txt             # Modern CMake build configuration (C++20)
├── CMakePresets.json          # Release (-O3) and Debug presets
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

### 3. Run Benchmarks
```powershell
.\build\release\bench_replay.exe
```

---

## 📊 V1 Performance Baseline

*Measurements taken on Windows (GCC 16.1.0, `-O3 -DNDEBUG`, deterministic non-RNG workloads).*

### 1. Throughput & Amortized Latency

| Workload | Operations | Median Latency | Throughput | Checksum |
| :--- | :---: | :---: | :---: | :---: |
| **Resting Insertion** | 200,000 | 129.98 ns/op | **7.69 Mops/s** | `0x89bc0e12cd2949bb` |
| **Matching-Heavy (Cross)** | 100,000 | 52.35 ns/op | **19.10 Mops/s** | `0xc8f95a19ef9af94a` |
| **Cancellation-Heavy** | 200,000 | 40.87 ns/op | **24.47 Mops/s** | `0x23560bb371fc6fb4` |
| **Mixed Flow (Add/Cancel/Match)** | 200,000 | 57.45 ns/op | **17.41 Mops/s** | `0xa8e45d2366ec03bc` |

### 2. High-Resolution Per-Operation Tail Latency (100,000 samples each)

| Operation | Median ($p50$) | $p90$ | $p99$ (Tail 1%) | $p99.9$ (Tail 0.1%) | Max Latency |
| :--- | :---: | :---: | :---: | :---: | :---: |
| **Order Insertion** | 100.0 ns | 100.0 ns | 400.0 ns | 20.70 µs | 1.19 ms |
| **Order Cancellation** | 100.0 ns | 100.0 ns | 100.0 ns | 300.0 ns | 138.6 µs |
| **Order Matching (Crossing)** | 100.0 ns | 100.0 ns | 200.0 ns | 1.10 µs | 427.6 µs |

> [!NOTE]
> **V1 Latency Analysis:**
> - Median operation latencies are fast ($50 - 130\text{ ns}$).
> - However, the $p99.9$ and Max tail latencies exhibit significant jitter ($20.7\ \mu\text{s}$ to $1.19\text{ ms}$) during heavy insertions. This is caused by dynamic heap allocations (`malloc`) inside node-based containers (`std::map`, `std::list`) and Red-Black tree pointer rebalancing.

---

## 🔮 Roadmap: V2 Optimizations

V2 will be developed in parallel as `OrderBookV2` to benchmark head-to-head against this V1 baseline:

1. **Zero-Allocation Object Pool:** Pre-allocate order memory arenas upfront; eliminate dynamic `malloc`/`free` on the critical path.
2. **Intrusive Doubly-Linked Lists:** Store order queues contiguously using array indices (`prev`/`next`) to eliminate cache line misses.
3. **Trade Emission Without Reallocation:** Replace dynamic `trades.push_back()` with pre-allocated circular ring buffers or event listener callbacks.
4. **Flat Price Array:** Replace pointer-chasing tree traversals with direct tick indexing for dense price levels.

