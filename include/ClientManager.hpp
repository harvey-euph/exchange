#pragma once

#include "ring/SHMRingBuffer.hpp"
#include "fbs/order_generated.h"
#include "WSAdaptor.hpp"
#include "ClientDatabase.hpp"
#include "Worker.hpp"
#include <memory>
#include <map>
#include <set>
#include <mutex>
#include <vector>
#include <unordered_map>

namespace Exchange {

class ClientManager : public Worker<ClientManager> {
public:
    ClientManager(int port, SHMRingBuffer* request_ring, SHMRingBuffer* response_ring, std::shared_ptr<ClientDatabase> db);

    void handle_execution_response(const OrderResponse* resp, const void* data, size_t size);
    void process_client_request(WSClientPtr client, const void* data, size_t size);
    
    int poll_client();
    int poll_server();

private:
    std::shared_ptr<std::mutex> get_client_lock(uint32_t client_id);

    std::shared_ptr<WSAdaptor> ws_adaptor_;
    SHMRingBuffer* request_ring_;
    SHMRingBuffer* response_ring_;
    std::shared_ptr<ClientDatabase> db_;
    std::map<uint32_t, std::vector<WSClientPtr>> client_sessions_;
    std::map<uint32_t, std::shared_ptr<std::mutex>> client_locks_;
    std::set<WSClientPtr> ready_sessions_;
    std::mutex sessions_mutex_;
    std::mutex ready_mutex_;
    std::unordered_map<uint64_t, uint64_t> order_start_times_;
};

} // namespace Exchange
