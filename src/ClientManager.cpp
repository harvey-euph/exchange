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

    ws_adaptor_->set_close_handler([this](WSClientPtr ws) {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        auto client_ptr = static_cast<CMClientPtr*>(ws->get_super());
        if (client_ptr && *client_ptr) {
            auto client = *client_ptr;
            client->remove_conn(ws);
            if (client->empty()) {
                db_->setClientISeqNum(client->client_id(), client->inbound_seq_num());
                db_->setClientOSeqNum(client->client_id(), client->outbound_seq_num());
                clients_.erase(client->client_id());
            }
        }
    });
    
    auto message_handler = [this](WSClientPtr client, const void* data, size_t size) {
        this->process_client_request(client, data, size);
    };

    ws_adaptor_->set_message_handler(message_handler);
    
    LOG_INFO("[ClientManager] WS Handlers registered.");
}

void ClientManager::handle_client_logon(WSClientPtr ws, const AdminRequest* admin_req)
{
    uint32_t client_id = admin_req->client_id();
    uint64_t expected_msg_seq_num = db_->getClientISeqNum(client_id) + 1;
    uint64_t expected_ack_seq_num = db_->getClientOSeqNum(client_id);
    
    uint64_t client_msg_seq_num = admin_req->msg_seq_num();
    uint64_t client_ack_seq_num = admin_req->ack_seq_num();

    if (client_msg_seq_num != expected_msg_seq_num || client_ack_seq_num > expected_ack_seq_num) {
        LOG_INFO("[ClientManager] Client %d connection rejected. Expected msg: %d, ack: %d", client_id, expected_msg_seq_num, expected_ack_seq_num);
        
        flatbuffers::FlatBufferBuilder fbb(128);
        auto admin_resp = CreateAdminResponse(fbb, AdminResponseType_Reject, client_id, expected_msg_seq_num, expected_ack_seq_num, RejectCode_InvalidSequenceNumber, db_->incrementAndGetClientOSeqNum(client_id));
        auto client_resp = CreateClientResponse(fbb, ClientResponseData_AdminResponse, admin_resp.Union());
        fbb.Finish(client_resp);
        ws->send(fbb.GetBufferPointer(), fbb.GetSize());
        // Do not store session or mark ready
        return;
    }

    db_->setClientISeqNum(client_id, client_msg_seq_num);
    db_->acknowledgeResponses(client_id, client_ack_seq_num);

    CMClientPtr client;
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        auto it = clients_.find(client_id);
        if (it == clients_.end()) {
            client = std::make_shared<CMClient>(client_id);
            client->set_client_id(client_id);
            client->set_inbound_seq_num(client_msg_seq_num);
            client->set_outbound_seq_num(expected_ack_seq_num);
            clients_[client_id] = client;
        } else {
            client = it->second;
        }
        client->add_conn(ws);
        ws->set_super(&clients_[client_id]);
    }
    LOG_INFO("[ClientManager] Client %d connected (connections: %zu).", client_id, client->get_conns().size());
    
    // Send missed executions (OrderResponse)
    auto missed_responses = db_->getResponsesSince(client_id, client_ack_seq_num);
    LOG_INFO("[ClientManager] Sending %d missed responses.", missed_responses.size());
    for (auto& resp : missed_responses) {
        flatbuffers::FlatBufferBuilder fbb(256);
        auto resp_offset = OrderResponse::Pack(fbb, &resp);
        auto client_resp = CreateClientResponse(fbb, ClientResponseData_OrderResponse, resp_offset.Union());
        fbb.Finish(client_resp);
        ws->send(fbb.GetBufferPointer(), fbb.GetSize());
        logOrderResponse(flatbuffers::GetRoot<ClientResponse>(fbb.GetBufferPointer())->data_as_OrderResponse(), "[ClientManager] Resending Missed:");
    }

    // Set ready for this client session
    ws->set_ready(true);

    // Send AdminResponse(Ready)
    flatbuffers::FlatBufferBuilder fbb(128);
    auto admin_resp = CreateAdminResponse(fbb, AdminResponseType_Ready, client_id, expected_msg_seq_num, expected_ack_seq_num, RejectCode_None, db_->incrementAndGetClientOSeqNum(client_id));
    auto client_resp = CreateClientResponse(fbb, ClientResponseData_AdminResponse, admin_resp.Union());
    fbb.Finish(client_resp);
    ws->send(fbb.GetBufferPointer(), fbb.GetSize());
    
    LOG_INFO("[ClientManager] Client %d session ready.", client_id);
}

void ClientManager::handle_client_logout(WSClientPtr ws, const AdminRequest* admin_req) {
    uint32_t client_id = admin_req->client_id();
    auto client_ptr = static_cast<CMClientPtr*>(ws->get_super());
    if (client_ptr && *client_ptr) {
        auto client = *client_ptr;
        client->remove_conn(ws);

        std::lock_guard<std::mutex> lock(clients_mutex_);
        if (client->empty()) {
            db_->setClientISeqNum(client_id, client->inbound_seq_num());
            db_->setClientOSeqNum(client_id, client->outbound_seq_num());
            clients_.erase(client_id);
        }
    }
    LOG_INFO("[ClientManager] Client %d disconnected.", client_id);
}

void ClientManager::process_client_request(WSClientPtr ws, const void* data, size_t size)
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
            this->handle_client_logon(ws, admin_req);
        } else if (admin_req->action() == AdminAction_LogOut) {
            this->handle_client_logout(ws, admin_req);
            // TODO: when received EOF, also process as logout
        }
        return;
    }

    // Check session readiness
    if (!ws->is_ready()) {
        // Optionally send an error or just ignore
        return;
    }

    auto client_ptr = static_cast<CMClientPtr*>(ws->get_super());
    if (!client_ptr || !*client_ptr) return;
    auto client = *client_ptr;

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

    if (client && !client->empty()) {
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
