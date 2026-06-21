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

/**
 * @brief Abstract interface for client data storage.
 * Following the adaptor/interface pattern to allow easy swapping to SQL/other DBs.
 */
class ClientDatabase {
public:
    virtual ~ClientDatabase() = default;

    // Sequence numbers
    virtual uint64_t getClientISeqNum(uint32_t client_id) = 0;
    virtual void setClientISeqNum(uint32_t client_id, uint64_t seq_num) = 0;
    virtual uint64_t getClientOSeqNum(uint32_t client_id) = 0;
    virtual uint64_t incrementAndGetClientOSeqNum(uint32_t client_id) = 0;

    // Unsent OrderResponse lists
    virtual void appendResponseLog(uint32_t client_id, const OrderResponseT& resp) = 0;
    virtual std::vector<OrderResponseT> popPendingResponses(uint32_t client_id) = 0;
    virtual std::vector<OrderResponseT> getResponsesSince(uint32_t client_id, uint64_t ack_seq_num) = 0;
    virtual void acknowledgeResponses(uint32_t client_id, uint64_t ack_seq_num) = 0;

    // Positions
    virtual int64_t getPosition(uint32_t client_id, uint32_t symbol_id) = 0;
    virtual std::map<uint32_t, int64_t> getAllPositions(uint32_t client_id) = 0;
    virtual void updatePosition(const OrderResponseT* resp) = 0;

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

    uint64_t getClientISeqNum(uint32_t client_id) override {
        return i_seq_nums_[client_id];
    }

    void setClientISeqNum(uint32_t client_id, uint64_t seq_num) override {
        i_seq_nums_[client_id] = seq_num;
    }

    uint64_t getClientOSeqNum(uint32_t client_id) override {
        return o_seq_nums_[client_id];
    }

    uint64_t incrementAndGetClientOSeqNum(uint32_t client_id) override {
        return ++o_seq_nums_[client_id];
    }

    void appendResponseLog(uint32_t client_id, const OrderResponseT& resp) override {
        uint64_t o_seq = resp.msg_seq_num;
        response_log_[client_id][o_seq] = resp;
    }

    std::vector<OrderResponseT> popPendingResponses([[maybe_unused]] uint32_t client_id) override {
        // Obsolete, use getResponsesSince
        return {};
    }

    std::vector<OrderResponseT> getResponsesSince(uint32_t client_id, uint64_t ack_seq_num) override {
        std::vector<OrderResponseT> result;
        auto it = response_log_.find(client_id);
        if (it != response_log_.end()) {
            for (auto const& [seq, resp] : it->second) {
                if (seq > ack_seq_num) {
                    result.push_back(resp);
                }
            }
        }
        return result;
    }

    void acknowledgeResponses(uint32_t client_id, uint64_t ack_seq_num) override {
        auto it = response_log_.find(client_id);
        if (it != response_log_.end()) {
            auto& log = it->second;
            for (auto it2 = log.begin(); it2 != log.end(); ) {
                if (it2->first <= ack_seq_num) {
                    it2 = log.erase(it2);
                } else {
                    ++it2;
                }
            }
        }
    }

    int64_t getPosition(uint32_t client_id, uint32_t symbol_id) override {
        auto& client_pos = get_or_create_client_positions(client_id);
        return client_pos[symbol_id];
    }

    std::map<uint32_t, int64_t> getAllPositions(uint32_t client_id) override {
        return get_or_create_client_positions(client_id);
    }

    void updatePosition(const OrderResponseT* resp) override {
        uint32_t client_id = resp->client_id;
        uint32_t symbol_id = resp->symbol_id;
        int64_t cost = static_cast<int64_t>(resp->p * resp->q);
        int64_t asset_delta = 0;
        int64_t cash_delta = 0;
        if (resp->side == Side_Buy) {
            asset_delta = static_cast<int64_t>(resp->q);
            cash_delta = -cost;
        } else {
            asset_delta = -static_cast<int64_t>(resp->q);
            cash_delta = cost;
        }
        auto& client_pos = get_or_create_client_positions(client_id);
        client_pos[symbol_id] += asset_delta;
        client_pos[0] += cash_delta;
    }

    void addOrUpdateOpenOrder(const OrderResponseT* resp) override {
        open_orders_[resp->client_id][resp->order_id] = *resp;
    }

    void removeOpenOrder(uint32_t client_id, uint64_t order_id) override {
        auto it = open_orders_.find(client_id);
        if (it != open_orders_.end()) {
            it->second.erase(order_id);
        }
    }

    void update_on_execution(const OrderResponseT* resp, [[maybe_unused]] bool not_sent) override {
        uint32_t client_id = resp->client_id;
        if ((EXEC_MASK_POSITION_UPDATE >> resp->exec_type) & 1) {
            updatePosition(resp);
        }
        if ((EXEC_MASK_UPSERT_OPEN >> resp->exec_type) & 1) {
            addOrUpdateOpenOrder(resp);
        } else if ((EXEC_MASK_REMOVE_OPEN >> resp->exec_type) & 1) {
            removeOpenOrder(client_id, resp->order_id);
        }
        appendResponseLog(client_id, *resp);
    }

    std::vector<std::vector<uint8_t>> getOpenOrders(uint32_t client_id) override {
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

    std::map<uint32_t, uint64_t> i_seq_nums_;
    std::map<uint32_t, uint64_t> o_seq_nums_;
    std::map<uint32_t, std::map<uint64_t, OrderResponseT>> response_log_;
    std::map<uint32_t, std::map<uint32_t, int64_t>> positions_;
    std::map<uint32_t, std::map<uint64_t, OrderResponseT>> open_orders_;
};

class PostgresClientDatabase : public ClientDatabase {
public:
    PostgresClientDatabase(const std::string& conn_str);
    ~PostgresClientDatabase() override;

    void appendResponseLog(uint32_t client_id, const OrderResponseT& resp) override;
    std::vector<OrderResponseT> popPendingResponses(uint32_t client_id) override;

    uint64_t getClientISeqNum(uint32_t client_id) override;
    void setClientISeqNum(uint32_t client_id, uint64_t seq_num) override;
    uint64_t getClientOSeqNum(uint32_t client_id) override;
    uint64_t incrementAndGetClientOSeqNum(uint32_t client_id) override;
    std::vector<OrderResponseT> getResponsesSince(uint32_t client_id, uint64_t ack_seq_num) override;
    void acknowledgeResponses(uint32_t client_id, uint64_t ack_seq_num) override;

    int64_t getPosition(uint32_t client_id, uint32_t symbol_id) override;
    std::map<uint32_t, int64_t> getAllPositions(uint32_t client_id) override;
    void updatePosition(const OrderResponseT* resp) override;

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
