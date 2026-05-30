#pragma once
#include <map>
#include <mutex>
#include <cstdint>
#include "fbs/order_generated.h"

namespace Exchange {

struct L2Book {
    uint32_t symbol_id = 0;
    std::map<int64_t, uint64_t> bids;
    std::map<int64_t, uint64_t> asks;
    std::mutex mutex;

    void update(Side side, int64_t price, uint64_t qty) {
        std::lock_guard<std::mutex> lock(mutex);
        if (side == Side_None) {
            bids.clear();
            asks.clear();
            return;
        }
        auto& levels = (side == Side_Buy) ? bids : asks;
        if (qty == 0) {
            levels.erase(price);
        } else {
            levels[price] = qty;
        }
    }
};

} // namespace Exchange
