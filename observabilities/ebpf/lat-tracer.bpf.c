#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>

char LICENSE[] SEC("license") = "GPL";

const volatile uint16_t target_port = 9001;

// Context to store recvmsg parameters across entry/exit
struct recv_ctx {
    struct sock *sk;
    uint8_t iter_type;
    uint8_t padding[7]; // align to 8-byte boundary
    size_t iov_offset;
    void *ubuf;
    const struct iovec *iov;
    uint32_t nr_segs;
};

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 1024);
    __type(key, uint32_t); // thread id
    __type(value, struct recv_ctx);
} recv_ctx_map SEC(".maps");

// Event structure pushed to userspace C++ app
struct event_data {
    uint64_t sock_ptr;
    uint64_t timestamp_ns;
    uint32_t len;
    uint8_t event_type; // 0: RX (recv), 1: TX (send)
    uint8_t padding[3];  // Explicit padding to align to 8 bytes boundary safely
    uint8_t payload[512];
};

struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 16777216); // 16MB
} rb SEC(".maps");

static __always_inline void copy_iov_iter(void *dst, uint8_t iter_type, size_t iov_offset, void *ubuf, const struct iovec *iov, uint32_t nr_segs)
{
    __builtin_memset(dst, 0, 512);

    if (iter_type == 0) { // ITER_UBUF
        if (ubuf) {
            bpf_probe_read_user(dst, 512, ubuf + iov_offset);
        }
    } else if (iter_type == 1) { // ITER_IOVEC
        if (iov) {
            struct iovec local_iov[2];
            __builtin_memset(local_iov, 0, sizeof(local_iov));

            // Copy first 2 iovec descriptors from kernel space
            bpf_probe_read_kernel(local_iov, sizeof(local_iov), iov);

            size_t offset = iov_offset;
            size_t copied = 0;

            // Segment 0
            if (offset < local_iov[0].iov_len) {
                void *base = local_iov[0].iov_base + offset;
                size_t avail = local_iov[0].iov_len - offset;
                size_t to_copy = avail;
                if (to_copy > 512) {
                    to_copy = 512;
                }
                bpf_probe_read_user(dst, to_copy, base);
                copied = to_copy;
                offset = 0;
            } else {
                offset -= local_iov[0].iov_len;
            }

            // Segment 1
            if (copied < 512 && offset < local_iov[1].iov_len) {
                void *base = local_iov[1].iov_base + offset;
                size_t avail = local_iov[1].iov_len - offset;
                size_t to_copy = 512 - copied;
                if (avail < to_copy) {
                    to_copy = avail;
                }

                unsigned int copy_offset = copied & 15;
                if (to_copy > 497) {
                    to_copy = 497;
                }
                bpf_probe_read_user((char *)dst + copy_offset, to_copy, base);
            }
        }
    }
}

SEC("kprobe/tcp_recvmsg")
int BPF_KPROBE(tcp_recvmsg, struct sock *sk, struct msghdr *msg)
{
    uint16_t sport = BPF_CORE_READ(sk, __sk_common.skc_num);
    if (target_port != 0 && sport != target_port)
        return 0;

    uint32_t tid = bpf_get_current_pid_tgid();
    struct recv_ctx rctx = {};
    rctx.sk = sk;
    rctx.iter_type = BPF_CORE_READ(msg, msg_iter.iter_type);
    rctx.iov_offset = BPF_CORE_READ(msg, msg_iter.iov_offset);
    rctx.ubuf = BPF_CORE_READ(msg, msg_iter.ubuf);
    rctx.iov = BPF_CORE_READ(msg, msg_iter.__iov);
    rctx.nr_segs = BPF_CORE_READ(msg, msg_iter.nr_segs);

    bpf_map_update_elem(&recv_ctx_map, &tid, &rctx, BPF_ANY);
    return 0;
}

SEC("kretprobe/tcp_recvmsg")
int BPF_KRETPROBE(tcp_recvmsg_ret, int ret)
{
    uint32_t tid = bpf_get_current_pid_tgid();
    struct recv_ctx *rctx = bpf_map_lookup_elem(&recv_ctx_map, &tid);
    if (!rctx)
        return 0;

    struct sock *sk = rctx->sk;

    uint8_t iter_type = rctx->iter_type;
    size_t iov_offset = rctx->iov_offset;
    void *ubuf = rctx->ubuf;
    const struct iovec *iov = rctx->iov;
    uint32_t nr_segs = rctx->nr_segs;

    bpf_map_delete_elem(&recv_ctx_map, &tid);

    if (ret <= 0)
        return 0;

    struct event_data *data = bpf_ringbuf_reserve(&rb, sizeof(*data), 0);
    if (!data) {
        bpf_printk("rx res err\n");
        return 0;
    }

    data->sock_ptr = (uint64_t)sk;
    data->timestamp_ns = bpf_ktime_get_ns();
    data->len = ret;
    data->event_type = 0; // 0: RX

    copy_iov_iter(data->payload, iter_type, iov_offset, ubuf, iov, nr_segs);

    bpf_ringbuf_submit(data, 0);
    return 0;
}

// Hook tcp_sendmsg (Entry) - Parse sent data
SEC("kprobe/tcp_sendmsg")
int BPF_KPROBE(tcp_sendmsg, struct sock *sk, struct msghdr *msg, size_t size)
{
    uint16_t sport = BPF_CORE_READ(sk, __sk_common.skc_num);
    if (target_port != 0 && sport != target_port)
        return 0;

    bpf_printk("tx ent size=%d\n", size);

    struct event_data *data = bpf_ringbuf_reserve(&rb, sizeof(*data), 0);
    if (!data) {
        bpf_printk("tx res err\n");
        return 0;
    }
    bpf_printk("tx res ok\n");

    data->sock_ptr = (uint64_t)sk;
    data->timestamp_ns = bpf_ktime_get_ns();
    data->len = size;
    data->event_type = 1; // 1: TX

    uint8_t iter_type = BPF_CORE_READ(msg, msg_iter.iter_type);
    size_t iov_offset = BPF_CORE_READ(msg, msg_iter.iov_offset);
    void *ubuf = BPF_CORE_READ(msg, msg_iter.ubuf);
    const struct iovec *iov = BPF_CORE_READ(msg, msg_iter.__iov);
    uint32_t nr_segs = BPF_CORE_READ(msg, msg_iter.nr_segs);

    copy_iov_iter(data->payload, iter_type, iov_offset, ubuf, iov, nr_segs);

    bpf_ringbuf_submit(data, 0);
    return 0;
}
