# eBPF 訊息流程分析報告

## 程式目的

`ebpf-msg-flow` 是一個**端到端延遲追蹤工具**，用於追蹤一筆訂單從網路接收到回應回傳的完整路徑，測量每個處理階段的延遲。

架構為典型的 eBPF CO-RE（Compile-Once, Run-Everywhere）模式：
- **核心程式** (`ebpf-msg-flow.bpf.c`)：在 kernel space 執行，hook 各個觀測點
- **使用者程式** (`ebpf-msg-flow.cpp`)：在 user space 執行，接收事件並統計分析

---

## 追蹤的生命週期（9 個時間戳）

```
ts0  tcp_recvmsg entry          ← 封包進 kernel TCP 層
ts1  req_entry (USDT)           ← 應用程式開始處理請求
ts2  req_enqueue (USDT)         ← 請求入隊（client-manager → matching-engine）
ts3  ob_req_entry (USDT)        ← matching-engine 開始處理
ts4  ob_resp_enqueue (USDT)     ← matching-engine 回應入隊
ts5  exec_resp_entry (USDT)     ← client-manager 開始處理回應
ts6  exec_resp_before_db (USDT) ← 寫入 DB 前
ts7  tcp_sendmsg entry          ← 開始傳送回應封包
ts8  tcp_sendmsg return         ← 傳送完成 → 計算各段延遲並發送到 ring buffer
```

每段延遲對應：
| 區段 | 意義 |
|------|------|
| 0→1  | TCP 接收到應用層感知的時間（recv 系統開銷） |
| 1→2  | 解析請求 + 入隊時間 |
| 2→3  | 訊息在隊列中等待的時間（IPC/lock 延遲） |
| 3→4  | matching engine 處理時間（核心業務邏輯） |
| 4→5  | 回應從 ME 到 CM 的傳輸時間 |
| 5→6  | CM 處理回應直到 DB 寫入前 |
| 6→7  | DB 寫入後到 send 開始 |
| 7→8  | 實際 TCP 傳送時間 |

---

## 技術實現細節

### 1. Kprobe：攔截 TCP 收發

```
kprobe/tcp_recvmsg        → 記錄 ts0 + 儲存 iov 資訊到 recv_ctx_map（by TID）
kretprobe/tcp_recvmsg     → 讀取 payload，呼叫 process_rx_packet()
kprobe/tcp_sendmsg        → 讀取 payload，呼叫 process_tx_packet()
kretprobe/tcp_sendmsg     → 記錄 ts8，提交事件到 ringbuf
```

> 過濾條件：`sport == target_port`（預設 9001）

### 2. WebSocket / FlatBuffers 解析（BPF 內）

`process_rx_packet` 和 `process_tx_packet` 各自對 payload 做：

1. **WebSocket frame 解析**：opcode check（binary=0x2）、len 欄位（7bit/16bit/64bit 三種）、mask key 擷取
2. **FlatBuffers 解析**：循著 root_offset → vtable → field offset 找到 `exec_id`，以及 `data_type`（只處理 `OrderRequest/OrderResponse`）

這些都是**在 BPF kernel space 內完成**，不需要傳回 user space 解析。

### 3. USDT 探針（靜態追蹤點）

附加到兩個 binary：
- `./build/services/client-manager`：`req_entry`, `req_enqueue`, `exec_resp_entry`, `exec_resp_before_db`
- `./build/services/matching-engine`：`ob_req_entry`, `ob_resp_enqueue`

透過 `exec_id` 作為 key 關聯不同探針的時間戳，存在 `flow_events` hash map 中。

### 4. BPF Maps 使用

| Map | 用途 |
|-----|------|
| `recv_ctx_map` | kprobe/kretprobe 之間傳遞 recv 上下文（by TID） |
| `scratch_map` | PERCPU 512-byte 暫存空間（繞過 BPF stack 512B 限制） |
| `flow_events` | 核心資料結構，用 exec_id 關聯 9 個時間戳 |
| `active_tx_exec_id` | sendmsg 的 kprobe→kretprobe 之間傳遞 exec_id |
| `tid_ts1` | req_entry 時間暫存（等 req_enqueue 事件來提取） |
| `rb` (ringbuf) | 事件從 kernel 傳到 user space |
| 其他 3 個 map | `manager_start_map`, `engine_start_map`, `manager_lat_map`, `engine_lat_map`, `pending_requests` — **宣告了但目前沒有任何程式碼使用** |

### 5. User Space 統計

- 按 `exec_type`（New / Modify-Short / Modify-Long / Cancel / Reject / Replaced）分組
- 對 8 個區段各自計算 p50/p90/p99/p99.9 延遲
- `--raw` 模式：CSV 輸出全部事件（適合離線分析）

---

## 可優化的地方

### 🔴 嚴重：宣告但未使用的 Maps（死碼）

以下 5 個 map 宣告了卻從未使用，白白佔用記憶體：

```c
pending_requests    // 65536 * 16 bytes = ~1MB
manager_start_map   // 65536 * 16 bytes = ~1MB
engine_start_map    // 65536 * 16 bytes = ~1MB
manager_lat_map     // 65536 * 16 bytes = ~1MB
engine_lat_map      // 65536 * 16 bytes = ~1MB
```

**直接刪除可省下約 5MB kernel 記憶體**。

### 🟠 中：`process_rx_packet` 和 `process_tx_packet` 大量重複

兩個函數有 **~80 行完全相同的 WebSocket frame 解析程式碼**（第 288–321 行 vs 第 384–417 行）。

可以拆出一個共用 helper：

```c
static __always_inline bool parse_ws_frame(
    const uint8_t *payload, uint32_t len, uint32_t curr_offset,
    uint32_t *out_payload_offset, uint32_t *out_masking_key,
    bool *out_mask, uint64_t *out_actual_payload_len)
```

這樣兩個函數只保留 FlatBuffers 解析的不同部分。

### 🟠 中：`get_exec_id_from_*` 系列函數未被核心路徑呼叫

```c
get_exec_id_from_client_request()     // 只在 .c 裡定義，沒有被呼叫
get_exec_id_from_order_request()      // 只在 .c 裡定義，沒有被呼叫
get_exec_id_from_order_response_t()   // 只在 .c 裡定義，沒有被呼叫
```

這三個函數用 `bpf_probe_read_user` 直接讀 user memory，而核心路徑用 `read_unmasked_*` 系列函數處理 WebSocket masking。兩套邏輯並存但只用了後者，前三個是**死碼**，可全部刪除。

### 🟡 小：`copy_iov_iter` 中 Segment 1 偏移量計算可疑

```c
// 第 166 行：奇怪的 mask 操作
unsigned int copy_offset = copied & 15;
// 實際 dst 指標用 copy_offset 而不是 copied
bpf_probe_read_user((char *)dst + copy_offset, to_copy, base);
```

`copied & 15` 通常得到 0（因為 `copied` 等於第一個 segment 讀了多少，不一定是 16 的倍數），這看起來是 **workaround BPF verifier 問題**（stack pointer 不能有 `|=` 運算），但程式碼需要加註解說明原因。

### 🟡 小：`flow_events` map 可考慮換成 LRU

```c
__uint(type, BPF_MAP_TYPE_HASH);
__uint(max_entries, 100000);
```

滿了之後新事件無法插入，訂單追蹤會靜默丟失。改用 `BPF_MAP_TYPE_LRU_HASH` 可自動淘汰舊條目。

---

## 精簡程式碼的具體建議

### 估計可刪減行數

| 項目 | 可刪行數 |
|------|---------|
| 5 個未使用 map 宣告 | ~25 行 |
| 3 個未使用函數 | ~35 行 |
| WS frame 解析重複碼（抽出 helper） | ~60 行重複 → 共用 1 個 helper 約 25 行 |
| **總計** | **~95 行（BPF 程式碼約減少 15%）** |

### 最小改動版本（保持功能不變）

1. 刪除 `pending_requests`, `manager_start_map`, `engine_start_map`, `manager_lat_map`, `engine_lat_map` 這 5 個 map
2. 刪除 `get_exec_id_from_client_request`, `get_exec_id_from_order_request`, `get_exec_id_from_order_response_t` 這 3 個函數
3. 將 WS frame 解析抽取為 `parse_ws_frame_header()` helper，供兩個 process_*_packet 共用

這三項改動**不影響任何功能**，但會讓核心 BPF 程式從 656 行降至約 520 行，且釋放約 5MB kernel 記憶體。
