#include "ring/SHMRingBuffer.hpp"
#include "define.hpp"
// #include "Telemetry.hpp"
#include "TimeUtil.hpp"
#include <iostream>
#include <thread>
#include <chrono>
#include <vector>
#include <memory>
#include <iomanip>
#include <csignal>
#include <atomic>
#include "SignalHandler.hpp"

struct RingInfo {
    std::string name;
    std::unique_ptr<Exchange::SHMObserver> observer;
};

int main()
{
    setup_signals();

    std::vector<std::string> ring_names = {
        ORDER_REQUEST,
        ORDER_RESPONSE,
        MARKET_DATA_RING
    };

    std::vector<RingInfo> rings;
    for (const auto& name : ring_names) {
        rings.push_back({name, nullptr});
    }

    std::cout << "[RingMonitor] Starting ring buffer monitoring loop (500ms)..." << std::endl;

    while (g_running) {
        // 嘗試連接處於離線狀態的 Ring Buffer
        for (auto& ring : rings) {
            if (!ring.observer) {
                try {
                    // 使用 SHMObserver (SHMRingBufferImpl<true>)
                    ring.observer = std::make_unique<Exchange::SHMObserver>(ring.name, 0);
                } catch (...) {
                    // 共享記憶體尚未建立或初始化中，默默跳過
                }
            }
        }

        std::cout << "\033[2J\033[H" << std::flush;
        std::cout << "================================================================================\n";
        std::cout << "                      EXCHANGE SHM RING BUFFER MONITOR                          \n";
        std::cout << "================================================================================\n";
        std::cout << std::left << std::setw(20) << "RING NAME" 
                  << std::setw(12) << "STATUS" 
                  << std::setw(15) << "CAPACITY(B)" 
                  << std::setw(15) << "COMMITTED(B)" 
                  << std::setw(17) << "UNCOMMITTED(B)" 
                  << "OCCUPANCY\n";
        std::cout << "--------------------------------------------------------------------------------\n";

        for (const auto& ring : rings) {
            std::cout << std::left << std::setw(20) << ring.name;
            if (ring.observer) {
                try {
                    uint64_t cap = ring.observer->get_capacity();
                    uint64_t reserved = ring.observer->get_reserved_depth();
                    uint64_t uncommitted = ring.observer->get_uncommitted_depth();
                    uint64_t committed = (reserved >= uncommitted) ? (reserved - uncommitted) : 0;
                    double ratio = ring.observer->get_occupancy_ratio() * 100.0;

                    // 產生 ASCII 進度條
                    int bar_width = 15;
                    int pos = static_cast<int>(bar_width * (ratio / 100.0));
                    if (pos > bar_width) pos = bar_width;
                    if (pos < 0) pos = 0;
                    
                    std::string bar = "[";
                    for (int i = 0; i < bar_width; ++i) {
                        if (i < pos) bar += "#";
                        else bar += " ";
                    }
                    bar += "]";

                    std::cout << std::setw(12) << "ONLINE"
                              << std::setw(15) << cap
                              << std::setw(15) << committed
                              << std::setw(17) << uncommitted
                              << std::fixed << std::setprecision(2) << ratio << "% " << bar << "\n";
                } catch (const std::exception& e) {
                    std::cout << std::setw(12) << "ERROR"
                              << std::setw(15) << "-"
                              << std::setw(15) << "-"
                              << std::setw(15) << "-"
                              << "-\n";
                }
            } else {
                std::cout << std::setw(12) << "OFFLINE"
                          << std::setw(15) << "-"
                          << std::setw(15) << "-"
                          << std::setw(15) << "-"
                          << "-\n";
            }
        }
        std::cout << "================================================================================\n";

        std::cout << " Press [Ctrl+C] to exit.\n";

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    std::cout << "[RingMonitor] Exiting..." << std::endl;
    return 0;
}
