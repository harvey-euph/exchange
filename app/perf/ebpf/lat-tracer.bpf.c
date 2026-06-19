#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-declarations"
#include "vmlinux.h"
#pragma clang diagnostic pop
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>
#include "ebpf_common.h"

char LICENSE[] SEC("license") = "GPL";

const volatile uint16_t target_port = 9001;

struct active_stage {
    uint32_t stage_id;
    uint32_t padding;
    uint64_t start_ts;
    uint64_t cycles0;
    uint64_t instructions0;
    uint64_t llc_miss0;
    uint64_t dtlb_miss0;
    uint64_t branch_miss0;
    uint64_t l1d_miss0;
    uint64_t l1i_miss0;
    uint32_t cpu_enter;
    uint32_t padding2;
    uint64_t page_faults;
    uint64_t ctx_switches;
    uint64_t runqueue_delay_ns;
};

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

// Maps for PMU Counters (PERF_EVENT_ARRAY)
struct {
    __uint(type, BPF_MAP_TYPE_PERF_EVENT_ARRAY);
    __uint(key_size, sizeof(uint32_t));
    __uint(value_size, sizeof(uint32_t));
} map_cycles SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_PERF_EVENT_ARRAY);
    __uint(key_size, sizeof(uint32_t));
    __uint(value_size, sizeof(uint32_t));
} map_instructions SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_PERF_EVENT_ARRAY);
    __uint(key_size, sizeof(uint32_t));
    __uint(value_size, sizeof(uint32_t));
} map_llc_misses SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_PERF_EVENT_ARRAY);
    __uint(key_size, sizeof(uint32_t));
    __uint(value_size, sizeof(uint32_t));
} map_dtlb_misses SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_PERF_EVENT_ARRAY);
    __uint(key_size, sizeof(uint32_t));
    __uint(value_size, sizeof(uint32_t));
} map_branch_misses SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_PERF_EVENT_ARRAY);
    __uint(key_size, sizeof(uint32_t));
    __uint(value_size, sizeof(uint32_t));
} map_l1d_misses SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_PERF_EVENT_ARRAY);
    __uint(key_size, sizeof(uint32_t));
    __uint(value_size, sizeof(uint32_t));
} map_l1i_misses SEC(".maps");

// Tracking maps
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 65536);
    __type(key, uint64_t); // exec_id
    __type(value, struct active_stage);
} active_stages SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 1024);
    __type(key, uint32_t); // tid
    __type(value, uint64_t); // exec_id
} active_exec_ids SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 1024);
    __type(key, uint32_t); // tid
    __type(value, uint64_t); // wakeup timestamp
} wake_timestamps SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(max_entries, 1);
    __type(key, uint32_t);
    __type(value, uint8_t[512]);
} scratch_map SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 1024);
    __type(key, uint32_t); // tid
    __type(value, struct recv_ctx);
} recv_ctx_map SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 1024);
    __type(key, uint32_t); // tid
    __type(value, uint64_t); // exec_id
} active_tx_exec_id SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 16777216); // 16MB
} rb SEC(".maps");

static __always_inline uint64_t read_perf_counter(void *map) {
    long val = bpf_perf_event_read(map, bpf_get_smp_processor_id());
    if (val < 0) {
        return 0;
    }
    return (uint64_t)val;
}

static __always_inline void start_stage(uint64_t exec_id, uint32_t stage_id, uint32_t tid, uint64_t now) {
    struct active_stage stage = {};
    stage.stage_id = stage_id;
    stage.start_ts = now;
    stage.cpu_enter = bpf_get_smp_processor_id();
    stage.cycles0 = read_perf_counter(&map_cycles);
    stage.instructions0 = read_perf_counter(&map_instructions);
    stage.llc_miss0 = read_perf_counter(&map_llc_misses);
    stage.dtlb_miss0 = read_perf_counter(&map_dtlb_misses);
    stage.branch_miss0 = read_perf_counter(&map_branch_misses);
    stage.l1d_miss0 = read_perf_counter(&map_l1d_misses);
    stage.l1i_miss0 = read_perf_counter(&map_l1i_misses);
    stage.page_faults = 0;
    stage.ctx_switches = 0;
    stage.runqueue_delay_ns = 0;

    bpf_map_update_elem(&active_stages, &exec_id, &stage, BPF_ANY);
    if (tid != 0) {
        bpf_map_update_elem(&active_exec_ids, &tid, &exec_id, BPF_ANY);
    }
}

static __always_inline void end_stage_with_type(uint64_t exec_id, uint32_t tid, uint64_t now, uint8_t exec_type) {
    struct active_stage *stage = bpf_map_lookup_elem(&active_stages, &exec_id);
    if (!stage) {
        return;
    }

    struct stage_sample *sample = bpf_ringbuf_reserve(&rb, sizeof(*sample), 0);
    if (sample) {
        sample->exec_id = exec_id;
        sample->stage_id = stage->stage_id;
        sample->exec_type = exec_type;
        sample->latency_ns = (now >= stage->start_ts) ? (now - stage->start_ts) : 0;
        sample->cpu_enter = stage->cpu_enter;
        sample->cpu_exit = bpf_get_smp_processor_id();
        sample->cpu_migrated = (sample->cpu_enter != sample->cpu_exit);
        
        if (stage->stage_id == 2 || stage->stage_id == 4) {
            sample->cycles = 0;
            sample->instructions = 0;
            sample->llc_miss = 0;
            sample->dtlb_miss = 0;
            sample->branch_miss = 0;
            sample->l1d_miss = 0;
            sample->l1i_miss = 0;
            sample->page_faults = 0;
            sample->ctx_switches = 0;
            sample->runqueue_delay_ns = 0;
        } else {
            uint64_t cycles1 = read_perf_counter(&map_cycles);
            uint64_t inst1 = read_perf_counter(&map_instructions);
            uint64_t llc1 = read_perf_counter(&map_llc_misses);
            uint64_t dtlb1 = read_perf_counter(&map_dtlb_misses);
            uint64_t br1 = read_perf_counter(&map_branch_misses);
            uint64_t l1d1 = read_perf_counter(&map_l1d_misses);
            uint64_t l1i1 = read_perf_counter(&map_l1i_misses);

            sample->cycles = (cycles1 >= stage->cycles0) ? (cycles1 - stage->cycles0) : 0;
            sample->instructions = (inst1 >= stage->instructions0) ? (inst1 - stage->instructions0) : 0;
            sample->llc_miss = (llc1 >= stage->llc_miss0) ? (llc1 - stage->llc_miss0) : 0;
            sample->dtlb_miss = (dtlb1 >= stage->dtlb_miss0) ? (dtlb1 - stage->dtlb_miss0) : 0;
            sample->branch_miss = (br1 >= stage->branch_miss0) ? (br1 - stage->branch_miss0) : 0;
            sample->l1d_miss = (l1d1 >= stage->l1d_miss0) ? (l1d1 - stage->l1d_miss0) : 0;
            sample->l1i_miss = (l1i1 >= stage->l1i_miss0) ? (l1i1 - stage->l1i_miss0) : 0;

            sample->page_faults = stage->page_faults;
            sample->ctx_switches = stage->ctx_switches;
            sample->runqueue_delay_ns = stage->runqueue_delay_ns;
        }

        bpf_ringbuf_submit(sample, 0);
    }

    bpf_map_delete_elem(&active_stages, &exec_id);
    if (tid != 0) {
        bpf_map_delete_elem(&active_exec_ids, &tid);
    }
}

static __always_inline void end_stage(uint64_t exec_id, uint32_t tid, uint64_t now) {
    end_stage_with_type(exec_id, tid, now, 255);
}

SEC("kprobe/tcp_recvmsg")
int BPF_KPROBE(tcp_recvmsg, struct sock *sk, struct msghdr *msg)
{
    uint16_t sport = BPF_CORE_READ(sk, __sk_common.skc_num);
    if (target_port != 0 && sport != target_port) return 0;

    uint32_t tid = bpf_get_current_pid_tgid();
    struct recv_ctx rctx = {};
    rctx.sk = sk;
    rctx.iter_type = BPF_CORE_READ(msg, msg_iter.iter_type);
    rctx.iov_offset = BPF_CORE_READ(msg, msg_iter.iov_offset);
    rctx.ubuf = BPF_CORE_READ(msg, msg_iter.ubuf);
    rctx.iov = BPF_CORE_READ(msg, msg_iter.__iov);
    rctx.nr_segs = BPF_CORE_READ(msg, msg_iter.nr_segs);
    rctx.ts0 = bpf_ktime_get_ns();

    bpf_map_update_elem(&recv_ctx_map, &tid, &rctx, BPF_ANY);
    return 0;
}

SEC("kretprobe/tcp_recvmsg")
int BPF_KRETPROBE(tcp_recvmsg_ret, int ret)
{
    uint32_t tid = bpf_get_current_pid_tgid();
    struct recv_ctx *rctx = bpf_map_lookup_elem(&recv_ctx_map, &tid);
    if (!rctx) return 0;

    uint8_t iter_type = rctx->iter_type;
    size_t iov_offset = rctx->iov_offset;
    void *ubuf = rctx->ubuf;
    const struct iovec *iov = rctx->iov;
    uint32_t nr_segs = rctx->nr_segs;
    uint64_t timestamp_ns = rctx->ts0;

    bpf_map_delete_elem(&recv_ctx_map, &tid);

    if (ret <= 0) return 0;

    uint32_t zero = 0;
    uint8_t *payload = bpf_map_lookup_elem(&scratch_map, &zero);
    if (!payload) return 0;

    copy_iov_iter(payload, iter_type, iov_offset, ubuf, iov, nr_segs);

    uint64_t exec_id = 0;
    if (parse_rx_exec_id(payload, ret, &exec_id)) {
        start_stage(exec_id, 0, tid, timestamp_ns);
    }
    return 0;
}

SEC("kprobe/tcp_sendmsg")
int BPF_KPROBE(tcp_sendmsg, struct sock *sk, struct msghdr *msg, size_t size)
{
    uint16_t sport = BPF_CORE_READ(sk, __sk_common.skc_num);
    if (target_port != 0 && sport != target_port)
        return 0;

    uint32_t zero = 0;
    uint8_t *payload = bpf_map_lookup_elem(&scratch_map, &zero);
    if (!payload) return 0;

    uint64_t timestamp_ns = bpf_ktime_get_ns();

    uint8_t iter_type = BPF_CORE_READ(msg, msg_iter.iter_type);
    size_t iov_offset = BPF_CORE_READ(msg, msg_iter.iov_offset);
    void *ubuf = BPF_CORE_READ(msg, msg_iter.ubuf);
    const struct iovec *iov = BPF_CORE_READ(msg, msg_iter.__iov);
    uint32_t nr_segs = BPF_CORE_READ(msg, msg_iter.nr_segs);

    copy_iov_iter(payload, iter_type, iov_offset, ubuf, iov, nr_segs);

    uint64_t exec_id = 0;
    uint8_t exec_type = 0;
    if (parse_tx_exec_id(payload, size, &exec_id, &exec_type)) {
        uint32_t tid = bpf_get_current_pid_tgid();
        bpf_map_update_elem(&active_tx_exec_id, &tid, &exec_id, BPF_ANY);
        
        uint8_t reported_exec_type = exec_type;
        if (exec_type == 5) { // ExecType_Replaced
            if (exec_id % 2 == 0) reported_exec_type = 100; // ModifyShort
            else reported_exec_type = 101; // ModifyLong
        }

        end_stage_with_type(exec_id, tid, timestamp_ns, reported_exec_type);
        start_stage(exec_id, 7, tid, timestamp_ns);
    }
    return 0;
}

SEC("kretprobe/tcp_sendmsg")
int BPF_KRETPROBE(tcp_sendmsg_ret, int ret)
{
    uint32_t tid = bpf_get_current_pid_tgid();
    uint64_t *exec_id_ptr = bpf_map_lookup_elem(&active_tx_exec_id, &tid);
    if (!exec_id_ptr) return 0;

    uint64_t exec_id = *exec_id_ptr;
    bpf_map_delete_elem(&active_tx_exec_id, &tid);

    uint64_t now = bpf_ktime_get_ns();
    end_stage(exec_id, tid, now);
    return 0;
}

#include <bpf/usdt.bpf.h>

SEC("usdt")
int req_entry(struct pt_regs *ctx) {
    uint32_t tid = bpf_get_current_pid_tgid();
    uint64_t *exec_id_ptr = bpf_map_lookup_elem(&active_exec_ids, &tid);
    if (exec_id_ptr) {
        uint64_t exec_id = *exec_id_ptr;
        uint64_t now = bpf_ktime_get_ns();
        end_stage(exec_id, tid, now);
        start_stage(exec_id, 1, tid, now);
    }
    return 0;
}

SEC("usdt")
int req_enqueue(struct pt_regs *ctx) {
    uint64_t exec_id = 0;
    bpf_usdt_arg(ctx, 0, (long *)&exec_id);
    uint32_t tid = bpf_get_current_pid_tgid();
    uint64_t now = bpf_ktime_get_ns();
    end_stage(exec_id, tid, now);
    start_stage(exec_id, 2, 0, now);
    return 0;
}

SEC("usdt")
int ob_req_entry(struct pt_regs *ctx) {
    uint64_t exec_id = 0;
    bpf_usdt_arg(ctx, 0, (long *)&exec_id);
    uint32_t tid = bpf_get_current_pid_tgid();
    uint64_t now = bpf_ktime_get_ns();
    end_stage(exec_id, 0, now);
    start_stage(exec_id, 3, tid, now);
    return 0;
}

SEC("usdt")
int ob_resp_enqueue(struct pt_regs *ctx) {
    uint64_t exec_id = 0;
    bpf_usdt_arg(ctx, 0, (long *)&exec_id);
    uint32_t tid = bpf_get_current_pid_tgid();
    uint64_t now = bpf_ktime_get_ns();
    end_stage(exec_id, tid, now);
    start_stage(exec_id, 4, 0, now);
    return 0;
}

SEC("usdt")
int exec_resp_entry(struct pt_regs *ctx) {
    uint64_t exec_id = 0;
    bpf_usdt_arg(ctx, 0, (long *)&exec_id);
    uint32_t tid = bpf_get_current_pid_tgid();
    uint64_t now = bpf_ktime_get_ns();
    end_stage(exec_id, 0, now);
    start_stage(exec_id, 5, tid, now);
    return 0;
}

SEC("usdt")
int exec_resp_before_db(struct pt_regs *ctx) {
    uint64_t exec_id = 0;
    bpf_usdt_arg(ctx, 0, (long *)&exec_id);
    uint32_t tid = bpf_get_current_pid_tgid();
    uint64_t now = bpf_ktime_get_ns();
    end_stage(exec_id, tid, now);
    start_stage(exec_id, 6, tid, now);
    return 0;
}

// Scheduler Tracepoints
SEC("raw_tracepoint/sched_switch")
int raw_sched_switch(struct bpf_raw_tracepoint_args *ctx) {
    struct task_struct *prev = (struct task_struct *)ctx->args[1];
    struct task_struct *next = (struct task_struct *)ctx->args[2];
    
    uint32_t prev_pid = BPF_CORE_READ(prev, pid);
    uint32_t next_pid = BPF_CORE_READ(next, pid);
    uint64_t now = bpf_ktime_get_ns();

    // Switch Out
    uint64_t *prev_exec_id_ptr = bpf_map_lookup_elem(&active_exec_ids, &prev_pid);
    if (prev_exec_id_ptr) {
        struct active_stage *stage = bpf_map_lookup_elem(&active_stages, prev_exec_id_ptr);
        if (stage) {
            stage->ctx_switches++;
        }
    }

    // Switch In
    uint64_t *next_exec_id_ptr = bpf_map_lookup_elem(&active_exec_ids, &next_pid);
    if (next_exec_id_ptr) {
        struct active_stage *stage = bpf_map_lookup_elem(&active_stages, next_exec_id_ptr);
        if (stage) {
            uint64_t *wake_ts = bpf_map_lookup_elem(&wake_timestamps, &next_pid);
            if (wake_ts) {
                uint64_t delay = now - *wake_ts;
                stage->runqueue_delay_ns += delay;
                bpf_map_delete_elem(&wake_timestamps, &next_pid);
            }
        }
    }
    return 0;
}

SEC("raw_tracepoint/sched_wakeup")
int raw_sched_wakeup(struct bpf_raw_tracepoint_args *ctx) {
    struct task_struct *p = (struct task_struct *)ctx->args[0];
    uint32_t pid = BPF_CORE_READ(p, pid);

    uint64_t *exec_id_ptr = bpf_map_lookup_elem(&active_exec_ids, &pid);
    if (exec_id_ptr) {
        uint64_t now = bpf_ktime_get_ns();
        bpf_map_update_elem(&wake_timestamps, &pid, &now, BPF_ANY);
    }
    return 0;
}

// Page Fault Tracepoints
SEC("tracepoint/exceptions/page_fault_user")
int handle_page_fault_user(void *ctx) {
    uint32_t tid = bpf_get_current_pid_tgid();
    uint64_t *exec_id_ptr = bpf_map_lookup_elem(&active_exec_ids, &tid);
    if (exec_id_ptr) {
        struct active_stage *stage = bpf_map_lookup_elem(&active_stages, exec_id_ptr);
        if (stage) {
            stage->page_faults++;
        }
    }
    return 0;
}

SEC("tracepoint/exceptions/page_fault_kernel")
int handle_page_fault_kernel(void *ctx) {
    uint32_t tid = bpf_get_current_pid_tgid();
    uint64_t *exec_id_ptr = bpf_map_lookup_elem(&active_exec_ids, &tid);
    if (exec_id_ptr) {
        struct active_stage *stage = bpf_map_lookup_elem(&active_stages, exec_id_ptr);
        if (stage) {
            stage->page_faults++;
        }
    }
    return 0;
}
