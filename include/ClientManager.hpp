#pragma once

#include "ring/SHMRingBuffer.hpp"
#include "fbs/exchange_generated.h"
#include "mmap_log.h"
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
    ClientManager(int port, SHMRingBuffer* request_ring, mmaplog::MmapReader* response_ring, std::shared_ptr<ClientDatabase> db);

    __attribute__((noinline)) void handle_execution_response(const OrderResponseT* resp);
    __attribute__((noinline)) void process_client_request(WSClientPtr client, const void* data, size_t size);
    __attribute__((noinline)) void handle_client_logon(WSClientPtr client, const AdminRequest* admin_req);
    __attribute__((noinline)) void handle_client_logout(WSClientPtr client, const AdminRequest* admin_req);
    
    int poll_client();
    int poll_server();

private:

    std::shared_ptr<WSAdaptor> ws_adaptor_;
    SHMRingBuffer* request_ring_;
    mmaplog::MmapReader* response_ring_;
    std::shared_ptr<ClientDatabase> db_;
    std::map<uint32_t, std::vector<WSClientPtr>> client_sessions_;
};

} // namespace Exchange
