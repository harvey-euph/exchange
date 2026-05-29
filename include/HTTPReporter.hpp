#pragma once

#include "ExecutionReporter.hpp"
#include "ring/SHMRingBuffer.hpp"
#include "fbs/order_generated.h"
#include <memory>
#include <string>

namespace Exchange {

class HTTPReporter final : public ExecutionReporter
{
public:
    HTTPReporter(const std::string& ring_name = "OrderResponse", size_t ring_size = 16384);
    ~HTTPReporter();

    void onRequest(const OrderRequest* req) override;
    void onAck(const OrderRequest* req, size_t price_index) override;
    void onCancelled(const OrderRequest* req) override;
    void onModified(const OrderRequest* req) override;
    void onReject(const OrderRequest* req, RejectCode code) override;
    void onFill(const Order* incoming,
                const Order* existing,
                int64_t price,
                uint64_t qty_fill) override;

private:
    void sendResponse(ExecType exec_type, 
                      uint64_t order_id, 
                      uint32_t client_id, 
                      uint64_t exec_id,
                      uint32_t symbol_id, 
                      int64_t price, 
                      uint64_t qty, 
                      RejectCode reject_code = RejectCode_None);

    SHMRingBuffer* ring_;
    flatbuffers::FlatBufferBuilder fbb_;
};

} // namespace Exchange
