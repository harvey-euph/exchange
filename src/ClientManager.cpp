#include "ClientManager.hpp"
#include "LogUtil.hpp"
#include "TimeUtil.hpp"
#include <iostream>
#include <algorithm>
#include <sys/sdt.h>

namespace Exchange {

ClientManager::ClientManager(int port, SHMRingBuffer* request_ring, SHMRingBuffer* response_ring, std::shared_ptr<ClientDatabase> db) 
    : ws_adaptor_(std::make_shared<WSAdaptor>(port))
    , request_ring_(request_ring)
    , response_ring_(response_ring)
    , db_(db)
{
    std::cout << "[ClientManager] Initializing on port " << port << std::endl;

    auto subscribe_handler = [this](WSClientPtr client, uint32_t client_id, bool is_subscribe)
    {
        std::lock_guard<std::mutex> sessions_guard(sessions_mutex_);
        if (is_subscribe) {
            client_sessions_[client_id].push_back(client);
            std::cout << "[ClientManager] Client " << client_id << " connected (sessions: " 
                      << client_sessions_[client_id].size() << ")." << std::endl;
            
            // 1. Send pending executions (OrderResponse)
            auto pending = db_->popPendingResponses(client_id);
            std::cout << "[ClientManager] Sending " << pending.size() << " pending responses." << std::endl;
            for (auto& resp : pending) {
                client->send(resp.data.data(), resp.data.size());
                auto client_resp = flatbuffers::GetRoot<ClientResponse>(resp.data.data());
                if (client_resp->data_type() == ClientResponseData_OrderResponse) {
                    logOrderResponse(client_resp->data_as_OrderResponse(), "[ClientManager] Resending Pending:");
                }
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
    };
    
    auto close_handler = [this](WSClientPtr client) {
        std::lock_guard<std::mutex> sessions_guard(sessions_mutex_);
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
        this->process_client_request(client, data, size);
    };

    ws_adaptor_->set_subscribe_handler(subscribe_handler);
    ws_adaptor_->set_close_handler(close_handler);
    ws_adaptor_->set_message_handler(message_handler);
    
    std::cout << "[ClientManager] WS Handlers registered." << std::endl;
}

void ClientManager::process_client_request(WSClientPtr client, const void* data, size_t size)
{    
    DTRACE_PROBE(exchange, req_entry);
    (void) size;

    // Check session readiness
    if (!client->is_ready.load(std::memory_order_acquire)) {
        // Optionally send an error or just ignore
        return;
    }

    auto request = flatbuffers::GetRoot<ClientRequest>(data);
    auto type = request->data_type();

    switch (type) {
        case ClientRequestData_OrderRequest: {
            auto order_req = request->data_as_OrderRequest();
            
            // logOrderRequest(order_req, "[ClientManager] Received Order Request:");

            flatbuffers::FlatBufferBuilder fbb(256);
            auto or_offset = CreateOrderRequest(fbb, 
                order_req->action(), order_req->exec_id(), order_req->order_id(), 
                order_req->client_id(), order_req->symbol_id(), order_req->side(), 
                order_req->type(), order_req->p(), order_req->q(), 
                order_req->visible_qty(), order_req->timestamp());
            fbb.Finish(or_offset);
            DTRACE_PROBE1(exchange, req_enqueue, order_req->exec_id());
            request_ring_->enqueue(fbb.GetBufferPointer(), fbb.GetSize());
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
        default: break;
    }
}

void ClientManager::handle_execution_response(const OrderResponseT* resp)
{
    DTRACE_PROBE1(exchange, exec_resp_entry, resp->exec_id);
    uint32_t client_id = resp->client_id;
    bool not_sent = true;

    {
        std::lock_guard<std::mutex> sessions_guard(sessions_mutex_);
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
        }
    }

    DTRACE_PROBE1(exchange, exec_resp_before_db, resp->exec_id);
    db_->update_on_execution(resp, not_sent);
}

int ClientManager::poll_client() {
    return ws_adaptor_->poll() > 0 ? 1 : 0;
}

int ClientManager::poll_server() {
    void* data_ptr = nullptr;
    size_t data_size = 0;
    if (response_ring_->dequeue(&data_ptr, &data_size)) {
        if (data_ptr && data_size >= sizeof(OrderResponseT)) {
            auto resp = reinterpret_cast<const OrderResponseT*>(data_ptr);
            handle_execution_response(resp);
            return 1;
        }
    }
    return 0;
}

} // namespace Exchange
