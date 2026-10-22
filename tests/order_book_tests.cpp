#include <cassert>
#include <iostream>
#include "order_book.hpp"

int main() {
    std::cout << "Running LimitOrderBook comprehensive test suite...\n\n";

    OrderBook book;

    // =========================================================================
    // Part 1: Basic Order Book Mechanics
    // =========================================================================

    // Test 1: FIFO ordering at the same price level
    book.clearBook();
    book.insert({1, orderType::buy, 100, 10});
    book.insert({2, orderType::buy, 100, 20});
    assert(book.getBids().count(100) == 1);
    assert(book.getBids().at(100).size() == 2);
    assert(book.getBids().at(100).front().orderId == 1); // Earliest arrival remains at front
    assert(book.getBids().at(100).back().orderId == 2);
    std::cout << "Test 1 Passed: FIFO ordering verified at same price level.\n";

    // Test 2: Multi-price levels on buy and sell sides
    book.clearBook();
    book.insert({1, orderType::buy, 99, 10});
    book.insert({2, orderType::buy, 100, 15});
    book.insert({3, orderType::sell, 105, 5});
    book.insert({4, orderType::sell, 110, 25});
    assert(book.getBids().size() == 2 && book.getAsks().size() == 2);
    assert(book.getBids().count(99) == 1 && book.getBids().count(100) == 1);
    assert(book.getAsks().count(105) == 1 && book.getAsks().count(110) == 1);
    assert(book.orderCount() == 4);
    std::cout << "Test 2 Passed: Multi-price levels on buy and sell sides verified.\n";

    // Test 3: Head-of-queue cancellation preserves remaining order
    book.clearBook();
    book.insert({1, orderType::buy, 100, 10});
    book.insert({2, orderType::buy, 100, 20});
    book.remove(1);
    assert(book.getBids().count(100) == 1);
    assert(book.getBids().at(100).size() == 1);
    assert(book.getBids().at(100).front().orderId == 2); // Order 2 promoted to front
    assert(!book.hasOrder(1));
    assert(book.hasOrder(2));
    std::cout << "Test 3 Passed: Head-of-queue cancellation preserved remaining order.\n";

    // Test 4: Empty price level successfully purged from book
    book.clearBook();
    book.insert({1, orderType::sell, 200, 50});
    book.remove(1);
    assert(book.getAsks().count(200) == 0); // Price node must be purged from map
    assert(book.getAsks().empty());
    assert(book.orderCount() == 0);
    std::cout << "Test 4 Passed: Empty price level successfully purged from map.\n";

    // Test 5: Non-existent order cancellation handled gracefully
    book.clearBook();
    book.insert({1, orderType::buy, 100, 10});
    book.remove(999);
    assert(book.getBids().at(100).size() == 1); // State unchanged
    assert(book.orderCount() == 1);
    std::cout << "Test 5 Passed: Non-existent order cancellation handled gracefully.\n\n";

    // =========================================================================
    // Part 2: Top-of-Book and Market State Queries
    // =========================================================================

    // Test 6: Empty book and one-sided queries
    book.clearBook();
    assert(book.getBestBid() == std::nullopt);
    assert(book.getBestAsk() == std::nullopt);
    book.insert({1, orderType::buy, 100, 10});
    assert(book.getBestBid() == 100);
    assert(book.getBestAsk() == std::nullopt);
    std::cout << "Test 6 Passed: Empty book and one-sided queries verified.\n";

    // Test 7: Multi-price best bid/ask and spread calculation
    book.clearBook();
    book.insert({1, orderType::buy, 95, 10});
    book.insert({2, orderType::buy, 105, 10});
    book.insert({3, orderType::buy, 100, 10});
    book.insert({4, orderType::sell, 120, 10});
    book.insert({5, orderType::sell, 110, 10});
    book.insert({6, orderType::sell, 115, 10});
    assert(book.getBestBid() == 105);
    assert(book.getBestAsk() == 110);
    assert(*book.getBestAsk() - *book.getBestBid() == 5);
    std::cout << "Test 7 Passed: Multi-price best bid/ask and spread verified.\n";

    // Test 8: Price level depletion and fallback to next-best
    book.clearBook();
    book.insert({1, orderType::buy, 100, 10});
    book.insert({2, orderType::buy, 105, 15});
    book.insert({3, orderType::buy, 105, 20});
    assert(book.getBestBid() == 105);
    book.remove(2); // One order remains at 105
    assert(book.getBestBid() == 105);
    book.remove(3); // Level 105 fully cleared
    assert(book.getBestBid() == 100); // Falls back to next best
    book.remove(1);
    assert(book.getBestBid() == std::nullopt);
    std::cout << "Test 8 Passed: Best-priced level depletion and fallback verified.\n\n";

    // =========================================================================
    // Part 3: Trade History & Matching (Buy Taker vs Resting Asks)
    // =========================================================================

    // Test 9: Buy consumes multiple resting Asks across levels (Does not rest)
    book.clearBook();
    book.insert({10, orderType::sell, 100, 5});  // Level 100 (5 shares)
    book.insert({20, orderType::sell, 101, 8});  // Level 101 (8 shares)
    book.insert({30, orderType::sell, 102, 10}); // Level 102 (10 shares)
    book.match({99, orderType::buy, 105, 16});   // Incoming buy for 16 shares (sweeps 5 + 8 + 3)

    assert(book.getTrades().size() == 3);
    assert(book.getTrades()[0].buyId == 99 && book.getTrades()[0].sellId == 10 && book.getTrades()[0].price == 100 && book.getTrades()[0].shares == 5);
    assert(book.getTrades()[1].buyId == 99 && book.getTrades()[1].sellId == 20 && book.getTrades()[1].price == 101 && book.getTrades()[1].shares == 8);
    assert(book.getTrades()[2].buyId == 99 && book.getTrades()[2].sellId == 30 && book.getTrades()[2].price == 102 && book.getTrades()[2].shares == 3);
    assert(book.getTrades()[0].shares + book.getTrades()[1].shares + book.getTrades()[2].shares == 16);
    assert(book.getAsks().count(100) == 0 && book.getAsks().count(101) == 0);
    assert(book.getAsks().at(102).front().shares == 7);
    assert(book.getBids().empty() && !book.hasOrder(99));
    std::cout << "Test 9 Passed: Buy multi-level consumption, trade records, and resting quantities verified.\n";

    // Test 10: Buy consumes resting Asks + remainder rests on book
    book.clearBook();
    book.insert({10, orderType::sell, 100, 6});
    book.insert({20, orderType::sell, 100, 4});
    book.match({99, orderType::buy, 100, 15}); // Fills 10 shares, 5 shares must rest

    assert(book.getTrades().size() == 2);
    assert(book.getTrades()[0].shares + book.getTrades()[1].shares == 10);
    assert(book.getAsks().empty());
    assert(book.getBids().count(100) == 1 && book.getBids().at(100).front().orderId == 99);
    assert(book.getBids().at(100).front().shares == 5);
    assert(book.hasOrder(99));
    assert(book.getBestBid() == 100);
    std::cout << "Test 10 Passed: Buy matching with remaining shares resting on book verified.\n";

    // Test 11: Buy exact quantity match (both fully filled)
    book.clearBook();
    book.insert({10, orderType::sell, 100, 12});
    book.match({99, orderType::buy, 100, 12});
    assert(book.getTrades().size() == 1);
    assert(book.getTrades()[0].buyId == 99 && book.getTrades()[0].sellId == 10 && book.getTrades()[0].shares == 12);
    assert(book.getAsks().empty() && book.getBids().empty() && book.orderCount() == 0);
    std::cout << "Test 11 Passed: Buy exact quantity match (both orders fully filled) verified.\n\n";

    // =========================================================================
    // Part 4: Trade History & Matching (Sell Taker vs Resting Bids)
    // =========================================================================

    // Test 12: Sell consumes multiple resting Bids across levels (Does not rest)
    book.clearBook();
    book.insert({10, orderType::buy, 102, 5});  // Level 102 (5 shares)
    book.insert({20, orderType::buy, 101, 8});  // Level 101 (8 shares)
    book.insert({30, orderType::buy, 100, 10}); // Level 100 (10 shares)
    book.match({99, orderType::sell, 95, 16});  // Sweeps 5 + 8 + 3

    assert(book.getTrades().size() == 3);
    assert(book.getTrades()[0].buyId == 10 && book.getTrades()[0].sellId == 99 && book.getTrades()[0].price == 102 && book.getTrades()[0].shares == 5);
    assert(book.getTrades()[1].buyId == 20 && book.getTrades()[1].sellId == 99 && book.getTrades()[1].price == 101 && book.getTrades()[1].shares == 8);
    assert(book.getTrades()[2].buyId == 30 && book.getTrades()[2].sellId == 99 && book.getTrades()[2].price == 100 && book.getTrades()[2].shares == 3);
    assert(book.getTrades()[0].shares + book.getTrades()[1].shares + book.getTrades()[2].shares == 16);
    assert(book.getBids().count(102) == 0 && book.getBids().count(101) == 0);
    assert(book.getBids().at(100).front().shares == 7);
    assert(book.getAsks().empty() && !book.hasOrder(99));
    std::cout << "Test 12 Passed: Sell multi-level consumption, trade records, and resting quantities verified.\n";

    // Test 13: Sell consumes resting Bids + remainder rests on book
    book.clearBook();
    book.insert({10, orderType::buy, 100, 6});
    book.insert({20, orderType::buy, 100, 4});
    book.match({99, orderType::sell, 100, 15}); // Fills 10 shares, 5 shares must rest

    assert(book.getTrades().size() == 2);
    assert(book.getTrades()[0].shares + book.getTrades()[1].shares == 10);
    assert(book.getBids().empty());
    assert(book.getAsks().count(100) == 1 && book.getAsks().at(100).front().orderId == 99);
    assert(book.getAsks().at(100).front().shares == 5);
    assert(book.hasOrder(99));
    assert(book.getBestAsk() == 100);
    std::cout << "Test 13 Passed: Sell matching with remaining shares resting on book verified.\n";

    // Test 14: Sell exact quantity match (both fully filled)
    book.clearBook();
    book.insert({10, orderType::buy, 100, 12});
    book.match({99, orderType::sell, 100, 12});
    assert(book.getTrades().size() == 1);
    assert(book.getTrades()[0].buyId == 10 && book.getTrades()[0].sellId == 99 && book.getTrades()[0].shares == 12);
    assert(book.getBids().empty() && book.getAsks().empty() && book.orderCount() == 0);
    std::cout << "Test 14 Passed: Sell exact quantity match (both orders fully filled) verified.\n\n";

    std::cout << "========================================\n";
    std::cout << "All 14 OrderBook tests passed successfully!\n";
    std::cout << "========================================\n";

    return 0;
}

