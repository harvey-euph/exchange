#include "OrderBook.hpp"
#include "ExecutionReporter.hpp"
#include "ring/SHMRingBuffer.hpp"
#include <iostream>
#include <atomic>
#include "define.hpp"
#include "SignalHandler.hpp"
#include "TimeUtil.hpp"
#include "Telemetry.hpp"
#include <limits>

int main()
{
    setup_signals();

    // Use a small trick to clear screen initially
    std::cout << "\033[2J\033[H" << std::flush;

    std::cout << "[OrderCore] Starting matching engine..." << std::endl;

    Exchange::ClientExecutionReporter reporter(ORDER_RESPONSE);
    Exchange::TelemetryProvider telemetry(EXCHANGE_TELEMETRY, false);

    Exchange::OrderBook book(1, 1, 2000, 8192, &reporter);

    Exchange::SHMRingBuffer request_ring(ORDER_REQUEST, ORDER_REQUEST_SIZE);

    void* data_ptr = nullptr;
    size_t data_size = 0;

    std::cout << "[OrderCore] Listening for requests on OrderRequest ring..." << std::endl;

    while (g_running)
    {
        if (request_ring.dequeue(&data_ptr, &data_size))
        {
            if (data_ptr && data_size > 0)
            {
                uint64_t start = Exchange::read_tsc_begin();

                auto req = flatbuffers::GetRoot<Exchange::OrderRequest>(data_ptr);
                book.processRequest(req);

                uint64_t diff = Exchange::read_tsc_end() - start;
                
                telemetry.data()->core_count.fetch_add(1, std::memory_order_relaxed);
                telemetry.data()->core_cycles_sum.fetch_add(diff, std::memory_order_relaxed);
            }
        }
        else 
        {
            POLL_BACKOFF();
        }
    }

    std::cout << "[OrderCore] Shutting down..." << std::endl;
    return 0;
}
