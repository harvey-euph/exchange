#include "AlgoTradingClient.hpp"
#include "L2Book.hpp"
#include "ClientAccount.hpp"
#include <iostream>
#include <iomanip>
#include <chrono>
#include <mutex>
#include <cmath>
#include <deque>
#include <numeric>
#include <algorithm>
#include <unordered_map>

namespace Exchange {

struct MarkoutRecord {
    uint64_t timestamp_ms;
    Side side;
    double fill_price;
    uint64_t qty;
    double micro_price_at_fill;
    
    // Results
    bool evaluated_1s = false;
    bool evaluated_5s = false;
    double edge_1s = 0;
    double edge_5s = 0;
};

class MarketMakerAdvanced : public AlgoTradingClient {
public:
    MarketMakerAdvanced(const Config& config, uint32_t target_symbol) 
        : AlgoTradingClient(config), target_symbol_(target_symbol) {
        local_book_.symbol_id = target_symbol;
        std::cout << "[MM-Advanced] Initialized for symbol " << target_symbol_ << std::endl;
    }

    void on_l2_update(const L2Update* update) override {
        if (update->symbol_id() != target_symbol_) return;

        std::lock_guard<std::mutex> lock(state_mtx_);
        local_book_.update(update->side(), update->p(), update->q());
        
        // 1. OBI & MicroPrice Calculation
        update_microstructure_signals();

        // 2. Trade-Through & Adverse Selection Protection
        // If the microprice moves violently against our resting quotes, pull them immediately.
        fast_cancel_protection();
    }

    void on_l3_update(const L3Update* update) override {
        if (update->symbol_id() != target_symbol_) return;
        if (update->exec_type() != ExecType_Fill && update->exec_type() != ExecType_PartialFill) return;

        std::lock_guard<std::mutex> lock(state_mtx_);
        
        // Infer aggressive flow side based on trade price vs BBO
        double p = static_cast<double>(update->p());
        double q = static_cast<double>(update->q());
        
        double current_mid = (best_bid_p_ + best_ask_p_) / 2.0;
        
        if (p >= current_mid) {
            aggressive_buy_vol_ += q;
        } else if (p < current_mid) {
            aggressive_sell_vol_ += q;
        }

        // Update Order Flow Toxicity (Trade Imbalance)
        update_toxicity();
    }

    void on_order_response(const OrderResponse* response) override {
        AlgoTradingClient::on_order_response(response);
        
        if (response->exec_type() == ExecType_Fill || response->exec_type() == ExecType_PartialFill) {
            std::lock_guard<std::mutex> lock(state_mtx_);
            uint64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
                
            MarkoutRecord rec;
            rec.timestamp_ms = now;
            rec.side = response->side();
            rec.fill_price = static_cast<double>(response->p());
            rec.qty = response->q();
            rec.micro_price_at_fill = micro_price_;
            markouts_.push_back(rec);
            
            total_volume_traded_ += response->q();
        }
    }

    void on_timer() override {
        std::lock_guard<std::mutex> lock(state_mtx_);
        
        if (best_bid_p_ == 0 || best_ask_p_ == 0) return;

        evaluate_markouts();
        update_inventory_metrics();
        
        // Core Quoting Logic
        manage_quotes();
        
        // Opportunistic Taking
        opportunistic_take();

        print_dashboard();
    }

private:
    struct PriceLevel {
        int64_t price;
        uint64_t qty;
    };

    void update_microstructure_signals() {
        std::vector<PriceLevel> bids;
        std::vector<PriceLevel> bids_all; // not actually needed
        std::vector<PriceLevel> asks;

        {
            std::lock_guard<std::mutex> lock(local_book_.mutex);
            int count = 0;
            for (const auto& pair : local_book_.bids) {
                bids.push_back({pair.first, pair.second});
                if (++count >= 5) break;
            }
            count = 0;
            for (const auto& pair : local_book_.asks) {
                asks.push_back({pair.first, pair.second});
                if (++count >= 5) break;
            }
        }

        if (bids.empty() || asks.empty()) return;

        best_bid_p_ = bids[0].price;
        best_bid_q_ = bids[0].qty;
        best_ask_p_ = asks[0].price;
        best_ask_q_ = asks[0].qty;

        // MicroPrice: Volume-weighted mid price
        micro_price_ = (best_ask_p_ * best_bid_q_ + best_bid_p_ * best_ask_q_) / 
                       static_cast<double>(best_bid_q_ + best_ask_q_);

        // Order Book Imbalance (OBI)
        double total_bid_q = 0;
        for (const auto& b : bids) total_bid_q += b.qty;
        
        double total_ask_q = 0;
        for (const auto& a : asks) total_ask_q += a.qty;

        obi_ = (total_bid_q - total_ask_q) / (total_bid_q + total_ask_q);
    }

    void update_toxicity() {
        // Decay previous volumes (exponential moving average)
        aggressive_buy_vol_ *= 0.95;
        aggressive_sell_vol_ *= 0.95;
        
        double total_aggressive = aggressive_buy_vol_ + aggressive_sell_vol_;
        if (total_aggressive > 0) {
            toxicity_ = (aggressive_buy_vol_ - aggressive_sell_vol_) / total_aggressive;
        }
    }

    void fast_cancel_protection() {
        // Event-driven latency reduction: Do not wait for on_timer()
        auto open_orders = account_.get_open_orders();
        for (const auto& o : open_orders) {
            if (o.symbol_id != target_symbol_) continue;
            
            // Trade-Through Detection:
            // If toxicity is very high against us, or microprice swept past our quotes
            if (o.side == Side_Sell && micro_price_ > o.p + 2.0 && toxicity_ > 0.6) {
                cancel_order(o.order_id, target_symbol_, o.side); // FADE ASK
                cancels_sent_++;
            } else if (o.side == Side_Buy && micro_price_ < o.p - 2.0 && toxicity_ < -0.6) {
                cancel_order(o.order_id, target_symbol_, o.side); // FADE BID
                cancels_sent_++;
            }
        }
    }

    void manage_quotes() {
        int64_t inventory = account_.get_position(target_symbol_);
        
        auto it = symbols_info_.find(target_symbol_);
        int64_t step = (it != symbols_info_.end()) ? it->second->price_min_step : 1;

        // 7. Inventory-Aware Quoting (Avellaneda-Stoikov Inspired)
        // Reservation price shifts away from inventory
        double inventory_skew = -inventory * inventory_risk_aversion_;
        double reservation_price = micro_price_ + inventory_skew;

        // 6. Dynamic Spread Model
        // Base spread + volatility penalty + toxicity penalty + inventory absolute penalty
        double base_half_spread = 2.0 * step;
        double volatility_penalty = std::abs(obi_) * 1.5 * step; // Wider when book is imbalanced
        double toxicity_penalty = std::abs(toxicity_) * 3.0 * step; // Wider when flow is toxic
        double inventory_penalty = std::abs(inventory) * 0.1 * step;

        double dynamic_half_spread = base_half_spread + volatility_penalty + toxicity_penalty + inventory_penalty;

        // Calculate Target Quotes
        double raw_bid = reservation_price - dynamic_half_spread;
        double raw_ask = reservation_price + dynamic_half_spread;

        int64_t target_bid = static_cast<int64_t>(std::round(raw_bid / step)) * step;
        int64_t target_ask = static_cast<int64_t>(std::round(raw_ask / step)) * step;

        if (it != symbols_info_.end()) {
            target_bid = std::max(it->second->price_min, std::min(it->second->price_max, target_bid));
            target_ask = std::max(it->second->price_min, std::min(it->second->price_max, target_ask));
        }

        // Quote Fade Logic (Don't quote if extremely toxic)
        bool quote_bid = toxicity_ > -0.8; 
        bool quote_ask = toxicity_ < 0.8;

        auto open_orders = account_.get_open_orders();
        bool has_bid = false, has_ask = false;

        for (const auto& o : open_orders) {
            if (o.symbol_id != target_symbol_) continue;

            if (o.side == Side_Buy) {
                if (!quote_bid || std::abs(o.p - target_bid) > 1.0 * step) {
                    cancel_order(o.order_id, target_symbol_, o.side);
                } else {
                    has_bid = true;
                }
            } else if (o.side == Side_Sell) {
                if (!quote_ask || std::abs(o.p - target_ask) > 1.0 * step) {
                    cancel_order(o.order_id, target_symbol_, o.side);
                } else {
                    has_ask = true;
                }
            }
        }

        uint64_t quote_qty = 20;
        if (!has_bid && quote_bid) new_limit_order(target_symbol_, Side_Buy, target_bid, quote_qty);
        if (!has_ask && quote_ask) new_limit_order(target_symbol_, Side_Sell, target_ask, quote_qty);
    }

    void opportunistic_take() {
        // 10. Opportunistic Taking
        // If MicroPrice + Imbalance strongly suggests short-term drift, take liquidity
        if (obi_ > 0.8 && toxicity_ > 0.7 && micro_price_ > best_ask_p_ + 0.5) {
            // Strong upward momentum -> Take Ask
            new_market_order(target_symbol_, Side_Buy, 10);
        } else if (obi_ < -0.8 && toxicity_ < -0.7 && micro_price_ < best_bid_p_ - 0.5) {
            // Strong downward momentum -> Take Bid
            new_market_order(target_symbol_, Side_Sell, 10);
        }
    }

    void evaluate_markouts() {
        uint64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
            
        for (auto& m : markouts_) {
            if (!m.evaluated_1s && now - m.timestamp_ms > 1000) {
                double edge = (m.side == Side_Buy) ? (micro_price_ - m.fill_price) : (m.fill_price - micro_price_);
                m.edge_1s = edge;
                m.evaluated_1s = true;
                total_realized_edge_1s_ += edge * m.qty;
            }
            if (!m.evaluated_5s && now - m.timestamp_ms > 5000) {
                double edge = (m.side == Side_Buy) ? (micro_price_ - m.fill_price) : (m.fill_price - micro_price_);
                m.edge_5s = edge;
                m.evaluated_5s = true;
                total_realized_edge_5s_ += edge * m.qty;
            }
        }
        
        // Clean up old markouts (older than 10s)
        while (!markouts_.empty() && now - markouts_.front().timestamp_ms > 10000) {
            markouts_.pop_front();
        }
    }

    void update_inventory_metrics() {
        int64_t pos = account_.get_position(target_symbol_);
        inventory_history_.push_back(pos);
        if (inventory_history_.size() > 100) inventory_history_.pop_front();
        
        double sum = 0;
        for (auto p : inventory_history_) sum += p;
        avg_inventory_ = sum / inventory_history_.size();
    }

    void print_dashboard() {
        static int ticks = 0;
        if (++ticks % 10 != 0) return;

        int64_t pos = account_.get_position(target_symbol_);
        double upnl = account_.get_cash() + pos * micro_price_ - initial_cash_;

        std::cout << "\033[H\033[J";
        std::cout << "========= HFT Principal MM Dashboard =========" << std::endl;
        std::cout << "Symbol: " << target_symbol_ << " | BBO: " << best_bid_p_ << "x" << best_ask_p_ << std::endl;
        std::cout << "---------------- Microstructure ----------------" << std::endl;
        std::cout << "MicroPrice: " << std::fixed << std::setprecision(2) << micro_price_ << std::endl;
        std::cout << "OBI (Imbalance): " << std::setprecision(2) << obi_ << " (Positive = Bid heavy)" << std::endl;
        std::cout << "Toxicity (VPIN): " << std::setprecision(2) << toxicity_ << " (Positive = Buy aggressive)" << std::endl;
        std::cout << "-------------- Inventory & Risk ----------------" << std::endl;
        std::cout << "Position: " << pos << " (Avg: " << std::setprecision(1) << avg_inventory_ << ")" << std::endl;
        std::cout << "Turnover (Total Vol): " << total_volume_traded_ << std::endl;
        std::cout << "Cancels Sent (Fade): " << cancels_sent_ << std::endl;
        std::cout << "---------------- Performance -------------------" << std::endl;
        std::cout << "Total PnL (Estimated): " << upnl << std::endl;
        std::cout << "1s Markout Edge:       " << total_realized_edge_1s_ << std::endl;
        std::cout << "5s Markout Edge:       " << total_realized_edge_5s_ << std::endl;
        
        if (toxicity_ > 0.6 || toxicity_ < -0.6) {
            std::cout << "\n[!] WARNING: High Toxicity Detected! Spreads Widened & Fading Quotes." << std::endl;
        }
    }

    uint32_t target_symbol_;
    L2Book local_book_;
    std::mutex state_mtx_;

    double initial_cash_ = 0;

    // Microstructure State
    double best_bid_p_ = 0, best_ask_p_ = 0;
    uint64_t best_bid_q_ = 0, best_ask_q_ = 0;
    double micro_price_ = 0;
    double obi_ = 0;
    
    // Toxicity Tracking
    double aggressive_buy_vol_ = 0;
    double aggressive_sell_vol_ = 0;
    double toxicity_ = 0;

    // Risk Parameters
    const double inventory_risk_aversion_ = 0.5;

    // Metrics
    std::deque<MarkoutRecord> markouts_;
    double total_realized_edge_1s_ = 0;
    double total_realized_edge_5s_ = 0;
    uint64_t total_volume_traded_ = 0;
    uint64_t cancels_sent_ = 0;
    
    std::deque<int64_t> inventory_history_;
    double avg_inventory_ = 0;
};

} // namespace Exchange

int main() {
    Exchange::AlgoTradingConfig config;
    config.client_id = 301; // Advanced HFT MM
    config.symbol_ids = {1};
    config.timer_interval_ms = 50; // High frequency tick (50ms)

    Exchange::MarketMakerAdvanced mm(config, 1);
    return mm.run();
}
