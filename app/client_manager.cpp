#include "ring/SHMRingBuffer.hpp"
#include "fbs/order_generated.h"
#include "WSAdaptor.hpp"
#include "ClientDatabase.hpp"
#include <iostream>
#include <vector>
#include <memory>
#include <atomic>
#include <signal.h>
#include <thread>
#include <map>
#include <mutex>
#include "define.hpp"

std::atomic<bool> g_running{true};

void signal_handler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        g_running = false;
    }
}

namespace Exchange {

class ClientManager {
public:
    ClientManager(int port, SHMRingBuffer* request_ring, std::shared_ptr<ClientDatabase> db) 
        : ws_adaptor_(std::make_shared<WSAdaptor>(port))
        , request_ring_(request_ring)
        , db_(db)
    {
        std::cout << "[ClientManager] Initializing on port " << port << std::endl;

        ws_adaptor_->set_subscribe_handler([this](WSClientPtr client, uint32_t client_id, bool is_subscribe) {
            auto lock = get_client_lock(client_id);
            std::lock_guard<std::mutex> client_guard(*lock);
            
            std::lock_guard<std::mutex> sessions_guard(sessions_mutex_);
            if (is_subscribe) {
                client_sessions_[client_id] = client;
                std::cout << "[ClientManager] Client " << client_id << " connected. Sending pending responses..." << std::endl;
                
                auto pending = db_->popPendingResponses(client_id);
                for (auto& resp : pending) {
                    client->send(resp.data.data(), resp.data.size());
                }
            } else {
                client_sessions_.erase(client_id);
                std::cout << "[ClientManager] Client " << client_id << " disconnected." << std::endl;
            }
        });

        ws_adaptor_->set_message_handler([this](WSClientPtr client, const void* data, size_t size) {
            // We need to peek at the client_id to lock the correct mutex
            flatbuffers::Verifier verifier(static_cast<const uint8_t*>(data), size);
            if (!verifier.VerifyBuffer<ClientRequest>(nullptr)) return;
            
            auto request = flatbuffers::GetRoot<ClientRequest>(data);
            uint32_t client_id = 0;
            if (request->data_type() == ClientRequestData_OrderRequest) {
                client_id = request->data_as_OrderRequest()->client_id();
            } else if (request->data_type() == ClientRequestData_PositionRequest) {
                client_id = request->data_as_PositionRequest()->client_id();
            }

            if (client_id != 0) {
                auto lock = get_client_lock(client_id);
                std::lock_guard<std::mutex> client_guard(*lock);
                this->process_client_request(client, data, size);
            } else {
                this->process_client_request(client, data, size);
            }
        });
        
        std::cout << "[ClientManager] WS Handlers registered." << std::endl;
    }

    void handle_execution_response(const OrderResponse* resp, const void* data, size_t size) {
        (void) data; (void) size;
        uint32_t client_id = resp->client_id();
        auto lock = get_client_lock(client_id);
        std::lock_guard<std::mutex> client_guard(*lock);

        // Update positions on fill
        if (resp->exec_type() == ExecType_Fill || resp->exec_type() == ExecType_PartialFill) {
            int64_t cost = static_cast<int64_t>(resp->p() * resp->q());
            if (resp->side() == Side_Buy) {
                db_->updatePosition(client_id, 0, -cost); // Pay USD
                db_->updatePosition(client_id, resp->symbol_id(), static_cast<int64_t>(resp->q())); // Get Asset
            } else {
                db_->updatePosition(client_id, 0, cost); // Get USD
                db_->updatePosition(client_id, resp->symbol_id(), -static_cast<int64_t>(resp->q())); // Give Asset
            }
        }

        flatbuffers::FlatBufferBuilder fbb(256);
        auto resp_offset = CreateOrderResponse(fbb, resp->exec_type(), resp->order_id(), resp->client_id(), resp->exec_id(), resp->symbol_id(), resp->side(), resp->p(), resp->q(), resp->reject_code());
        auto client_resp = CreateClientResponse(fbb, ClientResponseData_OrderResponse, resp_offset.Union());
        fbb.Finish(client_resp);
        
        std::lock_guard<std::mutex> sessions_guard(sessions_mutex_);
        auto it = client_sessions_.find(client_id);
        if (it != client_sessions_.end()) {
            it->second->send(fbb.GetBufferPointer(), fbb.GetSize());
        } else {
            std::cout << "[ClientManager] Client " << client_id << " offline. Storing pending response." << std::endl;
            db_->addPendingResponse(client_id, fbb.GetBufferPointer(), fbb.GetSize());
        }
    }

    void process_client_request(WSClientPtr client, const void* data, size_t size) {
        (void) size;
        auto request = flatbuffers::GetRoot<ClientRequest>(data);
        auto type = request->data_type();

        switch (type) {
            case ClientRequestData_OrderRequest: {
                auto order_req = request->data_as_OrderRequest();
                flatbuffers::FlatBufferBuilder fbb(256);
                auto or_offset = CreateOrderRequest(fbb, 
                    order_req->action(), order_req->exec_id(), order_req->order_id(), 
                    order_req->client_id(), order_req->symbol_id(), order_req->side(), 
                    order_req->type(), order_req->p(), order_req->q(), 
                    order_req->visible_qty(), order_req->timestamp());
                fbb.Finish(or_offset);
                request_ring_->enqueue(fbb.GetBufferPointer(), fbb.GetSize());
                break;
            }
            case ClientRequestData_PositionRequest: {
                auto pos_req = request->data_as_PositionRequest();
                int64_t pos = db_->getPosition(pos_req->client_id(), pos_req->symbol_id());
                
                flatbuffers::FlatBufferBuilder fbb(128);
                auto pos_resp = CreatePositionResponse(fbb, pos_req->client_id(), pos_req->symbol_id(), pos);
                auto client_resp = CreateClientResponse(fbb, ClientResponseData_PositionResponse, pos_resp.Union());
                fbb.Finish(client_resp);
                client->send(fbb.GetBufferPointer(), fbb.GetSize());
                break;
            }
            default: break;
        }
    }

private:
    std::shared_ptr<std::mutex> get_client_lock(uint32_t client_id) {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        auto& mutex_ptr = client_locks_[client_id];
        if (!mutex_ptr) {
            mutex_ptr = std::make_shared<std::mutex>();
        }
        return mutex_ptr;
    }

    std::shared_ptr<WSAdaptor> ws_adaptor_;
    SHMRingBuffer* request_ring_;
    std::shared_ptr<ClientDatabase> db_;
    std::map<uint32_t, WSClientPtr> client_sessions_;
    std::map<uint32_t, std::shared_ptr<std::mutex>> client_locks_;
    std::mutex sessions_mutex_;
};

} // namespace Exchange

int main() 
{
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    size_t ring_size = 16384;
    auto db = std::make_shared<Exchange::InMemoryClientDatabase>();

    Exchange::SHMRingBuffer* response_ring = nullptr;
    Exchange::SHMRingBuffer* request_ring = nullptr;
    try {
        response_ring = new Exchange::SHMRingBuffer(ORDER_RESPONSE, ring_size);
        request_ring = new Exchange::SHMRingBuffer(ORDER_REQUEST, ring_size);
    } catch (const std::exception& e) {
        std::cerr << "[ClientManager] FATAL: " << e.what() << std::endl;
        return -1;
    }

    Exchange::ClientManager manager(9001, request_ring, db);

    void* data_ptr = nullptr;
    size_t data_size = 0;

    while (g_running.load(std::memory_order_relaxed))
    {
        if (response_ring->dequeue(&data_ptr, &data_size))
        {
            if (data_ptr && data_size > 0) {
                auto resp = flatbuffers::GetRoot<Exchange::OrderResponse>(data_ptr);
                manager.handle_execution_response(resp, data_ptr, data_size);
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    delete response_ring;
    delete request_ring;
    return 0;
}
