#include "MarketDataServer.hpp"
#include "SignalHandler.hpp"
#include "ThreadUtil.hpp"
#include "AffinityConfig.hpp"
#include <iostream>

using namespace Exchange;

int main() {
    setup_signals();
    
    int main_core = MD_CORE;
    if (main_core >= 0) {
        set_thread_affinity(main_core, "MarketDataServer");
    }

    std::cout << "[MarketDataServer] Connecting to SHMRingBuffer: " << MARKET_DATA_RING << " (size: " << MARKET_DATA_RING_SIZE << ")..." << std::endl;
    SHMRingBuffer* ring_buffer = nullptr;
    try {
        ring_buffer = new SHMRingBuffer(MARKET_DATA_RING, MARKET_DATA_RING_SIZE);
    } catch (const std::exception& e) {
        std::cerr << "[MarketDataServer] FATAL: " << e.what() << std::endl;
        return -1;
    }

    std::cout << "[MarketDataServer] Polling ring buffer and WebSocket events..." << std::endl;
    MarketDataServer server(PORT_MARKET_DATA_SERVER, ring_buffer);
    server.run();

    delete ring_buffer;
    return 0;
}
