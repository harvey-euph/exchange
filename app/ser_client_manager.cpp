#include "ring/SHMRingBuffer.hpp"
#include "fbs/order_generated.h"
#include "WSAdaptor.hpp"
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
    ClientManager(int port, SHMRingBuffer* request_ring) 
        : ws_adaptor_(std::make_shared<WSAdaptor>(port))
        , request_ring_(request_ring) 
    {
        std::cout << "[ClientManager] Initializing on port " << port << std::endl;

        ws_adaptor_->set_subscribe_handler([this](WSClientPtr client, uint32_t client_id, bool is_subscribe) {
            std::lock_guard<std::mutex> lock(sessions_mutex_);
            if (is_subscribe) {
                client_sessions_[client_id] = client;
                std::cout << "[ClientManager] Client " << client_id << " connected and subscribed. Total active sessions: " << client_sessions_.size() << std::endl;
            } else {
                client_sessions_.erase(client_id);
                std::cout << "[ClientManager] Client " << client_id << " unsubscribed. Remaining sessions: " << client_sessions_.size() << std::endl;
            }
        });

        ws_adaptor_->set_message_handler([this](WSClientPtr client, const void* data, size_t size) {
            this->process_client_request(client, data, size);
        });
        
        std::cout << "[ClientManager] WS Handlers registered." << std::endl;
    }

    void handle_execution_response(const OrderResponse* resp, const void* data, size_t size) {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        auto it = client_sessions_.find(resp->client_id());
        if (it != client_sessions_.end()) {
            std::cout << "[ClientManager] Routing Execution Response to Client " << resp->client_id() 
                      << " | OrderID: " << resp->order_id() << " | Type: " << EnumNameExecType(resp->exec_type()) << std::endl;
            
            flatbuffers::FlatBufferBuilder fbb(size + 64);
            auto resp_offset = CreateOrderResponse(fbb, resp->exec_type(), resp->order_id(), resp->client_id(), resp->exec_id(), resp->symbol_id(), resp->p(), resp->q(), resp->reject_code());
            auto client_resp = CreateClientResponse(fbb, ClientResponseData_OrderResponse, resp_offset.Union());
            fbb.Finish(client_resp);
            
            it->second->send(fbb.GetBufferPointer(), fbb.GetSize());
        } else {
            std::cout << "[ClientManager] WARNING: Dropping execution for client " << resp->client_id() << " - No active WS session found." << std::endl;
        }
    }

    void process_client_request(WSClientPtr client, const void* data, size_t size) {
        if (size < 4) {
            std::cerr << "[ClientManager] Received malformed binary message (too small: " << size << " bytes)" << std::endl;
            return;
        }

        flatbuffers::Verifier verifier(static_cast<const uint8_t*>(data), size);
        if (!verifier.VerifyBuffer<ClientRequest>(nullptr)) {
            std::cerr << "[ClientManager] Received invalid ClientRequest FlatBuffer." << std::endl;
            return;
        }

        auto request = flatbuffers::GetRoot<ClientRequest>(data);
        auto type = request->data_type();

        std::cout << "[ClientManager] Received Request: " << EnumNameClientRequestData(type) << " (" << size << " bytes)" << std::endl;

        switch (type) {
            case ClientRequestData_OrderRequest: {
                auto order_req = request->data_as_OrderRequest();
                std::cout << "[ClientManager] -> OrderRequest: Client=" << order_req->client_id() 
                          << " Action=" << EnumNameOrderAction(order_req->action()) 
                          << " ID=" << order_req->order_id() << " ExecID=" << order_req->exec_id() << std::endl;
                
                flatbuffers::FlatBufferBuilder fbb(256);
                auto or_offset = CreateOrderRequest(fbb, 
                    order_req->action(), order_req->exec_id(), order_req->order_id(), 
                    order_req->client_id(), order_req->symbol_id(), order_req->side(), 
                    order_req->type(), order_req->p(), order_req->q(), 
                    order_req->visible_qty(), order_req->timestamp());
                fbb.Finish(or_offset);
                
                if (request_ring_->enqueue(fbb.GetBufferPointer(), fbb.GetSize())) {
                    std::cout << "[ClientManager] Enqueued OrderRequest to ORDER_REQUEST ring." << std::endl;
                } else {
                    std::cerr << "[ClientManager] ERROR: Failed to enqueue OrderRequest - Ring Buffer Full?" << std::endl;
                }
                break;
            }
            case ClientRequestData_PositionRequest: {
                auto pos_req = request->data_as_PositionRequest();
                if (pos_req->symbol_id() == 0) {
                    std::cout << "[ClientManager] -> Cash inquiry for client " << pos_req->client_id() << std::endl;
                    flatbuffers::FlatBufferBuilder fbb(128);
                    auto pos_resp = CreatePositionResponse(fbb, pos_req->client_id(), 0, 1000000); // Dummy cash 1M
                    auto client_resp = CreateClientResponse(fbb, ClientResponseData_PositionResponse, pos_resp.Union());
                    fbb.Finish(client_resp);
                    client->send(fbb.GetBufferPointer(), fbb.GetSize());
                } else {
                    std::cout << "[ClientManager] -> Position inquiry for client " << pos_req->client_id() << " symbol " << pos_req->symbol_id() << std::endl;
                    flatbuffers::FlatBufferBuilder fbb(128);
                    auto pos_resp = CreatePositionResponse(fbb, pos_req->client_id(), pos_req->symbol_id(), 1000); // Dummy position 1000
                    auto client_resp = CreateClientResponse(fbb, ClientResponseData_PositionResponse, pos_resp.Union());
                    fbb.Finish(client_resp);
                    client->send(fbb.GetBufferPointer(), fbb.GetSize());
                }
                break;
            }
            default:
                std::cout << "[ClientManager] Unknown request type received." << std::endl;
                break;
        }
    }

private:
    std::shared_ptr<WSAdaptor> ws_adaptor_;
    SHMRingBuffer* request_ring_;
    std::map<uint32_t, WSClientPtr> client_sessions_;
    std::mutex sessions_mutex_;
};

} // namespace Exchange

int main() 
{
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    size_t ring_size = 16384;

    std::cout << "[ClientManager] Connecting to RingBuffers..." << std::endl;

    Exchange::SHMRingBuffer* response_ring = nullptr;
    Exchange::SHMRingBuffer* request_ring = nullptr;
    try {
        response_ring = new Exchange::SHMRingBuffer(ORDER_RESPONSE, ring_size);
        request_ring = new Exchange::SHMRingBuffer(ORDER_REQUEST, ring_size);
        std::cout << "[ClientManager] RingBuffers connected: " << ORDER_RESPONSE << ", " << ORDER_REQUEST << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "[ClientManager] FATAL ERROR: Failed to connect to RingBuffers: " << e.what() << std::endl;
        return -1;
    }

    Exchange::ClientManager manager(9001, request_ring);

    std::cout << "[ClientManager] Service Ready. Polling " << ORDER_RESPONSE << "..." << std::endl;

    void* data_ptr = nullptr;
    size_t data_size = 0;

    while (g_running.load(std::memory_order_relaxed))
    {
        if (response_ring->dequeue(&data_ptr, &data_size))
        {
            if (data_ptr == nullptr || data_size == 0) {
                continue;
            }

            flatbuffers::Verifier verifier(static_cast<const uint8_t*>(data_ptr), data_size);
            if (verifier.VerifyBuffer<Exchange::OrderResponse>(nullptr)) {
                auto resp = flatbuffers::GetRoot<Exchange::OrderResponse>(data_ptr);
                std::cout << "[ClientManager] Dequeued execution response for client " << resp->client_id() << std::endl;
                manager.handle_execution_response(resp, data_ptr, data_size);
            } else {
                std::cerr << "[ClientManager] ERROR: Failed to verify OrderResponse from RingBuffer (Size: " << data_size << ")" << std::endl;
            }
        } 
        else 
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    std::cout << "[ClientManager] Shutting down and cleaning up..." << std::endl;
    delete response_ring;
    delete request_ring;

    return 0;
}
