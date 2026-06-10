#include "OrderBook.hpp"
#include "ExecutionReporter.hpp"
#include "TimeUtil.hpp"

#include <cstdio>
#include <iostream>
#include <random>

namespace Exchange {

thread_local uint64_t g_current_request_start_tsc = 0;

namespace {
const char* action_name(const OrderRequest* req)
{
    return req ? EnumNameOrderAction(req->action()) : "";
}

const char* side_name(const OrderRequest* req)
{
    return req ? EnumNameSide(req->side()) : "";
}

void print_client_channel(const char* event, uint32_t client_id, uint64_t order_id)
{
    (void) event;
    (void) client_id;
    (void) order_id;
    // std::printf("[client] event=%s client_id=%u order_id=%lu\n",
    //             event, client_id, order_id);
}

void send_response(SHMRingBuffer* ring, flatbuffers::FlatBufferBuilder& fbb,
                  ExecType exec_type, uint64_t order_id, uint32_t client_id,
                  uint64_t exec_id, uint32_t symbol_id, Side side, int64_t p, uint64_t q,
                  RejectCode reject_code)
{
    if (!ring) return;
    fbb.Clear();
    uint64_t engine_lat = 0;
    if (g_current_request_start_tsc > 0) {
        engine_lat = Exchange::read_tsc_end() - g_current_request_start_tsc;
    }
    auto resp = CreateOrderResponse(fbb, exec_type, order_id, client_id, exec_id, symbol_id, side, p, q, reject_code, engine_lat, 0);
    fbb.Finish(resp);
    ring->enqueue(fbb.GetBufferPointer(), fbb.GetSize());
}
} // namespace

ClientExecutionReporter::ClientExecutionReporter(SHMRingBuffer* ring): m_ring(ring), fbb(256) {}

void StdoutExecutionReporter::onRequest(const OrderRequest* req)
{
    if (!req) return;

    std::printf("[Order %lu] %s %s Price:%ld Qty:%lu Vis:%lu Client:%u\n",
                req->order_id(),
                side_name(req),
                action_name(req),
                req->p(),
                req->q(),
                req->visible_qty(),
                req->client_id());
}

void StdoutExecutionReporter::onAck(const OrderRequest* req)
{
    if (!req) return;

    std::printf("[ACK] order_id=%lu client_id=%u type=%d qty=%lu ts=%lu\n",
                req->order_id(),
                req->client_id(),
                static_cast<int>(req->type()),
                req->q(),
                req->timestamp());
}

void StdoutExecutionReporter::onCancelled(const OrderRequest* req)
{
    if (!req) return;

    std::printf("[CANCELLED] order_id=%lu client_id=%u\n",
                req->order_id(),
                req->client_id());
}

void StdoutExecutionReporter::onModified(const OrderRequest* req)
{
    if (!req) return;

    std::printf("[MODIFIED] order_id=%lu client_id=%u price=%ld qty=%lu\n",
                req->order_id(),
                req->client_id(),
                req->p(),
                req->q());
}

void StdoutExecutionReporter::onReject(const OrderRequest* req, RejectCode code)
{
    if (!req) return;

    std::printf("[REJECT] client_id=%u code=%s(%d)\n",
                req->client_id(),
                EnumNameRejectCode(code),
                static_cast<int>(code));
}

void StdoutExecutionReporter::onFill(const Order* taker,
                                     const Order* maker,
                                     const Side taker_side,
                                     int64_t price,
                                     uint64_t qty_fill)
{
    if (!taker || !maker) return;

    std::printf("[FILL] taker %s %lu from maker %lu price=%ld qty=%lu "
                "taker_remaining=%lu maker_remaining=%lu\n",
                EnumNameSide(taker_side),
                taker->order_id,
                maker->order_id,
                price,
                qty_fill,
                taker->qty_remaining,
                maker->qty_remaining);
}

void ClientExecutionReporter::onRequest(const OrderRequest* req)
{
    if (!req) return;
    print_client_channel("request", req->client_id(), req->order_id());
}

void ClientExecutionReporter::onAck(const OrderRequest* req)
{
    if (!req) return;
    send_response(m_ring, fbb, ExecType_New, req->order_id(), req->client_id(), req->exec_id(), req->symbol_id(), req->side(), req->p(), req->q(), RejectCode_None);
}

void ClientExecutionReporter::onCancelled(const OrderRequest* req)
{
    if (!req) return;
    print_client_channel("cancelled", req->client_id(), req->order_id());
    send_response(m_ring, fbb, ExecType_Cancelled, req->order_id(), req->client_id(), req->exec_id(), req->symbol_id(), req->side(), req->p(), req->q(), RejectCode_None);
}

void ClientExecutionReporter::onModified(const OrderRequest* req)
{
    if (!req) return;
    // std::printf("[client] event=modified client_id=%u order_id=%lu price=%ld qty=%lu\n",
    //             req->client_id(), req->order_id(), req->p(), req->q());
    send_response(m_ring, fbb, ExecType_Replaced, req->order_id(), req->client_id(), req->exec_id(), req->symbol_id(), req->side(), req->p(), req->q(), RejectCode_None);
}

void ClientExecutionReporter::onReject(const OrderRequest* req, RejectCode code)
{
    if (!req) return;
    // std::printf("[client] event=reject client_id=%u order_id=%lu code=%s(%d)\n",
    //             req->client_id(),
    //             req->order_id(),
    //             EnumNameRejectCode(code),
    //             static_cast<int>(code));
    send_response(m_ring, fbb, ExecType_Cancelled, req->order_id(), req->client_id(), req->exec_id(), req->symbol_id(), req->side(), req->p(), req->q(), code);
}

void ClientExecutionReporter::onFill(const Order* taker,
                                     const Order* maker,
                                     const Side taker_side,
                                     int64_t price,
                                     uint64_t qty_fill)
{
    if (!taker || !maker) return;
    
    static thread_local std::mt19937_64 gen(std::random_device{}());
    uint64_t rand_exec_id = gen();
    
    send_response(m_ring, fbb, 
                  taker->qty_remaining == 0 ? ExecType_Fill : ExecType_PartialFill,
                  taker->order_id, taker->client_id, rand_exec_id, 1, taker_side, price, qty_fill, RejectCode_None);

    const Side maker_side = static_cast<Side>(1-static_cast<int>(taker_side));
    
    send_response(m_ring, fbb,
                  maker->qty_remaining == 0 ? ExecType_Fill : ExecType_PartialFill,
                  maker->order_id, maker->client_id, rand_exec_id, 1, maker_side, price, qty_fill, RejectCode_None);
}

} // namespace Exchange
