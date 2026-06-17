# eBPF TX Latency Correlation Design

## Goal

Measure:

```text
Application write timestamp
        ↓
tcp_sendmsg()
        ↓
...
        ↓
tracepoint:net:net_dev_start_xmit()
        ↓
Packet begins transmission
```

and compute:

```text
TX Stack Latency =
net_dev_start_xmit.timestamp -
tcp_sendmsg.timestamp
```

while still preserving application-layer payload parsing.

---

# Why not use TID?

Do NOT correlate using TID (`pid_tgid`).

Reason:

- `tcp_sendmsg()` may execute in user thread.
- `net_dev_start_xmit()` may execute in:
  - same thread
  - softirq context
  - `ksoftirqd`
  - another CPU

Examples:

```text
tcp_sendmsg()              TID=100
net_dev_start_xmit()       TID=0
```

or

```text
tcp_sendmsg()              TID=100
net_dev_start_xmit()       TID=523
```

Therefore:

```text
TID correlation is unreliable.
```

---

# Recommended Correlation Keys

Priority:

1. skb pointer (best)
2. TCP sequence number
3. struct sock *
4. 5-tuple
5. TID (avoid)

---

# Recommended Design

## Stage 1: Hook tcp_sendmsg()

Purpose:

- Extract application payload
- Record initial timestamp
- Generate correlation key

Hook:

```c
SEC("kprobe/tcp_sendmsg")
```

Collect:

```c
struct sock *sk;
u64 timestamp_ns;
payload;
tcp_seq;
```

Suggested key:

```text
(sock *, tcp_seq)
```

Store:

```text
tx_pending_map[
    (sock*, seq)
] = {
    timestamp_ns,
    payload metadata
};
```

---

## Stage 2: Hook net_dev_start_xmit()

Purpose:

- Obtain actual transmit timestamp
- Correlate with application event

Hook:

```c
SEC("tracepoint/net/net_dev_start_xmit")
```

Input:

```c
struct sk_buff *skb;
```

Extract:

```c
skb->sk
TCP sequence number
```

Construct key:

```text
(sock*, seq)
```

Lookup:

```text
tx_pending_map[(sock*, seq)]
```

Compute latency:

```text
tx_latency_ns =
xmit_timestamp -
tcp_sendmsg_timestamp
```

Emit event:

```text
{
    payload metadata,
    tcp_sendmsg_ts,
    xmit_ts,
    tx_latency_ns
}
```

---

# Alternative: skb Pointer Correlation (More Precise)

If possible, hook:

```c
SEC("kprobe/tcp_transmit_skb")
```

Prototype:

```c
int tcp_transmit_skb(
    struct sock *sk,
    struct sk_buff *skb,
    ...
)
```

Store:

```text
skb_ptr → metadata
```

Then:

```c
tracepoint:net:net_dev_start_xmit
```

provides:

```c
ctx->skbaddr
```

Lookup:

```text
map[skb_ptr]
```

Advantages:

- Exact packet matching
- No TCP sequence parsing
- Works across softirq and thread migration

Flow:

```text
tcp_transmit_skb()
        ↓
store skb pointer
        ↓
dev_queue_xmit()
        ↓
net_dev_start_xmit()
        ↓
lookup skb pointer
        ↓
compute latency
```

---

# Caveats

## GSO/TSO

One send may become:

```text
1 tcp_sendmsg()
        ↓
multiple net_dev_start_xmit()
```

or:

```text
multiple sends
        ↓
one skb
```

TCP sequence numbers are more robust than TID.

---

## Payload Differences

Payload observed in:

```text
tcp_sendmsg()
```

is:

```text
Application payload
```

Payload observed in:

```text
net_dev_start_xmit()
```

is:

```text
Wire payload
```

Differences may arise due to:

- TLS encryption
- TCP segmentation
- GSO/TSO
- encapsulation
- checksum offload
- VLAN insertion

Therefore:

```text
Payload parsing should remain in tcp_sendmsg().
Timestamping should use net_dev_start_xmit().
```

---

# Final Recommendation

Best architecture:

```text
tcp_sendmsg()
        ↓
extract payload
record timestamp
store (sock*, seq)
        ↓
net_dev_start_xmit()
        ↓
extract (sock*, seq)
lookup metadata
compute latency
emit event
```

Even better:

```text
tcp_transmit_skb()
        ↓
store skb pointer
        ↓
net_dev_start_xmit()
        ↓
lookup skb pointer
compute latency
```

Avoid:

```text
TID-based correlation
```