#include<bits/stdc++.h>
#include<cassert>
using namespace std;

enum class orderType{buy , sell};

struct Order{
    int orderId;
    orderType type;
    int limit;
    int shares;
};

struct Trade{
    int tradeId;
    int buyId;
    int sellId;
    int price;
    int shares;
};

using List = list<Order>;
using Book = map<int,List>;

Book bid;
Book ask;

unordered_map<int,pair<orderType, pair<Book::iterator, List::iterator>>> mp;
int trdId = 1;
vector<Trade> trades;

void insert(Order ord){
    Book &target = (ord.type == orderType::buy) ? bid : ask;
    auto [bookIt, inserted] = target.try_emplace(ord.limit);
    List &lst = bookIt->second;
    lst.emplace_back(ord);
    auto listIt = prev(lst.end());
    mp[ord.orderId] = {ord.type,{bookIt,listIt}};
}

void remove(int id){
    auto it = mp.find(id);
    if(it == mp.end()) return;
    auto type = it->second.first;
    auto [bookIt, listIt] = it->second.second;
    Book &target = (type == orderType::buy) ? bid : ask;
    bookIt->second.erase(listIt);
    if(bookIt->second.empty()) target.erase(bookIt);
    mp.erase(it);
}

void clearBook(){
    bid.clear();
    ask.clear();
    mp.clear();
    trades.clear();
}

optional<int> getBestBid(){
    if(bid.empty()) return nullopt;
    else return bid.rbegin()->first;
}

optional<int> getBestAsk(){
    if(ask.empty()) return nullopt;
    else return ask.begin()->first;
}

void match(Order ord){
    if(ord.type == orderType::buy){
        while(!ask.empty() && ord.shares>0){
            if(getBestAsk() > ord.limit){
                break;
            }
            else{
                auto match = ask.begin()->second.begin(); 
                if(match->shares >= ord.shares){
                    Trade trd = {trdId++, ord.orderId, match->orderId, match->limit, ord.shares};
                    trades.push_back(trd);
                    match->shares -= ord.shares;
                    ord.shares = 0;
                    if(match->shares == 0) remove(match->orderId);
                }
                else{
                    Trade trd = {trdId++, ord.orderId, match->orderId, match->limit, match->shares};
                    trades.push_back(trd);
                    ord.shares -= match->shares;
                    remove(match->orderId);
                }
            }
        }
        if(ord.shares != 0){
            insert(ord);
        }
    }
    else{
        while(!bid.empty() && ord.shares>0){
            if(getBestBid() < ord.limit){
                break;
            }
            else{
                auto match = bid.rbegin()->second.begin(); 
                if(match->shares >= ord.shares){
                    Trade trd = {trdId++, match->orderId, ord.orderId, match->limit, ord.shares};
                    trades.push_back(trd);
                    match->shares -= ord.shares;
                    ord.shares = 0;
                    if(match->shares == 0) remove(match->orderId);
                }
                else{
                    Trade trd = {trdId++, match->orderId, ord.orderId, match->limit, match->shares};
                    trades.push_back(trd);
                    ord.shares -= match->shares;
                    remove(match->orderId);
                }
            }
        }
        if(ord.shares != 0){
            insert(ord);
        }
    }
}

int main(){
    assert(trdId == 1); // Verify initial monotonic trade counter

    // =========================================================================
    // Trade History Tests: Buy Taker vs Resting Maker Asks
    // =========================================================================

    // Test 1: Incoming Buy consumes multiple resting Asks across levels (Fully filled, does NOT rest)
    clearBook();
    insert({10, orderType::sell, 100, 5});  // Level 100 (5 shares)
    insert({20, orderType::sell, 101, 8});  // Level 101 (8 shares)
    insert({30, orderType::sell, 102, 10}); // Level 102 (10 shares)
    int startId1 = trdId;
    match({99, orderType::buy, 105, 16});   // Incoming buy for 16 shares (sweeps 5 + 8 + 3)

    assert(trades.size() == 3);
    // Trade 1: sweeps Order 10 at 100
    assert(trades[0].tradeId == startId1 && trades[0].buyId == 99 && trades[0].sellId == 10);
    assert(trades[0].price == 100 && trades[0].shares == 5);
    // Trade 2: sweeps Order 20 at 101
    assert(trades[1].tradeId == startId1 + 1 && trades[1].buyId == 99 && trades[1].sellId == 20);
    assert(trades[1].price == 101 && trades[1].shares == 8);
    // Trade 3: partially fills Order 30 at 102
    assert(trades[2].tradeId == startId1 + 2 && trades[2].buyId == 99 && trades[2].sellId == 30);
    assert(trades[2].price == 102 && trades[2].shares == 3);
    assert(trdId == startId1 + 3);

    // Verify trade volume sum equals total executed taker shares
    int buyTotalShares = trades[0].shares + trades[1].shares + trades[2].shares;
    assert(buyTotalShares == 16);

    // Verify resting book state
    assert(ask.count(100) == 0 && ask.count(101) == 0);
    assert(ask.count(102) == 1 && ask[102].front().shares == 7);
    assert(ask[102].front().orderId == 30);
    assert(mp.count(10) == 0 && mp.count(20) == 0 && mp.count(30) == 1);

    // Incoming order had 0 shares remaining -> must NOT rest
    assert(bid.empty() && mp.count(99) == 0);
    cout << "Test 1 Passed: Buy multi-level consumption, trade records, and resting quantities verified.\n";

    // Test 2: Incoming Buy consumes resting Asks + has remaining shares (Must rest)
    clearBook();
    insert({10, orderType::sell, 100, 6});
    insert({20, orderType::sell, 100, 4});
    int startId2 = trdId;
    match({99, orderType::buy, 100, 15}); // Fills 10 shares, 5 shares must rest

    assert(trades.size() == 2);
    assert(trades[0].tradeId == startId2 && trades[0].buyId == 99 && trades[0].sellId == 10 && trades[0].shares == 6);
    assert(trades[1].tradeId == startId2 + 1 && trades[1].buyId == 99 && trades[1].sellId == 20 && trades[1].shares == 4);
    assert(trades[0].shares + trades[1].shares == 10);
    assert(trdId == startId2 + 2);

    // Asks fully cleared
    assert(ask.empty());
    // Remainder (5 shares) rests on bid book
    assert(bid.count(100) == 1 && bid[100].front().orderId == 99);
    assert(bid[100].front().shares == 5);
    assert(mp.count(99) == 1);
    assert(getBestBid() == 100);
    cout << "Test 2 Passed: Buy matching with remaining shares resting on book verified.\n";

    // Test 3: Exact quantity match (Both incoming and resting fully filled)
    clearBook();
    insert({10, orderType::sell, 100, 12});
    int startId3 = trdId;
    match({99, orderType::buy, 100, 12});

    assert(trades.size() == 1);
    assert(trades[0].tradeId == startId3 && trades[0].buyId == 99 && trades[0].sellId == 10);
    assert(trades[0].price == 100 && trades[0].shares == 12);
    assert(trdId == startId3 + 1);
    assert(ask.empty() && bid.empty() && mp.empty());
    cout << "Test 3 Passed: Buy exact quantity match (both orders fully filled) verified.\n";

    // =========================================================================
    // Trade History Tests: Sell Taker vs Resting Maker Bids
    // =========================================================================

    // Test 4: Incoming Sell consumes multiple resting Bids across levels (Fully filled, does NOT rest)
    clearBook();
    insert({10, orderType::buy, 102, 5});  // Level 102 (5 shares)
    insert({20, orderType::buy, 101, 8});  // Level 101 (8 shares)
    insert({30, orderType::buy, 100, 10}); // Level 100 (10 shares)
    int startId4 = trdId;
    match({99, orderType::sell, 95, 16});  // Incoming sell for 16 shares (sweeps 5 + 8 + 3)

    assert(trades.size() == 3);
    // Trade 1: sweeps Order 10 at 102
    assert(trades[0].tradeId == startId4 && trades[0].buyId == 10 && trades[0].sellId == 99);
    assert(trades[0].price == 102 && trades[0].shares == 5);
    // Trade 2: sweeps Order 20 at 101
    assert(trades[1].tradeId == startId4 + 1 && trades[1].buyId == 20 && trades[1].sellId == 99);
    assert(trades[1].price == 101 && trades[1].shares == 8);
    // Trade 3: partially fills Order 30 at 100
    assert(trades[2].tradeId == startId4 + 2 && trades[2].buyId == 30 && trades[2].sellId == 99);
    assert(trades[2].price == 100 && trades[2].shares == 3);
    assert(trdId == startId4 + 3);

    // Verify trade volume sum equals total executed taker shares
    int sellTotalShares = trades[0].shares + trades[1].shares + trades[2].shares;
    assert(sellTotalShares == 16);

    // Verify resting book state
    assert(bid.count(102) == 0 && bid.count(101) == 0);
    assert(bid.count(100) == 1 && bid[100].front().shares == 7);
    assert(bid[100].front().orderId == 30);
    assert(mp.count(10) == 0 && mp.count(20) == 0 && mp.count(30) == 1);

    // Incoming order had 0 shares remaining -> must NOT rest
    assert(ask.empty() && mp.count(99) == 0);
    cout << "Test 4 Passed: Sell multi-level consumption, trade records, and resting quantities verified.\n";

    // Test 5: Incoming Sell consumes resting Bids + has remaining shares (Must rest)
    clearBook();
    insert({10, orderType::buy, 100, 6});
    insert({20, orderType::buy, 100, 4});
    int startId5 = trdId;
    match({99, orderType::sell, 100, 15}); // Fills 10 shares, 5 shares must rest

    assert(trades.size() == 2);
    assert(trades[0].tradeId == startId5 && trades[0].buyId == 10 && trades[0].sellId == 99 && trades[0].shares == 6);
    assert(trades[1].tradeId == startId5 + 1 && trades[1].buyId == 20 && trades[1].sellId == 99 && trades[1].shares == 4);
    assert(trades[0].shares + trades[1].shares == 10);
    assert(trdId == startId5 + 2);

    // Bids fully cleared
    assert(bid.empty());
    // Remainder (5 shares) rests on ask book
    assert(ask.count(100) == 1 && ask[100].front().orderId == 99);
    assert(ask[100].front().shares == 5);
    assert(mp.count(99) == 1);
    assert(getBestAsk() == 100);
    cout << "Test 5 Passed: Sell matching with remaining shares resting on book verified.\n";

    // Test 6: Exact quantity match (Both incoming and resting fully filled)
    clearBook();
    insert({10, orderType::buy, 100, 12});
    int startId6 = trdId;
    match({99, orderType::sell, 100, 12});

    assert(trades.size() == 1);
    assert(trades[0].tradeId == startId6 && trades[0].buyId == 10 && trades[0].sellId == 99);
    assert(trades[0].price == 100 && trades[0].shares == 12);
    assert(trdId == startId6 + 1);
    assert(bid.empty() && ask.empty() && mp.empty());
    cout << "Test 6 Passed: Sell exact quantity match (both orders fully filled) verified.\n";

    return 0;
}
