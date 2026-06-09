#include "ring/SHMRingBuffer.hpp"
#include "define.hpp"
#include "Telemetry.hpp"
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
        L2_UPDATE_RING,
        L3_UPDATE_RING
    };

    std::vector<RingInfo> rings;
    for (const auto& name : ring_names) {
        rings.push_back({name, nullptr});
    }

    // Calibrate TSC Frequency
    double tsc_hz = 0.0;
    {
        auto start_time = std::chrono::steady_clock::now();
        uint64_t start_tsc = Exchange::read_tsc_begin();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        uint64_t end_tsc = Exchange::read_tsc_end();
        auto end_time = std::chrono::steady_clock::now();
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count();
        if (ns > 0) {
            tsc_hz = static_cast<double>(end_tsc - start_tsc) / (static_cast<double>(ns) / 1e9);
        }
    }

    std::cout << "[RingMonitor] Starting ring buffer monitoring loop (500ms)..." << std::endl;

    std::unique_ptr<Exchange::TelemetryProvider> telemetry;
    bool telemetry_init = false;
    uint64_t last_core_count = 0;
    uint64_t last_mgmt_count = 0;
    uint64_t last_e2e_count = 0;
    auto last_tps_time = std::chrono::steady_clock::now();
    double core_tps = 0.0;
    double mgmt_tps = 0.0;
    double e2e_tps = 0.0;

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

        // 嘗試連接 Telemetry
        if (!telemetry) {
            try {
                telemetry = std::make_unique<Exchange::TelemetryProvider>(EXCHANGE_TELEMETRY, true);
            } catch (...) {
                // 尚未建立，默默跳過
            }
        }

        if (telemetry && !telemetry_init) {
            last_core_count = telemetry->data()->core_count.load(std::memory_order_relaxed);
            last_mgmt_count = telemetry->data()->mgmt_count.load(std::memory_order_relaxed);
            last_e2e_count = telemetry->data()->e2e_count.load(std::memory_order_relaxed);
            last_tps_time = std::chrono::steady_clock::now();
            telemetry_init = true;
        }

        if (telemetry && telemetry_init) {
            auto now = std::chrono::steady_clock::now();
            double elapsed_sec = std::chrono::duration_cast<std::chrono::duration<double>>(now - last_tps_time).count();
            if (elapsed_sec >= 0.2) {
                uint64_t cur_core = telemetry->data()->core_count.load(std::memory_order_relaxed);
                uint64_t cur_mgmt = telemetry->data()->mgmt_count.load(std::memory_order_relaxed);
                uint64_t cur_e2e = telemetry->data()->e2e_count.load(std::memory_order_relaxed);

                core_tps = static_cast<double>(cur_core - last_core_count) / elapsed_sec;
                mgmt_tps = static_cast<double>(cur_mgmt - last_mgmt_count) / elapsed_sec;
                e2e_tps = static_cast<double>(cur_e2e - last_e2e_count) / elapsed_sec;

                last_core_count = cur_core;
                last_mgmt_count = cur_mgmt;
                last_e2e_count = cur_e2e;
                last_tps_time = now;
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

        std::cout << "\n";
        std::cout << "================================================================================\n";
        std::cout << "                      EXCHANGE LATENCY & PERFORMANCE MONITOR                    \n";
        std::cout << "================================================================================\n";
        std::cout << "Telemetry Status: " << (telemetry ? "ONLINE" : "OFFLINE") << "    ";
        if (tsc_hz > 0.0) {
            std::cout << "TSC Frequency: " << std::fixed << std::setprecision(2) << (tsc_hz / 1e9) << " GHz\n";
        } else {
            std::cout << "TSC Frequency: UNKNOWN\n";
        }
        std::cout << "--------------------------------------------------------------------------------\n";

        if (telemetry) {
            uint64_t core_cnt = telemetry->data()->core_count.load(std::memory_order_relaxed);
            uint64_t core_cyc = telemetry->data()->core_cycles_sum.load(std::memory_order_relaxed);
            uint64_t mgmt_cnt = telemetry->data()->mgmt_count.load(std::memory_order_relaxed);
            uint64_t mgmt_cyc = telemetry->data()->mgmt_cycles_sum.load(std::memory_order_relaxed);
            uint64_t e2e_cnt  = telemetry->data()->e2e_count.load(std::memory_order_relaxed);
            uint64_t e2e_cyc  = telemetry->data()->e2e_cycles_sum.load(std::memory_order_relaxed);

            double core_avg_cyc = (core_cnt > 0) ? (static_cast<double>(core_cyc) / core_cnt) : 0.0;
            double mgmt_avg_cyc = (mgmt_cnt > 0) ? (static_cast<double>(mgmt_cyc) / mgmt_cnt) : 0.0;
            double e2e_avg_cyc  = (e2e_cnt > 0)  ? (static_cast<double>(e2e_cyc)  / e2e_cnt)  : 0.0;

            double ring_avg_cyc = e2e_avg_cyc - core_avg_cyc - mgmt_avg_cyc;
            if (ring_avg_cyc < 0.0) ring_avg_cyc = 0.0;

            auto print_metric = [&](const std::string& label, uint64_t cnt, double avg_cyc, bool show_cnt) {
                double avg_us = (tsc_hz > 0.0) ? (avg_cyc / tsc_hz * 1e6) : 0.0;
                std::cout << std::left << std::setw(20) << label;
                if (show_cnt) {
                    std::cout << std::setw(15) << cnt;
                } else {
                    std::cout << std::setw(15) << "-";
                }
                std::cout << std::fixed << std::setprecision(1) << std::setw(25) << avg_cyc;
                if (tsc_hz > 0.0) {
                    std::cout << std::fixed << std::setprecision(3) << avg_us << " us\n";
                } else {
                    std::cout << "-\n";
                }
            };

            std::cout << std::left << std::setw(20) << "METRIC"
                      << std::setw(15) << "COUNT"
                      << std::setw(25) << "AVG LATENCY (cycles)"
                      << "AVG LATENCY (us)\n";
            std::cout << "--------------------------------------------------------------------------------\n";

            print_metric("Core Process", core_cnt, core_avg_cyc, true);
            print_metric("Client Manager", mgmt_cnt, mgmt_avg_cyc, true);
            print_metric("Total E2E", e2e_cnt, e2e_avg_cyc, true);
            print_metric("Stay in RingBuf", 0, ring_avg_cyc, false);

            std::cout << "--------------------------------------------------------------------------------\n";
            std::cout << "TPS (Transactions Per Second):\n";
            std::cout << "  Core Matching TPS:   " << std::fixed << std::setprecision(2) << core_tps << "\n";
            std::cout << "  Client Manager TPS:  " << std::fixed << std::setprecision(2) << mgmt_tps << "\n";
            std::cout << "  Total E2E TPS:       " << std::fixed << std::setprecision(2) << e2e_tps << "\n";
        } else {
            std::cout << " Waiting for Telemetry Provider to start...\n";
        }
        std::cout << "================================================================================\n";

        std::cout << " Press [Ctrl+C] to exit.\n";

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    std::cout << "[RingMonitor] Exiting..." << std::endl;
    return 0;
}
