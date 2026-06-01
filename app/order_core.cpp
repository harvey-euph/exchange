#include "OrderBook.hpp"
#include "ExecutionReporter.hpp"
#include "ring/SHMRingBuffer.hpp"
#include <iostream>
#include <chrono>
#include <thread>
#include <signal.h>
#include <atomic>
#include "define.hpp"

std::atomic<bool> g_running{true};

void signal_handler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        g_running = false;
    }
}

int main() {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Use a small trick to clear screen initially
    std::cout << "\033[2J\033[H" << std::flush;

    std::cout << "[OrderCore] Starting matching engine..." << std::endl;

    Exchange::ClientExecutionReporter reporter(ORDER_RESPONSE);

    Exchange::OrderBook book(1, 1, 2000, 8192, &reporter);

    Exchange::SHMRingBuffer request_ring(ORDER_REQUEST, 16384);

    void* data_ptr = nullptr;
    size_t data_size = 0;

    std::cout << "[OrderCore] Listening for requests on OrderRequest ring..." << std::endl;

    while (g_running)
    {
        if (request_ring.dequeue(&data_ptr, &data_size))
        {
            if (data_ptr && data_size > 0) {
                auto req = flatbuffers::GetRoot<Exchange::OrderRequest>(data_ptr);
                // std::cout << "[OrderCore] Dequeued Request: exec_id=" << req->exec_id() << std::endl;
                book.processRequest(req);
                // book.showL2();
            }
        }
        else 
        {
            // for dev env
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    std::cout << "[OrderCore] Shutting down..." << std::endl;
    return 0;
}
