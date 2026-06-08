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

int main() {
    setup_signals();

    // Use a small trick to clear screen initially
    std::cout << "\033[2J\033[H" << std::flush;

    std::cout << "[OrderCore] Starting matching engine..." << std::endl;

    Exchange::ClientExecutionReporter reporter(ORDER_RESPONSE);
    Exchange::TelemetryProvider telemetry("EXCHANGE_TELEMETRY", false);

    Exchange::OrderBook book(1, 1, 2000, 8192, &reporter);

    Exchange::SHMRingBuffer request_ring(ORDER_REQUEST, 16384);

    void* data_ptr = nullptr;
    size_t data_size = 0;

    uint64_t total_cycles = 0;
    uint64_t count = 0;
    uint64_t min_cycles = std::numeric_limits<uint64_t>::max();
    uint64_t max_cycles = 0;
    const uint64_t warm_up = 100;
    const uint64_t report_interval = 10000;

    std::cout << "[OrderCore] Listening for requests on OrderRequest ring..." << std::endl;

    while (g_running)
    {
        if (request_ring.dequeue(&data_ptr, &data_size))
        {
            if (data_ptr && data_size > 0) {
                auto req = flatbuffers::GetRoot<Exchange::OrderRequest>(data_ptr);
                
                uint64_t start = Exchange::read_tsc_begin();
                book.processRequest(req);
                uint64_t end = Exchange::read_tsc_end();
                
                uint64_t diff = end - start;
                
                // 記錄至共享記憶體指標中
                telemetry.data()->core_count.fetch_add(1, std::memory_order_relaxed);
                telemetry.data()->core_cycles_sum.fetch_add(diff, std::memory_order_relaxed);
                
                if (count >= warm_up) {
                    total_cycles += diff;
                    min_cycles = std::min(min_cycles, diff);
                    max_cycles = std::max(max_cycles, diff);
                    
                    if ((count - warm_up + 1) % report_interval == 0) {
                        uint64_t avg = total_cycles / (count - warm_up + 1);
                        std::cout << "[Latency] Processed=" << (count - warm_up + 1)
                                  << " Avg=" << avg 
                                  << " Min=" << min_cycles 
                                  << " Max=" << max_cycles << " (cycles)" << std::endl;
                    }
                }
                count++;
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
