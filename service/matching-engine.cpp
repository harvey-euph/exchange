#include "MatchingEngine.hpp"
#include "ExecutionReporter.hpp"
#include "ThreadUtil.hpp"
#include "AffinityConfig.hpp"
#include "SignalHandler.hpp"
#include <iostream>


int main()
{
    setup_signals();

    int main_core = ME_MAIN_CORE;
    if (main_core >= 0) {
        Exchange::set_thread_affinity(main_core, "MatchingEngineS");
    }

    // Use a small trick to clear screen initially
    std::cout << "\033[2J\033[H" << std::flush;

    std::cout << "[OrderCore] Starting matching engine..." << std::endl;

    Exchange::ClientExecutionReporter reporter(ORDER_RESPONSE);

    Exchange::OrderBook book(1, 1, 2000, 8192, &reporter);

    Exchange::SHMRingBuffer request_ring(ORDER_REQUEST, ORDER_REQUEST_SIZE);

    std::cout << "[OrderCore] Listening for requests on OrderRequest ring..." << std::endl;

    Exchange::MatchingEngine engine(&request_ring, &book);
    engine.run();

    std::cout << "[OrderCore] Shutting down..." << std::endl;
    return 0;
}
