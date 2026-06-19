#include <iostream>
#include <vector>
#include <unistd.h>
#include <signal.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <linux/perf_event.h>
#include <bpf/libbpf.h>
#include <bpf/bpf.h>
#include <algorithm>
#include <iomanip>
#include <map>
#include <chrono>
#include <cstring>
#include <fstream>
#include "lat-tracer.skel.h"

static volatile bool keep_running = true;

static void sig_handler(int) {
    keep_running = false;
}

struct stage_sample {
    uint32_t stage_id;
    uint8_t exec_type;
    uint8_t padding[3];
    uint64_t latency_ns;
    uint64_t cycles;
    uint64_t instructions;
    uint64_t llc_miss;
    uint64_t dtlb_miss;
    uint64_t branch_miss;
    uint64_t l1d_miss;
    uint64_t l1i_miss;
    uint64_t page_faults;
    uint64_t ctx_switches;
    uint64_t runqueue_delay_ns;
    uint32_t cpu_enter;
    uint32_t cpu_exit;
    uint32_t cpu_migrated;
    uint32_t padding3;
    uint64_t exec_id;
};

std::string get_exec_type_name(uint8_t type) {
    switch (type) {
        case 0: return "NEW";
        case 100: return "MODIFY-S";
        case 101: return "MODIFY-L";
        case 4: return "CANCEL";
        case 8: return "REJECT";
        case 5: return "REPLACED";
        default: return "UNKNOWN";
    }
}

// Completed message flows container
struct MsgFlow {
    uint64_t exec_id;
    std::string exec_type;
    stage_sample stages[8];
    bool stage_present[8] = {false};
};

static std::vector<MsgFlow> completed_flows;
static std::map<uint64_t, MsgFlow> pending_flows;
static uint64_t total_completed_msgs = 0;
const uint64_t TARGET_MSG_COUNT = 10000;

static long perf_event_open(struct perf_event_attr *hw_event, pid_t pid, int cpu, int group_fd, unsigned long flags) {
    return syscall(__NR_perf_event_open, hw_event, pid, cpu, group_fd, flags);
}

bool enable_perf_event_for_map(int map_fd, uint32_t type, uint64_t config, int num_cpus, std::vector<int>& fds) {
    struct perf_event_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.type = type;
    attr.size = sizeof(attr);
    attr.config = config;
    attr.disabled = 1;
    attr.inherit = 0;

    bool success = false;
    for (int cpu = 0; cpu < num_cpus; cpu++) {
        int fd = perf_event_open(&attr, -1, cpu, -1, 0);
        if (fd < 0) {
            continue;
        }
        
        if (ioctl(fd, PERF_EVENT_IOC_ENABLE, 0) < 0) {
            close(fd);
            continue;
        }

        if (bpf_map_update_elem(map_fd, &cpu, &fd, BPF_ANY) < 0) {
            close(fd);
            continue;
        }

        fds.push_back(fd);
        success = true;
    }
    return success;
}

static int handle_event(void *ctx, void *data, size_t data_sz) {
    if (data_sz < sizeof(stage_sample)) {
        return 0;
    }
    auto *s = static_cast<const stage_sample *>(data);
    
    if (s->stage_id > 7) return 0;

    auto& flow = pending_flows[s->exec_id];
    flow.exec_id = s->exec_id;
    flow.stages[s->stage_id] = *s;
    flow.stage_present[s->stage_id] = true;

    if (s->exec_type != 255) {
        flow.exec_type = get_exec_type_name(s->exec_type);
    }

    // Check if the flow is finished (we assume it's finished when stage 7 is received and we have at least stages 0 and 7)
    // To be strictly correct, we check if all 8 stages are present, or if stage 7 is present and we've gathered all we can.
    bool all_present = true;
    for (int i = 0; i < 8; ++i) {
        if (!flow.stage_present[i]) {
            all_present = false;
            break;
        }
    }

    if (all_present) {
        // If exec_type is empty, default to UNKNOWN
        if (flow.exec_type.empty()) {
            flow.exec_type = "UNKNOWN";
        }
        completed_flows.push_back(flow);
        pending_flows.erase(s->exec_id);
        total_completed_msgs++;

        if (total_completed_msgs % 500 == 0) {
            std::cout << "Collected " << total_completed_msgs << " / " << TARGET_MSG_COUNT << " flows..." << std::endl;
        }

        if (total_completed_msgs >= TARGET_MSG_COUNT) {
            keep_running = false;
        }
    }

    // Prevent leaks in pending_flows if some stages are lost
    if (pending_flows.size() > 5000) {
        // Evict the oldest flow
        auto it = pending_flows.begin();
        pending_flows.erase(it);
    }

    return 0;
}

void write_csv() {
    std::string csv_path = "log/latency_attribution.csv";
    std::ofstream out(csv_path);
    if (!out.is_open()) {
        std::cerr << "Failed to open output CSV file: " << csv_path << std::endl;
        return;
    }

    // Write header
    out << "execType,stage,latency(ns),page_faults,ctx_switches,runqueue_delay,llc_miss,l1d_miss,l1i_miss,dtlb_miss,cpu_migrated,IPC,branch_miss\n";

    for (const auto& flow : completed_flows) {
        for (int i = 0; i < 8; ++i) {
            const auto& s = flow.stages[i];
            double ipc = 0.0;
            if (s.cycles > 0) {
                ipc = (double)s.instructions / s.cycles;
            }
            out << flow.exec_type << ","
                << i << ","
                << s.latency_ns << ","
                << s.page_faults << ","
                << s.ctx_switches << ","
                << s.runqueue_delay_ns << ","
                << s.llc_miss << ","
                << s.l1d_miss << ","
                << s.l1i_miss << ","
                << s.dtlb_miss << ","
                << (s.cpu_migrated ? 1 : 0) << ","
                << std::fixed << std::setprecision(4) << ipc << ","
                << s.branch_miss << "\n";
        }
    }

    out.close();
    std::cout << "\nSuccess: Written " << completed_flows.size() << " message flows (8 stages each, "
              << completed_flows.size() * 8 << " records) to " << csv_path << std::endl;
}

int main(int argc, char **argv) {
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    int num_cpus = sysconf(_SC_NPROCESSORS_ONLN);
    std::cout << "Detecting " << num_cpus << " online CPUs..." << std::endl;

    struct lat_tracer_bpf *skel = lat_tracer_bpf__open();
    if (!skel) {
        std::cerr << "Failed to open BPF skeleton" << std::endl;
        return 1;
    }

    if (lat_tracer_bpf__load(skel)) {
        std::cerr << "Failed to load BPF skeleton" << std::endl;
        return 1;
    }

    // Set up perf event maps
    std::vector<int> perf_fds;
    
    // Cycles
    enable_perf_event_for_map(bpf_map__fd(skel->maps.map_cycles), PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES, num_cpus, perf_fds);
    // Instructions
    enable_perf_event_for_map(bpf_map__fd(skel->maps.map_instructions), PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS, num_cpus, perf_fds);
    // LLC Misses
    enable_perf_event_for_map(bpf_map__fd(skel->maps.map_llc_misses), PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_MISSES, num_cpus, perf_fds);
    // dTLB Misses
    uint64_t dtlb_config = PERF_COUNT_HW_CACHE_DTLB | (PERF_COUNT_HW_CACHE_OP_READ << 8) | (PERF_COUNT_HW_CACHE_RESULT_MISS << 16);
    enable_perf_event_for_map(bpf_map__fd(skel->maps.map_dtlb_misses), PERF_TYPE_HW_CACHE, dtlb_config, num_cpus, perf_fds);
    // Branch Misses
    enable_perf_event_for_map(bpf_map__fd(skel->maps.map_branch_misses), PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_MISSES, num_cpus, perf_fds);
    // L1D Misses
    uint64_t l1d_config = PERF_COUNT_HW_CACHE_L1D | (PERF_COUNT_HW_CACHE_OP_READ << 8) | (PERF_COUNT_HW_CACHE_RESULT_MISS << 16);
    enable_perf_event_for_map(bpf_map__fd(skel->maps.map_l1d_misses), PERF_TYPE_HW_CACHE, l1d_config, num_cpus, perf_fds);
    // L1I Misses
    uint64_t l1i_config = PERF_COUNT_HW_CACHE_L1I | (PERF_COUNT_HW_CACHE_OP_READ << 8) | (PERF_COUNT_HW_CACHE_RESULT_MISS << 16);
    enable_perf_event_for_map(bpf_map__fd(skel->maps.map_l1i_misses), PERF_TYPE_HW_CACHE, l1i_config, num_cpus, perf_fds);

    if (lat_tracer_bpf__attach(skel)) {
        std::cerr << "Failed to attach BPF skeleton" << std::endl;
        return 1;
    }

    const char *cm_path = "./build/services/client-manager";
    const char *me_path = "./build/services/matching-engine";
    long pid = -1;

    // Attach USDT probes manually to the specific binaries
    skel->links.req_entry = bpf_program__attach_usdt(skel->progs.req_entry, pid, cm_path, "exchange", "req_entry", NULL);
    skel->links.req_enqueue = bpf_program__attach_usdt(skel->progs.req_enqueue, pid, cm_path, "exchange", "req_enqueue", NULL);
    skel->links.ob_req_entry = bpf_program__attach_usdt(skel->progs.ob_req_entry, pid, me_path, "exchange", "ob_req_entry", NULL);
    skel->links.ob_resp_enqueue = bpf_program__attach_usdt(skel->progs.ob_resp_enqueue, pid, me_path, "exchange", "ob_resp_enqueue", NULL);
    skel->links.exec_resp_entry = bpf_program__attach_usdt(skel->progs.exec_resp_entry, pid, cm_path, "exchange", "exec_resp_entry", NULL);
    skel->links.exec_resp_before_db = bpf_program__attach_usdt(skel->progs.exec_resp_before_db, pid, cm_path, "exchange", "exec_resp_before_db", NULL);

    if (!skel->links.req_entry || !skel->links.req_enqueue || !skel->links.ob_req_entry || 
        !skel->links.ob_resp_enqueue || !skel->links.exec_resp_entry || !skel->links.exec_resp_before_db) {
        std::cerr << "Failed to attach some USDT probes! Check binary paths." << std::endl;
    }

    struct ring_buffer *rb = ring_buffer__new(bpf_map__fd(skel->maps.rb), handle_event, NULL, NULL);
    if (!rb) {
        std::cerr << "Failed to create ring buffer" << std::endl;
        return 1;
    }

    std::cout << "Monitoring stages. Will record " << TARGET_MSG_COUNT << " message flows and output to log/latency_attribution.csv. Press Ctrl-C to force quit..." << std::endl;

    while (keep_running) {
        ring_buffer__poll(rb, 100);
    }

    std::cout << "Cleaning up..." << std::endl;
    write_csv();

    ring_buffer__free(rb);
    for (int fd : perf_fds) {
        close(fd);
    }
    lat_tracer_bpf__destroy(skel);
    return 0;
}
