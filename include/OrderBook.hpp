#pragma once
#include <map>
#include <vector>
#include <atomic>
#include <cstdint>
#include <unordered_map>
// #include <rte_ring.h>
#include "generated/order_generated.h"

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
    size_t   price_idx = 0;
    uint64_t total_qty = 0;
    uint64_t order_count = 0;

    Order    dummy_head;   // prev always nullptr
    Order    dummy_tail;   // next always nullptr

    PriceLevel* better = nullptr;
    PriceLevel* worse  = nullptr;

    PriceLevel(size_t p = 0) : price_idx(p) {
        dummy_head.next = &dummy_tail;
        dummy_tail.prev = &dummy_head;
    }
};

class OrderBook
{
public:
    explicit OrderBook(
        int64_t min_step, 
        int64_t price_offset, 
        size_t max_price_levels = 65536
    );

    ~OrderBook();

    void processRequest(const OrderRequest* req);
    void printOrderRequest(const OrderRequest* req);
    
    void showL2(size_t depth = UINT32_MAX);

    using MatchCallback = void(*)(
        uint64_t taker_order_id, uint64_t maker_order_id,
        int64_t price, uint64_t qty, Side maker_side
    );

    void setMatchCallback(MatchCallback cb) { match_cb_ = cb; }

private:
    const int64_t min_step_;           // 最小價格單位 (定點數)
    const int64_t price_offset_;       // 價格基準 (array index 0 對應的價格)
    const size_t  max_price_levels_;   // price_array_ 大小

    int price_invalid(const int64_t price) {
        if (price_offset_ > price) return 1;
        if (price_offset_ + (int64_t)max_price_levels_ < price) return -1;
        return 0;
    }
    size_t price_to_index(const int64_t price) {
        return price - price_offset_;
    }
    size_t index_to_price(const size_t index) {
        return index + price_offset_;
    }

    Order* createOrder(const OrderRequest* req);

    std::vector<PriceLevel> price_array_;
    // PriceLevel unmatched_market_orders[2]; // [B][A]
    PriceLevel* best_levels_[2] = {nullptr, nullptr};  // [B1][A1] , can be market order

    std::unordered_map<uint64_t, Order*> active_orders_;
    std::map<size_t, PriceLevel*> active_levels_;

    MatchCallback match_cb_ = nullptr;

    // 內部 helper
    PriceLevel* GetOrCreatePriceLevel(int64_t price, Side side);
    void removePriceLevelIfEmpty(PriceLevel* pl);
    void match(Order* incoming);
    void addToBook(Order* order);

    void handleNewOrder(Side side, size_t price_idx, const Order* incoming);
    void handleCancelOrder(uint32_t client_id, uint64_t order_id);
    void handleModifyOrder(uint32_t client_id, uint64_t order_id, size_t new_price_idx, uint64_t new_qty);

    // Linked list 操作
    void insertOrderToLevel(PriceLevel* level, Order* order);
    void removeOrderFromLevel(Order* order);

    void send_reject(uint32_t client_id, std::string reason /* TODO: change to enum */);
    void send_acked(const Order* incoming);
    void send_fill(const Order* incoming, const Order* existing, uint64_t qty_fill);
};
}