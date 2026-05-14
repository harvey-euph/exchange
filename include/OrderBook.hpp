#pragma once
#include <cstdint>
#include <vector>
#include <unordered_map>
#include <atomic>
// #include <rte_ring.h>
#include "generated/order_generated.h"

namespace Exchange {

struct Order
{
    uint64_t order_id;
    uint32_t client_id;
    uint64_t quantity;     // 剩餘量
    uint64_t visible_qty;  // Iceberg 用

    Side side;
    OrderType type;

    Order* prev = nullptr;
    Order* next = nullptr;

    struct PriceLevel* price_level = nullptr;

    uint64_t timestamp;
};

struct PriceLevel
{
    int64_t  price = 0;
    uint64_t total_qty = 0;
    uint64_t order_count = 0;

    Order    dummy_head;   // prev 永遠是 nullptr
    Order    dummy_tail;   // next 永遠是 nullptr

    PriceLevel* higher = nullptr;
    PriceLevel* lower  = nullptr;

    PriceLevel(int64_t p = 0) : price(p) {
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
    void showL2(size_t depth = UINT32_MAX);

    using MatchCallback = void(*)(uint64_t taker_order_id, uint64_t maker_order_id,
                                  int64_t price, uint64_t qty, Side maker_side);

    void setMatchCallback(MatchCallback cb) { match_cb_ = cb; }

private:
    const int64_t min_step_;           // 最小價格單位 (定點數)
    const int64_t price_offset_;       // 價格基準 (array index 0 對應的價格)
    const size_t  max_price_levels_;   // price_array_ 大小

    std::vector<PriceLevel*> price_array_;

    PriceLevel* bid_head_ = nullptr;   // 最高買價
    PriceLevel* ask_head_ = nullptr;   // 最低賣價

    std::unordered_map<uint64_t, Order*> active_orders_;

    MatchCallback match_cb_ = nullptr;

    // 內部 helper
    PriceLevel* GetOrCreatePriceLevel(int64_t price, Side side);
    void eemovePriceLevelIfEmpty(PriceLevel* pl);
    void match(Order* incoming);
    void addToBook(Order* order);
    void cancelOrder(uint64_t order_id);
    void modifyOrder(uint64_t order_id, int64_t new_price, uint64_t new_qty);

    // Linked list 操作
    void insertOrderToLevel(PriceLevel* level, Order* order);
    void removeOrderFromLevel(Order* order);
};
}