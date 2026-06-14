#pragma once

#include "SimpleWSClient.hpp"
#include "fbs/exchange_generated.h"
#include "ClientAccount.hpp"
#include <string>
#include <vector>
#include <memory>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <unordered_map>

namespace Exchange {

struct AlgoTradingConfig {
    std::string host = "127.0.0.1";
    std::string mgmt_port = "9001";
    std::string l2_port = "9002";
    std::string l3_port = "9003";
    std::string http_port = "8080";
    bool use_http = false;
    uint32_t client_id = 101;
    std::vector<uint32_t> symbol_ids = {1};
    uint32_t timer_interval_ms = 1000;
};

class AlgoTradingClient {
public:
    using Config = AlgoTradingConfig;

    AlgoTradingClient(const Config& config = Config());
    virtual ~AlgoTradingClient();

    virtual void on_l2_update(const L2Update* update) = 0;
    virtual void on_l3_update(const L3Update* update) = 0;
    virtual void on_order_response(const OrderResponse* response);
    virtual void on_position_response(const PositionResponse* response);
    virtual void on_timer() {}

    // Layer 2: Convenience
    void new_limit_order(uint32_t symbol_id, Side side, int64_t p, uint64_t q, uint64_t visible_qty = 0);
    void new_market_order(uint32_t symbol_id, Side side, uint64_t q);

    // Layer 1: Basic Actions
    void new_order(uint32_t symbol_id, Side side, OrderType type, int64_t p, uint64_t q, uint64_t visible_qty = 0);
    void replace_order(uint64_t order_id, int64_t p, uint64_t q, uint32_t symbol_id = 0, Side side = Side_None);
    void cancel_order(uint64_t order_id, uint32_t symbol_id = 0, Side side = Side_None);

    // Layer 0: Raw Request
    virtual void send_order_request(OrderRequestT& order);

    void query_position(uint32_t symbol_id);

    int run();
    void stop();

    void set_timer_interval(uint32_t ms) { config_.timer_interval_ms = ms; }

    bool is_ready() const { return ready_; }
    void wait_until_ready();

protected:
    Config config_;
    std::unique_ptr<SimpleWSClient> mgmt_client_;
    std::unique_ptr<SimpleWSClient> md_client_;
    bool running_ = true;
    uint64_t next_id_ = 1000001;

    std::atomic<bool> ready_{false};
    std::mutex ready_mtx_;
    std::condition_variable ready_cv_;

    bool validate_price(uint32_t symbol_id, int64_t p, std::string& err_msg);
    std::unordered_map<uint32_t, std::unique_ptr<SymbolInfoT>> symbols_info_;

    ClientAccount account_;
};

} // namespace Exchange
