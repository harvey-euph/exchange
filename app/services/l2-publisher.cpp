#include "L2Publisher.hpp"
#include "ThreadUtil.hpp"
#include "AffinityConfig.hpp"
#include "SignalHandler.hpp"
#include "AsyncLogger.hpp"
#include <iostream>

int main() 
{
    setup_signals();
    Exchange::AsyncLogger::get().init(LOG_RING_L2);

    std::cout << "[L2Publisher] Connecting to SHMRingBuffer: " << L2_UPDATE_RING << "..." << std::endl;

    Exchange::SHMRingBuffer* ring_buffer = nullptr;
    try {
        ring_buffer = new Exchange::SHMRingBuffer(L2_UPDATE_RING, L2_UPDATE_RING_SIZE);
    } catch (const std::exception& e) {
        std::cerr << "[L2Publisher] Failed to connect to SHMRingBuffer: " << e.what() << std::endl;
        return -1;
    }

    int main_core = L2_MAIN_CORE;
    if (main_core >= 0) {
        Exchange::set_thread_affinity(main_core, "L2Publisher");
    }

    std::cout << "[L2Publisher] Connected successfully. Start consuming..." << std::endl;

    Exchange::L2Publisher publisher(9002, ring_buffer);
    publisher.run();

    std::cout << "[L2Publisher] Exiting and cleaning up..." << std::endl;
    delete ring_buffer;

    return 0;
}
