#include <iostream>
#include <vector>
#include <unistd.h>
#include <signal.h>
#include <bpf/libbpf.h>
#include "ebpf-msg-flow.skel.h"

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
    uint8_t exec_type;
    uint8_t padding[7];
};

static int handle_event(void *ctx, void *data, size_t data_sz) {
    auto *ev = static_cast<const msg_flow_event *>(data);
    const char *type_str = "UNKNOWN";
    if (ev->exec_type == 0) type_str = "NEW";
    else if (ev->exec_type == 4) type_str = "CANCEL";
    else if (ev->exec_type == 8) type_str = "REJECT";
    else if (ev->exec_type == 100) type_str = "MOD Short";
    else if (ev->exec_type == 101) type_str = "MOD Long";
    else if (ev->exec_type == 5) type_str = "REPLACED";

    printf("\n--- Exec ID: %lu (%s) ---\n", ev->exec_id, type_str);
    printf("0. tcp_recv_entry:        %lu ns\n", ev->ts0);
    printf("1. req_entry:             %lu ns (+%lu ns)\n", ev->ts1, ev->ts1 - ev->ts0);
    printf("2. req_enqueue:           %lu ns (+%lu ns)\n", ev->ts2, ev->ts2 - ev->ts1);
    printf("3. ob_req_entry:          %lu ns (+%lu ns)\n", ev->ts3, ev->ts3 - ev->ts2);
    printf("4. ob_resp_enqueue:       %lu ns (+%lu ns)\n", ev->ts4, ev->ts4 - ev->ts3);
    printf("5. exec_resp_entry:       %lu ns (+%lu ns)\n", ev->ts5, ev->ts5 - ev->ts4);
    printf("6. exec_resp_before_db:   %lu ns (+%lu ns)\n", ev->ts6, ev->ts6 - ev->ts5);
    printf("7. tcp_send_ret:          %lu ns (+%lu ns)\n", ev->ts7, ev->ts7 - ev->ts6);
    printf("Total Latency (0->7):     %lu ns\n", ev->ts7 - ev->ts0);
    return 0;
}

int main(int, char **) {
    signal(SIGINT, sig_handler);
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
    skel->links.req_entry = bpf_program__attach_usdt(skel->progs.req_entry, pid, cm_path, "exchange", "req_entry", NULL);
    skel->links.req_enqueue = bpf_program__attach_usdt(skel->progs.req_enqueue, pid, cm_path, "exchange", "req_enqueue", NULL);
    skel->links.ob_req_entry = bpf_program__attach_usdt(skel->progs.ob_req_entry, pid, me_path, "exchange", "ob_req_entry", NULL);
    skel->links.ob_resp_enqueue = bpf_program__attach_usdt(skel->progs.ob_resp_enqueue, pid, me_path, "exchange", "ob_resp_enqueue", NULL);
    skel->links.exec_resp_entry = bpf_program__attach_usdt(skel->progs.exec_resp_entry, pid, cm_path, "exchange", "exec_resp_entry", NULL);
    skel->links.exec_resp_before_db = bpf_program__attach_usdt(skel->progs.exec_resp_before_db, pid, cm_path, "exchange", "exec_resp_before_db", NULL);

    if (!skel->links.req_entry || !skel->links.req_enqueue || !skel->links.ob_req_entry || 
        !skel->links.ob_resp_enqueue || !skel->links.exec_resp_entry || !skel->links.exec_resp_before_db) {
        std::cerr << "Failed to attach some USDT probes! Check binary paths.\n";
    }

    struct ring_buffer *rb = ring_buffer__new(bpf_map__fd(skel->maps.rb), handle_event, NULL, NULL);
    if (!rb) {
        std::cerr << "Failed to create ring buffer\n";
        return 1;
    }

    std::cout << "Tracing Message Flow using BPF Skeleton... Ctrl-C to exit.\n";
    while (keep_running) {
        ring_buffer__poll(rb, 100);
    }

    ring_buffer__free(rb);
    ebpf_msg_flow_bpf__destroy(skel);
    return 0;
}
