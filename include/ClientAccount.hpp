#pragma once

#include "fbs/exchange_generated.h"
#include <map>
#include <vector>
#include <cstdint>
#include <mutex>
#include <algorithm>

namespace Exchange {

/**
 * @brief Manages client account state including positions and open orders.
 */
class ClientAccount {
public:
    /**
     * @brief Processes an incoming order response to update open orders and positions.
     * @param response The order response from the exchange.
     */
    void handle_order_response(const OrderResponse* response) {
        if (!response) return;

        std::lock_guard<std::mutex> lock(mtx_);

        uint64_t order_id = response->order_id();
        ExecType exec_type = response->exec_type();

        auto it = std::find_if(open_orders_.begin(), open_orders_.end(),
                               [order_id](const OrderResponseT& o) { return o.order_id == order_id; });

        switch (exec_type) {
            case ExecType_OrderStatus:
            case ExecType_New:
            case ExecType_Replaced:
                if (it == open_orders_.end()) {
                    OrderResponseT order;
                    response->UnPackTo(&order);
                    open_orders_.push_back(std::move(order));
                } else {
                    response->UnPackTo(&(*it));
                }
                break;

            case ExecType_PartialFill:
                // Update position and cash based on the fill quantity and price
                update_position_and_cash_internal(response->symbol_id(), response->side(), response->q(), response->p());
                
                // Do not unpack PartialFill into open_orders_ because it contains fill quantity/price,
                // not the order's requested quantity/price.
                break;

            case ExecType_Fill:
                // Update position and cash based on the fill quantity and price
                update_position_and_cash_internal(response->symbol_id(), response->side(), response->q(), response->p());
                
                // Remove from open orders as it is fully filled
                if (it != open_orders_.end()) {
                    open_orders_.erase(it);
                }
                break;

            case ExecType_Cancelled:
                // Remove from open orders
                if (it != open_orders_.end()) {
                    open_orders_.erase(it);
                }
                break;

            case ExecType_Rejected:
                if (response->reject_code() == RejectCode_OrderNotFound) {
                    if (it != open_orders_.end()) {
                        open_orders_.erase(it);
                    }
                }
                break;

            case ExecType_Complete:
                // Synchronization completion marker - ignore for state updates
                break;

            default:
                // For other types, we don't update positions/orders
                break;
        }
    }

    /**
     * @brief Processes a position response to sync the current position.
     * @param response The position response from the exchange.
     */
    void handle_position_response(const PositionResponse* response) {
        if (!response) return;
        std::lock_guard<std::mutex> lock(mtx_);
        positions_[response->symbol_id()] = response->position();
    }

    /**
     * @brief Gets the current position for a specific symbol.
     * @param symbol_id The ID of the symbol. Symbol 0 is cash.
     * @return The current position quantity or cash balance.
     */
    int64_t get_position(uint32_t symbol_id) const {
        std::lock_guard<std::mutex> lock(mtx_);
        auto it = positions_.find(symbol_id);
        return (it != positions_.end()) ? it->second : 0;
    }

    /**
     * @brief Gets the current cash balance (Symbol 0).
     */
    int64_t get_cash() const {
        return get_position(0);
    }

    /**
     * @brief Gets all current positions.
     * @return A map of symbol_id to position quantity.
     */
    std::map<uint32_t, int64_t> get_all_positions() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return positions_;
    }

    /**
     * @brief Gets the list of current open orders.
     * @return A vector of open order responses.
     */
    std::vector<OrderResponseT> get_open_orders() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return open_orders_;
    }

    /**
     * @brief Clears all local state. Useful for resets or reconnections.
     */
    void clear() {
        std::lock_guard<std::mutex> lock(mtx_);
        positions_.clear();
        open_orders_.clear();
    }

private:
    void update_position_and_cash_internal(uint32_t symbol_id, Side side, uint64_t qty, int64_t price) {
        int64_t cash_change = static_cast<int64_t>(qty) * price;
        if (side == Side_Buy) {
            positions_[symbol_id] += static_cast<int64_t>(qty);
            positions_[0] -= cash_change; // Symbol 0 is cash
        } else if (side == Side_Sell) {
            positions_[symbol_id] -= static_cast<int64_t>(qty);
            positions_[0] += cash_change; // Symbol 0 is cash
        }
    }

    std::map<uint32_t, int64_t> positions_;
    std::vector<OrderResponseT> open_orders_;
    mutable std::mutex mtx_;
};

} // namespace Exchange
