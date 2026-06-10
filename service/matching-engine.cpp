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
#include "Worker.hpp"

namespace Exchange {
extern thread_local uint64_t g_current_request_start_tsc;

class MatchingEngine : public Worker<MatchingEngine> {
public:
    MatchingEngine(SHMRingBuffer* request_ring, OrderBook* book)
        : request_ring_(request_ring)
        , book_(book)
    {}

    int poll_client() {
        return 0; // No client network polling needed for Matching Engine
    }

    int poll_server() {
        void* data_ptr = nullptr;
        size_t data_size = 0;
        if (request_ring_->dequeue(&data_ptr, &data_size)) {
            if (!data_ptr || !data_size) return 0;

            g_current_request_start_tsc = read_tsc_begin();

            auto req = flatbuffers::GetRoot<OrderRequest>(data_ptr);
            book_->processRequest(req);

            g_current_request_start_tsc = 0;
            return 1;
        }
        return 0;
    }

private:
    SHMRingBuffer* request_ring_;
    OrderBook* book_;
};

} // namespace Exchange

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
