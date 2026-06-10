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

struct event_data {
    uint64_t sock_ptr;
    uint64_t timestamp_ns;
    uint32_t len;
    uint8_t event_type; // 0: RX (recv), 1: TX (send)
    uint8_t padding[3];  // Explicit padding to align to 8 bytes boundary
    uint8_t payload[512];
};

static volatile bool keep_running = true;

static void sig_handler(int signo) {
    keep_running = false;
}

static std::unordered_map<uint64_t, uint64_t> pending_requests;

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
        // Format each sub-value to 7 characters with 2 decimal places to ensure `/` align perfectly
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

static void print_usage(const char* prog) {
    std::cout << "Usage: " << prog << " [options]\n"
              << "Options:\n"
              << "  -p <port>          Specify TCP port to monitor directly (default: 9001)\n"
              << "  -h, --help         Show this help message\n";
}

static void handle_rx_event(event_data *ev, size_t safe_len) {
    const uint8_t* ptr = ev->payload;
    size_t remaining = safe_len;
    
    while (remaining > 2) {
        uint8_t opcode = ptr[0] & 0x0F;
        uint8_t mask = (ptr[1] & 0x80) >> 7;
        uint8_t payload_len_field = ptr[1] & 0x7F;
        
        if (opcode != 0x2) {
            break;
        }
        
        size_t header_len = 2;
        uint64_t actual_payload_len = payload_len_field;
        
        if (payload_len_field == 126) {
            if (remaining < 4) break;
            actual_payload_len = (ptr[2] << 8) | ptr[3];
            header_len = 4;
        } else if (payload_len_field == 127) {
            if (remaining < 10) break;
            actual_payload_len = 0;
            for (int i = 0; i < 8; ++i) {
                actual_payload_len = (actual_payload_len << 8) | ptr[2 + i];
            }
            header_len = 10;
        }
        
        size_t frame_total_len = header_len + (mask ? 4 : 0) + actual_payload_len;
        size_t payload_offset = header_len + (mask ? 4 : 0);
        
        if (remaining <= payload_offset) {
            if (frame_total_len == 0 || frame_total_len >= remaining) break;
            ptr += frame_total_len;
            remaining -= frame_total_len;
            continue;
        }
        
        size_t avail_payload_len = std::min(remaining - payload_offset, (size_t)actual_payload_len);
        std::vector<uint8_t> decoded(avail_payload_len);
        if (mask == 1) {
            uint8_t masking_key[4];
            for (int i = 0; i < 4; ++i) {
                masking_key[i] = ptr[header_len + i];
            }
            for (size_t i = 0; i < avail_payload_len; ++i) {
                decoded[i] = ptr[payload_offset + i] ^ masking_key[i % 4];
            }
        } else {
            for (size_t i = 0; i < avail_payload_len; ++i) {
                decoded[i] = ptr[payload_offset + i];
            }
        }
        
        [&]() {
            if (decoded.size() <= sizeof(flatbuffers::uoffset_t)) return;
            
            flatbuffers::Verifier verifier(decoded.data(), decoded.size());
            if (!verifier.VerifyBuffer<Exchange::ClientRequest>(nullptr)) return;
            
            auto req = flatbuffers::GetRoot<Exchange::ClientRequest>(decoded.data());
            if (!req || req->data_type() != Exchange::ClientRequestData_OrderRequest) return;
            
            auto order_req = req->data_as_OrderRequest();
            if (!order_req) return;
            
            uint64_t exec_id = order_req->exec_id();
            pending_requests[exec_id] = ev->timestamp_ns;
        }();
        
        if (frame_total_len == 0 || frame_total_len >= remaining) {
            break;
        }
        ptr += frame_total_len;
        remaining -= frame_total_len;
    }
}

static void handle_tx_event(event_data *ev, size_t safe_len) {
    const uint8_t* ptr = ev->payload;
    size_t remaining = safe_len;
    
    while (remaining > 2) {
        uint8_t opcode = ptr[0] & 0x0F;
        uint8_t mask = (ptr[1] & 0x80) >> 7;
        uint8_t payload_len_field = ptr[1] & 0x7F;
        
        if (opcode != 0x2) {
            break;
        }
        
        size_t header_len = 2;
        uint64_t actual_payload_len = payload_len_field;
        
        if (payload_len_field == 126) {
            if (remaining < 4) break;
            actual_payload_len = (ptr[2] << 8) | ptr[3];
            header_len = 4;
        } else if (payload_len_field == 127) {
            if (remaining < 10) break;
            actual_payload_len = 0;
            for (int i = 0; i < 8; ++i) {
                actual_payload_len = (actual_payload_len << 8) | ptr[2 + i];
            }
            header_len = 10;
        }
        
        size_t frame_total_len = header_len + (mask ? 4 : 0) + actual_payload_len;
        size_t payload_offset = header_len + (mask ? 4 : 0);
        
        if (remaining <= payload_offset) {
            if (frame_total_len == 0 || frame_total_len >= remaining) break;
            ptr += frame_total_len;
            remaining -= frame_total_len;
            continue;
        }
        
        size_t avail_payload_len = std::min(remaining - payload_offset, (size_t)actual_payload_len);
        std::vector<uint8_t> decoded(avail_payload_len);
        if (mask == 1) {
            uint8_t masking_key[4];
            for (int i = 0; i < 4; ++i) {
                masking_key[i] = ptr[header_len + i];
            }
            for (size_t i = 0; i < avail_payload_len; ++i) {
                decoded[i] = ptr[payload_offset + i] ^ masking_key[i % 4];
            }
        } else {
            for (size_t i = 0; i < avail_payload_len; ++i) {
                decoded[i] = ptr[payload_offset + i];
            }
        }
        
        [&]() {
            if (decoded.size() <= sizeof(flatbuffers::uoffset_t)) return;
            
            flatbuffers::Verifier verifier(decoded.data(), decoded.size());
            if (!verifier.VerifyBuffer<Exchange::ClientResponse>(nullptr)) return;
            
            auto resp = flatbuffers::GetRoot<Exchange::ClientResponse>(decoded.data());
            if (!resp || resp->data_type() != Exchange::ClientResponseData_OrderResponse) return;
            
            auto order_resp = resp->data_as_OrderResponse();
            if (!order_resp) return;
            
            Exchange::ExecType exec_type = order_resp->exec_type();
            if ((~Exchange::EXEC_MASK_LATENCY_TRACK >> exec_type) & 1) return;

            uint64_t exec_id = order_resp->exec_id();
            
            auto it = pending_requests.find(exec_id);
            if (it == pending_requests.end()) return;
            
            uint64_t latency_ns = ev->timestamp_ns - it->second;
            pending_requests.erase(it);
            
            uint64_t engine_lat = order_resp->engine_latency();
            uint64_t manager_lat = order_resp->manager_latency();

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
        }();
        
        if (frame_total_len == 0 || frame_total_len >= remaining) {
            break;
        }
        ptr += frame_total_len;
        remaining -= frame_total_len;
    }
}

static int handle_event(void *ctx, void *data, size_t data_sz) {
    if (data_sz < sizeof(event_data)) {
        std::cout << "[eBPF Monitor] ERROR: Received event with size " << data_sz 
                  << " which is smaller than expected " << sizeof(event_data) << std::endl;
        return 0;
    }
    auto *ev = static_cast<event_data*>(data);

    if (ev->event_type != 0 && ev->event_type != 1) {
        return 0;
    }

    size_t safe_len = std::min((size_t)ev->len, sizeof(ev->payload));

    if (ev->event_type == 0) {
        handle_rx_event(ev, safe_len);
    } else {
        handle_tx_event(ev, safe_len);
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

    std::cout << "[Latency Tracer] Started. Monitoring TCP port " << selected_port << "...\n";
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
    
    std::cout << "\n[Latency Tracer] Final Latency Summary:\n";
    last_printed_lines = 0;
    print_stats_table();
    
    std::cout << "[Latency Tracer] Stopped.\n";
    return 0;
}
