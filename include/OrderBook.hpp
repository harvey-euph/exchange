#pragma once
#include <map>
#include <vector>
#include <atomic>
#include <cstdint>
#include <unordered_map>
#include "L3Updater.hpp"
#include "fbs/exchange_generated.h"
#include "ring/SHMRingBuffer.hpp"
#include "Order.hpp"

#include "gtest/gtest_prod.h"
namespace Exchange {

struct PriceLevel
{
    uint64_t total_qty = 0;
    uint64_t order_count = 0;

    Order    dummy_head;   // prev always nullptr
    Order    dummy_tail;   // next always nullptr

    PriceLevel* better = nullptr;
    PriceLevel* worse  = nullptr;

    PriceLevel() {
        dummy_head.next = &dummy_tail;
        dummy_tail.prev = &dummy_head;
    }
};

class OrderBookTest;
class MarketOrderTest;
class CSVDataGen;

class OrderBook
{
    friend class CSVDataGen;

    friend class OrderBookTest;
    friend class MarketOrderTest;
    FRIEND_TEST(OrderBookTest, InsertBidAsk);
    FRIEND_TEST(OrderBookTest, CancelAndModify);
    FRIEND_TEST(OrderBookTest, CancelFullLevelMaintainsPriceLinks);
    FRIEND_TEST(OrderBookTest, MatchSingleLayer);
    FRIEND_TEST(OrderBookTest, MatchingMultiLayer);
    
    FRIEND_TEST(MarketOrderTest, MarketBuyMatchesAllAsks);
    FRIEND_TEST(MarketOrderTest, MarketSellMatchesAllBids);
    FRIEND_TEST(MarketOrderTest, MarketOrderRestInBookAtExtremePrice);
    
public:
    explicit OrderBook(uint64_t symbol_id,
                       int64_t min_step,
                       int64_t price_offset,
                       size_t max_price_levels = 65536,
                       SHMRingBuffer* response_ring = nullptr);

    ~OrderBook();

    __attribute__((noinline)) void processRequest(const OrderRequest* req);

private:
    const uint64_t symbol_id_;
    const int64_t min_step_;           // 最小價格單位 (定點數)
    const int64_t price_index_offset_; // 
    const size_t  max_price_levels_;   // price_array_ 大小
    SHMRingBuffer* response_ring_;
    OrderResponseT resp;
    L3Updater l3;

    size_t price_to_index(const int64_t price) const {
        return price / min_step_ - price_index_offset_;
    }
    size_t index_to_price(const size_t index) const {
        return (index + price_index_offset_) * min_step_;
    }
    int price_invalid(const int64_t price) const {
        if (price / min_step_ < price_index_offset_) return 1;
        if (price_index_offset_ + (int64_t)max_price_levels_ < price / min_step_) return -1;
        return 0;
    }

    Order* createOrder(const OrderRequest* req);

    std::vector<PriceLevel> price_array_;
    std::unordered_map<uint64_t, Order*> active_orders_;

    PriceLevel* best_levels_[2] = {nullptr, nullptr};  // [B1][A1] , can be market order
    std::map<size_t, PriceLevel*> active_levels_[2];// [B][A]
    
    PriceLevel* GetOrCreatePriceLevel(size_t price_index, Side side);
    void removePriceLevel(PriceLevel* pl, Side side);
    void sendResponse(ExecType exec_type, uint64_t order_id, uint32_t client_id,
                      uint64_t exec_id, Side side, int64_t p, uint64_t q,
                      RejectCode reject_code = RejectCode_None);

    void handleNewOrder(const OrderRequest* req, bool report_ack = true);
    void handleCancelOrder(const OrderRequest* req, bool report_cancelled = true);
    void handleModifyOrder(const OrderRequest* req);

    // Linked list 操作
    void insertOrderToLevel(PriceLevel* level, Order* order, Side side);
    void removeOrderFromLevel(Order* order);
};
}
