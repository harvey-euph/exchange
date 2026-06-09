#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <flatbuffers/flatbuffers.h>
#include "ring/SHMRingBuffer.hpp"
#include "fbs/order_generated.h"
#include "define.hpp"

namespace Exchange {

struct Order;

class ExecutionReporter
{
public:
    virtual ~ExecutionReporter() = default;

    virtual void onRequest(const OrderRequest* req) = 0;
    virtual void onAck(const OrderRequest* req, size_t price_index) = 0;
    virtual void onCancelled(const OrderRequest* req) = 0;
    virtual void onModified(const OrderRequest* req) = 0;
    virtual void onReject(const OrderRequest* req, RejectCode code) = 0;
    virtual void onFill(const Order* taker,
                        const Order* maker,
                        const Side side,
                        int64_t price,
                        uint64_t qty_fill) = 0;
};

class StdoutExecutionReporter final : public ExecutionReporter
{
public:
    void onRequest(const OrderRequest* req) override;
    void onAck(const OrderRequest* req, size_t price_index) override;
    void onCancelled(const OrderRequest* req) override;
    void onModified(const OrderRequest* req) override;
    void onReject(const OrderRequest* req, RejectCode code) override;
    void onFill(const Order* taker,
                const Order* maker,
                const Side side,
                int64_t price,
                uint64_t qty_fill) override;
};

class ClientExecutionReporter final : public ExecutionReporter
{
public:
    ClientExecutionReporter(const std::string& ring_name = ORDER_RESPONSE, unsigned int ring_size = ORDER_RESPONSE_SIZE);
    ~ClientExecutionReporter();

    void onRequest(const OrderRequest* req) override;
    void onAck(const OrderRequest* req, size_t price_index) override;
    void onCancelled(const OrderRequest* req) override;
    void onModified(const OrderRequest* req) override;
    void onReject(const OrderRequest* req, RejectCode code) override;
    void onFill(const Order* taker,
                const Order* maker,
                const Side taker_side,
                int64_t price,
                uint64_t qty_fill) override;

private:
    SHMRingBuffer* m_ring = nullptr;
    flatbuffers::FlatBufferBuilder fbb;
};

} // namespace Exchange
