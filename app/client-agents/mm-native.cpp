#include "AlgoTradingClient.hpp"
#include "HttpUtil.hpp"
#include "JsonUtil.hpp"
#include <iostream>
#include <iomanip>
#include <random>
#include <chrono>
#include <thread>
#include <csignal>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cmath>
#include <atomic>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/error.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <openssl/ssl.h>

namespace Exchange {

class MarketMakerNative : public AlgoTradingClient {
public:
    MarketMakerNative(const Config& config) : AlgoTradingClient(config) {
        std::cout << "[MM-Native] Started. ClientID=" << config_.client_id << std::endl;
    }

    ~MarketMakerNative() override = default;

    void on_l2_update(const L2Update*) override {}
    void on_l3_update(const L3Update*) override {}

    void on_timer() override {
        tick_count_++;
        bool is_fetch_tick = (tick_count_ % 10 == 1); // Fetch on 1, 11, 21...
        
        if (is_fetch_tick) {
            double fetched = fetch_binance_price();
            if (fetched > 0.0) {
                mm_mid_price_ = fetched;
            }
        }

        if (mm_mid_price_ <= 0.0) {
            return; // Wait until we fetch the first price
        }

        auto it = symbols_info_.find(1);
        [[maybe_unused]] int64_t step = (it != symbols_info_.end()) ? it->second->price_min_step : 1;

        double estimation = mm_mid_price_;
        if (it != symbols_info_.end()) {
            const auto& info = it->second;
            estimation = std::max(static_cast<double>(info->price_min), std::min(static_cast<double>(info->price_max), estimation));
        }

        manage_orders(estimation, is_fetch_tick);

        static int count = 0;
        if (++count % 10 == 0) {
            std::cout << "[MM-Native] Binance Mid: " << std::fixed << std::setprecision(2) << mm_mid_price_ 
                      << " | Active Orders: " << account_.get_open_orders().size() << std::endl;
        }
    }

private:

    double fetch_binance_price() {
        namespace beast = boost::beast;
        namespace http = beast::http;
        try {
            std::string body = perform_https_request(
                "api.binance.com", "443",
                http::verb::get,
                "/api/v3/ticker/price?symbol=BTCUSDT"
            );

            std::string price_str = get_json_string(body, "price");
            if (!price_str.empty()) {
                double price = std::stod(price_str);
                double scale = 100.0;
                auto it = symbols_info_.find(1);
                if (it != symbols_info_.end()) {
                    scale = std::pow(10, -it->second->price_exp);
                }
                return price * scale;
            }
        } catch (const std::exception& e) {
            std::cerr << "[MM-Native] Error fetching Binance price: " << e.what() << std::endl;
        }
        return -1.0;
    }

    void manage_orders(double estimation, bool is_fetch_tick) {
        auto open_orders = account_.get_open_orders();
        auto it = symbols_info_.find(1);
        int64_t step = (it != symbols_info_.end()) ? it->second->price_min_step : 1;

        std::vector<OrderResponseT> bids;
        std::vector<OrderResponseT> asks;
        for (const auto& o : open_orders) {
            if (o.side == Side_Buy) bids.push_back(o);
            else asks.push_back(o);
        }

        // Sort orders by price so we map the closest layers first
        std::sort(bids.begin(), bids.end(), [](const OrderResponseT& a, const OrderResponseT& b) {
            return a.p > b.p; // Descending for bids (highest bid first)
        });
        std::sort(asks.begin(), asks.end(), [](const OrderResponseT& a, const OrderResponseT& b) {
            return a.p < b.p; // Ascending for asks (lowest ask first)
        });

        int target_levels = 10;
        
        if (is_fetch_tick || bids.size() < (size_t)target_levels || asks.size() < (size_t)target_levels) {
            int64_t est_ticks = std::round(estimation / step) * step;
            bool price_went_up = (est_ticks > last_est_ticks_);
            last_est_ticks_ = est_ticks;
            
            auto handle_bids = [&]() {
                for (int i = 0; i < std::max((int)bids.size(), target_levels); ++i) {
                    int64_t target_p = est_ticks - (i + 1) * step;
                    if (it != symbols_info_.end()) {
                        target_p = std::max(it->second->price_min, std::min(it->second->price_max, target_p));
                    }
                    
                    if (i < (int)bids.size()) {
                        if (i < target_levels) {
                            uint64_t target_q = bids[i].q;
                            if (bids[i].p != target_p) {
                                replace_order(bids[i].order_id, target_p, target_q, bids[i].symbol_id, bids[i].side);
                            }
                        } else {
                            cancel_order(bids[i].order_id, bids[i].symbol_id, bids[i].side);
                        }
                    } else {
                        uint64_t q = std::uniform_int_distribution<uint64_t>(10, 50)(gen_);
                        new_limit_order(1, Side_Buy, target_p, q);
                    }
                }
            };

            auto handle_asks = [&]() {
                for (int i = 0; i < std::max((int)asks.size(), target_levels); ++i) {
                    int64_t target_p = est_ticks + (i + 1) * step;
                    if (it != symbols_info_.end()) {
                        target_p = std::max(it->second->price_min, std::min(it->second->price_max, target_p));
                    }
                    
                    if (i < (int)asks.size()) {
                        if (i < target_levels) {
                            uint64_t target_q = asks[i].q;
                            if (asks[i].p != target_p) {
                                replace_order(asks[i].order_id, target_p, target_q, asks[i].symbol_id, asks[i].side);
                            }
                        } else {
                            cancel_order(asks[i].order_id, asks[i].symbol_id, asks[i].side);
                        }
                    } else {
                        uint64_t q = std::uniform_int_distribution<uint64_t>(10, 50)(gen_);
                        new_limit_order(1, Side_Sell, target_p, q);
                    }
                }
            };

            // Order of modification is critical to avoid self-crossing:
            // Always retreat the eaten side first before advancing the other side.
            if (price_went_up) {
                // Price goes UP: Asks are eaten. Retreat Asks first, then advance Bids.
                handle_asks();
                handle_bids();
            } else {
                // Price goes DOWN (or unchanged): Bids are eaten. Retreat Bids first, then advance Asks.
                handle_bids();
                handle_asks();
            }
        } else {
            // Not a fetch tick: randomly jitter qty of existing orders or make the book jump
            if (!open_orders.empty()) {
                bool jump_book = (std::uniform_int_distribution<int>(1, 10)(gen_) == 1);
                if (jump_book && !bids.empty() && !asks.empty()) {
                    int64_t bid_out = last_est_ticks_ - (target_levels + 5) * step;
                    int64_t ask_out = last_est_ticks_ + (target_levels + 5) * step;
                    if (it != symbols_info_.end()) {
                        bid_out = std::max(it->second->price_min, bid_out);
                        ask_out = std::min(it->second->price_max, ask_out);
                    }
                    // Replace innermost to outermost
                    replace_order(bids[0].order_id, bid_out, bids[0].q, bids[0].symbol_id, bids[0].side);
                    replace_order(asks[0].order_id, ask_out, asks[0].q, asks[0].symbol_id, asks[0].side);
                } else {
                    // Modify 1 to 3 random orders to simulate activity
                    int mods = std::uniform_int_distribution<int>(1, 3)(gen_);
                    for (int m = 0; m < mods; ++m) {
                        size_t idx = std::uniform_int_distribution<size_t>(0, open_orders.size() - 1)(gen_);
                        const auto& o = open_orders[idx];
                        int64_t qty_change = std::uniform_int_distribution<int>(-5, 5)(gen_);
                        int64_t new_q = static_cast<int64_t>(o.q) + qty_change;
                        if (new_q < 5) new_q = 5;
                        if (new_q > 50) new_q = 50;
                        
                        if (new_q != static_cast<int64_t>(o.q)) {
                            replace_order(o.order_id, o.p, new_q, o.symbol_id, o.side);
                        }
                    }
                }
            }
        }
    }

    std::mt19937 gen_{std::random_device{}()};
    int tick_count_ = 0;
    double mm_mid_price_ = 0.0;
    int64_t last_est_ticks_ = 0;
};

} // namespace Exchange

int main() {
    Exchange::AlgoTradingConfig config;
    config.client_id = 100; // Native Market Maker
    config.symbol_ids = {1};
    config.timer_interval_ms = 100; // Faster tick (100ms)

    Exchange::MarketMakerNative mm(config);
    return mm.run();
}
