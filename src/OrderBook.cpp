#include "OrderBook.hpp"
#include "define.hpp"
#include "LogUtil.hpp"
#include "TimeUtil.hpp"
#include <algorithm>
#include <random>
#include <sys/sdt.h>

using namespace Exchange;

thread_local uint64_t g_current_request_start_tsc = 0;

OrderBook::OrderBook(
    uint64_t symbol_id,
    int64_t min_step, 
    int64_t price_index_offset, 
    size_t max_price_levels,
    mmaplog::MmapWriter* response_ring
)   : symbol_id_(symbol_id)
    , min_step_(min_step)
    , price_index_offset_(price_index_offset)
    , max_price_levels_(max_price_levels)
    , response_ring_(response_ring)
    , price_array_(max_price_levels_)
{
    resp.symbol_id = symbol_id_;

    if (min_step <= 0) {
        throw std::invalid_argument("min_step must be positive");
    }
    if (max_price_levels == 0) {
        throw std::invalid_argument("max_price_levels must be > 0");
    }
}

OrderBook::~OrderBook() {}

static Order* createOrder(const OrderRequestT* req)
{
    return new Order {
        req->exec_id,
        req->order_id,
        req->client_id,
        req->q,
        req->q,
        req->type,
        nullptr,
        nullptr,
        nullptr,
        req->timestamp,
        req->symbol_id
    };
}

void OrderBook::handleNewOrder(const OrderRequestT* req, bool report_ack)
{
    if (active_orders_.count(req->order_id)) {
        sendResponse(ExecType_Rejected, req->order_id, req->client_id, req->exec_id, req->side, req->p, req->q, RejectCode_DuplicateOrderID, req->msg_seq_num);
        return;
    }

    Order* taker = createOrder(req);

    const int side_int = static_cast<int>(req->side);
    const size_t price_idx = (req->type == OrderType_Market)
        ? (req->side == Side_Buy ? max_price_levels_ - 1 : 0)
        : price_to_index(req->p);

    if (report_ack) {
        sendResponse(ExecType_New, req->order_id, req->client_id, req->exec_id, req->side, req->p, req->q, RejectCode_None, req->msg_seq_num);
    }

    PriceLevel **oppo = &best_levels_[1^side_int];

    while (*oppo && taker->qty_remaining)
    {
        const size_t oppo_idx = (*oppo) - price_array_.data();
        const size_t p = index_to_price(oppo_idx);
        const bool crossed = (req->side == Side_Buy) ? (price_idx >= oppo_idx) : (price_idx <= oppo_idx);
        if (!crossed) break;

        Order* maker = (*oppo)->dummy_head.next;
        while (maker != &(*oppo)->dummy_tail && taker->qty_remaining)
        {
            const uint64_t qty_fill = std::min(maker->qty_remaining, taker->qty_remaining);

            maker->qty_remaining -= qty_fill;
            taker->qty_remaining -= qty_fill;
            (*oppo)->total_qty   -= qty_fill;

            {
                static thread_local std::mt19937_64 gen(std::random_device{}());
                uint64_t exec_id = gen();
                const Side maker_side = static_cast<Side>(1^side_int);
                sendResponse(
                    taker->qty_remaining == 0 ? ExecType_Fill : ExecType_PartialFill,
                    taker->order_id, taker->client_id, exec_id, req->side, p, qty_fill);
                sendResponse(
                    maker->qty_remaining == 0 ? ExecType_Fill : ExecType_PartialFill,
                    maker->order_id, maker->client_id, exec_id, maker_side, p, qty_fill);
            }

            if (maker->qty_remaining) continue;

            active_orders_.erase(maker->order_id);
            removeOrderFromLevel(maker);
            Order *next = maker->next;
            delete maker;
            maker = next;
        }

        if (!(*oppo)->order_count)
        {
            removePriceLevel(*oppo, (Side)(1^side_int));
            *oppo = best_levels_[1^side_int];
        }
    }

    if (!taker->qty_remaining)
    {
        delete taker;
        return;
    }

    PriceLevel* level = GetOrCreatePriceLevel(price_idx, req->side);
    insertOrderToLevel(level, taker, req->side);

    active_orders_[taker->order_id] = taker;
}

void OrderBook::handleCancelOrder(const OrderRequestT* req, bool report_cancelled)
{
    auto it = active_orders_.find(req->order_id);
    if (it == active_orders_.end()) {
        sendResponse(ExecType_Rejected, req->order_id, req->client_id, req->exec_id, req->side, req->p, req->q, RejectCode_OrderNotFound, req->msg_seq_num);
        return;
    }

    Order *o = it->second;

    active_orders_.erase(o->order_id);
    removeOrderFromLevel(o);

    PriceLevel *pl = o->price_level;
    pl->total_qty -= o->qty_remaining;

    const size_t p = index_to_price(pl - price_array_.data());

    if (!pl->order_count)
        removePriceLevel(pl, req->side);

    if (report_cancelled) {
        sendResponse(ExecType_Cancelled, o->order_id, o->client_id, req->exec_id, req->side, p, req->q, RejectCode_None, req->msg_seq_num);
    }
    delete o;
}

void OrderBook::handleModifyOrder(const OrderRequestT* req)
{
    auto it = active_orders_.find(req->order_id);
    if (it == active_orders_.end()) {
        sendResponse(ExecType_Rejected, req->order_id, req->client_id, req->exec_id, req->side, req->p, req->q, RejectCode_OrderNotFound, req->msg_seq_num);
        return;
    }

    Order *o = it->second;
    PriceLevel *pl = o->price_level;
    PriceLevel *target = req->p ? &price_array_[price_to_index(req->p)] : pl;
    int64_t qty_diff = req->q
        ? static_cast<int64_t>(req->q) - static_cast<int64_t>(o->qty_original)
        : 0;

    if (pl == target) {
        if (!qty_diff) {
            sendResponse(ExecType_Replaced, req->order_id, req->client_id, req->exec_id, req->side, req->p, req->q, RejectCode_None, req->msg_seq_num);
            return;
        }

        const uint64_t executed_qty = o->qty_original - o->qty_remaining;
        const uint64_t new_qty = req->q;
        if (new_qty < executed_qty) {
            sendResponse(ExecType_Rejected, req->order_id, req->client_id, req->exec_id, req->side, req->p, req->q, RejectCode_InvalidModify, req->msg_seq_num);
            return;
        }

        pl->total_qty += qty_diff;

        o->qty_remaining = new_qty - executed_qty;
        o->qty_original = new_qty;
        sendResponse(ExecType_Replaced, req->order_id, req->client_id, req->exec_id, req->side, req->p, req->q, RejectCode_None, req->msg_seq_num);
        return;
    }
    // TODO: Use ExecType_Replaced to handle
    // TODO: Check why making qty up still go to short path
    // sendResponse(ExecType_Replaced, req->order_id, req->client_id, req->exec_id, req->side, req->p, req->q, RejectCode_None, req->msg_seq_num);
    handleCancelOrder(req);
    handleNewOrder(req);
}

void OrderBook::processRequest(const OrderRequestT* req)
{
    if (!req) return;

    DTRACE_PROBE1(exchange, ob_req_entry, req->exec_id);

    switch (req->action) {
    case OrderAction_Cancel:
        handleCancelOrder(req);
        return;
    case OrderAction_Modify:
        if (req->p && price_invalid(req->p)) {
            sendResponse(ExecType_Rejected, req->order_id, req->client_id, req->exec_id, req->side, req->p, req->q, RejectCode_PriceInvalid, req->msg_seq_num);
            return;
        }
        handleModifyOrder(req);
        return;
    case OrderAction_New:
        if (req->type != OrderType_Market && price_invalid(req->p)) {
            sendResponse(ExecType_Rejected, req->order_id, req->client_id, req->exec_id, req->side, req->p, req->q, RejectCode_PriceInvalid, req->msg_seq_num);
            return;
        }
        handleNewOrder(req);
        return;
    default:
        sendResponse(ExecType_Rejected, req->order_id, req->client_id, req->exec_id, req->side, req->p, req->q, RejectCode_InvalidAction, req->msg_seq_num);
        return;
    }
}



void OrderBook::insertOrderToLevel(PriceLevel* pl, Order* order, [[maybe_unused]] Side side)
{
    order->price_level = pl;

    Order* old_tail = pl->dummy_tail.prev;

    old_tail->next = order;
    order->prev = old_tail;

    order->next = &pl->dummy_tail;
    pl->dummy_tail.prev = order;

    ++pl->order_count;
    pl->total_qty += order->qty_remaining;
}

void OrderBook::removeOrderFromLevel(Order *o)
{
    o->prev->next = o->next;
    o->next->prev = o->prev;
    o->price_level->order_count -= 1;
}

void OrderBook::removePriceLevel(PriceLevel *pl, Side side)
{
    if (!pl) return;

    const size_t price_idx = pl - price_array_.data();
    const int s = static_cast<int>(side);

    if (pl->better) {
        pl->better->worse = pl->worse;
    } else {
        best_levels_[s] = pl->worse;
    }

    if (pl->worse) {
        pl->worse->better = pl->better;
    }

    active_levels_[s].erase(price_idx);
}

PriceLevel* OrderBook::GetOrCreatePriceLevel(size_t price_idx, Side side)
{
    PriceLevel* level = &price_array_[price_idx];

    if (level->order_count > 0) return level;

    level->better = nullptr;
    level->worse  = nullptr;

    const int s = static_cast<int>(side);

    auto& active = active_levels_[s];

    auto it = active.lower_bound(price_idx);

    PriceLevel* better = nullptr;
    PriceLevel* worse  = nullptr;

    if (side == Side_Buy)
    {
        if (it != active.end())
        {
            better = it->second;
        }

        if (it != active.begin())
        {
            auto prev = it;
            --prev;
            worse = prev->second;
        }
    }
    else
    {
        if (it != active.begin())
        {
            auto prev = it;
            --prev;
            better = prev->second;
        }

        if (it != active.end())
        {
            worse = it->second;
        }
    }

    level->better = better;
    level->worse  = worse;

    if (better)
    {
        better->worse = level;
    }
    else
    {
        best_levels_[s] = level;
    }

    if (worse)
    {
        worse->better = level;
    }

    active[price_idx] = level;

    return level;
}

void OrderBook::sendResponse(ExecType exec_type, uint64_t order_id, uint32_t client_id,
                             uint64_t exec_id, Side side, int64_t p, uint64_t q,
                             RejectCode reject_code, uint64_t orig_msg_seq_num)
{
    if (!response_ring_) return;
    
    uint64_t offset;
    void* ptr = response_ring_->reserve(sizeof(OrderResponseT), offset);
    if (!ptr) return;

    new (ptr) OrderResponseT {
        .exec_type = exec_type,
        .order_id = order_id,
        .client_id = client_id,
        .exec_id = exec_id,
        .symbol_id = this->symbol_id_,
        .side = side,
        .p = p,
        .q = q,
        .reject_code = reject_code,
        .msg_seq_num = 0,
        .orig_msg_seq_num = orig_msg_seq_num
    };
    
    DTRACE_PROBE1(exchange, ob_resp_enqueue, exec_id);
    response_ring_->commit(ptr);
}
