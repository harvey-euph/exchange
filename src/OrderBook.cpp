#include "OrderBook.hpp"
#include "define.hpp"
#include "LogUtil.hpp"
#include <algorithm>

using namespace Exchange;

namespace {
ExecutionReporter& defaultReporter()
{
    static StdoutExecutionReporter reporter;
    return reporter;
}
}

OrderBook::OrderBook(
    uint64_t symbol_id,
    int64_t min_step, 
    int64_t price_index_offset, 
    size_t max_price_levels,
    ExecutionReporter* reporter
)   : symbol_id_(symbol_id)
    , min_step_(min_step)
    , price_index_offset_(price_index_offset)
    , max_price_levels_(max_price_levels)
    , reporter_(reporter ? reporter : &defaultReporter())
    , l2(L2_UPDATE_RING)
    , l3(L3_UPDATE_RING)
    , price_array_(max_price_levels_)
{
    if (min_step <= 0) {
        throw std::invalid_argument("min_step must be positive");
    }
    if (max_price_levels == 0) {
        throw std::invalid_argument("max_price_levels must be > 0");
    }
}

OrderBook::~OrderBook() {}

Order* OrderBook::createOrder(const OrderRequest* req)
{
    return new Order {
        req->exec_id(),
        req->order_id(),
        req->client_id(),
        req->q(),
        req->q(),
        req->type(),
        nullptr,
        nullptr,
        nullptr,
        req->timestamp()
    };
}

void OrderBook::processRequest(const OrderRequest* req)
{
    if (!req) {
        return;
    }

    // logOrderRequest(req);

    switch (req->action()) {
    case OrderAction_Cancel:
        handleCancelOrder(req);
        return;

    case OrderAction_Modify:
        if (req->p() && price_invalid(req->p())) {
            reporter_->onReject(req, RejectCode_PriceInvalid);
            return;
        }
        handleModifyOrder(req);
        return;

    case OrderAction_New:
        if (req->type() != OrderType_Market && price_invalid(req->p())) {
            reporter_->onReject(req, RejectCode_PriceInvalid);
            return;
        }
        handleNewOrder(req);
        return;

    default:
        reporter_->onReject(req, RejectCode_InvalidAction);
        return;
    }
}

void OrderBook::handleNewOrder(const OrderRequest* req, bool report_ack)
{
    if (active_orders_.count(req->order_id())) {
        reporter_->onReject(req, RejectCode_DuplicateOrderID);
        return;
    }
    
    Order* taker = createOrder(req);
    
    const int side_int = static_cast<int>(req->side());
    const size_t price_idx = (req->type() == OrderType_Market)
    ? (req->side() == Side_Buy ? max_price_levels_ - 1 : 0)
    : price_to_index(req->p());
    
    PriceLevel **oppo = &best_levels_[1^side_int];

    size_t ack_idx = (req->type() == OrderType_Market) ? 0 : price_to_index(req->p());

    while (*oppo && taker->qty_remaining)
    {
        const size_t oppo_idx = (*oppo) - price_array_.data();
        const size_t p = index_to_price(oppo_idx);
        const bool crossed = (req->side() == Side_Buy) ? (price_idx >= oppo_idx) : (price_idx <= oppo_idx);
        if (!crossed) break;

        Order* maker = (*oppo)->dummy_head.next;
        while (maker != &(*oppo)->dummy_tail && taker->qty_remaining)
        {
            const uint64_t qty_fill = std::min(maker->qty_remaining, taker->qty_remaining);

            maker->qty_remaining -= qty_fill;
            taker->qty_remaining -= qty_fill;
            (*oppo)->total_qty   -= qty_fill;
            
            if (report_ack) { 
                reporter_->onAck(req, ack_idx); 
                report_ack = false; 
            }
            reporter_->onFill(taker, maker, req->side(), p, qty_fill);
            
            l3.update(symbol_id_, maker->qty_remaining == 0 ? ExecType_Fill : ExecType_PartialFill,
                maker->order_id, (Exchange::Side)(1^side_int), p, qty_fill
            );
            
            l3.update(symbol_id_, taker->qty_remaining == 0 ? ExecType_Fill : ExecType_PartialFill,
                taker->order_id, (Exchange::Side)side_int, p, qty_fill
            );
            
            if (maker->qty_remaining) continue;
            
            active_orders_.erase(maker->order_id);
            removeOrderFromLevel(maker);
            Order *next = maker->next;
            delete maker;
            maker = next;
        }
        
        l2.update(symbol_id_, (Exchange::Side)(1^side_int), p, (*oppo)->total_qty);

        if (!(*oppo)->order_count)
        {
            removePriceLevel(*oppo, (Side)(1^side_int));
            *oppo = best_levels_[1^side_int];
        }
    }

    if (taker->qty_remaining == 0)
    {
        delete taker;
        return;
    }

    PriceLevel* level = GetOrCreatePriceLevel(price_idx, req->side());

    insertOrderToLevel(level, taker, req->side());

    l3.update(symbol_id_, ExecType_New, 
        taker->order_id, req->side(), index_to_price(price_idx), taker->qty_remaining
    );

    active_orders_[taker->order_id] = taker;

    if (report_ack) { 
        reporter_->onAck(req, ack_idx); 
        report_ack = false; 
    }
}

void OrderBook::handleCancelOrder(const OrderRequest* req, bool report_cancelled)
{
    auto it = active_orders_.find(req->order_id());
    if (it == active_orders_.end()) {
        reporter_->onReject(req, RejectCode_OrderNotFound);
        return;
    }

    Order *o = it->second;

    active_orders_.erase(o->order_id);
    removeOrderFromLevel(o);

    PriceLevel *pl = o->price_level;
    pl->total_qty -= o->qty_remaining;

    const size_t p = index_to_price(pl - price_array_.data());

    l3.update(symbol_id_, ExecType_Cancelled, o->order_id,req->side(), p, o->qty_remaining);
    l2.update(symbol_id_, req->side(), p, pl->total_qty);
   
    if (!pl->order_count) 
        removePriceLevel(pl, req->side());
    
    delete o;
    if (report_cancelled) {
        reporter_->onCancelled(req);
    }
}

void OrderBook::handleModifyOrder(const OrderRequest* req)
{
    auto it = active_orders_.find(req->order_id());
    if (it == active_orders_.end()) {
        reporter_->onReject(req, RejectCode_OrderNotFound);
        return;
    }

    Order *o = it->second;
    PriceLevel *pl = o->price_level;
    PriceLevel *target = req->p() ? &price_array_[price_to_index(req->p())] : pl;
    int64_t qty_diff = req->q()
        ? static_cast<int64_t>(req->q()) - static_cast<int64_t>(o->qty_original)
        : 0;

    if (pl == target) {
        if (!qty_diff) {
            return;
        }

        const uint64_t executed_qty = o->qty_original - o->qty_remaining;
        const uint64_t new_qty = req->q();
        if (new_qty < executed_qty) {
            reporter_->onReject(req, RejectCode_InvalidModify);
            return;
        }

        pl->total_qty += qty_diff;
        
        const size_t p = index_to_price(pl - price_array_.data());
        l3.update(symbol_id_, ExecType_Replaced, o->order_id, req->side(), p, new_qty - executed_qty);
        l2.update(symbol_id_, req->side(), p, pl->total_qty);
        o->qty_remaining = new_qty - executed_qty;
        o->qty_original = new_qty;
        reporter_->onModified(req);
        return;
    }
    handleCancelOrder(req, false);
    handleNewOrder(req, false);
    reporter_->onModified(req);
}

void OrderBook::insertOrderToLevel(PriceLevel* pl, Order* order, Side side)
{
    order->price_level = pl;

    Order* old_tail = pl->dummy_tail.prev;

    old_tail->next = order;
    order->prev = old_tail;

    order->next = &pl->dummy_tail;
    pl->dummy_tail.prev = order;

    ++pl->order_count;
    pl->total_qty += order->qty_remaining;
    
    l2.update(symbol_id_, side, index_to_price(pl - price_array_.data()), pl->total_qty);
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

