#include "L3Publisher.hpp"
#include "ThreadUtil.hpp"
#include "AffinityConfig.hpp"
#include "SignalHandler.hpp"
#include <iostream>

int main() 
{
    setup_signals();

    std::cout << "[L3Publisher] Connecting to SHMRingBuffer: " << L3_UPDATE_RING << "..." << std::endl;

    Exchange::SHMRingBuffer* ring_buffer = nullptr;
    try {
        ring_buffer = new Exchange::SHMRingBuffer(L3_UPDATE_RING, L3_UPDATE_RING_SIZE);
    } catch (const std::exception& e) {
        std::cerr << "[L3Publisher] Failed to connect to SHMRingBuffer: " << e.what() << std::endl;
        return -1;
    }

    int main_core = L3_MAIN_CORE;
    if (main_core >= 0) {
        Exchange::set_thread_affinity(main_core, "L3Publisher");
    }

    std::cout << "[L3Publisher] Connected successfully. Start consuming..." << std::endl;

    Exchange::L3Publisher publisher(PORT_L3_PUBLISHER, ring_buffer);
    publisher.run();

    std::cout << "[L3Publisher] Exiting and cleaning up..." << std::endl;
    delete ring_buffer;

    return 0;
}
