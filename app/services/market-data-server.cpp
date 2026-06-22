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

    std::cout << "[MarketDataServer] Connecting to Response Ring..." << std::endl;
    mmaplog::MmapReader* response_ring = nullptr;
    try {
        response_ring = new mmaplog::MmapReader("./log/execution-journals");
    } catch (const std::exception& e) {
        std::cerr << "[MarketDataServer] FATAL: " << e.what() << std::endl;
        return -1;
    }

    std::cout << "[MarketDataServer] Polling response ring and WebSocket events..." << std::endl;
    MarketDataServer server(PORT_MARKET_DATA_SERVER, response_ring);
    server.run();

    delete response_ring;
    return 0;
}
