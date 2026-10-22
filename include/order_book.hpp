#pragma once
#include<bits/stdc++.h>
#include"order.hpp"
#include"trade.hpp"

class OrderBook{
    private:
        using List = std::list<Order>;
        using Book = std::map<int,List>;
        Book bid;
        Book ask;   
        std::unordered_map<int,std::pair<orderType, std::pair<Book::iterator, List::iterator>>> mp;
        int trdId = 1;
        std::vector<Trade> trades;
        void executeFill(Order& incoming, Order& resting, int buyId, int sellId, int tradePrice);
    
    public:
        void insert(Order ord);
        void remove(int id);
        void match(Order ord);
        void clearBook();
        std::optional<int> getBestBid() const;
        std::optional<int> getBestAsk() const;
        // --- State Inspection Getters (for testing & queries) ---
        const std::vector<Trade>& getTrades() const { return trades; }
        const Book& getBids() const { return bid; }
        const Book& getAsks() const { return ask; }
        bool hasOrder(int id) const { return mp.find(id) != mp.end(); }
        size_t orderCount() const { return mp.size(); }
};