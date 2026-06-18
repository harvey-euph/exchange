#include <iostream>
#include <vector>
#include <unistd.h>
#include <signal.h>
#include <bpf/libbpf.h>
#include <algorithm>
#include <iomanip>
#include <map>
#include <chrono>
#include <cstring>
#include "ebpf-msg-flow.skel.h"

// Arrow / Parquet headers (only used when --parquet is requested)
#ifdef USE_PARQUET
#include <arrow/api.h>
#include <arrow/io/api.h>
#include <parquet/arrow/writer.h>
#endif

static volatile bool keep_running = true;

static void sig_handler(int) {
    keep_running = false;
}

struct msg_flow_event {
    uint64_t exec_id;
    uint64_t ts0;
    uint64_t ts1;
    uint64_t ts2;
    uint64_t ts3;
    uint64_t ts4;
    uint64_t ts5;
    uint64_t ts6;
    uint64_t ts7;
    uint64_t ts8;
    uint8_t exec_type;
    uint8_t padding[7];
};

struct LatencyStats {
    std::vector<uint64_t> s01, s12, s23, s34, s45, s56, s67, s78;
    void add(const msg_flow_event* ev) {
        if (ev->ts1 >= ev->ts0) s01.push_back(ev->ts1 - ev->ts0);
        if (ev->ts2 >= ev->ts1) s12.push_back(ev->ts2 - ev->ts1);
        if (ev->ts3 >= ev->ts2) s23.push_back(ev->ts3 - ev->ts2);
        if (ev->ts4 >= ev->ts3) s34.push_back(ev->ts4 - ev->ts3);
        if (ev->ts5 >= ev->ts4) s45.push_back(ev->ts5 - ev->ts4);
        if (ev->ts6 >= ev->ts5) s56.push_back(ev->ts6 - ev->ts5);
        if (ev->ts7 >= ev->ts6) s67.push_back(ev->ts7 - ev->ts6);
        if (ev->ts8 >= ev->ts7) s78.push_back(ev->ts8 - ev->ts7);
    }
};

std::map<uint8_t, LatencyStats> stats_by_type;
LatencyStats all_stats;

static bool raw_mode    = false;
static bool parquet_mode = false;
static std::string parquet_path;
static std::vector<msg_flow_event> raw_events;

std::string get_exec_type_name(uint8_t type) {
    switch (type) {
        case 0:   return "New";
        case 100: return "Modify-Short";
        case 101: return "Modify-Long";
        case 4:   return "Cancel";
        case 8:   return "Reject";
        case 5:   return "Replaced";
        default:  return "Unknown(" + std::to_string(type) + ")";
    }
}

static int handle_event(void *ctx, void *data, size_t data_sz) {
    auto *ev = static_cast<const msg_flow_event *>(data);
    stats_by_type[ev->exec_type].add(ev);
    all_stats.add(ev);

    if (raw_mode || parquet_mode) {
        raw_events.push_back(*ev);
    }
    return 0;
}

uint64_t get_p(std::vector<uint64_t>& v, double p) {
    if (v.empty()) return 0;
    size_t idx = static_cast<size_t>(p * v.size());
    if (idx >= v.size()) idx = v.size() - 1;
    return v[idx];
}

void print_stats() {
    for (auto& pair : stats_by_type) {
        auto& st = pair.second;
        std::sort(st.s01.begin(), st.s01.end());
        std::sort(st.s12.begin(), st.s12.end());
        std::sort(st.s23.begin(), st.s23.end());
        std::sort(st.s34.begin(), st.s34.end());
        std::sort(st.s45.begin(), st.s45.end());
        std::sort(st.s56.begin(), st.s56.end());
        std::sort(st.s67.begin(), st.s67.end());
        std::sort(st.s78.begin(), st.s78.end());
    }
    std::sort(all_stats.s01.begin(), all_stats.s01.end());
    std::sort(all_stats.s12.begin(), all_stats.s12.end());
    std::sort(all_stats.s23.begin(), all_stats.s23.end());
    std::sort(all_stats.s34.begin(), all_stats.s34.end());
    std::sort(all_stats.s45.begin(), all_stats.s45.end());
    std::sort(all_stats.s56.begin(), all_stats.s56.end());
    std::sort(all_stats.s67.begin(), all_stats.s67.end());
    std::sort(all_stats.s78.begin(), all_stats.s78.end());

    std::cout << "\nStage Meanings:\n";
    std::cout << "0. tcp_recv_entry\n";
    std::cout << "1. req_entry\n";
    std::cout << "2. req_enqueue\n";
    std::cout << "3. req_dequeue\n";
    std::cout << "4. resp_enqueue\n";
    std::cout << "5. resp_dequeue\n";
    std::cout << "6. user_write\n";
    std::cout << "7. tcp_send_entry\n";
    std::cout << "8. tcp_send_ret\n\n";

    int table_width = 109;
    std::string header_equals = std::string(42, '=');
    std::cout << header_equals << " Latency Statistics (ns) " << header_equals << "\n";
    std::cout << "| " << std::left << std::setw(14) << "ExecType" << " | "
              << std::right << std::setw(7) << "Count" << " | "
              << std::setw(7) << "0-1" << " | " << std::setw(7) << "1-2" << " | "
              << std::setw(7) << "2-3" << " | " << std::setw(7) << "3-4" << " | "
              << std::setw(7) << "4-5" << " | " << std::setw(7) << "5-6" << " | "
              << std::setw(7) << "6-7" << " | " << std::setw(7) << "7-8" << " |\n";
    std::cout << std::string(table_width, '=') << "\n";

    auto print_row = [](const char* name, LatencyStats& st) {
        if (st.s01.empty()) return;
        
        std::cout << "| " << std::left << std::setw(14) << name << " | "
                  << std::right << std::setw(7) << st.s01.size() << " | ";
        
        double ps[] = {0.50, 0.90, 0.99, 0.999};

        for (int i = 0; i < 4; ++i) {
            if (i > 0) std::cout << "| " << std::string(14, ' ') << " | " << std::string(7, ' ') << " | ";
            std::cout << std::setw(7) << get_p(st.s01, ps[i]) << " | "
                      << std::setw(7) << get_p(st.s12, ps[i]) << " | "
                      << std::setw(7) << get_p(st.s23, ps[i]) << " | "
                      << std::setw(7) << get_p(st.s34, ps[i]) << " | "
                      << std::setw(7) << get_p(st.s45, ps[i]) << " | "
                      << std::setw(7) << get_p(st.s56, ps[i]) << " | "
                      << std::setw(7) << get_p(st.s67, ps[i]) << " | "
                      << std::setw(7) << get_p(st.s78, ps[i]) << " |\n";
        }
    };

    const std::vector<std::pair<uint8_t, const char*>> types = {
        {0, "New"},
        {100, "Modify-Short"},
        {101, "Modify-Long"},
        {4, "Cancel"},
        {8, "Reject"},
        {5, "Replaced"}
    };

    bool first = true;
    for (const auto& pair : types) {
        if (!stats_by_type[pair.first].s01.empty()) {
            if (!first) {
                std::cout << std::string(table_width, '-') << "\n";
            }
            print_row(pair.second, stats_by_type[pair.first]);
            first = false;
        }
    }

    if (!all_stats.s01.empty()) {
        std::cout << std::string(table_width, '=') << "\n";
        print_row("ALL", all_stats);
    }
    std::cout << std::string(table_width, '=') << "\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// Parquet output  (compiled only when -DUSE_PARQUET is present)
// ─────────────────────────────────────────────────────────────────────────────

#ifdef USE_PARQUET
static void write_parquet(const std::string& path, const std::vector<msg_flow_event>& events) {
    using namespace arrow;

    const int64_t n = static_cast<int64_t>(events.size());

    // Builders
    StringBuilder exec_type_builder;
    Int64Builder  lat01_builder, lat12_builder, lat23_builder, lat34_builder;
    Int64Builder  lat45_builder, lat56_builder, lat67_builder, lat78_builder;

    // Reserve space; ignore status – only fails on OOM
    (void)exec_type_builder.Reserve(n);
    (void)lat01_builder.Reserve(n); (void)lat12_builder.Reserve(n);
    (void)lat23_builder.Reserve(n); (void)lat34_builder.Reserve(n);
    (void)lat45_builder.Reserve(n); (void)lat56_builder.Reserve(n);
    (void)lat67_builder.Reserve(n); (void)lat78_builder.Reserve(n);

    for (const auto& ev : events) {
        (void)exec_type_builder.Append(get_exec_type_name(ev.exec_type));

        auto append_lat = [](Int64Builder& b, uint64_t a, uint64_t b2) {
            if (b2 >= a) (void)b.Append(static_cast<int64_t>(b2 - a));
            else         (void)b.AppendNull();
        };

        append_lat(lat01_builder, ev.ts0, ev.ts1);
        append_lat(lat12_builder, ev.ts1, ev.ts2);
        append_lat(lat23_builder, ev.ts2, ev.ts3);
        append_lat(lat34_builder, ev.ts3, ev.ts4);
        append_lat(lat45_builder, ev.ts4, ev.ts5);
        append_lat(lat56_builder, ev.ts5, ev.ts6);
        append_lat(lat67_builder, ev.ts6, ev.ts7);
        append_lat(lat78_builder, ev.ts7, ev.ts8);
    }

    std::shared_ptr<Array> exec_type_arr, lat01_arr, lat12_arr, lat23_arr, lat34_arr;
    std::shared_ptr<Array> lat45_arr, lat56_arr, lat67_arr, lat78_arr;

    // Finish builders; cast to void to suppress [[nodiscard]] on Status
    (void)exec_type_builder.Finish(&exec_type_arr);
    (void)lat01_builder.Finish(&lat01_arr); (void)lat12_builder.Finish(&lat12_arr);
    (void)lat23_builder.Finish(&lat23_arr); (void)lat34_builder.Finish(&lat34_arr);
    (void)lat45_builder.Finish(&lat45_arr); (void)lat56_builder.Finish(&lat56_arr);
    (void)lat67_builder.Finish(&lat67_arr); (void)lat78_builder.Finish(&lat78_arr);

    auto schema = arrow::schema({
        field("ExecType", utf8()),
        field("0-1 lat",  int64()),
        field("1-2 lat",  int64()),
        field("2-3 lat",  int64()),
        field("3-4 lat",  int64()),
        field("4-5 lat",  int64()),
        field("5-6 lat",  int64()),
        field("6-7 lat",  int64()),
        field("7-8 lat",  int64()),
    });

    auto table = Table::Make(schema, {
        exec_type_arr,
        lat01_arr, lat12_arr, lat23_arr, lat34_arr,
        lat45_arr, lat56_arr, lat67_arr, lat78_arr,
    });

    auto outfile_result = io::FileOutputStream::Open(path);
    if (!outfile_result.ok()) {
        std::cerr << "[ebpf-msg-flow] Failed to open " << path
                  << ": " << outfile_result.status().message() << "\n";
        return;
    }
    auto outfile = outfile_result.ValueOrDie();

    parquet::WriterProperties::Builder props_builder;
    props_builder.compression(parquet::Compression::ZSTD);
    props_builder.compression_level(3);
    auto props = props_builder.build();

    auto write_status = parquet::arrow::WriteTable(
        *table, arrow::default_memory_pool(), outfile, /*chunk_size=*/1 << 20, props);
    if (!write_status.ok()) {
        std::cerr << "[ebpf-msg-flow] Parquet write error: " << write_status.message() << "\n";
    }
    (void)outfile->Close();

    std::cerr << "[ebpf-msg-flow] Wrote " << n << " events to " << path << "\n";
}
#endif // USE_PARQUET


// ─────────────────────────────────────────────────────────────────────────────
// CSV output  (legacy --raw mode, kept for backward compatibility)
// ─────────────────────────────────────────────────────────────────────────────

static void write_csv(const std::vector<msg_flow_event>& events) {
    std::cout << "ExecType,0-1 lat,1-2 lat,2-3 lat,3-4 lat,4-5 lat,5-6 lat,6-7 lat,7-8 lat\n";
    for (const auto& ev : events) {
        auto lat = [](uint64_t a, uint64_t b) -> int64_t {
            return b >= a ? (int64_t)(b - a) : -1;
        };
        std::cout << get_exec_type_name(ev.exec_type) << ","
                  << lat(ev.ts0, ev.ts1) << "," << lat(ev.ts1, ev.ts2) << ","
                  << lat(ev.ts2, ev.ts3) << "," << lat(ev.ts3, ev.ts4) << ","
                  << lat(ev.ts4, ev.ts5) << "," << lat(ev.ts5, ev.ts6) << ","
                  << lat(ev.ts6, ev.ts7) << "," << lat(ev.ts7, ev.ts8) << "\n";
    }
    std::cout << std::flush;
}

int main(int argc, char **argv) {
    bool silent = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-s" || arg == "--silent") {
            silent = true;
        } else if (arg == "--raw") {
            raw_mode = true;
            silent   = true;
        } else if (arg == "--parquet") {
#ifdef USE_PARQUET
            parquet_mode = true;
            silent       = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                parquet_path = argv[++i];
            } else {
                std::cerr << "Error: --parquet requires an output file path\n";
                return 1;
            }
#else
            std::cerr << "Error: this binary was not compiled with Parquet support "
                         "(rebuild with -DUSE_PARQUET and link -larrow -lparquet)\n";
            return 1;
#endif
        }
    }

    if (parquet_mode || raw_mode) {
        raw_events.reserve(500000);
    }

    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);

    const char *cm_path = "./build/services/client-manager";
    const char *me_path = "./build/services/matching-engine";

    struct ebpf_msg_flow_bpf *skel = ebpf_msg_flow_bpf__open();
    if (!skel) {
        std::cerr << "Failed to open skeleton\n";
        return 1;
    }

    if (ebpf_msg_flow_bpf__load(skel)) {
        std::cerr << "Failed to load skeleton\n";
        return 1;
    }

    if (ebpf_msg_flow_bpf__attach(skel)) {
        std::cerr << "Failed to attach skeleton kprobes\n";
        return 1;
    }

    long pid = -1;
    
    // Attach USDT probes manually to the specific binaries
    skel->links.req_entry          = bpf_program__attach_usdt(skel->progs.req_entry,          pid, cm_path, "exchange", "req_entry",          NULL);
    skel->links.req_enqueue        = bpf_program__attach_usdt(skel->progs.req_enqueue,        pid, cm_path, "exchange", "req_enqueue",        NULL);
    skel->links.ob_req_entry       = bpf_program__attach_usdt(skel->progs.ob_req_entry,       pid, me_path, "exchange", "ob_req_entry",       NULL);
    skel->links.ob_resp_enqueue    = bpf_program__attach_usdt(skel->progs.ob_resp_enqueue,    pid, me_path, "exchange", "ob_resp_enqueue",    NULL);
    skel->links.exec_resp_entry    = bpf_program__attach_usdt(skel->progs.exec_resp_entry,    pid, cm_path, "exchange", "exec_resp_entry",    NULL);
    skel->links.exec_resp_before_db= bpf_program__attach_usdt(skel->progs.exec_resp_before_db,pid, cm_path, "exchange", "exec_resp_before_db",NULL);

    if (!skel->links.req_entry || !skel->links.req_enqueue || !skel->links.ob_req_entry || 
        !skel->links.ob_resp_enqueue || !skel->links.exec_resp_entry || !skel->links.exec_resp_before_db) {
        std::cerr << "Failed to attach some USDT probes! Check binary paths.\n";
    }

    struct ring_buffer *rb = ring_buffer__new(bpf_map__fd(skel->maps.rb), handle_event, NULL, NULL);
    if (!rb) {
        std::cerr << "Failed to create ring buffer\n";
        return 1;
    }

    if (parquet_mode) {
        std::cerr << "[ebpf-msg-flow] Parquet mode → " << parquet_path << "\n" << std::flush;
    } else if (raw_mode) {
        // Print CSV header immediately so the pipe/file receiver can see it
        std::cout << "ExecType,0-1 lat,1-2 lat,2-3 lat,3-4 lat,4-5 lat,5-6 lat,6-7 lat,7-8 lat\n" << std::flush;
    } else if (!silent) {
        std::cout << "Tracing Message Flow using BPF Skeleton... Ctrl-C to exit.\n";
    }

    auto last_print = std::chrono::steady_clock::now();
    while (keep_running) {
        ring_buffer__poll(rb, 100);
        auto now = std::chrono::steady_clock::now();
        if (!silent && std::chrono::duration_cast<std::chrono::seconds>(now - last_print).count() >= 1) {
            std::cout << "\033[2J\033[H";
            print_stats();
            last_print = now;
        }
    }

    ring_buffer__free(rb);
    ebpf_msg_flow_bpf__destroy(skel);

    if (parquet_mode) {
#ifdef USE_PARQUET
        write_parquet(parquet_path, raw_events);
#endif
    } else if (raw_mode) {
        write_csv(raw_events);
    } else {
        print_stats();
    }

    return 0;
}
