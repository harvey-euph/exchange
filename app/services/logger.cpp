#include "ring/SHMRingBuffer.hpp"
#include "define.hpp"
#include "SignalHandler.hpp"
#include "ThreadUtil.hpp"
#include "AffinityConfig.hpp"
#include <iostream>
#include <fstream>
#include <filesystem>
#include <string>
#include <thread>
#include <chrono>

int main() {
    setup_signals();

    int main_core = LOGGER_MAIN_CORE;
    if (main_core >= 0) {
        Exchange::set_thread_affinity(main_core, "LoggerService");
    }

    std::filesystem::path p("log/service.log");
    if (p.has_parent_path() && !std::filesystem::exists(p.parent_path())) {
        std::filesystem::create_directories(p.parent_path());
    }
    
    std::ofstream file("log/service.log", std::ios::app);
    if (!file.is_open()) {
        std::cerr << "Failed to open log file" << std::endl;
        return -1;
    }

    std::vector<std::string> ring_names = {
        LOG_RING_CM, LOG_RING_ME, LOG_RING_L2, LOG_RING_L3, LOG_RING_HTTP, LOG_RING_PD
    };

    std::vector<Exchange::SHMRingBuffer*> rings;
    for (const auto& name : ring_names) {
        std::cout << "[Logger] Connecting to SHMRingBuffer: " << name << std::endl;
        try {
            rings.push_back(new Exchange::SHMRingBuffer(name, LOG_RING_SIZE));
        } catch (const std::exception& e) {
            std::cerr << "[Logger] Failed to connect to " << name << ": " << e.what() << std::endl;
        }
    }

    if (rings.empty()) {
        std::cerr << "[Logger] No ring buffers connected, exiting." << std::endl;
        return -1;
    }

    void* data_ptr = nullptr;
    size_t data_size = 0;

    while (g_running) {
        bool processed_any = false;
        for (auto ring : rings) {
            while (ring->dequeue(&data_ptr, &data_size)) {
                if (data_size > 0 && data_ptr != nullptr) {
                    file.write(static_cast<const char*>(data_ptr), data_size);
                    file.put('\n');
                }
                processed_any = true;
            }
        }
        if (processed_any) {
            file.flush();
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    std::cout << "[Logger] Shutting down..." << std::endl;
    for (auto ring : rings) {
        delete ring;
    }
    return 0;
}
