#pragma once

#include <vector>
#include <map>
#include <cstdint>
#include <mutex>
#include <memory>
#include "fbs/exchange_generated.h"
#include "define.hpp"

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
    virtual void addOrUpdateOpenOrder(const OrderResponseT* resp) = 0;
    virtual void removeOpenOrder(uint32_t client_id, uint64_t order_id) = 0;
    virtual std::vector<std::vector<uint8_t>> getOpenOrders(uint32_t client_id) = 0;

    // Execution processing
    virtual void update_on_execution(const OrderResponseT* resp, bool not_sent) = 0;
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

    void addOrUpdateOpenOrder(const OrderResponseT* resp) override {
        std::lock_guard<std::mutex> lock(mutex_);
        open_orders_[resp->client_id][resp->order_id] = *resp;
    }

    void removeOpenOrder(uint32_t client_id, uint64_t order_id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = open_orders_.find(client_id);
        if (it != open_orders_.end()) {
            it->second.erase(order_id);
        }
    }

    void update_on_execution(const OrderResponseT* resp, bool not_sent) override {
        uint32_t client_id = resp->client_id;
        if ((EXEC_MASK_POSITION_UPDATE >> resp->exec_type) & 1) {
            int64_t cost = static_cast<int64_t>(resp->p * resp->q);
            if (resp->side == Side_Buy) {
                updatePosition(client_id, 0, -cost);
                updatePosition(client_id, resp->symbol_id, static_cast<int64_t>(resp->q));
            } else {
                updatePosition(client_id, 0, cost);
                updatePosition(client_id, resp->symbol_id, -static_cast<int64_t>(resp->q));
            }
        }
        if ((EXEC_MASK_UPSERT_OPEN >> resp->exec_type) & 1) {
            addOrUpdateOpenOrder(resp);
        } else if ((EXEC_MASK_REMOVE_OPEN >> resp->exec_type) & 1) {
            removeOpenOrder(client_id, resp->order_id);
        }
        if (not_sent) {
            flatbuffers::FlatBufferBuilder fbb(256);
            auto resp_offset = OrderResponse::Pack(fbb, resp);
            auto client_resp = CreateClientResponse(fbb, ClientResponseData_OrderResponse, resp_offset.Union());
            fbb.Finish(client_resp);
            addPendingResponse(client_id, fbb.GetBufferPointer(), fbb.GetSize());
        }
    }

    std::vector<std::vector<uint8_t>> getOpenOrders(uint32_t client_id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::vector<uint8_t>> result;
        auto it = open_orders_.find(client_id);
        if (it != open_orders_.end()) {
            for (auto const& [order_id, resp] : it->second) {
                flatbuffers::FlatBufferBuilder fbb(256);
                auto resp_offset = CreateOrderResponse(fbb, ExecType_OrderStatus, resp.order_id, resp.client_id, resp.exec_id, resp.symbol_id, resp.side, resp.p, resp.q, resp.reject_code);
                auto client_resp = CreateClientResponse(fbb, ClientResponseData_OrderResponse, resp_offset.Union());
                fbb.Finish(client_resp);
                result.push_back(std::vector<uint8_t>(fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize()));
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
    std::map<uint32_t, std::map<uint64_t, OrderResponseT>> open_orders_;
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

    void addOrUpdateOpenOrder(const OrderResponseT* resp) override;
    void removeOpenOrder(uint32_t client_id, uint64_t order_id) override;
    std::vector<std::vector<uint8_t>> getOpenOrders(uint32_t client_id) override;

    void update_on_execution(const OrderResponseT* resp, bool not_sent) override;

private:
    void reconnect_if_needed();

    std::string conn_str_;
    std::unique_ptr<pqxx::connection> conn_;
    std::mutex mutex_;
};

} // namespace Exchange
