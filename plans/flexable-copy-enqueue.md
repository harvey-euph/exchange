# Role & Context
You are an expert low-latency infrastructure C++ engineer. Your task is to refactor the interface of our existing ring buffer/lock-free queue (`request_ring_`) to support extreme Zero-Copy (In-place writing).

# Objective
Currently, our queue only supports a monolithic `enqueue(const uint8_t* data, size_t size)` interface, which forces a double-copy behavior: Serializing to an external buffer first, and then `memcpy` into the queue.
We need to decouple `enqueue` into a 3-step lifecycle interface: `Reserve -> Copy (In-place Write) -> Commit` to achieve true 1-copy performance.

# Detailed Requirements

## 1. Interface Design
Refactor or extend the Queue class to provide the following two core methods to replace/supplement the traditional `enqueue`:

1. **`uint8_t* reserve(size_t required_size)`**
   - **Purpose:** Checks if the ring buffer has enough continuous space for the `required_size`.
   - **Behavior:** If space is available, it returns a raw pointer (`uint8_t*`) pointing directly to the next available write slot *inside* the queue's internal buffer. If full, handles it gracefully (returns `nullptr` or triggers backpressure/drop logic depending on current architecture).
   - **Thread Safety:** Must be lock-free and thread-safe for the Producer thread.

2. **`void commit(size_t actual_written_size)`**
   - **Purpose:** Finalizes the write operation.
   - **Behavior:** Advances the producer cursor (write index) by `actual_written_size`. 
   - **Memory Barrier:** Must use an atomic store with `std::memory_order_release` to guarantee that the downstream consumer thread can safely see the completely written data, preventing any CPU instruction reordering.

## 2. Refactoring Target (Gateway Logic)
Modify the existing `switch-case` handling for `ClientRequestData_OrderRequest` inside the Gateway/Network thread to use this new interface. 

**Current Code to Refactor:**
```cpp
case ClientRequestData_OrderRequest: {
    auto order_req = request->data_as_OrderRequest();

    flatbuffers::FlatBufferBuilder fbb(256);
    auto or_offset = CreateOrderRequest(fbb, 
        order_req->action(), order_req->exec_id(), order_req->order_id(), 
        order_req->client_id(), order_req->symbol_id(), order_req->side(), 
        order_req->type(), order_req->p(), order_req->q(), 
        order_req->visible_qty(), order_req->timestamp());
    fbb.Finish(or_offset);
    DTRACE_PROBE1(exchange, req_enqueue, order_req->exec_id());
    request_ring_->enqueue(fbb.GetBufferPointer(), fbb.GetSize());
    break;
}