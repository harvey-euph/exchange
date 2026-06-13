#include "AlgoTradingClient.hpp"
#include "ring/SHMRingBuffer.hpp"
#include "define.hpp"
#include "TimeUtil.hpp"
#include <iostream>
#include <fstream>
#include <string>
#include <iomanip>
#include <random>
#include <chrono>
#include <thread>
#include <atomic>
#include <vector>
#include <memory>
#include <mutex>
#include <csignal>
#include <algorithm>
#include <unordered_set>
#include <sstream>

namespace Exchange {

volatile sig_atomic_t g_stop = 0;

class BenchmarkTrader : public AlgoTradingClient {
public:
    BenchmarkTrader(const Config& config) : AlgoTradingClient(config), gen_(1337) 
    {
        config_.timer_interval_ms = 100;
        benchmark_thread_ = std::thread(&BenchmarkTrader::benchmark_loop, this);
    }

    ~BenchmarkTrader() override {
        running_ = false;
        if (benchmark_thread_.joinable()) {
            benchmark_thread_.join();
        }
    }

    void on_l2_update(const L2Update*) override {}
    void on_l3_update(const L3Update*) override {}

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

    void on_timer() override {
        if (g_stop) {
            std::cout << "Benchmark interrupted by user.\n";
            running_ = false;
        }
    }

private:
    void high_precision_delay(double sleep_us) {
        if (sleep_us <= 0.0) return;
        auto start = std::chrono::steady_clock::now();
        double target_ns = sleep_us * 1000.0;
        while (true) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now - start).count();
            if (elapsed_ns >= target_ns) break;
            #if defined(__x86_64__) || defined(_M_X64)
                __builtin_ia32_pause();
            #else
                std::this_thread::yield();
            #endif
        }
    }

    void benchmark_loop() {
        while (running_ && !is_ready()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        if (!config_.symbol_ids.empty()) {
            uint32_t symbol_id = config_.symbol_ids[0];
            auto it = symbols_info_.find(symbol_id);
            if (it != symbols_info_.end()) {
                const auto& info = it->second;
                mid_price_ = (info->price_min + info->price_max) / 2;
            }
        }

        start_time_ = std::chrono::steady_clock::now();
        auto last_print_time = start_time_;
        bool waiting_for_responses = false;
        std::chrono::steady_clock::time_point wait_start_time;

        while (running_) {
            if (sent_count_.load(std::memory_order_relaxed) < TARGET_REQUESTS) {
                do_trading_action();
                high_precision_delay(200.0);
            } else {
                if (!waiting_for_responses) {
                    waiting_for_responses = true;
                    wait_start_time = std::chrono::steady_clock::now();
                }

                size_t received_count = 0;
                {
                    std::lock_guard<std::mutex> lock(stats_mtx_);
                    received_count = rtts_all_.size();
                }
                
                if (received_count >= TARGET_REQUESTS) {
                    break;
                }

                auto wait_now = std::chrono::steady_clock::now();
                if (std::chrono::duration_cast<std::chrono::seconds>(wait_now - wait_start_time).count() >= 5) {
                    std::cout << "\n[Warning] Timeout waiting for last " << (TARGET_REQUESTS - received_count) << " responses. Proceeding to report.\n";
                    break;
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_print_time).count() >= 500) {
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
                std::cout << "\rNEW: " << recv_new << "/" << sent_new_count_.load(std::memory_order_relaxed)
                          << " | MOD-S: " << recv_mod_short << "/" << sent_modify_short_count_.load(std::memory_order_relaxed)
                          << " | MOD-L: " << recv_mod_long << "/" << sent_modify_long_count_.load(std::memory_order_relaxed)
                          << " | CAN: " << recv_can << "/" << sent_cancel_count_.load(std::memory_order_relaxed)
                          << " | REJ: " << recv_rej << reject_info
                          << "        " << std::flush;
                last_print_time = now;
            }
        }

        if (sent_count_.load(std::memory_order_relaxed) > 0) {
            report_stats();
        }
        running_ = false;
    }

    void do_trading_action() {
        if (config_.symbol_ids.empty()) return;
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
        size_t idx = std::uniform_int_distribution<size_t>(0, open_orders.size() - 1)(gen_);
        const auto& order = open_orders[idx];

        cancel_order(order.order_id, order.symbol_id, order.side);
        sent_cancel_count_.fetch_add(1, std::memory_order_relaxed);
        sent_count_.fetch_add(1, std::memory_order_relaxed);
    }

    struct Stats {
        uint64_t count = 0;
        double p50 = 0, p90 = 0, p99 = 0, p999 = 0;
    };

    Stats calc_stats(std::vector<double>& rtts) {
        Stats s;
        s.count = rtts.size();
        if (!rtts.empty()) {
            std::sort(rtts.begin(), rtts.end());
            s.p50 = rtts[std::min(static_cast<size_t>(rtts.size() * 0.50), rtts.size() - 1)];
            s.p90 = rtts[std::min(static_cast<size_t>(rtts.size() * 0.90), rtts.size() - 1)];
            s.p99 = rtts[std::min(static_cast<size_t>(rtts.size() * 0.99), rtts.size() - 1)];
            s.p999 = rtts[std::min(static_cast<size_t>(rtts.size() * 0.999), rtts.size() - 1)];
        }
        return s;
    }

    std::string format_stats_row(const std::string& name, const Stats& s) {
        std::ostringstream oss;
        oss << std::left << std::setw(21) << name << std::right << std::setw(9) << s.count << "    ";
        if (s.count > 0) {
            oss << std::fixed << std::setprecision(2) << std::setw(6) << s.p50 << "/ "
                << std::setw(6) << s.p90 << "/ "
                << std::setw(6) << s.p99 << "/ "
                << std::setw(6) << s.p999;
        } else {
            oss << "   0.00/   0.00/   0.00/   0.00";
        }
        return oss.str();
    }

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

    std::thread benchmark_thread_;
    std::atomic<uint64_t> sent_count_{0};
    std::atomic<uint64_t> sent_new_count_{0};
    std::atomic<uint64_t> sent_modify_short_count_{0};
    std::atomic<uint64_t> sent_modify_long_count_{0};
    std::atomic<uint64_t> sent_cancel_count_{0};
    const uint64_t TARGET_REQUESTS = 500000;

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

    int current_mod_type_{0};

    std::mt19937 gen_;
    std::uniform_real_distribution<> dist_action_{0.0, 1.0};
    std::uniform_int_distribution<> dist_buy_sell_{0, 1};
    int64_t mid_price_{5000};
};

} // namespace Exchange

int main() {
    std::signal(SIGINT, [](int) {
        std::cout << "\n[BenchmarkTrader] Caught SIGINT. Gracefully shutting down..." << std::endl;
        Exchange::g_stop = 1;
    });

    Exchange::AlgoTradingConfig config;
    config.client_id = 1000;
    config.symbol_ids = {1};
    config.timer_interval_ms = 100;

    Exchange::BenchmarkTrader trader(config);
    return trader.run();
}
