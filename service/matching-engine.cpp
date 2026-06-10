#include "OrderBook.hpp"
#include "ExecutionReporter.hpp"
#include "ring/SHMRingBuffer.hpp"
#include <iostream>
#include <atomic>
#include "define.hpp"
#include "SignalHandler.hpp"
#include "TimeUtil.hpp"
// #include "Telemetry.hpp"
#include <limits>
#include "ThreadUtil.hpp"
#include "AffinityConfig.hpp"
#include <cstdlib>

namespace Exchange {
extern thread_local uint64_t g_current_request_start_tsc;
}

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

    void* data_ptr = nullptr;
    size_t data_size = 0;

    std::cout << "[OrderCore] Listening for requests on OrderRequest ring..." << std::endl;

    while (g_running)
    {
        if (request_ring.dequeue(&data_ptr, &data_size))
        {
            if (!data_ptr || !data_size) continue;

            Exchange::g_current_request_start_tsc = Exchange::read_tsc_begin();

            auto req = flatbuffers::GetRoot<Exchange::OrderRequest>(data_ptr);
            book.processRequest(req);

            Exchange::g_current_request_start_tsc = 0;
        }
        else 
        {
            POLL_BACKOFF();
        }
    }

    std::cout << "[OrderCore] Shutting down..." << std::endl;
    return 0;
}
