#include "HTTPReporter.hpp"
#include "OrderBook.hpp"
#include <iostream>

namespace Exchange {

HTTPReporter::HTTPReporter(const std::string& ring_name, size_t ring_size)
{
    ring_ = new SHMRingBuffer(ring_name, ring_size);
}

HTTPReporter::~HTTPReporter()
{
    delete ring_;
}

void HTTPReporter::onRequest(const OrderRequest* /*req*/)
{
    // Usually no response needed for just receiving the request in the engine
}

void HTTPReporter::onAck(const OrderRequest* req, size_t /*price_index*/)
{
    sendResponse(ExecType_New, 
                 req->order_id(), 
                 req->client_id(), 
                 req->exec_id(),
                 req->symbol_id(), 
                 req->p(), 
                 req->q());
}

void HTTPReporter::onCancelled(const OrderRequest* req)
{
    sendResponse(ExecType_Cancelled, 
                 req->order_id(), 
                 req->client_id(), 
                 req->exec_id(),
                 req->symbol_id(), 
                 req->p(), 
                 req->q());
}

void HTTPReporter::onModified(const OrderRequest* req)
{
    sendResponse(ExecType_Replaced, 
                 req->order_id(), 
                 req->client_id(), 
                 req->exec_id(),
                 req->symbol_id(), 
                 req->p(), 
                 req->q());
}

void HTTPReporter::onReject(const OrderRequest* req, RejectCode code)
{
    // On reject, we still want to use the same OrderID if provided, but mark it rejected
    sendResponse(ExecType_Cancelled, // Or some error state if we have one
                 req->order_id(), 
                 req->client_id(), 
                 req->exec_id(),
                 req->symbol_id(), 
                 req->p(), 
                 req->q(), 
                 code);
}

void HTTPReporter::onFill(const Order* incoming,
                         const Order* existing,
                         int64_t price,
                         uint64_t qty_fill)
{
    // Report for Taker
    sendResponse(incoming->qty_remaining == 0 ? ExecType_Fill : ExecType_PartialFill,
                 incoming->order_id,
                 incoming->client_id,
                 incoming->exec_id,
                 1, // symbol_id (could be passed in)
                 price,
                 qty_fill);

    // Report for Maker
    sendResponse(existing->qty_remaining == 0 ? ExecType_Fill : ExecType_PartialFill,
                 existing->order_id,
                 existing->client_id,
                 existing->exec_id,
                 1, // symbol_id
                 price,
                 qty_fill);
}

void HTTPReporter::sendResponse(ExecType exec_type, 
                               uint64_t order_id, 
                               uint32_t client_id, 
                               uint64_t exec_id,
                               uint32_t symbol_id, 
                               int64_t price, 
                               uint64_t qty, 
                               RejectCode reject_code)
{
    fbb_.Clear();
    auto resp = CreateOrderResponse(fbb_, exec_type, order_id, client_id, exec_id, symbol_id, price, qty, reject_code);
    fbb_.Finish(resp);

    void* ptr = fbb_.GetBufferPointer();
    size_t size = fbb_.GetSize();
    
    std::cout << "[HTTPReporter] Sending Response: exec_id=" << exec_id << " order_id=" << order_id << " type=" << (int)exec_type << std::endl;
    if (!ring_->enqueue(ptr, size)) {
        fprintf(stderr, "[HTTPReporter] Failed to enqueue response for order %lu\n", order_id);
    }
}

} // namespace Exchange
