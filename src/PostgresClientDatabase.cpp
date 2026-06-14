#include "ClientDatabase.hpp"
#include "define.hpp"
#include <iostream>
#include <pqxx/pqxx>

namespace Exchange {

PostgresClientDatabase::PostgresClientDatabase(const std::string& conn_str)
    : conn_str_(conn_str)
{
    try {
        conn_ = std::make_unique<pqxx::connection>(conn_str_);
    } catch (const std::exception& e) {
        std::cerr << "[PostgresClientDatabase] Connection failed: " << e.what() << std::endl;
        throw;
    }
}

PostgresClientDatabase::~PostgresClientDatabase() = default;

void PostgresClientDatabase::addPendingResponse(uint32_t client_id, const OrderResponseT& resp) {
    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t exec_id = resp.exec_id;
    
    flatbuffers::FlatBufferBuilder fbb(256);
    auto resp_offset = OrderResponse::Pack(fbb, &resp);
    auto client_resp = CreateClientResponse(fbb, ClientResponseData_OrderResponse, resp_offset.Union());
    fbb.Finish(client_resp);

    reconnect_if_needed();
    pqxx::work w(*conn_);
    w.exec(
        "INSERT INTO clients (client_id, username) VALUES ($1, $2) ON CONFLICT (client_id) DO NOTHING",
        pqxx::params{client_id, "client_" + std::to_string(client_id)}
    );
    w.exec(
        "INSERT INTO pending_responses (client_id, exec_id, serialized_data) VALUES ($1, $2, $3)",
        pqxx::params{client_id, exec_id, pqxx::bytes_view{reinterpret_cast<const std::byte*>(fbb.GetBufferPointer()), fbb.GetSize()}}
    );
    w.commit();
}

std::vector<OrderResponseT> PostgresClientDatabase::popPendingResponses(uint32_t client_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    reconnect_if_needed();
    std::vector<OrderResponseT> result;
    pqxx::work w(*conn_);
    pqxx::result r = w.exec(
        "SELECT serialized_data FROM pending_responses WHERE client_id = $1 ORDER BY response_id ASC",
        pqxx::params{client_id}
    );
    w.exec("DELETE FROM pending_responses WHERE client_id = $1", pqxx::params{client_id});
    w.commit();
    
    for (auto const& row : r) {
        auto bytes = row[0].as<pqxx::bytes>();
        auto client_resp = flatbuffers::GetRoot<ClientResponse>(bytes.data());
        if (client_resp->data_type() == ClientResponseData_OrderResponse) {
            OrderResponseT resp;
            client_resp->data_as_OrderResponse()->UnPackTo(&resp);
            result.push_back(std::move(resp));
        }
    }
    return result;
}

int64_t PostgresClientDatabase::getPosition(uint32_t client_id, uint32_t symbol_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    reconnect_if_needed();
    pqxx::work w(*conn_);
    pqxx::result r = w.exec(
        "SELECT position FROM positions WHERE client_id = $1 AND symbol_id = $2",
        pqxx::params{client_id, symbol_id}
    );
    if (r.empty()) {
        return (symbol_id == 0) ? 1000000 : 0;
    }
    return r[0][0].as<int64_t>();
}

std::map<uint32_t, int64_t> PostgresClientDatabase::getAllPositions(uint32_t client_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    reconnect_if_needed();
    pqxx::work w(*conn_);
    pqxx::result r = w.exec(
        "SELECT symbol_id, position FROM positions WHERE client_id = $1",
        pqxx::params{client_id}
    );
    std::map<uint32_t, int64_t> result;
    for (auto const& row : r) {
        result[row[0].as<uint32_t>()] = row[1].as<int64_t>();
    }
    if (result.find(0) == result.end()) {
        result[0] = 1000000;
    }
    if (result.find(1) == result.end()) {
        result[1] = 0;
    }
    return result;
}

void PostgresClientDatabase::updatePosition(const OrderResponseT* resp) {
    std::lock_guard<std::mutex> lock(mutex_);
    reconnect_if_needed();
    
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

    pqxx::work w(*conn_);
    std::string username = "client_" + std::to_string(client_id);

    w.exec(
        "INSERT INTO clients (client_id, username) VALUES ($1, $2) "
        "ON CONFLICT (client_id) DO NOTHING",
        pqxx::params{client_id, username}
    );

    // Update Cash position (symbol_id = 0, initial_pos = 1,000,000)
    w.exec(
        "INSERT INTO positions (client_id, symbol_id, position) "
        "VALUES ($1, 0, 1000000 + $2) "
        "ON CONFLICT (client_id, symbol_id) "
        "DO UPDATE SET position = positions.position + $3",
        pqxx::params{client_id, cash_delta, cash_delta}
    );

    // Update Asset position (symbol_id, initial_pos = 0)
    if (symbol_id != 0) {
        w.exec(
            "INSERT INTO positions (client_id, symbol_id, position) "
            "VALUES ($1, $2, $3) "
            "ON CONFLICT (client_id, symbol_id) "
            "DO UPDATE SET position = positions.position + $4",
            pqxx::params{client_id, symbol_id, asset_delta, asset_delta}
        );
    }
    w.commit();
}

void PostgresClientDatabase::update_on_execution(const OrderResponseT* resp, bool not_sent) {
    uint32_t client_id = resp->client_id;
    if ((EXEC_MASK_POSITION_UPDATE >> resp->exec_type) & 1) {
        updatePosition(resp);
    }
    if ((EXEC_MASK_UPSERT_OPEN >> resp->exec_type) & 1) {
        addOrUpdateOpenOrder(resp);
    } else if ((EXEC_MASK_REMOVE_OPEN >> resp->exec_type) & 1) {
        removeOpenOrder(client_id, resp->order_id);
    }
    if (not_sent) {
        addPendingResponse(client_id, *resp);
    }
}

void PostgresClientDatabase::addOrUpdateOpenOrder(const OrderResponseT* resp) {
    std::lock_guard<std::mutex> lock(mutex_);
    reconnect_if_needed();

    uint32_t client_id = resp->client_id;
    uint64_t order_id = resp->order_id;
    uint32_t symbol_id = resp->symbol_id;
    int16_t side = static_cast<int16_t>(resp->side);
    int64_t price = resp->p;
    uint64_t qty = resp->q;
 
    pqxx::work w(*conn_);

    std::string username = "client_" + std::to_string(client_id);

    w.exec(
        "INSERT INTO clients (client_id, username) "
        "VALUES ($1, $2) "
        "ON CONFLICT (client_id) DO NOTHING",
        pqxx::params{client_id, username}
    );

    w.exec(
        "INSERT INTO open_orders "
        "(order_id, client_id, symbol_id, side, price_mantissa, qty, visible_qty, timestamp) "
        "VALUES ($1, $2, $3, $4, $5, $6, $7, NOW()) ON CONFLICT (order_id) DO UPDATE SET "
        "price_mantissa = EXCLUDED.price_mantissa, "
        "qty = EXCLUDED.qty, visible_qty = EXCLUDED.visible_qty, updated_at = NOW()",
        pqxx::params{order_id, client_id, symbol_id, side, price, qty, qty}
    );
    w.commit();
}

void PostgresClientDatabase::removeOpenOrder(uint32_t client_id, uint64_t order_id) {
    (void)client_id;
    std::lock_guard<std::mutex> lock(mutex_);
    reconnect_if_needed();
    pqxx::work w(*conn_);
    w.exec("DELETE FROM open_orders WHERE order_id = $1", pqxx::params{order_id});
    w.commit();
}

std::vector<std::vector<uint8_t>> PostgresClientDatabase::getOpenOrders(uint32_t client_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    reconnect_if_needed();
    pqxx::work w(*conn_);
    pqxx::result r = w.exec(
        "SELECT order_id, symbol_id, side, price_mantissa, qty FROM open_orders WHERE client_id = $1",
        pqxx::params{client_id}
    );
    std::vector<std::vector<uint8_t>> result;
    for (auto const& row : r) {
        uint64_t order_id = row[0].as<uint64_t>();
        uint32_t symbol_id = row[1].as<uint32_t>();
        int16_t side = row[2].as<int16_t>();
        int64_t price = row[3].as<int64_t>();
        uint64_t qty = row[4].as<uint64_t>();

        flatbuffers::FlatBufferBuilder fbb(256);
        auto resp_offset = CreateOrderResponse(fbb, ExecType_OrderStatus, order_id, client_id, 0 /* exec_id */, symbol_id, static_cast<Side>(side), price, qty, RejectCode_None);
        auto client_resp = CreateClientResponse(fbb, ClientResponseData_OrderResponse, resp_offset.Union());
        fbb.Finish(client_resp);

        uint8_t* buf = fbb.GetBufferPointer();
        size_t sz = fbb.GetSize();
        result.push_back(std::vector<uint8_t>(buf, buf + sz));
    }
    return result;
}

void PostgresClientDatabase::reconnect_if_needed() {
    if (!conn_ || !conn_->is_open()) {
        try {
            conn_ = std::make_unique<pqxx::connection>(conn_str_);
        } catch (...) {
            // Ignore
        }
    }
}

} // namespace Exchange
