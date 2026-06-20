#include "AlgoTradingClient.hpp"
#include "L2Book.hpp"
#include "DisplayUtil.hpp"
#include <iostream>
#include <random>
#include <thread>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cmath>

namespace Exchange {

class NoiseTrader : public AlgoTradingClient {
public:
    NoiseTrader(const Config& config, uint32_t target_symbol) 
        : AlgoTradingClient(config), target_symbol_(target_symbol) {
        book_.symbol_id = target_symbol;
    }

    void on_l2_update(const L2Update* update) override {
        if (update->symbol_id() == target_symbol_) {
            book_.update(update->side(), update->p(), update->q());
        }
    }

    void on_l3_update(const L3Update*) override {}

    void on_order_response(const OrderResponse* response) override {
        AlgoTradingClient::on_order_response(response);
        if (response->exec_type() == ExecType_Fill || response->exec_type() == ExecType_PartialFill) {
            std::lock_guard<std::mutex> lock(strategy_mtx_);
            int64_t current_pos = account_.get_position(target_symbol_);
            if ((current_pos > 0 && response->side() == Side_Buy) || 
                (current_pos < 0 && response->side() == Side_Sell)) {
                entry_price_ = response->p();
            }
        }
    }

    void on_timer() override {
        if (!is_ready()) return;

        std::lock_guard<std::mutex> lock(book_.mutex);
        std::lock_guard<std::mutex> strat_lock(strategy_mtx_);
        
        int64_t cash = account_.get_cash();
        int64_t pos = account_.get_position(target_symbol_);
        auto open_orders = account_.get_open_orders();

        int64_t pending_buy_qty = 0;
        int64_t pending_sell_qty = 0;
        for (const auto& o : open_orders) {
            if (o.symbol_id != target_symbol_) continue;
            if (o.side == Side_Buy) pending_buy_qty += o.q;
            else if (o.side == Side_Sell) pending_sell_qty += o.q;
        }

        double price = 0;
        if (!book_.bids.empty()) price = book_.bids.begin()->first;
        else if (!book_.asks.empty()) price = book_.asks.begin()->first;

        // Startup/reconnect fallback for entry_price_ if starting with active position
        if (pos != 0 && entry_price_ == 0.0) {
            if (price > 0.0) {
                entry_price_ = price;
            } else {
                return; // Do not quote until we have a valid market price to use as base
            }
        }

        auto it = symbols_info_.find(target_symbol_);
        int64_t step = (it != symbols_info_.end()) ? it->second->price_min_step : 1;

        // Bracket parameters for NoiseTrader (wider than Insider to allow more noise) scaled by step size
        double take_profit_ticks = 10.0 * step;
        double stop_loss_ticks = 8.0 * step;

        if (pos == 0) {
            closing_ = false;
            if (!open_orders.empty()) {
                for (const auto& o : open_orders) cancel_order(o.order_id, target_symbol_, o.side);
                return;
            }

            std::random_device rd;
            std::mt19937 gen(rd());
            std::uniform_int_distribution<> side_dist(0, 1);

            if (side_dist(gen) == 0) { // BUY
                if (!book_.asks.empty()) {
                    auto best_ask = book_.asks.begin();
                    new_limit_order(target_symbol_, Side_Buy, best_ask->first, 2);
                    display_.add_message("Noise Entry: BUY @ " + std::to_string(best_ask->first));
                }
            } else { // SELL
                if (!book_.bids.empty()) {
                    auto best_bid = book_.bids.begin();
                    new_limit_order(target_symbol_, Side_Sell, best_bid->first, 2);
                    display_.add_message("Noise Entry: SELL @ " + std::to_string(best_bid->first));
                }
            }
        } else if (pos > 0) {
            if (pending_sell_qty == 0 && !closing_) {
                int64_t tp_price = static_cast<int64_t>(std::round((entry_price_ + take_profit_ticks) / step)) * step;
                if (it != symbols_info_.end()) {
                    tp_price = std::max(it->second->price_min, std::min(it->second->price_max, tp_price));
                }
                new_limit_order(target_symbol_, Side_Sell, tp_price, pos);
                display_.add_message("Noise TP (SELL) @ " + std::to_string(tp_price));
            }
            if (price > 0 && price <= entry_price_ - stop_loss_ticks && !closing_) {
                closing_ = true;
                for (const auto& o : open_orders) if (o.side == Side_Sell) cancel_order(o.order_id, target_symbol_, Side_Sell);
                new_market_order(target_symbol_, Side_Sell, pos);
                display_.add_message("Noise SL: Market SELL");
            }
        } else if (pos < 0) {
            if (pending_buy_qty == 0 && !closing_) {
                int64_t tp_price = static_cast<int64_t>(std::round((entry_price_ - take_profit_ticks) / step)) * step;
                if (it != symbols_info_.end()) {
                    tp_price = std::max(it->second->price_min, std::min(it->second->price_max, tp_price));
                }
                new_limit_order(target_symbol_, Side_Buy, tp_price, std::abs(pos));
                display_.add_message("Noise TP (BUY) @ " + std::to_string(tp_price));
            }
            if (price > 0 && price >= entry_price_ + stop_loss_ticks && !closing_) {
                closing_ = true;
                for (const auto& o : open_orders) if (o.side == Side_Buy) cancel_order(o.order_id, target_symbol_, Side_Buy);
                new_market_order(target_symbol_, Side_Buy, std::abs(pos));
                display_.add_message("Noise SL: Market BUY");
            }
        }

        display_.display("NoiseTrader", config_.client_id, target_symbol_, pos, cash, price, account_.get_open_orders());

        // Update random timer for next iteration
        std::random_device rd2;
        std::mt19937 gen2(rd2());
        std::uniform_int_distribution<> sleep_dist(500, 2000);
        set_timer_interval(sleep_dist(gen2));
    }

private:

    uint32_t target_symbol_;
    L2Book book_;
    DisplayFramework display_;

    std::mutex strategy_mtx_;
    double entry_price_ = 0.0;
    bool closing_ = false;
};

} // namespace Exchange

int main() {
    Exchange::AlgoTradingConfig config;
    config.client_id = 401;
    config.symbol_ids = {1};

    Exchange::NoiseTrader noise(config, 1);
    return noise.run();
}
