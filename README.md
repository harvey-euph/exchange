## Summary

It all started as a simple practice exercise to build a C++ matching engine. However, the relentless pursuit of lower latency and higher throughput quickly escalated the scope. What began as a single component has evolved into a complete, high-performance exchange ecosystem.

Designed with a strong emphasis on low-latency architecture and systematic observability, the project now features a comprehensive suite of components:
- **Core Engine & Gateways:** A highly optimized Matching Engine decoupled from the Client Manager and HTTP Acceptors via lock-free Shared Memory (SHM) Ring Buffers.
- **Client & Market Data Protocols:** WebSocket-based streaming for L2/L3 order book updates and execution reports, utilizing zero-allocation FlatBuffers for ultra-fast serialization.
- **Observability (eBPF):** A custom Linux eBPF latency tracer (`lat-tracer`) that hooks into kernel network stacks (`tcp_recvmsg`/`tcp_sendmsg`) and user-space C++ functions (`uprobes`). It measures end-to-end latency at the microsecond level, mathematically isolating kernel network overhead from application processing time.
- **Automated Trading Agents:** A built-in C++ algorithmic trading ecosystem, including a Market Maker for liquidity provision and a Stress Trader for simulating high-frequency market chaos and load testing.
- **Modern Web Frontend:** A React/TypeScript UI featuring dynamic data throttling and state decoupling to handle massive bursts of order book updates without freezing the browser.

This project serves as a showcase of applying both low-level system engineering (eBPF, IPC, memory management) and big-picture architectural design (microservices, real-time web, state decoupling) to build a robust trading system.

## Requires

```sh
sudo apt install -y flatbuffers-compiler libflatbuffers-dev
sudo apt install -y libgtest-dev libgmock-dev
sudo apt install -y build-essential git libssl-dev zlib1g-dev libboost-all-dev
# eBPF monitoring tools dependencies
sudo apt install -y clang libbpf-dev bpftool
```

## Log in process

```mermaid
sequenceDiagram
    autonumber
    actor Client as Client API/APP
    participant CM as Client Manager
    participant DB as Client Database

    Client->>CM: Connect (WS Handshake)
    activate CM
    
    CM->>DB: 1. Pop pending responses
    DB-->>CM: Pending Executions list
    loop Send pending executions
        CM->>Client: WS ClientResponse (OrderResponse)
    end
    
    CM->>DB: 2. Get current open orders
    DB-->>CM: Open orders list
    loop Send open orders
        CM->>Client: WS ClientResponse (OrderResponse, ExecType=OrderStatus)
    end

    CM->>DB: 3. Get all positions (Cash & Assets)
    DB-->>CM: Positions list
    loop Send positions
        CM->>Client: WS ClientResponse (PositionResponse)
    end

    Note over CM: Mark session as ready
    CM->>Client: 4. Send Ready Frame (OrderResponse, ExecType=Complete)
    deactivate CM

    Note over Client: CRITICAL: Client MUST receive Ready Frame<br/>(ExecType=Complete) before sending any OrderRequest!
```

## Order & Market Data flow

```mermaid
graph TD
    Client[Client API/APP] -->|1. HTTP /order| HTTP[http_accepter]
    Client -->|1. WS OrderRequest| CM[client_manager]
    
    HTTP -->|2. Enqueue| RB_REQ[ORDER_REQUEST SHM Ring Buffer]
    CM -->|2. Enqueue| RB_REQ
    
    RB_REQ -->|3. Dequeue| OC[Matching Engine]
    OC -->|4. ExecutionReport| RB_RESP[ORDER_RESPONSE SHM Ring Buffer]
    OC -->|4. L2 Update| RB_L2[L2_UPDATE_RING SHM Ring Buffer]
    OC -->|4. L3 Update| RB_L3[L3_UPDATE_RING SHM Ring Buffer]
    
    RB_RESP -->|5. Dequeue| CM
    CM -->|6. WS ClientResponse / DB| Client
    
    RB_L2 -->|5. Dequeue| L2P[l2_publisher]
    L2P -->|6. WS L2Update| Client
    
    RB_L3 -->|5. Dequeue| L3P[l3_publisher]
    L3P -->|6. WS L3Update| Client
```

## L2/L3 Update & Subscription Phase

```mermaid
sequenceDiagram
    autonumber
    actor Client as Client API/APP
    participant Pub as L2/L3 Publisher

    Client->>Pub: Connect (WS Handshake)
    activate Pub
    
    Client->>Pub: Subscribe / Request SNAPSHOT
    
    Pub->>Client: Send Empty Frame (Side = None)
    Note over Client: MUST clear local L2/L3 data store<br/>upon receiving Empty Frame
    
    Note over Pub: [TODO] Send SNAPSHOT guarantees sequence:<br/>BEST BID -> BEST ASK -> OTHER LAYER
    
    Pub->>Client: WS L2/L3 Update (Snapshot)
    
    loop Send INCREMENTALS
        Pub->>Client: WS L2/L3 Update (Incremental)
    end
    
    Client->>Pub: Re-Subscribe / Request SNAPSHOT (Anytime)
    Note over Client, Pub: Triggers a new Empty Frame & Snapshot process
    
    deactivate Pub
```

## Data Flow and Client Protocol

    ---H> Send through HTTP Channel
    --WS> Send through WebSocket Channel
    --RB> Send through Ring Buffer

OrderRequest: Client ---H> HTTP Server --RB> ORDERBOOK_CORE
    
    OrderRequest in flatbuffers format

PreAcked:     HTTP Server ---H> Client

    Only inform Client that HTTP server received OrderRequest, futher information will be sent as Executions from CLIENT_MANAGEMENT.

Executions:   ORDERBOOK_CORE --RB> CLIENT_MANAGEMENT 
                             --WS> Client + ---> DB(Status=SENT)      (if Client loged in)
                             ----> DB(Status=PENDING)                 (if Client loged out)
    
    Connection built -> Client log in -> Check and send cached Executions -> accept request
    Acceptable Request: 1. OrderRequest in flatbuffers format
                        2. Current position and Cash
                        3. Current Pending Order Status
                        (Use union format flatbuffers, TODO)

L2 Update:    ORDERBOOK_CORE --RB> L2_PUBLISHER --WS> Client

    Connection built -> Accepting Request
    Acceptable Request: 1. Subscription -> receive SNAPSHOT -> receive INCREMENTALS
                        2. Unsubscription
                        3. Resend Subscription will be consider identically as the first time and send SNAPSHOT, in case client lost.

    SNAPSHOT: Empty frame ahead, same sturcture as INCREMENTALS
    Empty frame: Side=None
    RB: L2_UPDATE_RING

L3 Update:    Same as L2

    RB: L3_UPDATE_RING

## eBPF Latency Tracer (lat-tracer)

The `lat-tracer` eBPF program measures end-to-end latency of order requests by hooking into kernel networking functions and user-space application functions (uprobes). It breaks down the total latency into Kernel Network Overhead, Client Manager Processing, and Matching Engine Processing.

### Latency Tracing Workflow

#### 1. Recording starting time (tcp_recvmsg)

```mermaid
sequenceDiagram
    participant Kernel as tcp_recvmsg<br/>(Kernel)
    participant CtxMap as recv_ctx_map
    participant ReqMap as pending_requests

    Note over Kernel: [kprobe] tcp_recvmsg
    Kernel->>CtxMap: Save sock & msghdr context (key: TID)
    
    Note over Kernel: [kretprobe] tcp_recvmsg_ret
    CtxMap-->>Kernel: Read & Delete context (key: TID)
    Note over Kernel: Parse FlatBuffer Payload
    Kernel->>ReqMap: Save Start Timestamp T0 (key: exec_id)
```

#### 2. Client Manager Processing

```mermaid
sequenceDiagram
    participant CM as Client Manager<br/>(User Space)
    participant StartMap as manager_start_map
    participant TidMap as active_exec_id_map
    participant LatMap as manager_lat_map

    Note over CM: [uprobe] process_client_request
    CM->>StartMap: Save CM Start Time (key: exec_id)
    
    Note over CM: [uprobe] handle_execution_response
    CM->>TidMap: Save active exec_id (key: TID)
    
    Note over CM: [uretprobe] handle_execution_response_ret
    TidMap-->>CM: Read & Delete active exec_id (key: TID)
    StartMap-->>CM: Read & Delete CM Start Time (key: exec_id)
    CM->>LatMap: Save CM Latency (key: exec_id)
```

#### 3. Matching Engine Processing

```mermaid
sequenceDiagram
    participant OB as Matching Engine<br/>(User Space)
    participant StartMap as engine_start_map
    participant TidMap as active_exec_id_map
    participant LatMap as engine_lat_map

    Note over OB: [uprobe] processRequest
    OB->>StartMap: Save Engine Start Time (key: exec_id)
    OB->>TidMap: Save active exec_id (key: TID)
    
    Note over OB: [uretprobe] processRequest_ret
    TidMap-->>OB: Read & Delete active exec_id (key: TID)
    StartMap-->>OB: Read & Delete Engine Start Time (key: exec_id)
    OB->>LatMap: Save Engine Latency (key: exec_id)
```

#### 4. TX Path & Userspace Aggregation (tcp_sendmsg)

```mermaid
sequenceDiagram
    participant Kernel as tcp_sendmsg<br/>(Kernel)
    participant TxCtx as tx_ctx_map
    participant ReqMap as pending_requests
    participant EngineLat as engine_lat_map
    participant MgrLat as manager_lat_map
    participant RB as BPF RingBuffer
    participant Tracer as lat-tracer<br/>(User Space)

    Note over Kernel: [kprobe] tcp_sendmsg
    Note over Kernel: Parse FlatBuffer Payload
    Kernel->>TxCtx: Save tx_ctx with exec_ids (key: TID)
    
    Note over Kernel: [kretprobe] tcp_sendmsg_ret
    Note over Kernel: Capture TX End Timestamp
    TxCtx-->>Kernel: Read & Delete tx_ctx (key: TID)
    ReqMap-->>Kernel: Read & Delete T0 (key: exec_id)
    EngineLat-->>Kernel: Read & Delete Engine Latency (key: exec_id)
    MgrLat-->>Kernel: Read & Delete CM Latency (key: exec_id)
    
    Kernel->>RB: Submit latency_event
    
    RB-->>Tracer: Poll event
    Note over Tracer: Calculate Kernel Overhead<br/>(Total - Engine - CM)
    Note over Tracer: Aggregate & Display Stats
```

#### Responsibilities

**Kernel Space (eBPF)**:
1. **Network Hooks**: Intercepts `tcp_recvmsg` (entry/exit) to parse incoming FlatBuffer payloads for `exec_id` and records the start timestamp. Intercepts `tcp_sendmsg` to parse outgoing responses and compute total latency.
2. **Application Hooks (Uprobes)**: Attaches to C++ functions in `ClientManager` and `OrderBook`. Computes the time spent inside the Matching Engine and the Client Manager.
3. **Data Aggregation**: Retrieves the latency components when the response is sent out, bundles them into a `latency_event`, and pushes them to User Space.

**User Space (C++)**:
1. **Setup**: Loads the eBPF object, attaches kprobes and uprobes, and sets up the Ring Buffer.
2. **Processing**: Polls the Ring Buffer for `latency_event` structures.
3. **Analytics & Display**: Calculates the pure kernel networking overhead by subtracting application latencies from the total latency. Aggregates data by execution type (New, Modify, Cancel) and calculates percentiles (p50, p90, p99, p999), printing a real-time table to standard output.
