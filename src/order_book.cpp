#include <bits/stdc++.h>
#include "order_book.hpp"

void OrderBook::insert(Order ord){
    Book &target = (ord.type == orderType::buy) ? bid : ask;
    auto [bookIt, inserted] = target.try_emplace(ord.limit);
    List &lst = bookIt->second;
    lst.emplace_back(ord);
    auto listIt = std::prev(lst.end());
    mp[ord.orderId] = {ord.type,{bookIt,listIt}};
}
void OrderBook::remove(int id){
    auto it = mp.find(id);
    if(it == mp.end()) return;
    auto type = it->second.first;
    auto [bookIt, listIt] = it->second.second;
    Book &target = (type == orderType::buy) ? bid : ask;
    bookIt->second.erase(listIt);
    if(bookIt->second.empty()) target.erase(bookIt);
    mp.erase(it);
}  
std::optional<int> OrderBook::getBestBid() const{
    if(bid.empty()) return std::nullopt;
    else return bid.rbegin()->first;
}
std::optional<int> OrderBook::getBestAsk() const{
    if(ask.empty()) return std::nullopt;
    else return ask.begin()->first;
}
void OrderBook::executeFill(Order& incoming, Order& resting, int buyId, int sellId, int tradePrice){
    int fillQty = std::min(incoming.shares, resting.shares);
    trades.push_back({trdId++, buyId, sellId, tradePrice, fillQty});
    incoming.shares -= fillQty;
    resting.shares -= fillQty;
    if (resting.shares == 0) remove(resting.orderId);
}
void OrderBook::match(Order ord){
    if(ord.type == orderType::buy){
        while(!ask.empty() && ord.shares>0 && ask.begin()->first <= ord.limit){
            auto match = ask.begin()->second.begin();
            executeFill(ord, *match, ord.orderId, match->orderId, match->limit);
        }
    }
    else{
        while(!bid.empty() && ord.shares>0 && bid.rbegin()->first >= ord.limit){
            auto match = bid.rbegin()->second.begin();
            executeFill(ord, *match, match->orderId, ord.orderId, match->limit);
        }
    }
    if(ord.shares > 0) insert(ord);
}
void OrderBook::clearBook(){
    bid.clear();
    ask.clear();
    mp.clear();
    trades.clear();
    trdId = 1;
}