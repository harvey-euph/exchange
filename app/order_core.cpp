#include "OrderBook.hpp"
#include "HTTPReporter.hpp"
#include "ring/SHMRingBuffer.hpp"
#include <iostream>
#include <chrono>
#include <thread>
#include <signal.h>
#include <atomic>

std::atomic<bool> g_running{true};

void signal_handler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        g_running = false;
    }
}

int main() {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    std::cout << "[OrderCore] Starting matching engine..." << std::endl;

    Exchange::HTTPReporter reporter("OrderResponse", 16384);
    
    // min_step=10000, price_offset=2000
    Exchange::OrderBook book(10000, 2000, 65536, &reporter);

    Exchange::SHMRingBuffer request_ring("OrderRequest", 16384);

    void* data_ptr = nullptr;
    size_t data_size = 0;

    std::cout << "[OrderCore] Listening for requests on OrderRequest ring..." << std::endl;

    while (g_running) {
        if (request_ring.dequeue(&data_ptr, &data_size)) {
            if (data_ptr && data_size > 0) {
                auto req = flatbuffers::GetRoot<Exchange::OrderRequest>(data_ptr);
                std::cout << "[OrderCore] Dequeued Request: exec_id=" << req->exec_id() << std::endl;
                book.processRequest(req);
            }
        } else {
            std::this_thread::sleep_for(std::chrono::microseconds(10));
        }
    }

    std::cout << "[OrderCore] Shutting down..." << std::endl;
    return 0;
}
