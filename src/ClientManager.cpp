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
    LOG_INFO("[ClientManager] Initializing on port %d", port);

    CMClient::bind_adaptor(
        ws_adaptor_,
        nullptr,
        [this](CMClientPtr client) {
            this->handle_client_logout(client);
        },
        [this](CMClientPtr client, const void* data, size_t size) {
            this->process_client_request(client, data, size);
        }
    );
    
    LOG_INFO("[ClientManager] WS Handlers registered.");
}

void ClientManager::handle_client_logon(CMClientPtr new_client, const AdminRequest* admin_req)
{
    uint32_t client_id = admin_req->client_id();
    uint64_t client_msg_seq_num = admin_req->msg_seq_num();
    uint64_t client_ack_seq_num = admin_req->ack_seq_num();

    uint64_t expected_msg_seq_num = 0;
    uint64_t expected_ack_seq_num = 0;

    CMClientPtr old_client;

    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        auto it = clients_.find(client_id);
        if (it != clients_.end()) {
            old_client = it->second;
        }
    }

    if (old_client) {
        expected_msg_seq_num = old_client->inbound_seq_num() + 1;
        expected_ack_seq_num = old_client->outbound_seq_num();
    } else {
        expected_msg_seq_num = db_->getClientISeqNum(client_id) + 1;
        expected_ack_seq_num = db_->getClientOSeqNum(client_id);
    }

    if (client_msg_seq_num != expected_msg_seq_num || client_ack_seq_num > expected_ack_seq_num) {
        LOG_INFO("[ClientManager] Client %d connection rejected. Expected msg: %ld, ack: %ld", client_id, expected_msg_seq_num, expected_ack_seq_num);
        
        flatbuffers::FlatBufferBuilder fbb(128);
        uint64_t rej_seq_num = old_client ? old_client->increment_outbound_seq_num() : db_->incrementAndGetClientOSeqNum(client_id);
        auto admin_resp = CreateAdminResponse(fbb, AdminResponseType_Reject, client_id, expected_msg_seq_num, expected_ack_seq_num, RejectCode_InvalidSequenceNumber, rej_seq_num);
        auto client_resp = CreateClientResponse(fbb, ClientResponseData_AdminResponse, admin_resp.Union());
        fbb.Finish(client_resp);
        new_client->send(fbb.GetBufferPointer(), fbb.GetSize());
        // Do not store session or mark ready
        return;
    }


    uint64_t shared_msg_seq_num = 0;

    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        if (old_client) {
            shared_msg_seq_num = old_client->increment_outbound_seq_num();
            WSClientPtr old_ws = old_client->get_conn();
            if (old_ws && old_ws != new_client->get_conn()) {
                flatbuffers::FlatBufferBuilder fbb(128);
                auto admin_resp = CreateAdminResponse(fbb, AdminResponseType_Reject, client_id, expected_msg_seq_num, expected_ack_seq_num, RejectCode_LoginAtOtherSession, shared_msg_seq_num);
                auto client_resp = CreateClientResponse(fbb, ClientResponseData_AdminResponse, admin_resp.Union());
                fbb.Finish(client_resp);
                old_client->send(fbb.GetBufferPointer(), fbb.GetSize());
                
                old_ws->close();
            }
        } else {
            shared_msg_seq_num = expected_ack_seq_num + 1;
        }
        
        new_client->set_client_id(client_id);
        new_client->set_inbound_seq_num(client_msg_seq_num);
        new_client->set_outbound_seq_num(shared_msg_seq_num);
        clients_[client_id] = new_client;
    }
    LOG_INFO("[ClientManager] Client %d connected.", client_id);
    
    // Send missed executions (OrderResponse)
    auto missed_responses = db_->getResponsesSince(client_id, client_ack_seq_num);
    LOG_INFO("[ClientManager] Sending %ld missed responses.", missed_responses.size());
    for (auto& resp : missed_responses) {
        flatbuffers::FlatBufferBuilder fbb(256);
        auto resp_offset = OrderResponse::Pack(fbb, &resp);
        auto client_resp = CreateClientResponse(fbb, ClientResponseData_OrderResponse, resp_offset.Union());
        fbb.Finish(client_resp);
        new_client->send(fbb.GetBufferPointer(), fbb.GetSize());
        logOrderResponse(flatbuffers::GetRoot<ClientResponse>(fbb.GetBufferPointer())->data_as_OrderResponse(), "[ClientManager] Resending Missed:");
    }

    // Set ready for this client session
    new_client->set_ready(true);

    // Send AdminResponse(Ready)
    flatbuffers::FlatBufferBuilder fbb(128);
    auto admin_resp = CreateAdminResponse(fbb, AdminResponseType_Ready, client_id, expected_msg_seq_num, expected_ack_seq_num, RejectCode_None, shared_msg_seq_num);
    auto client_resp = CreateClientResponse(fbb, ClientResponseData_AdminResponse, admin_resp.Union());
    fbb.Finish(client_resp);
    new_client->send(fbb.GetBufferPointer(), fbb.GetSize());
    
    LOG_INFO("[ClientManager] Client %d session ready.", client_id);
}

void ClientManager::handle_client_logout(CMClientPtr client) {
    if (!client) return;
    uint32_t client_id = client->client_id();
    if (client_id == 0) return; // Unauthenticated client, nothing to remove from DB or map

    std::lock_guard<std::mutex> lock(clients_mutex_);
    auto it = clients_.find(client_id);
    if (it != clients_.end() && it->second == client) {
        db_->setClientISeqNum(client_id, client->inbound_seq_num());
        db_->setClientOSeqNum(client_id, client->outbound_seq_num());
        clients_.erase(it);
    }
    
    LOG_INFO("[ClientManager] Client %d disconnected.", client_id);
}

void ClientManager::process_client_request(CMClientPtr client, const void* data, size_t size)
{
    DTRACE_PROBE(exchange, req_entry);
    flatbuffers::Verifier verifier(reinterpret_cast<const uint8_t*>(data), size);
    if (!verifier.VerifyBuffer<ClientRequest>(nullptr)) {
        LOG_WARN("[ClientManager] Received invalid data from a client (failed flatbuffer verification).");
        return;
    }

    auto request = flatbuffers::GetRoot<ClientRequest>(data);
    auto type = request->data_type();

    // Logon/Logoff can be executed without is_ready check
    if (type == ClientRequestData_AdminRequest) {
        auto admin_req = request->data_as_AdminRequest();
        if (admin_req->action() == AdminAction_LogOn) {
            handle_client_logon(client, admin_req);
        } else if (admin_req->action() == AdminAction_LogOut) {
            this->handle_client_logout(client);
        }
        return;
    }

    WSClientPtr ws = client->get_conn();
    if (!client->is_ready()) {
        LOG_WARN("[ClientManager] Received non-admin request before logon completed.");
        return;
    }

    switch (type) {
        case ClientRequestData_OrderRequest: {
            auto order_req = request->data_as_OrderRequest();
            uint64_t expected_i_seq = client->inbound_seq_num() + 1;
            if (order_req->msg_seq_num() != expected_i_seq) {
                LOG_WARN("[ClientManager] Order seqnum mismatch. Expected %lu, got %lu", expected_i_seq, order_req->msg_seq_num());
                flatbuffers::FlatBufferBuilder fbb(128);
                uint64_t new_o_seq = client->increment_outbound_seq_num();
                auto resp_offset = CreateOrderResponse(fbb, ExecType_Rejected, order_req->order_id(), order_req->client_id(), order_req->exec_id(), order_req->symbol_id(), order_req->side(), order_req->p(), order_req->q(), RejectCode_InvalidSequenceNumber, new_o_seq, order_req->msg_seq_num());
                auto client_resp = CreateClientResponse(fbb, ClientResponseData_OrderResponse, resp_offset.Union());
                fbb.Finish(client_resp);
                ws->send(fbb.GetBufferPointer(), fbb.GetSize());
                return;
            }
            client->increment_inbound_seq_num();

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
            ws->send(fbb.GetBufferPointer(), fbb.GetSize());
            break;
        }
        case ClientRequestData_OpenOrderRequest: {
            auto open_req = request->data_as_OpenOrderRequest();
            uint32_t client_id = open_req->client_id();

            auto open_orders = db_->getOpenOrders(client_id);
            LOG_INFO("[ClientManager] Sending %d open orders on request.", open_orders.size());
            for (auto& order_data : open_orders) {
                ws->send(order_data.data(), order_data.size());
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

    CMClientPtr client;
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        auto it = clients_.find(client_id);
        if (it != clients_.end()) {
            client = it->second;
        }
    }

    if (client) {
        mut_resp.msg_seq_num = client->increment_outbound_seq_num();
    } else {
        mut_resp.msg_seq_num = db_->incrementAndGetClientOSeqNum(client_id);
    }

    if (client) {
        flatbuffers::FlatBufferBuilder fbb(256);
        auto resp_offset = OrderResponse::Pack(fbb, &mut_resp);
        auto client_resp = CreateClientResponse(fbb, ClientResponseData_OrderResponse, resp_offset.Union());
        fbb.Finish(client_resp);

        client->send(fbb.GetBufferPointer(), fbb.GetSize());
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
