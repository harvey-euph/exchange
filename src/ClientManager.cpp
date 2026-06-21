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

void ClientManager::handle_client_logon(WSClientPtr client, const AdminRequest* admin_req)
{
    uint32_t client_id = admin_req->client_id();
    uint64_t expected_msg_seq_num = db_->getClientISeqNum(client_id) + 1;
    uint64_t expected_ack_seq_num = db_->getClientOSeqNum(client_id);
    
    uint64_t client_msg_seq_num = admin_req->msg_seq_num();
    uint64_t client_ack_seq_num = admin_req->ack_seq_num();

    if (client_msg_seq_num != expected_msg_seq_num || client_ack_seq_num > expected_ack_seq_num) {
        std::cout << "[ClientManager] Client " << client_id << " connection rejected. Expected msg: " 
                  << expected_msg_seq_num << ", ack: " << expected_ack_seq_num << std::endl;
        
        flatbuffers::FlatBufferBuilder fbb(128);
        auto admin_resp = CreateAdminResponse(fbb, AdminResponseType_Reject, client_id, expected_msg_seq_num, expected_ack_seq_num, RejectCode_InvalidSequenceNumber, db_->incrementAndGetClientOSeqNum(client_id));
        auto client_resp = CreateClientResponse(fbb, ClientResponseData_AdminResponse, admin_resp.Union());
        fbb.Finish(client_resp);
        client->send(fbb.GetBufferPointer(), fbb.GetSize());
        // Do not store session or mark ready
        return;
    }

    db_->setClientISeqNum(client_id, client_msg_seq_num);
    db_->acknowledgeResponses(client_id, client_ack_seq_num);

    client_sessions_[client_id].push_back(client);
    std::cout << "[ClientManager] Client " << client_id << " connected (sessions: " 
              << client_sessions_[client_id].size() << ")." << std::endl;
    
    // Send missed executions (OrderResponse)
    auto missed_responses = db_->getResponsesSince(client_id, client_ack_seq_num);
    std::cout << "[ClientManager] Sending " << missed_responses.size() << " missed responses." << std::endl;
    for (auto& resp : missed_responses) {
        flatbuffers::FlatBufferBuilder fbb(256);
        auto resp_offset = OrderResponse::Pack(fbb, &resp);
        auto client_resp = CreateClientResponse(fbb, ClientResponseData_OrderResponse, resp_offset.Union());
        fbb.Finish(client_resp);
        client->send(fbb.GetBufferPointer(), fbb.GetSize());
        logOrderResponse(flatbuffers::GetRoot<ClientResponse>(fbb.GetBufferPointer())->data_as_OrderResponse(), "[ClientManager] Resending Missed:");
    }

    // Set ready for this client session
    client->is_ready.store(true, std::memory_order_release);

    // Send AdminResponse(Ready)
    flatbuffers::FlatBufferBuilder fbb(128);
    auto admin_resp = CreateAdminResponse(fbb, AdminResponseType_Ready, client_id, expected_msg_seq_num, expected_ack_seq_num, RejectCode_None, db_->incrementAndGetClientOSeqNum(client_id));
    auto client_resp = CreateClientResponse(fbb, ClientResponseData_AdminResponse, admin_resp.Union());
    fbb.Finish(client_resp);
    client->send(fbb.GetBufferPointer(), fbb.GetSize());
    
    std::cout << "[ClientManager] Client " << client_id << " session ready." << std::endl;
}

void ClientManager::handle_client_logout(WSClientPtr client, const AdminRequest* admin_req) {
    uint32_t client_id = admin_req->client_id();
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
            this->handle_client_logon(client, admin_req);
        } else if (admin_req->action() == AdminAction_LogOut) {
            this->handle_client_logout(client, admin_req);
            // TODO: when received EOF, also process as logout
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
                .msg_seq_num = order_req->msg_seq_num(),
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

    OrderResponseT mut_resp = *resp;
    mut_resp.msg_seq_num = db_->incrementAndGetClientOSeqNum(client_id);

    auto it = client_sessions_.find(client_id);
    if (it != client_sessions_.end() && !it->second.empty()) {
        flatbuffers::FlatBufferBuilder fbb(256);
        auto resp_offset = OrderResponse::Pack(fbb, &mut_resp);
        auto client_resp = CreateClientResponse(fbb, ClientResponseData_OrderResponse, resp_offset.Union());
        fbb.Finish(client_resp);

        for (auto& session : it->second) {
            session->send(fbb.GetBufferPointer(), fbb.GetSize());
        }
        not_sent = false;
        DTRACE_PROBE1(exchange, exec_resp_before_db, mut_resp.exec_id);
    }    

    db_->update_on_execution(&mut_resp, not_sent);
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
