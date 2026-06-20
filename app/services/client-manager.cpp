#include "ClientManager.hpp"
#include "ring/SHMRingBuffer.hpp"
#include "ClientDatabase.hpp"
#include "DbUtil.hpp"
#include "ThreadUtil.hpp"
#include "AffinityConfig.hpp"
#include "SignalHandler.hpp"
#include <iostream>
#include "mmap_log.h"

int main() 
{
    setup_signals();

#ifdef USE_PGSQL
    auto db = std::make_shared<Exchange::PostgresClientDatabase>(Exchange::DbUtil::getConnectionString());
#else
    auto db = std::make_shared<Exchange::InMemoryClientDatabase>();
#endif

    mmaplog::MmapReader* response_ring = nullptr;
    Exchange::SHMRingBuffer* request_ring = nullptr;
    try {
        response_ring = new mmaplog::MmapReader("./execution_journals");
        request_ring = new Exchange::SHMRingBuffer(ORDER_REQUEST, ORDER_REQUEST_SIZE);
    } catch (const std::exception& e) {
        std::cerr << "[ClientManager] FATAL: " << e.what() << std::endl;
        return -1;
    }

    Exchange::ClientManager manager(PORT_CLIENT_MANAGER, request_ring, response_ring, db);

    int main_core = CM_CORE;
    if (main_core >= 0) {
        Exchange::set_thread_affinity(main_core, "ClientManager");
    }

    manager.run();

    delete response_ring;
    delete request_ring;
    return 0;
}
