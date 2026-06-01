#pragma once
#include <cstdint>
#include "fbs/order_generated.h"

namespace Exchange {

struct PriceLevel; // Forward declaration

struct Order
{
    uint64_t exec_id;
    uint64_t order_id;
    uint32_t client_id;
    uint64_t qty_original;
    uint64_t qty_remaining;
    // uint64_t qty_visible;  // For Iceberg

    OrderType type; // Only consider limit and market order for now

    Order* prev = nullptr;
    Order* next = nullptr;

    PriceLevel* price_level = nullptr;

    uint64_t timestamp;
};

} // namespace Exchange
