#include <iostream>
#include "order_book.hpp"

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "   Limit Order Book (C++ Demo)          " << std::endl;
    std::cout << "========================================" << std::endl;

    OrderBook book;

    std::cout << "1. Inserting resting sell orders (Asks)...\n";
    book.insert({1, orderType::sell, 105, 10});
    book.insert({2, orderType::sell, 106, 15});

    std::cout << "2. Inserting resting buy orders (Bids)...\n";
    book.insert({3, orderType::buy, 100, 10});
    book.insert({4, orderType::buy, 99, 20});

    std::cout << "Current Best Bid: " << (book.getBestBid() ? std::to_string(*book.getBestBid()) : "None") << "\n";
    std::cout << "Current Best Ask: " << (book.getBestAsk() ? std::to_string(*book.getBestAsk()) : "None") << "\n";

    std::cout << "\n3. Submitting incoming Buy order (Limit 105, 5 shares) to cross the spread...\n";
    book.match({5, orderType::buy, 105, 5});

    std::cout << "Matching finished!\n";
    std::cout << "Updated Best Ask: " << (book.getBestAsk() ? std::to_string(*book.getBestAsk()) : "None") << "\n";

    return 0;
}
