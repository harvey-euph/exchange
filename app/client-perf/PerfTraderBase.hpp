#pragma once

#include "AlgoTradingClient.hpp"
#include "PerfCommon.hpp"
#include <iostream>
#include <random>
#include <atomic>
#include <mutex>
#include <vector>
#include <unordered_map>
#include <string>

namespace Exchange {

class PerfTraderBase : public AlgoTradingClient {
public:
    PerfTraderBase(const Config& config, bool silent) 
        : AlgoTradingClient(config), silent_(silent), gen_(1337) {}

    virtual ~PerfTraderBase() = default;

    void on_order_response(const OrderResponse* response) override {
        AlgoTradingClient::on_order_response(response);
        
        auto exec = response->exec_type();
        if (exec == ExecType_New || exec == ExecType_Replaced || exec == ExecType_Cancelled || exec == ExecType_Rejected) {
            uint64_t exec_id = response->exec_id();
            std::chrono::steady_clock::time_point start_time;
            int mod_type = 0;
            bool found = false;
            {
                std::lock_guard<std::mutex> lock(send_times_mtx_);
                auto it = request_send_times_.find(exec_id);
                if (it != request_send_times_.end()) {
                    start_time = it->second.first;
                    mod_type = it->second.second;
                    request_send_times_.erase(it);
                    found = true;
                }
            }
            if (found) {
                auto end_time = std::chrono::steady_clock::now();
                double rtt_us = std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count() / 1000.0;
                
                std::lock_guard<std::mutex> lock(stats_mtx_);
                if (exec == ExecType_New) {
                    rtts_new_.push_back(rtt_us);
                } else if (exec == ExecType_Replaced) {
                    if (mod_type == 1) {
                        rtts_modify_short_.push_back(rtt_us);
                    } else if (mod_type == 2) {
                        rtts_modify_long_.push_back(rtt_us);
                    } else {
                        rtts_modify_.push_back(rtt_us);
                    }
                } else if (exec == ExecType_Cancelled) {
                    rtts_cancel_.push_back(rtt_us);
                } else if (exec == ExecType_Rejected) {
                    reject_codes_count_[static_cast<int>(response->reject_code())]++;
                    rtts_reject_.push_back(rtt_us);
                }
                rtts_all_.push_back(rtt_us);
                
                on_response_processed(rtt_us);
            }
        }
    }

    void send_order_request(OrderRequestT& order) override {
        AlgoTradingClient::send_order_request(order);
        
        {
            std::lock_guard<std::mutex> lock(send_times_mtx_);
            request_send_times_[order.exec_id] = {std::chrono::steady_clock::now(), current_mod_type_};
        }
    }

    // Hook for round-trip-sender to chain next request
    virtual void on_response_processed(double rtt_us) {}

    void report_stats() {
        std::vector<double> rtts_new, rtts_modify, rtts_modify_short, rtts_modify_long, rtts_cancel, rtts_reject, rtts_all;
        {
            std::lock_guard<std::mutex> lock(stats_mtx_);
            rtts_new = rtts_new_;
            rtts_modify = rtts_modify_;
            rtts_modify_short = rtts_modify_short_;
            rtts_modify_long = rtts_modify_long_;
            rtts_cancel = rtts_cancel_;
            rtts_reject = rtts_reject_;
            rtts_all = rtts_all_;
        }
        
        Stats s_new = calc_stats(rtts_new);
        Stats s_modify = calc_stats(rtts_modify);
        Stats s_modify_short = calc_stats(rtts_modify_short);
        Stats s_modify_long = calc_stats(rtts_modify_long);
        Stats s_cancel = calc_stats(rtts_cancel);
        Stats s_reject = calc_stats(rtts_reject);
        Stats s_all = calc_stats(rtts_all);

        std::cout << "\n=================================================== Latency Statistics (us) ===================================================\n";
        std::cout << std::left << std::setw(22) << "ExecType" << std::setw(22) << "Count" << "client-E2E\n";
        std::cout << "-------------------------------------------------------------------------------------------------------------------------------\n";
        std::cout << format_stats_row("New", s_new) << "\n";
        if (s_modify.count > 0) std::cout << format_stats_row("Modify", s_modify) << "\n";
        std::cout << format_stats_row("Modify-Short", s_modify_short) << "\n";
        std::cout << format_stats_row("Modify-Long", s_modify_long) << "\n";
        std::cout << format_stats_row("Cancel", s_cancel) << "\n";
        std::cout << format_stats_row("Reject", s_reject) << "\n";
        std::cout << "-------------------------------------------------------------------------------------------------------------------------------\n";
        std::cout << format_stats_row("ALL", s_all) << "\n";
        std::cout << "===============================================================================================================================\n";
    }

    void do_trading_action() {
        if (config_.symbol_ids.empty()) {
            return;
        }
        uint32_t symbol_id = config_.symbol_ids[0];
        
        auto open_orders = account_.get_open_orders();
        double roll = dist_action_(gen_);

        if (open_orders.empty()) {
            build_depth_scenario(symbol_id);
        } else {
            if (roll < 0.25) {
                build_depth_scenario(symbol_id);
            } else if (roll < 0.50) {
                cancel_random_scenario(open_orders);
            } else if (roll < 0.75) {
                modify_short_scenario(open_orders);
            } else {
                modify_long_scenario(open_orders);
            }
        }
    }

    void build_depth_scenario(uint32_t symbol_id) {
        auto it = symbols_info_.find(symbol_id);
        int64_t step = (it != symbols_info_.end()) ? it->second->price_min_step : 1;
        bool is_buy = dist_buy_sell_(gen_);
        int64_t level = std::uniform_int_distribution<int64_t>(1, 5)(gen_) * step;
        int64_t price = is_buy ? (mid_price_ - level) : (mid_price_ + level);
        if (it != symbols_info_.end()) {
            price = std::max(it->second->price_min, std::min(it->second->price_max, price));
        }
        uint64_t qty = std::uniform_int_distribution<uint64_t>(20, 100)(gen_);

        new_limit_order(symbol_id, is_buy ? Side_Buy : Side_Sell, price, qty);
        sent_new_count_.fetch_add(1, std::memory_order_relaxed);
        sent_count_.fetch_add(1, std::memory_order_relaxed);
    }

    void modify_short_scenario(const std::vector<OrderResponseT>& open_orders) {
        if (open_orders.empty()) return;
        size_t idx = std::uniform_int_distribution<size_t>(0, open_orders.size() - 1)(gen_);
        const auto& order = open_orders[idx];
        
        uint64_t new_qty = order.q;
        if (new_qty > 1) {
            new_qty = std::uniform_int_distribution<uint64_t>(1, order.q - 1)(gen_);
        }
        
        current_mod_type_ = 1;
        if (next_id_ % 2 != 0) next_id_++; // Enforce Even for Mod-Short
        replace_order(order.order_id, order.p, new_qty, order.symbol_id, order.side);
        current_mod_type_ = 0;
        
        sent_modify_short_count_.fetch_add(1, std::memory_order_relaxed);
        sent_count_.fetch_add(1, std::memory_order_relaxed);
    }

    void modify_long_scenario(const std::vector<OrderResponseT>& open_orders) {
        if (open_orders.empty()) return;
        size_t idx = std::uniform_int_distribution<size_t>(0, open_orders.size() - 1)(gen_);
        const auto& order = open_orders[idx];
        auto it = symbols_info_.find(order.symbol_id);
        int64_t step = (it != symbols_info_.end()) ? it->second->price_min_step : 1;

        int64_t price_shift = std::uniform_int_distribution<int64_t>(-2, 2)(gen_) * step;
        if (price_shift == 0) price_shift = step;
        int64_t new_price = order.p + price_shift;
        if (it != symbols_info_.end()) {
            new_price = std::max(it->second->price_min, std::min(it->second->price_max, new_price));
        } else {
            if (new_price <= 0) new_price = 1;
        }

        uint64_t new_qty = order.q;
        new_qty = std::uniform_int_distribution<uint64_t>(5, 100)(gen_);

        current_mod_type_ = 2;
        if (next_id_ % 2 == 0) next_id_++; // Enforce Odd for Mod-Long
        replace_order(order.order_id, new_price, new_qty, order.symbol_id, order.side);
        current_mod_type_ = 0;
        
        sent_modify_long_count_.fetch_add(1, std::memory_order_relaxed);
        sent_count_.fetch_add(1, std::memory_order_relaxed);
    }

    void cancel_random_scenario(const std::vector<OrderResponseT>& open_orders) {
        if (open_orders.empty()) return;
        size_t idx = std::uniform_int_distribution<size_t>(0, open_orders.size() - 1)(gen_);
        const auto& order = open_orders[idx];

        cancel_order(order.order_id, order.symbol_id, order.side);
        sent_cancel_count_.fetch_add(1, std::memory_order_relaxed);
        sent_count_.fetch_add(1, std::memory_order_relaxed);
    }

    void print_progress(const std::string& prefix = "\r") {
        size_t recv_new = 0, recv_mod_short = 0, recv_mod_long = 0, recv_can = 0, recv_rej = 0;
        std::string reject_info;
        {
            std::lock_guard<std::mutex> lock(stats_mtx_);
            recv_new = rtts_new_.size();
            recv_mod_short = rtts_modify_short_.size();
            recv_mod_long = rtts_modify_long_.size();
            recv_can = rtts_cancel_.size();
            recv_rej = rtts_reject_.size();
            if (recv_rej > 0) {
                reject_info = " (";
                bool first = true;
                for (const auto& kv : reject_codes_count_) {
                    if (!first) reject_info += ", ";
                    reject_info += "Code" + std::to_string(kv.first) + ":" + std::to_string(kv.second);
                    first = false;
                }
                reject_info += ")";
            }
        }
        size_t recv_total = recv_new + recv_mod_short + recv_mod_long + recv_can + recv_rej;
        std::cout << prefix << "NEW: " << recv_new << "/" << sent_new_count_.load(std::memory_order_relaxed)
                  << " | MOD-S: " << recv_mod_short << "/" << sent_modify_short_count_.load(std::memory_order_relaxed)
                  << " | MOD-L: " << recv_mod_long << "/" << sent_modify_long_count_.load(std::memory_order_relaxed)
                  << " | CAN: " << recv_can << "/" << sent_cancel_count_.load(std::memory_order_relaxed)
                  << " | REJ: " << recv_rej << reject_info
                  << " | Total: " << recv_total
                  << "        " << std::flush;
    }

protected:
    std::atomic<uint64_t> sent_count_{0};
    std::atomic<uint64_t> sent_new_count_{0};
    std::atomic<uint64_t> sent_modify_short_count_{0};
    std::atomic<uint64_t> sent_modify_long_count_{0};
    std::atomic<uint64_t> sent_cancel_count_{0};

    std::chrono::steady_clock::time_point start_time_;

    std::unordered_map<uint64_t, std::pair<std::chrono::steady_clock::time_point, int>> request_send_times_;
    std::mutex send_times_mtx_;

    std::mutex stats_mtx_;
    std::vector<double> rtts_new_;
    std::vector<double> rtts_modify_;
    std::vector<double> rtts_modify_short_;
    std::vector<double> rtts_modify_long_;
    std::vector<double> rtts_cancel_;
    std::vector<double> rtts_reject_;
    std::vector<double> rtts_all_;
    std::unordered_map<int, uint64_t> reject_codes_count_;

    bool silent_{false};
    int current_mod_type_{0};

    std::mt19937 gen_;
    std::uniform_real_distribution<> dist_action_{0.0, 1.0};
    std::uniform_int_distribution<> dist_buy_sell_{0, 1};
    int64_t mid_price_{5000};
};

} // namespace Exchange
