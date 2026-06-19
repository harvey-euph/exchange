#include "PerfTraderBase.hpp"
#include <iostream>
#include <csignal>

namespace Exchange {

volatile sig_atomic_t g_stop = 0;

class RoundTripSender : public PerfTraderBase {
public:
    RoundTripSender(const Config& config, bool silent = false) 
        : PerfTraderBase(config, silent) 
    {
        sender_thread_ = std::thread(&RoundTripSender::sender_loop, this);
    }

    ~RoundTripSender() override {
        running_ = false;
        if (sender_thread_.joinable()) {
            sender_thread_.join();
        }
    }

    void on_l2_update(const L2Update*) override {}
    void on_l3_update(const L3Update*) override {}

    void on_response_processed(double rtt_us) override {
        if (running_) {
            do_trading_action();
        }
    }

    void on_timer() override {
        if (g_stop) {
            std::cout << "Sender interrupted by user.\n";
            running_ = false;
        }
    }

private:
    void sender_loop() {
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

        if (running_) {
            do_trading_action();
        }

        while (running_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            
            auto now = std::chrono::steady_clock::now();
            if (!silent_ && std::chrono::duration_cast<std::chrono::milliseconds>(now - last_print_time).count() >= 5000) {
                print_progress();
                last_print_time = now;
            }
        }

        if (sent_count_.load(std::memory_order_relaxed) > 0) {
            report_stats();
        }
        running_ = false;
    }

    std::thread sender_thread_;
};

} // namespace Exchange

int main(int argc, char** argv) {
    bool silent = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-s" || arg == "--silent") {
            silent = true;
        }
    }

    std::signal(SIGINT, [](int) {
        std::cout << "\n[RoundTripSender] Caught SIGINT. Gracefully shutting down..." << std::endl;
        Exchange::g_stop = 1;
    });

    Exchange::AlgoTradingConfig config;
    config.client_id = 1001; 
    config.symbol_ids = {1};
    config.timer_interval_ms = 100;

    Exchange::RoundTripSender sender(config, silent);
    return sender.run();
}
