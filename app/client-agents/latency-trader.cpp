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

namespace Exchange {

volatile sig_atomic_t g_stop = 0;

class StressTrader : public AlgoTradingClient {
public:
    StressTrader(const Config& config, bool is_latency_mode) : AlgoTradingClient(config), is_latency_mode_(is_latency_mode) 
    {
        if (is_latency_mode_) {
            double limit_us = 500.0;
            std::ifstream ifs("docs/limit.md");
            if (ifs.is_open()) {
                std::string line;
                while (std::getline(ifs, line)) {
                    if (line.find("limit_interval_us=") != std::string::npos) {
                        limit_us = std::stod(line.substr(18));
                    }
                }
            }
            current_interval_us_ = limit_us * 2.0;
        }

        // Check step progression and unresponsiveness frequently (every 100ms)
        config_.timer_interval_ms = 100;

        // Calibrate TSC frequency
        auto start_time = std::chrono::steady_clock::now();
        uint64_t start_tsc = read_tsc_begin();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        uint64_t end_tsc = read_tsc_end();
        auto end_time = std::chrono::steady_clock::now();
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count();
        if (ns > 0) {
            tsc_hz_ = static_cast<double>(end_tsc - start_tsc) / (static_cast<double>(ns) / 1e9);
        }

        // Establish connections to SHM observers
        reconnect_shm();

        // Initialize time references
        step_start_time_ = std::chrono::steady_clock::now();
        last_response_time_ = std::chrono::steady_clock::now();

        // Print header for the step-load test report
        std::string border_line(150, '=');
        std::string sep_line(150, '-');
        std::cout << "\n" << border_line << "\n";
        std::cout << std::string(50, ' ') << "STRESS TESTING STEP-LOAD TEST REPORT (60s Steps)\n";
        std::cout << border_line << "\n";
        std::cout << "| Time     |  Interval     | Rate          | Avg RTT      | P90 RTT      | P99 RTT      | Max RTT      | Peak Ring Occupancy                         |\n";
        std::cout << sep_line << "\n";
        std::cout << std::flush;

        // Launch high-frequency queue monitoring thread (10us sampling rate)
        monitoring_thread_ = std::thread(&StressTrader::monitoring_loop, this);

        // Launch high-frequency stress-testing thread
        stress_thread_ = std::thread(&StressTrader::stress_loop, this);
    }

    ~StressTrader() override {
        running_ = false;
        if (stress_thread_.joinable()) {
            stress_thread_.join();
        }
        if (monitoring_thread_.joinable()) {
            monitoring_thread_.join();
        }
    }

    void on_l2_update(const L2Update*) override {}
    void on_l3_update(const L3Update*) override {}

    void on_order_response(const OrderResponse* response) override {
        AlgoTradingClient::on_order_response(response);
        
        auto exec = response->exec_type();
        if (exec == ExecType_New) {
            ack_count_.fetch_add(1, std::memory_order_relaxed);
        } else if (exec == ExecType_Replaced) {
            modify_count_.fetch_add(1, std::memory_order_relaxed);
        } else if (exec == ExecType_Cancelled) {
            cancel_count_.fetch_add(1, std::memory_order_relaxed);
        } else if (exec == ExecType_Fill || exec == ExecType_PartialFill) {
            fill_count_.fetch_add(1, std::memory_order_relaxed);
        }

        // Update response heartbeat
        last_response_time_ = std::chrono::steady_clock::now();

        // Measure RTT for direct request confirmations
        if (exec == ExecType_New || exec == ExecType_Replaced || exec == ExecType_Cancelled) {
            uint64_t exec_id = response->exec_id();
            std::chrono::steady_clock::time_point start_time;
            bool found = false;
            bool is_short_mod = false;
            {
                std::lock_guard<std::mutex> lock(send_times_mtx_);
                auto it = request_send_times_.find(exec_id);
                if (it != request_send_times_.end()) {
                    start_time = it->second;
                    request_send_times_.erase(it);
                    found = true;
                }
                auto sit = short_mod_exec_ids_.find(exec_id);
                if (sit != short_mod_exec_ids_.end()) {
                    is_short_mod = true;
                    short_mod_exec_ids_.erase(sit);
                }
            }
            if (found) {
                auto end_time = std::chrono::steady_clock::now();
                double rtt_us = std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count() / 1000.0;
                
                std::lock_guard<std::mutex> lock(step_stats_mtx_);
                step_rtt_sum_us_ += rtt_us;
                step_rtt_count_++;
                step_rtts_.push_back(rtt_us);
                if (rtt_us > step_max_rtt_us_) {
                    step_max_rtt_us_ = rtt_us;
                }
                if (is_latency_mode_) {
                    if (exec == ExecType_New) rtt_new_.push_back(rtt_us);
                    else if (exec == ExecType_Replaced) {
                        if (is_short_mod) rtt_mod_short_.push_back(rtt_us);
                        else rtt_mod_long_.push_back(rtt_us);
                    }
                    else if (exec == ExecType_Cancelled) rtt_can_.push_back(rtt_us);
                }
            }
        }
    }

    void send_order_request(OrderRequestT& order) override {
        AlgoTradingClient::send_order_request(order);
        
        // Record send time against exec_id
        {
            std::lock_guard<std::mutex> lock(send_times_mtx_);
            request_send_times_[order.exec_id] = std::chrono::steady_clock::now();
            if (next_is_short_mod_.exchange(false, std::memory_order_relaxed)) {
                short_mod_exec_ids_.insert(order.exec_id);
            }
        }
    }

    void on_timer() override {

        auto now = std::chrono::steady_clock::now();

        if (g_stop) {
            std::string reason = "User Interrupted (Ctrl+C)";
            std::cout << "Stress test terminated by user. Reason: " << reason << ". Writing limit and exiting...\n";
            if (!is_latency_mode_) {
                std::ofstream ofs("docs/limit.md");
                ofs << "limit_interval_us=" << current_interval_us_.load() << "\n";
                ofs << "stop_reason=" << reason << "\n";
                ofs.flush();
                ofs.close();
            }
            std::_Exit(0);
        }

        // Check for server death/unresponsiveness (termination condition)
        if (!is_latency_mode_ && is_ready() && sent_count_.load(std::memory_order_relaxed) > 0) {
            double secs_since_resp = std::chrono::duration_cast<std::chrono::seconds>(now - last_response_time_).count();
            double resp_ratio = resp_observer_ ? resp_observer_->get_occupancy_ratio() : 0.0;
            
            bool timeout = (secs_since_resp >= 1.0);
            bool resp_ring_full = (resp_ratio > 0.50);

            if (timeout || resp_ring_full) {
                std::string reason = timeout ? "Timeout > 1s" : "Resp Ring > 50%";
                std::cout << "Stress limit reached. Reason: " << reason << ". Writing limit and exiting...\n";
                std::ofstream ofs("docs/limit.md");
                ofs << "limit_interval_us=" << current_interval_us_.load() << "\n";
                ofs << "stop_reason=" << reason << "\n";
                ofs.flush();
                ofs.close();
                std::_Exit(0);
            }
        }
        if (is_ready() && sent_count_.load(std::memory_order_relaxed) > 0) {
            double secs_since_resp = std::chrono::duration_cast<std::chrono::seconds>(now - last_response_time_).count();
            if (secs_since_resp >= 10.0) {
                std::cout << "\n=========================================================================================================================\n";
                std::cout << "CRITICAL FAILURE: Server has stopped responding! (No response received for " << secs_since_resp << " seconds).\n";
                std::cout << "Matching Engine is likely deadlocked, crashed, or queue buffers are completely blocked.\n";
                std::cout << "Stress test terminated.\n";
                std::cout << "=========================================================================================================================\n";
                std::_Exit(0);
            }
        }

        // Check if 60 seconds have elapsed to report and adjust interval
        double elapsed_sec = std::chrono::duration_cast<std::chrono::seconds>(now - step_start_time_).count();
        if (elapsed_sec >= 60.0) {
            report_step_row(elapsed_sec);
        }
    }

private:
    void reconnect_shm() {
        if (!req_observer_) {
            try {
                req_observer_ = std::make_unique<Exchange::SHMObserver>(ORDER_REQUEST, 0);
            } catch (...) {}
        }
        if (!resp_observer_) {
            try {
                resp_observer_ = std::make_unique<Exchange::SHMObserver>(ORDER_RESPONSE, 0);
            } catch (...) {}
        }
        if (!l2_observer_) {
            try {
                l2_observer_ = std::make_unique<Exchange::SHMObserver>(L2_UPDATE_RING, 0);
            } catch (...) {}
        }
        if (!l3_observer_) {
            try {
                l3_observer_ = std::make_unique<Exchange::SHMObserver>(L3_UPDATE_RING, 0);
            } catch (...) {}
        }
    }

    void high_precision_delay(double sleep_us) {
        if (sleep_us <= 0.0) return;
        if (sleep_us >= 1000.0) {
            std::this_thread::sleep_for(std::chrono::microseconds(static_cast<int64_t>(sleep_us)));
        } else {
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
    }

    // Dedicated high-frequency monitoring thread (samples all 4 rings at 50us intervals)
    void monitoring_loop() {
        while (running_) {
            reconnect_shm();

            double req_ratio = req_observer_ ? req_observer_->get_occupancy_ratio() : 0.0;
            double resp_ratio = resp_observer_ ? resp_observer_->get_occupancy_ratio() : 0.0;
            double l2_ratio = l2_observer_ ? l2_observer_->get_occupancy_ratio() : 0.0;
            double l3_ratio = l3_observer_ ? l3_observer_->get_occupancy_ratio() : 0.0;

            {
                std::lock_guard<std::mutex> lock(step_stats_mtx_);
                if (req_ratio * 100.0 > peak_req_ratio_) peak_req_ratio_ = req_ratio * 100.0;
                if (resp_ratio * 100.0 > peak_resp_ratio_) peak_resp_ratio_ = resp_ratio * 100.0;
                if (l2_ratio * 100.0 > peak_l2_ratio_) peak_l2_ratio_ = l2_ratio * 100.0;
                if (l3_ratio * 100.0 > peak_l3_ratio_) peak_l3_ratio_ = l3_ratio * 100.0;
            }

            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    }

    void stress_loop() {
        // Wait until WebSocket session is logged in and fully ready
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

        step_start_time_ = std::chrono::steady_clock::now();
        last_response_time_ = std::chrono::steady_clock::now();

        if (is_latency_mode_) {
            gen_.seed(12345);
        }
        while (running_) {
            if (is_latency_mode_ && sent_count_.load(std::memory_order_relaxed) >= 100000) {
                std::this_thread::sleep_for(std::chrono::seconds(2));
                generate_latency_report();
                running_ = false;
                std::_Exit(0);
            }
            // Execute trading actions
            do_trading_action();

            // Apply fixed delay for the current step-load level
            double cur_sleep = current_interval_us_.load(std::memory_order_relaxed);
            high_precision_delay(cur_sleep);
        }
    }

    void do_trading_action() {
        if (config_.symbol_ids.empty()) return;
        uint32_t symbol_id = config_.symbol_ids[0];
        auto it = symbols_info_.find(symbol_id);
        int64_t step = (it != symbols_info_.end()) ? it->second->price_min_step : 1;

        // Random walk of the mid price (5% chance per action to shift by 1 tick)
        if (dist_action_(gen_) < 0.05) {
            mid_price_ += (std::uniform_int_distribution<int>(0, 1)(gen_) == 0 ? step : -step);
            if (it != symbols_info_.end()) {
                mid_price_ = std::max(it->second->price_min, std::min(it->second->price_max, mid_price_));
            } else {
                if (mid_price_ < 1000) mid_price_ = 1000;
            }
        }

        auto open_orders = account_.get_open_orders();
        double roll = dist_action_(gen_);

        if (open_orders.size() < 10000) {
            // Build-up phase: mostly NEW to increase open orders, but still do some MOD/CANCEL
            if (open_orders.empty() || roll < 0.70) {
                double r2 = dist_action_(gen_);
                if (r2 < 0.80) build_depth_scenario(symbol_id);
                else if (r2 < 0.95) take_one_layer_scenario(symbol_id);
                else sweep_multi_layers_scenario(symbol_id);
            } else if (roll < 0.80) {
                modify_short_scenario(open_orders);
            } else if (roll < 0.90) {
                modify_long_scenario(open_orders);
            } else {
                cancel_random_scenario(open_orders);
            }
        } else {
            // Balanced phase: 1:1:1:1
            if (roll < 0.25) {
                double r2 = dist_action_(gen_);
                if (r2 < 0.40) build_depth_scenario(symbol_id);
                else if (r2 < 0.80) take_one_layer_scenario(symbol_id);
                else sweep_multi_layers_scenario(symbol_id);
            } else if (roll < 0.50) {
                modify_short_scenario(open_orders);
            } else if (roll < 0.75) {
                modify_long_scenario(open_orders);
            } else {
                cancel_random_scenario(open_orders);
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
        sent_count_.fetch_add(1, std::memory_order_relaxed);
    }

    void take_one_layer_scenario(uint32_t symbol_id) {
        auto it = symbols_info_.find(symbol_id);
        int64_t step = (it != symbols_info_.end()) ? it->second->price_min_step : 1;
        bool is_buy = dist_buy_sell_(gen_);
        int64_t price = is_buy ? (mid_price_ + step) : (mid_price_ - step);
        if (it != symbols_info_.end()) {
            price = std::max(it->second->price_min, std::min(it->second->price_max, price));
        }
        uint64_t qty = std::uniform_int_distribution<uint64_t>(10, 40)(gen_);

        new_limit_order(symbol_id, is_buy ? Side_Buy : Side_Sell, price, qty);
        sent_count_.fetch_add(1, std::memory_order_relaxed);
    }

    void sweep_multi_layers_scenario(uint32_t symbol_id) {
        auto it = symbols_info_.find(symbol_id);
        int64_t step = (it != symbols_info_.end()) ? it->second->price_min_step : 1;
        bool is_buy = dist_buy_sell_(gen_);
        int64_t price = is_buy ? (mid_price_ + 5 * step) : (mid_price_ - 5 * step);
        if (it != symbols_info_.end()) {
            price = std::max(it->second->price_min, std::min(it->second->price_max, price));
        }
        uint64_t qty = std::uniform_int_distribution<uint64_t>(150, 500)(gen_);

        new_limit_order(symbol_id, is_buy ? Side_Buy : Side_Sell, price, qty);
        sent_count_.fetch_add(1, std::memory_order_relaxed);
    }

    void modify_short_scenario(const std::vector<OrderResponseT>& open_orders) {
        size_t idx = std::uniform_int_distribution<size_t>(0, open_orders.size() - 1)(gen_);
        const auto& order = open_orders[idx];
        
        uint64_t new_qty = order.q;
        if (new_qty > 1) {
            new_qty = std::uniform_int_distribution<uint64_t>(1, order.q - 1)(gen_);
        }
        
        next_is_short_mod_.store(true, std::memory_order_relaxed);
        replace_order(order.order_id, order.p, new_qty, order.symbol_id, order.side);
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
        if (dist_aggressive_(gen_) < 0.10) {
            new_qty = std::uniform_int_distribution<uint64_t>(5, 100)(gen_);
        }

        replace_order(order.order_id, new_price, new_qty, order.symbol_id, order.side);
        sent_count_.fetch_add(1, std::memory_order_relaxed);
    }

    void cancel_random_scenario(const std::vector<OrderResponseT>& open_orders) {
        size_t idx = std::uniform_int_distribution<size_t>(0, open_orders.size() - 1)(gen_);
        const auto& order = open_orders[idx];

        cancel_order(order.order_id, order.symbol_id, order.side);
        sent_count_.fetch_add(1, std::memory_order_relaxed);
    }

    void report_step_row(double elapsed_sec) {
        uint64_t cur_sent = sent_count_.load(std::memory_order_relaxed);
        double tps = static_cast<double>(cur_sent - last_sent_count_) / elapsed_sec;
        last_sent_count_ = cur_sent;

        double cur_interval_us = current_interval_us_.load(std::memory_order_relaxed);

        // Fetch RTT metrics under lock
        double avg_rtt_us = 0.0;
        double p90_rtt_us = 0.0;
        double p99_rtt_us = 0.0;
        double max_rtt_us = 0.0;
        double peak_req = 0.0;
        double peak_resp = 0.0;
        double peak_l2 = 0.0;
        double peak_l3 = 0.0;
        
        std::vector<double> rtts;
        {
            std::lock_guard<std::mutex> lock(step_stats_mtx_);
            if (step_rtt_count_ > 0) {
                avg_rtt_us = step_rtt_sum_us_ / step_rtt_count_;
            }
            max_rtt_us = step_max_rtt_us_;
            rtts = std::move(step_rtts_);
            step_rtts_.clear();
            
            peak_req = peak_req_ratio_;
            peak_resp = peak_resp_ratio_;
            peak_l2 = peak_l2_ratio_;
            peak_l3 = peak_l3_ratio_;

            // Reset current step accumulators
            step_rtt_sum_us_ = 0.0;
            step_rtt_count_ = 0;
            step_max_rtt_us_ = 0.0;
            peak_req_ratio_ = 0.0;
            peak_resp_ratio_ = 0.0;
            peak_l2_ratio_ = 0.0;
            peak_l3_ratio_ = 0.0;
        }
        
        if (!rtts.empty()) {
            std::sort(rtts.begin(), rtts.end());
            size_t p90_idx = static_cast<size_t>(rtts.size() * 0.90);
            size_t p99_idx = static_cast<size_t>(rtts.size() * 0.99);
            if (p90_idx >= rtts.size()) p90_idx = rtts.size() - 1;
            if (p99_idx >= rtts.size()) p99_idx = rtts.size() - 1;
            p90_rtt_us = rtts[p90_idx];
            p99_rtt_us = rtts[p99_idx];
        }

        // Print row
        std::cout << "| " << std::setw(5) << (step_counter_ * 10) << " s  "
                  << "| " << std::setw(8) << std::fixed << std::setprecision(2) << cur_interval_us << " us   "
                  << "| " << std::setw(7) << std::fixed << std::setprecision(1) << tps << " tps   "
                  << "| " << std::setw(7) << std::fixed << std::setprecision(1) << avg_rtt_us << " us   "
                  << "| " << std::setw(7) << std::fixed << std::setprecision(1) << p90_rtt_us << " us   "
                  << "| " << std::setw(7) << std::fixed << std::setprecision(1) << p99_rtt_us << " us   "
                  << "| " << std::setw(7) << std::fixed << std::setprecision(1) << max_rtt_us << " us   "
                  << "| Req:" << std::setw(4) << std::fixed << std::setprecision(1) << peak_req << "%"
                  << ", Resp:" << std::setw(4) << peak_resp << "%"
                  << ", L2:" << std::setw(4) << peak_l2 << "%"
                  << ", L3:" << std::setw(4) << peak_l3 << "%   |" << std::endl;

        // Target latency: durable_lat = 1000 ms = 1,000,000 us
        double target_lat_us = 1000.0 * 1000.0;
        double current_lat = avg_rtt_us;
        
        // Handle the case where no responses were received but we sent orders.
        // This indicates possible congestion or server deadlock, so latency is treated as very high.
        if (current_lat == 0.0 && step_rtt_count_ == 0) {
            if (sent_count_.load(std::memory_order_relaxed) > last_sent_count_) {
                current_lat = target_lat_us * 5.0;
            }
        }

        if (!is_latency_mode_) {
            // 只會一直加速，不會調整 (only accelerates, never adjusts back)
            // Reduce interval by 10% to continuously accelerate
            double next_interval = current_interval_us_.load(std::memory_order_relaxed) * 0.90;
            if (next_interval < 1.0) {
                next_interval = 1.0;
            }
            current_interval_us_.store(next_interval, std::memory_order_relaxed);
        }

        // Prep snapshots for next step
        step_counter_++;
        step_start_time_ = std::chrono::steady_clock::now();
    }

    // High-performance concurrency variables
    std::thread stress_thread_;
    std::thread monitoring_thread_;
    std::atomic<uint64_t> sent_count_{0};
    uint64_t last_sent_count_{0};
    std::chrono::steady_clock::time_point step_start_time_;
    std::chrono::steady_clock::time_point last_response_time_;

    std::atomic<double> current_interval_us_{250.0}; // Starts at 250 us
    int step_counter_ = 1;

    // Observability observers
    std::unique_ptr<Exchange::SHMObserver> req_observer_;
    std::unique_ptr<Exchange::SHMObserver> resp_observer_;
    std::unique_ptr<Exchange::SHMObserver> l2_observer_;
    std::unique_ptr<Exchange::SHMObserver> l3_observer_;

    double tsc_hz_ = 0.0;

    // Step stats accumulators (mutex-protected)
    std::mutex step_stats_mtx_;
    double step_rtt_sum_us_ = 0.0;
    uint64_t step_rtt_count_ = 0;
    double step_max_rtt_us_ = 0.0;
    std::vector<double> step_rtts_;
    double peak_req_ratio_ = 0.0;
    double peak_resp_ratio_ = 0.0;
    double peak_l2_ratio_ = 0.0;
    double peak_l3_ratio_ = 0.0;

    // General stats counters
    std::atomic<uint64_t> ack_count_{0};
    std::atomic<uint64_t> modify_count_{0};
    std::atomic<uint64_t> cancel_count_{0};
    std::atomic<uint64_t> fill_count_{0};

    // RTT correlation mapping
    std::unordered_map<uint64_t, std::chrono::steady_clock::time_point> request_send_times_;
    std::mutex send_times_mtx_;
    std::atomic<double> recent_rtt_us_{0.0};

    // RTT accumulated total variables
    std::atomic<uint64_t> rtt_count_{0};
    double total_rtt_sum_us_{0.0};
    std::mutex rtt_stats_mtx_;

    // Random walk and order generation distributions
    std::mt19937 gen_{std::random_device{}()};
    std::uniform_real_distribution<> dist_action_{0.0, 1.0};
    std::uniform_int_distribution<> dist_buy_sell_{0, 1};
    std::uniform_real_distribution<> dist_aggressive_{0.0, 1.0};
    std::uniform_int_distribution<int64_t> dist_price_offset_{0, 5};
    std::uniform_int_distribution<uint64_t> dist_qty_{1, 10};
    int64_t mid_price_{5000};
    
    bool is_latency_mode_ = false;
    std::vector<double> rtt_new_;
    std::vector<double> rtt_mod_short_;
    std::vector<double> rtt_mod_long_;
    std::vector<double> rtt_can_;
    std::atomic<bool> next_is_short_mod_{false};
    std::unordered_set<uint64_t> short_mod_exec_ids_;

    void generate_latency_report() {
        auto calc_stats = [](std::vector<double>& rtts) {
            if (rtts.empty()) return std::string("       -       ");
            std::sort(rtts.begin(), rtts.end());
            double sum = 0; for(auto v: rtts) sum += v;
            double avg = sum / rtts.size();
            double p90 = rtts[static_cast<size_t>(rtts.size() * 0.90)];
            double p99 = rtts[static_cast<size_t>(rtts.size() * 0.99)];
            double p999 = rtts[static_cast<size_t>(rtts.size() * 0.999)];
            double max_v = rtts.back();
            char buf[256];
            snprintf(buf, sizeof(buf), "%7.2f/%7.2f/%7.2f/%7.2f/%7.2f", avg, p90, p99, p999, max_v);
            return std::string(buf);
        };
        
        std::lock_guard<std::mutex> lock(step_stats_mtx_);
        std::string stat_new = calc_stats(rtt_new_);
        std::string stat_mod_short = calc_stats(rtt_mod_short_);
        std::string stat_mod_long = calc_stats(rtt_mod_long_);
        std::string stat_can = calc_stats(rtt_can_);
        std::vector<double> rtt_all;
        rtt_all.insert(rtt_all.end(), rtt_new_.begin(), rtt_new_.end());
        rtt_all.insert(rtt_all.end(), rtt_mod_short_.begin(), rtt_mod_short_.end());
        rtt_all.insert(rtt_all.end(), rtt_mod_long_.begin(), rtt_mod_long_.end());
        rtt_all.insert(rtt_all.end(), rtt_can_.begin(), rtt_can_.end());
        std::string stat_all = calc_stats(rtt_all);

        std::ifstream ifs("docs/latency-report");
        std::vector<std::string> lines;
        std::string line;
        while(std::getline(ifs, line)) {
            lines.push_back(line);
        }
        
        std::string h1 = "    client E2E (avg/p90/p99/p999/max)";
        std::string sep = "-------------------------------------------";

        if (lines.size() >= 9) {
            lines[0] += " ==========================================";
            lines[1] += " " + h1;
            lines[2] += " " + sep;
            lines[3] += "  " + stat_new;
            lines[4] += "  " + stat_mod_long; // Assume Modify row is 2nd in latency-report
            lines[5] += "  " + stat_can;
            lines[6] += " " + sep;
            lines[7] += "  " + stat_all;
            lines[8] += " ==========================================";
        } else {
            lines.push_back(h1);
            lines.push_back("New:       " + stat_new);
            lines.push_back("Mod Short: " + stat_mod_short);
            lines.push_back("Mod Long:  " + stat_mod_long);
            lines.push_back("Cancel:    " + stat_can);
            lines.push_back("ALL:       " + stat_all);
        }

        std::ofstream ofs("docs/latency-report");
        for (const auto& l : lines) {
            ofs << l << "\n";
            std::cout << l << "\n";
        }
    }
};

} // namespace Exchange

int main(int argc, char** argv) {
    std::signal(SIGINT, [](int) {
        std::cout << "\n[StressTrader] Caught SIGINT. Gracefully shutting down..." << std::endl;
        Exchange::g_stop = 1;
    });

    bool is_latency_mode = false;
    if (argc > 1 && std::string(argv[1]) == "latency") {
        is_latency_mode = true;
    }

    Exchange::AlgoTradingConfig config;
    config.client_id = 999;
    config.symbol_ids = {1};
    config.timer_interval_ms = 100; // fast on_timer checking

    Exchange::StressTrader trader(config, is_latency_mode);
    return trader.run();
}
