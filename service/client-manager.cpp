#include "ClientManager.hpp"
#include "ring/SHMRingBuffer.hpp"
#include "ClientDatabase.hpp"
#include "ThreadUtil.hpp"
#include "AffinityConfig.hpp"
#include "SignalHandler.hpp"
#include <iostream>

int main() 
{
    setup_signals();

    auto db = std::make_shared<Exchange::InMemoryClientDatabase>();

    Exchange::SHMRingBuffer* response_ring = nullptr;
    Exchange::SHMRingBuffer* request_ring = nullptr;
    try {
        response_ring = new Exchange::SHMRingBuffer(ORDER_RESPONSE, ORDER_RESPONSE_SIZE);
        request_ring = new Exchange::SHMRingBuffer(ORDER_REQUEST, ORDER_REQUEST_SIZE);
    } catch (const std::exception& e) {
        std::cerr << "[ClientManager] FATAL: " << e.what() << std::endl;
        return -1;
    }

    Exchange::ClientManager manager(9001, request_ring, response_ring, db);

    int main_core = CM_MAIN_CORE;
    if (main_core >= 0) {
        Exchange::set_thread_affinity(main_core, "ClientManager_Main");
    }

    manager.run();

    delete response_ring;
    delete request_ring;
    return 0;
}
