#pragma once

#include <vector>
#include <map>
#include <cstdint>
#include <mutex>
#include <memory>
#include "fbs/exchange_generated.h"

namespace pqxx {
class connection;
}

namespace Exchange {

// Represents a serialized response (ClientResponse FlatBuffer)
struct PendingResponse {
    std::vector<uint8_t> data;
};

/**
 * @brief Abstract interface for client data storage.
 * Following the adaptor/interface pattern to allow easy swapping to SQL/other DBs.
 */
class ClientDatabase {
public:
    virtual ~ClientDatabase() = default;

    // Unsent OrderResponse lists
    virtual void addPendingResponse(uint32_t client_id, const uint8_t* data, size_t size) = 0;
    virtual std::vector<PendingResponse> popPendingResponses(uint32_t client_id) = 0;

    // Positions
    virtual int64_t getPosition(uint32_t client_id, uint32_t symbol_id) = 0;
    virtual std::map<uint32_t, int64_t> getAllPositions(uint32_t client_id) = 0;
    virtual void updatePosition(uint32_t client_id, uint32_t symbol_id, int64_t delta) = 0;

    // Open Orders
    virtual void addOrUpdateOpenOrder(uint32_t client_id, uint64_t order_id, const uint8_t* data, size_t size) = 0;
    virtual void removeOpenOrder(uint32_t client_id, uint64_t order_id) = 0;
    virtual std::vector<std::vector<uint8_t>> getOpenOrders(uint32_t client_id) = 0;
};

/**
 * @brief In-memory implementation of ClientDatabase.
 */
class InMemoryClientDatabase : public ClientDatabase {
public:
    InMemoryClientDatabase() = default;

    void addPendingResponse(uint32_t client_id, const uint8_t* data, size_t size) override {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_responses_[client_id].push_back({std::vector<uint8_t>(data, data + size)});
    }

    std::vector<PendingResponse> popPendingResponses(uint32_t client_id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = pending_responses_.find(client_id);
        if (it != pending_responses_.end()) {
            std::vector<PendingResponse> res = std::move(it->second);
            pending_responses_.erase(it);
            return res;
        }
        return {};
    }

    int64_t getPosition(uint32_t client_id, uint32_t symbol_id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& client_pos = get_or_create_client_positions(client_id);
        return client_pos[symbol_id];
    }

    std::map<uint32_t, int64_t> getAllPositions(uint32_t client_id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        return get_or_create_client_positions(client_id);
    }

    void updatePosition(uint32_t client_id, uint32_t symbol_id, int64_t delta) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& client_pos = get_or_create_client_positions(client_id);
        client_pos[symbol_id] += delta;
    }

    void addOrUpdateOpenOrder(uint32_t client_id, uint64_t order_id, const uint8_t* data, size_t size) override {
        std::lock_guard<std::mutex> lock(mutex_);
        open_orders_[client_id][order_id] = std::vector<uint8_t>(data, data + size);
    }

    void removeOpenOrder(uint32_t client_id, uint64_t order_id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = open_orders_.find(client_id);
        if (it != open_orders_.end()) {
            it->second.erase(order_id);
        }
    }

    std::vector<std::vector<uint8_t>> getOpenOrders(uint32_t client_id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::vector<uint8_t>> result;
        auto it = open_orders_.find(client_id);
        if (it != open_orders_.end()) {
            for (auto const& [order_id, data] : it->second) {
                result.push_back(data);
            }
        }
        return result;
    }

private:
    std::map<uint32_t, int64_t>& get_or_create_client_positions(uint32_t client_id) {
        auto it = positions_.find(client_id);
        if (it == positions_.end()) {
            auto& client_pos = positions_[client_id];
            client_pos[0] = 1000000; // 10M USD
            client_pos[1] = 0;       //   0 Symbol 1
            return client_pos;
        }
        return it->second;
    }

    std::mutex mutex_;
    std::map<uint32_t, std::vector<PendingResponse>> pending_responses_;
    std::map<uint32_t, std::map<uint32_t, int64_t>> positions_;
    std::map<uint32_t, std::map<uint64_t, std::vector<uint8_t>>> open_orders_;
};

class PostgresClientDatabase : public ClientDatabase {
public:
    PostgresClientDatabase(const std::string& conn_str);
    ~PostgresClientDatabase() override;

    void addPendingResponse(uint32_t client_id, const uint8_t* data, size_t size) override;
    std::vector<PendingResponse> popPendingResponses(uint32_t client_id) override;

    int64_t getPosition(uint32_t client_id, uint32_t symbol_id) override;
    std::map<uint32_t, int64_t> getAllPositions(uint32_t client_id) override;
    void updatePosition(uint32_t client_id, uint32_t symbol_id, int64_t delta) override;

    void addOrUpdateOpenOrder(uint32_t client_id, uint64_t order_id, const uint8_t* data, size_t size) override;
    void removeOpenOrder(uint32_t client_id, uint64_t order_id) override;
    std::vector<std::vector<uint8_t>> getOpenOrders(uint32_t client_id) override;

private:
    void reconnect_if_needed();

    std::string conn_str_;
    std::unique_ptr<pqxx::connection> conn_;
    std::mutex mutex_;
};

} // namespace Exchange
