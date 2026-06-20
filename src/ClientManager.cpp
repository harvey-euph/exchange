#include "ClientManager.hpp"
#include "LogUtil.hpp"
#include "TimeUtil.hpp"
#include <iostream>
#include <algorithm>
#include <new>
#include <sys/sdt.h>

namespace Exchange {

ClientManager::ClientManager(int port, SHMRingBuffer* request_ring, mmaplog::MmapReader* response_ring, std::shared_ptr<ClientDatabase> db) 
    : ws_adaptor_(std::make_shared<WSAdaptor>(port))
    , request_ring_(request_ring)
    , response_ring_(response_ring)
    , db_(db)
{
    std::cout << "[ClientManager] Initializing on port " << port << std::endl;

    auto close_handler = [this](WSClientPtr client) {
        for (auto it = client_sessions_.begin(); it != client_sessions_.end(); ) {
            auto& sessions = it->second;
            auto original_size = sessions.size();
            sessions.erase(std::remove(sessions.begin(), sessions.end(), client), sessions.end());
            
            if (sessions.size() < original_size) {
                uint32_t client_id = it->first;
                std::cout << "[ClientManager] Session break detected for client " << client_id 
                << ". Treating as automatic logout." << std::endl;
                
                if (sessions.empty()) {
                    it = client_sessions_.erase(it);
                    continue; 
                }
            }
            ++it;
        }
        client->is_ready.store(false, std::memory_order_release);
    };
    
    auto message_handler = [this](WSClientPtr client, const void* data, size_t size) {
        DTRACE_PROBE(exchange, req_entry);
        this->process_client_request(client, data, size);
    };

    ws_adaptor_->set_close_handler(close_handler);
    ws_adaptor_->set_message_handler(message_handler);
    
    std::cout << "[ClientManager] WS Handlers registered." << std::endl;
}

void ClientManager::handle_client_subscription(WSClientPtr client, uint32_t client_id, bool is_subscribe) {
    if (is_subscribe) {
        client_sessions_[client_id].push_back(client);
        std::cout << "[ClientManager] Client " << client_id << " connected (sessions: " 
                  << client_sessions_[client_id].size() << ")." << std::endl;
        
        // 1. Send pending executions (OrderResponse)
        auto pending = db_->popPendingResponses(client_id);
        std::cout << "[ClientManager] Sending " << pending.size() << " pending responses." << std::endl;
        for (auto& resp : pending) {
            flatbuffers::FlatBufferBuilder fbb(256);
            auto resp_offset = OrderResponse::Pack(fbb, &resp);
            auto client_resp = CreateClientResponse(fbb, ClientResponseData_OrderResponse, resp_offset.Union());
            fbb.Finish(client_resp);
            client->send(fbb.GetBufferPointer(), fbb.GetSize());
            logOrderResponse(flatbuffers::GetRoot<ClientResponse>(fbb.GetBufferPointer())->data_as_OrderResponse(), "[ClientManager] Resending Pending:");
        }

        // 2. Send current open order as OrderResponse with ExecType=OrderStatus
        auto open_orders = db_->getOpenOrders(client_id);
        std::cout << "[ClientManager] Sending " << open_orders.size() << " open orders." << std::endl;
        for (auto& order_data : open_orders) {
            client->send(order_data.data(), order_data.size());
            auto client_resp = flatbuffers::GetRoot<ClientResponse>(order_data.data());
            if (client_resp->data_type() == ClientResponseData_OrderResponse) {
                logOrderResponse(client_resp->data_as_OrderResponse(), "[ClientManager] Resending Open Order:");
            }
        }

        // 3. Send current positions for all non-zero asset and cash(symbol_id=0)
        auto positions = db_->getAllPositions(client_id);
        std::cout << "[ClientManager] Sending positions for " << positions.size() << " symbols." << std::endl;
        for (auto const& [sym, pos] : positions) {
            if (pos != 0 || sym == 0) {
                flatbuffers::FlatBufferBuilder fbb(128);
                auto pos_resp = CreatePositionResponse(fbb, client_id, sym, pos);
                auto client_resp = CreateClientResponse(fbb, ClientResponseData_PositionResponse, pos_resp.Union());
                fbb.Finish(client_resp);
                client->send(fbb.GetBufferPointer(), fbb.GetSize());
            }
        }

        // 4. Set ready for this client session
        client->is_ready.store(true, std::memory_order_release);

        // 5. Send ready frame (OrderResponse with ExecType=Complete)
        flatbuffers::FlatBufferBuilder fbb(128);
        auto ready_resp = CreateOrderResponse(fbb, ExecType_Complete, 0, client_id, 0, 0, Side_None, 0, 0, RejectCode_None);
        auto client_resp = CreateClientResponse(fbb, ClientResponseData_OrderResponse, ready_resp.Union());
        fbb.Finish(client_resp);
        client->send(fbb.GetBufferPointer(), fbb.GetSize());
        
        auto ready_resp_ptr = flatbuffers::GetRoot<ClientResponse>(fbb.GetBufferPointer())->data_as_OrderResponse();
        logOrderResponse(ready_resp_ptr, "[ClientManager] Mgmt Ready:");

        std::cout << "[ClientManager] Client " << client_id << " session ready (Mgmt Ready)." << std::endl;
    } else {
        auto it = client_sessions_.find(client_id);
        if (it != client_sessions_.end()) {
            auto& sessions = it->second;
            sessions.erase(std::remove(sessions.begin(), sessions.end(), client), sessions.end());
            if (sessions.empty()) {
                client_sessions_.erase(it);
            }
        }
        client->is_ready.store(false, std::memory_order_release);
        std::cout << "[ClientManager] Client " << client_id << " disconnected." << std::endl;
    }
}

void ClientManager::process_client_request(WSClientPtr client, const void* data, size_t size)
{
    flatbuffers::Verifier verifier(reinterpret_cast<const uint8_t*>(data), size);
    if (!verifier.VerifyBuffer<ClientRequest>(nullptr)) {
        return;
    }

    auto request = flatbuffers::GetRoot<ClientRequest>(data);
    auto type = request->data_type();

    // Logon/Logoff can be executed without is_ready check
    if (type == ClientRequestData_AdminRequest) {
        auto admin_req = request->data_as_AdminRequest();
        if (admin_req->action() == AdminAction_LogOn) {
            this->handle_client_subscription(client, admin_req->client_id(), true);
        } else if (admin_req->action() == AdminAction_LogOut) {
            this->handle_client_subscription(client, admin_req->client_id(), false);
        }
        return;
    }

    // Check session readiness
    if (!client->is_ready.load(std::memory_order_acquire)) {
        // Optionally send an error or just ignore
        return;
    }

    switch (type) {
        case ClientRequestData_OrderRequest: {
            auto order_req = request->data_as_OrderRequest();
            auto token = request_ring_->reserve(sizeof(OrderRequestT));
            if (!token) {
                // TODO: Send some alert
                return;
            }
            new (token->payload) OrderRequestT {
                .action      = order_req->action(),
                .exec_id     = order_req->exec_id(),
                .order_id    = order_req->order_id(),
                .client_id   = order_req->client_id(),
                .symbol_id   = order_req->symbol_id(),
                .side        = order_req->side(),
                .type        = order_req->type(),
                .p           = order_req->p(),
                .q           = order_req->q(),
                .visible_qty = order_req->visible_qty(),
                .timestamp   = order_req->timestamp(),
            };
            request_ring_->commit(*token);
            DTRACE_PROBE1(exchange, req_enqueue, order_req->exec_id());
            break;
        }
        case ClientRequestData_PositionRequest: {
            auto post_req = request->data_as_PositionRequest();
            int64_t pos = db_->getPosition(post_req->client_id(), post_req->symbol_id());
            
            flatbuffers::FlatBufferBuilder fbb(128);
            auto pos_resp = CreatePositionResponse(fbb, post_req->client_id(), post_req->symbol_id(), pos);
            auto client_resp = CreateClientResponse(fbb, ClientResponseData_PositionResponse, pos_resp.Union());
            fbb.Finish(client_resp);
            client->send(fbb.GetBufferPointer(), fbb.GetSize());
            break;
        }
        case ClientRequestData_OpenOrderRequest: {
            auto open_req = request->data_as_OpenOrderRequest();
            uint32_t client_id = open_req->client_id();

            auto open_orders = db_->getOpenOrders(client_id);
            std::cout << "[ClientManager] Sending " << open_orders.size() << " open orders on request." << std::endl;
            for (auto& order_data : open_orders) {
                client->send(order_data.data(), order_data.size());
            }
            break;
        }
        default: break;
    }
}

void ClientManager::handle_execution_response(const OrderResponseT* resp)
{
    DTRACE_PROBE1(exchange, exec_resp_entry, resp->exec_id);
    uint32_t client_id = resp->client_id;
    bool not_sent = true;

    auto it = client_sessions_.find(client_id);
    if (it != client_sessions_.end() && !it->second.empty()) {
        flatbuffers::FlatBufferBuilder fbb(256);
        auto resp_offset = OrderResponse::Pack(fbb, resp);
        auto client_resp = CreateClientResponse(fbb, ClientResponseData_OrderResponse, resp_offset.Union());
        fbb.Finish(client_resp);

        for (auto& session : it->second) {
            session->send(fbb.GetBufferPointer(), fbb.GetSize());
        }
        not_sent = false;
        DTRACE_PROBE1(exchange, exec_resp_before_db, resp->exec_id);
    }    

    db_->update_on_execution(resp, not_sent);
}

int ClientManager::poll_client()
{
    return ws_adaptor_->poll();
}

int ClientManager::poll_server()
{
    const void* data = nullptr;
    uint32_t len = 0;
    if (!response_ring_->read_next(data, len)) {
        return 0;
    }

    if (len >= sizeof(OrderResponseT)) {
        auto resp = reinterpret_cast<const OrderResponseT*>(data);
        handle_execution_response(resp);
    }
    return 1;
}

} // namespace Exchange
