#include <iostream>
#include <vector>
#include <iomanip>
#include <unistd.h>
#include <signal.h>
#include <getopt.h>
#include <string>
#include <algorithm>
#include <limits>
#include <unordered_map>
#include <chrono>
#include <thread>
#include <bpf/libbpf.h>
#include "lat-tracer.skel.h"
#include "fbs/order_generated.h"
#include "define.hpp"
#include "TimeUtil.hpp"

struct latency_event {
    uint64_t exec_id;
    uint64_t latency_ns;
    uint64_t engine_latency;
    uint64_t manager_latency;
    uint8_t exec_type;
    uint8_t padding[7]; // align to 8-byte boundary
};

static volatile bool keep_running = true;

static void sig_handler(int signo) {
    keep_running = false;
}

struct LatencyStats {
    std::vector<uint64_t> samples;
    uint64_t max_val = 0;
    uint64_t total_count = 0;
    
    void add(uint64_t val) {
        samples.push_back(val);
        total_count++;
        if (val > max_val) {
            max_val = val;
        }
    }
};

struct LatencyRow {
    LatencyStats kernel;
    LatencyStats manager;
    LatencyStats engine;
};

static std::unordered_map<Exchange::ExecType, LatencyRow> stats_by_type;
static LatencyRow global_stats;

static int last_printed_lines = 0;
static double tsc_hz = 0.0;

static std::string get_row_label(Exchange::ExecType type) {
    if (type == Exchange::ExecType_New) return "New";
    if (type == Exchange::ExecType_Replaced) return "Modify";
    if (type == Exchange::ExecType_Cancelled) return "Cancel";
    return "Unknown";
}

static void print_stats_table() {
    bool has_any = false;
    for (const auto& pair : stats_by_type) {
        if (pair.second.kernel.total_count > 0) {
            has_any = true;
            break;
        }
    }
    if (!has_any) return;

    if (last_printed_lines > 0) {
        std::cout << "\033[" << last_printed_lines << "A\033[J";
    }

    int printed_lines = 0;
    
    double tsc_factor = (tsc_hz > 0.0) ? (1.0 / tsc_hz * 1e6) : 0.0;
    double ns_factor = 1.0 / 1000.0;

    auto format_stats = [](LatencyStats& s, double factor) -> std::string {
        if (s.total_count == 0) {
            return "   -   /   -   /   -   /   -   ";
        }
        double p50 = 0.0, p90 = 0.0, p99 = 0.0, max_val = 0.0;
        if (!s.samples.empty()) {
            std::sort(s.samples.begin(), s.samples.end());
            size_t n = s.samples.size();
            p50 = s.samples[n * 0.50] * factor;
            p90 = s.samples[n * 0.90] * factor;
            p99 = s.samples[n * 0.99] * factor;
            max_val = s.max_val * factor;
        }
        char buf[128];
        std::snprintf(buf, sizeof(buf), "%7.2f/%7.2f/%7.2f/%7.2f", p50, p90, p99, max_val);
        return std::string(buf);
    };

    std::cout << "=================================================== Latency Statistics (us) ===================================================\n";
    printed_lines++;
    std::cout << std::left << std::setw(15) << "Exec Type"
              << std::right << std::setw(12) << "Total Count"
              << std::right << std::setw(32) << "kernel - manager"
              << std::right << std::setw(32) << "manager - engine"
              << std::right << std::setw(32) << "engine" << "\n";
    printed_lines++;
    std::cout << "-------------------------------------------------------------------------------------------------------------------------------\n";
    printed_lines++;

    auto print_row = [&](const std::string& label, LatencyRow& r) {
        std::cout << std::left << std::setw(15) << label
                  << std::right << std::setw(12) << r.kernel.total_count
                  << std::right << std::setw(32) << format_stats(r.kernel, ns_factor)
                  << std::right << std::setw(32) << format_stats(r.manager, tsc_factor)
                  << std::right << std::setw(32) << format_stats(r.engine, tsc_factor) << "\n";
        printed_lines++;
    };

    std::vector<Exchange::ExecType> types_order = {
        Exchange::ExecType_New,
        Exchange::ExecType_Replaced,
        Exchange::ExecType_Cancelled
    };

    for (auto type : types_order) {
        auto it = stats_by_type.find(type);
        if (it != stats_by_type.end()) {
            print_row(get_row_label(type), it->second);
        }
    }

    std::cout << "-------------------------------------------------------------------------------------------------------------------------------\n";
    printed_lines++;
    print_row("ALL", global_stats);
    std::cout << "===============================================================================================================================\n" << std::flush;
    printed_lines++;

    last_printed_lines = printed_lines;
}

static int handle_event(void *ctx, void *data, size_t data_sz) {
    if (data_sz < sizeof(latency_event)) {
        std::cout << "[eBPF Monitor] ERROR: Received event with size " << data_sz 
                  << " which is smaller than expected " << sizeof(latency_event) << std::endl;
        return 0;
    }
    auto *ev = static_cast<latency_event*>(data);

    Exchange::ExecType exec_type = static_cast<Exchange::ExecType>(ev->exec_type);
    if ((~Exchange::EXEC_MASK_LATENCY_TRACK >> exec_type) & 1) return 0;

    uint64_t latency_ns = ev->latency_ns;
    uint64_t engine_lat = ev->engine_latency;
    uint64_t manager_lat = ev->manager_latency;

    if (manager_lat > 0 && engine_lat > 0) {
        uint64_t engine_val = engine_lat;
        uint64_t manager_minus_engine_val = (manager_lat > engine_lat) ? (manager_lat - engine_lat) : 0;
        
        uint64_t manager_ns = (tsc_hz > 0.0) ? static_cast<uint64_t>(static_cast<double>(manager_lat) / tsc_hz * 1e9) : 0;
        uint64_t kernel_minus_manager_val = (latency_ns > manager_ns) ? (latency_ns - manager_ns) : 0;

        auto& row = stats_by_type[exec_type];
        row.kernel.add(kernel_minus_manager_val);
        row.manager.add(manager_minus_engine_val);
        row.engine.add(engine_val);

        global_stats.kernel.add(kernel_minus_manager_val);
        global_stats.manager.add(manager_minus_engine_val);
        global_stats.engine.add(engine_val);
    }

    return 0;
}

int main(int argc, char *argv[]) {
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    uint16_t selected_port = 9001;

    // Calibrate TSC Frequency
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

    struct lat_tracer_bpf *skel = lat_tracer_bpf__open();
    if (!skel) {
        std::cerr << "Failed to open BPF skeleton\n";
        return 1;
    }

    skel->rodata->target_port = selected_port;

    int err = lat_tracer_bpf__load(skel);
    if (err) {
        std::cerr << "Failed to load BPF skeleton\n";
        lat_tracer_bpf__destroy(skel);
        return 1;
    }

    err = lat_tracer_bpf__attach(skel);
    if (err) {
        std::cerr << "Failed to attach BPF skeleton\n";
        lat_tracer_bpf__destroy(skel);
        return 1;
    }

    struct ring_buffer *ring_buf = ring_buffer__new(bpf_map__fd(skel->maps.rb), handle_event, nullptr, nullptr);
    if (!ring_buf) {
        std::cerr << "Failed to create ring buffer manager\n";
        lat_tracer_bpf__destroy(skel);
        return 1;
    }

    std::cout << "[Latency Tracer (Kernel-Matched)] Started. Monitoring TCP port " << selected_port << "...\n";
    std::cout << "TSC Frequency: " << std::fixed << std::setprecision(2) << (tsc_hz / 1e9) << " GHz\n";
    std::cout << "Press Ctrl+C to exit.\n\n";

    auto last_print = std::chrono::steady_clock::now();
    while (keep_running) {
        err = ring_buffer__poll(ring_buf, 100);
        if (err < 0 && err != -EINTR) {
            std::cerr << "Error polling ring buffer\n";
            break;
        }
        
        auto now = std::chrono::steady_clock::now();
        if (now - last_print >= std::chrono::seconds(1)) {
            print_stats_table();
            last_print = now;
        }
    }

    ring_buffer__free(ring_buf);
    lat_tracer_bpf__destroy(skel);
    
    std::cout << "\n[Latency Tracer (Kernel-Matched)] Final Latency Summary:\n";
    last_printed_lines = 0;
    print_stats_table();
    
    std::cout << "[Latency Tracer (Kernel-Matched)] Stopped.\n";
    return 0;
}
