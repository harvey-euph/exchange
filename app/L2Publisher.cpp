#include "ring/SHMRingBuffer.hpp"
#include "fbs/order_generated.h"
#include "WSAdaptor.hpp"
#include <iostream>
#include <vector>
#include <memory>
#include <atomic>
#include <signal.h>
#include <thread>

std::atomic<bool> g_running{true};

void signal_handler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        g_running = false;
    }
}

int main() 
{
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    std::string ring_name = "L2_Update_Ring"; 
    size_t ring_size = 16384;

    std::cout << "[L2Publisher] Connecting to SHMRingBuffer: " << ring_name << "..." << std::endl;

    Exchange::SHMRingBuffer* ring_buffer = nullptr;
    try {
        ring_buffer = new Exchange::SHMRingBuffer(ring_name, ring_size);
    } catch (const std::exception& e) {
        std::cerr << "[L2Publisher] Failed to connect to SHMRingBuffer: " << e.what() << std::endl;
        return -1;
    }

    std::vector<std::unique_ptr<Exchange::L2OutputAdaptor>> adaptors;
    adaptors.push_back(std::make_unique<Exchange::StdoutAdaptor>());
    try {
        adaptors.push_back(std::make_unique<Exchange::WSAdaptor>(9002));
    } catch (const std::exception& e) {
        std::cerr << "[L2Publisher] Failed to start WSAdaptor: " << e.what() << std::endl;
    }

    std::cout << "[L2Publisher] Connected successfully. Start consuming..." << std::endl;

    void* data_ptr = nullptr;
    size_t data_size = 0;

    while (g_running.load(std::memory_order_relaxed))
    {
        if (ring_buffer->dequeue(&data_ptr, &data_size))
        {
            if (data_ptr == nullptr || data_size == 0) {
                continue;
            }

            auto l2_update = flatbuffers::GetRoot<Exchange::L2Update>(data_ptr);
            for (auto& adaptor : adaptors) {
                adaptor->publish(l2_update, data_ptr, data_size);
            }
        } 
        else 
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    std::cout << "[L2Publisher] Exiting and cleaning up..." << std::endl;
    delete ring_buffer;

    return 0;
}
