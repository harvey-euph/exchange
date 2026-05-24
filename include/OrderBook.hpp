#pragma once
#include <map>
#include <vector>
#include <atomic>
#include <cstdint>
#include <unordered_map>
// #include <rte_ring.h>
#include "generated/order_generated.h"

#include "gtest/gtest_prod.h"
namespace Exchange {

struct Order
{
    uint64_t order_id;
    uint32_t client_id;
    uint64_t qty_original;
    uint64_t qty_remaining;
    // uint64_t qty_visible;  // For Iceberg

    OrderType type; // Only consider limit and market order for now

    Order* prev = nullptr;
    Order* next = nullptr;

    struct PriceLevel* price_level = nullptr;

    uint64_t timestamp;
};

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

class OrderBook
{
    friend class OrderBookTest;
    FRIEND_TEST(OrderBookTest, InsertBidAsk);
    FRIEND_TEST(OrderBookTest, CancelAndModify);
    FRIEND_TEST(OrderBookTest, MatchSingleLayer);
    FRIEND_TEST(OrderBookTest, MatchingMultiLayer);
    
    enum class Mode { Normal, Modify };
public:
    explicit OrderBook(int64_t min_step, int64_t price_offset, size_t max_price_levels = 65536);

    ~OrderBook();

    void processRequest(const OrderRequest* req);
    void printOrderRequest(const OrderRequest* req);
    void showL2(size_t depth = UINT32_MAX);

private:
    const int64_t min_step_;           // 最小價格單位 (定點數)
    const int64_t price_index_offset_; // 
    const size_t  max_price_levels_;   // price_array_ 大小

    size_t price_to_index(const int64_t price) {
        return price / min_step_ - price_index_offset_;
    }
    size_t index_to_price(const size_t index) {
        return (index + price_index_offset_) * min_step_;
    }
    int price_invalid(const int64_t price) {
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
    void removePriceLevel(PriceLevel* pl);
    void match(Order* incoming);
    void addToBook(Order* order);

    template <Mode m>
    void handleNewOrder(const OrderRequest* req);
    
    template <Mode m>
    void handleCancelOrder(const OrderRequest* req);
    void handleModifyOrder(const OrderRequest* req);

    // Linked list 操作
    void insertOrderToLevel(PriceLevel* level, Order* order);
    void removeOrderFromLevel(Order* order);

    void send_acked(const OrderRequest* req);
    void send_cancelled(const OrderRequest* req);
    void send_modified(const OrderRequest* req);
    void send_reject(const OrderRequest* req, std::string reason /* TODO: change to enum */);

    void send_fill(const Order* incoming, const Order* existing, uint64_t qty_fill);
};
}