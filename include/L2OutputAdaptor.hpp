#pragma once
#include "fbs/order_generated.h"
#include <iostream>
#include <chrono>
#include <string>

namespace Exchange {

/**
 * @brief L2 輸出適配器抽象基底類別 (Interface)
 */
class L2OutputAdaptor {
public:
    virtual ~L2OutputAdaptor() = default;
    virtual void publish(const Exchange::L2Update* l2_update, const void* raw_data, size_t raw_size) = 0;
};

/**
 * @brief 標準輸出適配器實作
 */
class StdoutAdaptor : public L2OutputAdaptor {
public:
    void publish(const Exchange::L2Update* l2_update, const void* /*raw_data*/, size_t /*raw_size*/) override 
    {
        uint32_t symbol_id  = l2_update->symbol_id();
        uint64_t seq        = l2_update->seq_num();
        Exchange::Side side = l2_update->side();
        int64_t price       = l2_update->p();
        uint64_t qty        = l2_update->q();
        uint64_t ts         = l2_update->timestamp();

        std::string side_str = (side == Exchange::Side_Buy) ? "BUY" : "SELL";

        auto now = std::chrono::duration_cast<std::chrono::microseconds>(
                       std::chrono::system_clock::now().time_since_epoch()
                   ).count();
        int64_t latency = now - static_cast<int64_t>(ts);

        std::cout << "[L2] Seq: " << seq 
                  << " | Symbol: " << symbol_id 
                  << " | " << side_str 
                  << " | Price: " << price 
                  << " | Qty: " << qty 
                  << " | Latency: " << latency << " us\n";
    }
};

} // namespace Exchange
