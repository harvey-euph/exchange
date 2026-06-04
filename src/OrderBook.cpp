#include "OrderBook.hpp"
#include "define.hpp"
#include "LogUtil.hpp"
#include <iostream>
#include <cstdio>
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

    std::cout << "[OrderBook] Initialized with:\n"
              << "  min_step           = " << min_step << "\n"
              << "  price_index_offset = " << price_index_offset_ << "\n"
              << "  max_levels         = " << max_price_levels << "\n"
              << "  price range        ≈ " 
              << index_to_price(price_index_offset_) << " ~ " 
              << index_to_price(price_index_offset_ + max_price_levels)
              << "\n";
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

    logOrderRequest(req);

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

    if (report_ack) {
        size_t ack_idx = (req->type() == OrderType_Market) ? 0 : price_to_index(req->p());
        reporter_->onAck(req, ack_idx);
    }

    Order* incoming = createOrder(req);

    const int side_int = static_cast<int>(req->side());
    const size_t price_idx = (req->type() == OrderType_Market)
        ? (req->side() == Side_Buy ? max_price_levels_ - 1 : 0)
        : price_to_index(req->p());

    PriceLevel **oppo = &best_levels_[1 - side_int];

    while (*oppo && incoming->qty_remaining)
    {
        bool crossed = false;
        const size_t oppo_idx = (*oppo) - price_array_.data();

        if (req->side() == Side_Buy)
        {
            crossed = price_idx >= oppo_idx;
        }
        else
        {
            crossed = price_idx <= oppo_idx;
        }

        if (!crossed)
            break;

        Order* existing = (*oppo)->dummy_head.next;

        while (existing != &(*oppo)->dummy_tail &&
               incoming->qty_remaining)
        {
            const uint64_t qty_fill =
                std::min(existing->qty_remaining,
                         incoming->qty_remaining);

            existing->qty_remaining -= qty_fill;
            incoming->qty_remaining -= qty_fill;
            (*oppo)->total_qty      -= qty_fill;
            
            reporter_->onFill(
                incoming,
                existing,
                req->side(),
                index_to_price(oppo_idx),
                qty_fill);

            l3.update(
                symbol_id_,
                existing->qty_remaining == 0 ? ExecType_Fill : ExecType_PartialFill,
                existing->order_id,
                (Exchange::Side)(1 - side_int),
                index_to_price(oppo_idx),
                qty_fill
            );

            l3.update(
                symbol_id_,
                incoming->qty_remaining == 0 ? ExecType_Fill : ExecType_PartialFill,
                incoming->order_id,
                (Exchange::Side)side_int,
                index_to_price(oppo_idx),
                qty_fill
            );
            
            if (!existing->qty_remaining)
            {
                active_orders_.erase(existing->order_id);
                removeOrderFromLevel(existing);
                Order *next = existing->next;
                delete existing;
                existing = next;
            }
        }
        
        // Update L2 once per price level fill
        l2.update(
            symbol_id_,
            (Exchange::Side)(1 - side_int),
            index_to_price(oppo_idx), 
            (*oppo)->total_qty
        );

        if (!(*oppo)->order_count)
        {
            removePriceLevel(*oppo, (Side)(1-side_int));
            *oppo = best_levels_[1-side_int];
        }
    }

    //
    // taker fully filled
    //
    if (incoming->qty_remaining == 0)
    {
        delete incoming;
        return;
    }

    //
    // rest into book
    //
    PriceLevel* level = GetOrCreatePriceLevel(price_idx, req->side());

    insertOrderToLevel(level, incoming, req->side());

    l3.update(
        symbol_id_,
        ExecType_New,
        incoming->order_id,
        req->side(),
        index_to_price(price_idx),
        incoming->qty_remaining
    );

    active_orders_[incoming->order_id] = incoming;
}

void OrderBook::handleCancelOrder(const OrderRequest* req, bool report_cancelled)
{
    std::cout << "[OrderBook] Cancel Order: order_id=" << req->order_id() << std::endl;
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

    l3.update(
        symbol_id_,
        ExecType_Cancelled,
        o->order_id,
        req->side(),
        index_to_price(pl - price_array_.data()),
        o->qty_remaining
    );

    l2.update(
        symbol_id_,
        req->side(),
        index_to_price(pl - price_array_.data()), 
        pl->total_qty
    );
   
    if (!pl->order_count) 
        removePriceLevel(pl, req->side());
    
    delete o;
    if (report_cancelled) {
        reporter_->onCancelled(req);
    }
}

void OrderBook::handleModifyOrder(const OrderRequest* req)
{
    std::cout << "[OrderBook] Modify Order: order_id=" << req->order_id() << " new_price=" << req->p() << " new_qty=" << req->q() << std::endl;
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
            std::cout << "Condition unchanged.\n";
            return;
        }

        const uint64_t executed_qty = o->qty_original - o->qty_remaining;
        const uint64_t new_qty = req->q();
        if (new_qty < executed_qty) {
            reporter_->onReject(req, RejectCode_InvalidModify);
            return;
        }

        pl->total_qty += qty_diff;
        
        l3.update(
            symbol_id_,
            ExecType_Replaced,
            o->order_id,
            req->side(),
            index_to_price(pl - price_array_.data()),
            new_qty - executed_qty
        );

        l2.update(
            symbol_id_,
            req->side(),
            index_to_price(pl - price_array_.data()), 
            pl->total_qty
        );
        o->qty_remaining = new_qty - executed_qty;
        o->qty_original = new_qty;
        reporter_->onModified(req);
        return;
    }
    handleCancelOrder(req, false);
    handleNewOrder(req, false);
    reporter_->onModified(req);
}

void OrderBook::showL2(size_t depth)
{
    // \033[H moves cursor to top-left (home)
    std::cout << "\033[H";
    printf("==================== Order Book ====================\n");

    // Asks: High to Low (Closest to spread at bottom)
    std::vector<std::pair<size_t, PriceLevel*>> asks;
    for (auto const& pair : active_levels_[1]) {
        asks.push_back(pair);
    }
    
    size_t show_asks = std::min(asks.size(), depth);
    for (int i = (int)show_asks - 1; i >= 0; --i) {
        size_t idx = asks[i].first;
        PriceLevel* pl = asks[i].second;
        if (pl == best_levels_[1]) printf("[A1] "); else printf("     ");
        printf("[%6lu] price=%10ld qty=%8lu nr=%3lu\n", 
               idx, index_to_price(idx), pl->total_qty, pl->order_count);
    }

    printf("----------------------------------------------------\n");

    // Bids: High to Low (Closest to spread at top)
    size_t count = 0;
    for (auto it = active_levels_[0].rbegin(); it != active_levels_[0].rend() && count < depth; ++it, ++count) {
        size_t idx = it->first;
        PriceLevel* pl = it->second;
        if (pl == best_levels_[0]) printf("[B1] "); else printf("     ");
        printf("[%6lu] price=%10ld qty=%8lu nr=%3lu\n", 
               idx, index_to_price(idx), pl->total_qty, pl->order_count);
    }

    printf("====================================================\n");
    // Clear anything below the book in case depth decreased
    std::cout << "\033[J" << std::flush;
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
    
    l2.update(
        symbol_id_,
        side,
        index_to_price(pl - price_array_.data()), 
        pl->total_qty
    );
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

std::vector<OrderBook::SnapshotOrder> OrderBook::getL3Snapshot() const
{
    std::vector<SnapshotOrder> snapshot;
    for (int side = 0; side < 2; ++side) {
        for (auto const& [price_idx, level] : active_levels_[side]) {
            Order* o = level->dummy_head.next;
            while (o != &level->dummy_tail) {
                snapshot.push_back({
                    o->order_id,
                    static_cast<Side>(side),
                    static_cast<int64_t>(index_to_price(price_idx)),
                    o->qty_remaining
                });
                o = o->next;
            }
        }
    }
    return snapshot;
}
