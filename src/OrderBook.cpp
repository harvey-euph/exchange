#include "OrderBook.hpp"
#include <iostream>

using namespace Exchange;

namespace {
ExecutionReporter& defaultReporter()
{
    static StdoutExecutionReporter reporter;
    return reporter;
}
}

OrderBook::OrderBook(
    int64_t min_step, 
    int64_t price_index_offset, 
    size_t max_price_levels,
    ExecutionReporter* reporter
)   : min_step_(min_step)
    , price_index_offset_(price_index_offset)
    , max_price_levels_(max_price_levels)
    , reporter_(reporter ? reporter : &defaultReporter())
    , l2("L2_Update_Ring")
    , price_array_(max_price_levels)
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

    std::cout << "[OrderBook] Processing Request: action=" << EnumNameOrderAction(req->action())
              << ", order_id=" << req->order_id()
              << ", client_id=" << req->client_id()
              << ", symbol_id=" << req->symbol_id()
              << ", side=" << (req->side() == Side_Buy ? "Buy" : "Sell")
              << ", price=" << req->p()
              << ", qty=" << req->q()
              << ", exec_id=" << req->exec_id()
              << std::endl;

    reporter_->onRequest(req);

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
        if (price_invalid(req->p())) {
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
    std::cout << "[OrderBook] New Order: order_id=" << req->order_id() << " price=" << req->p() << " qty=" << req->q() << std::endl;
    if (report_ack) {
        reporter_->onAck(req, price_to_index(req->p()));
    }

    Order* incoming = createOrder(req);

    const int side_int = static_cast<int>(req->side());
    const size_t price_idx = price_to_index(req->p());

    PriceLevel **oppo = &best_levels_[1 - side_int];

    while (*oppo && incoming->qty_remaining)
    {
        bool crossed = false;

        if (req->side() == Side_Buy)
        {
            crossed = price_idx >= (size_t)((*oppo) - price_array_.data());
        }
        else
        {
            crossed = price_idx <= (size_t)((*oppo) - price_array_.data());
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
            l2.update(
                1, //symbol_id
                (Exchange::Side)(1 - side_int),
                index_to_price((*oppo) - price_array_.data()), 
                (*oppo)->total_qty
            );

            reporter_->onFill(
                incoming,
                existing,
                index_to_price(existing->price_level - price_array_.data()),
                qty_fill);

            if (!existing->qty_remaining)
            {
                active_orders_.erase(existing->order_id);
                removeOrderFromLevel(existing);
                Order *next = existing->next;
                delete existing;
                existing = next;
            }
        }
        if (!(*oppo)->order_count)
        {
            active_levels_[1 - side_int].erase(*oppo - price_array_.data());
            (*oppo) = (*oppo)->worse;
            if (*oppo) (*oppo)->better = nullptr;
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

    insertOrderToLevel(level, incoming);

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
    l2.update(
        1, //symbol_id
        req->side(),
        index_to_price(pl - price_array_.data()), 
        pl->total_qty
    );
   
    if (!pl->order_count) 
        removePriceLevel(pl);
    
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
        
        l2.update(
            1, //symbol_id
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
    (void) depth;

    printf("\n====================================================\n");

    for (auto [k, v] : active_levels_[1])
    {
        std::cout << k << ' ';
    }
    std::cout << '\n';

    for (size_t i = price_array_.size()-1; i < price_array_.size(); --i)
    {
        if (
            price_array_[i].order_count || price_array_[i].total_qty || 
            price_array_[i].dummy_head.next != &price_array_[i].dummy_tail ||
            price_array_[i].dummy_tail.prev != &price_array_[i].dummy_head
        )
            //  || price_array_[i].better || price_array_[i].worse)
        {
            if (&price_array_[i] == best_levels_[1]) {
                printf("[A1] ");
            } else if (&price_array_[i] == best_levels_[0]) {
                printf("------------------------------------------------------\n[B1] ");
            } else {
                printf("     ");
            }
            auto better = price_array_[i].better? price_array_[i].better : price_array_.data();
            auto worse  = price_array_[i].worse?  price_array_[i].worse  : price_array_.data();
            printf("[%6lu] qty= %6lu nr= %3lu better=[%6lu] worse=[%6lu]\n", 
                i, 
                price_array_[i].total_qty,
                price_array_[i].order_count,
                (uint64_t)(better - price_array_.data()),
                (uint64_t)(worse - price_array_.data())
            );
        }
    }
    for (auto [k, v] : active_levels_[0])
    {
        std::cout << k << ' ';
    }
    std::cout << '\n';

    printf("====================================================\n\n");
}

void OrderBook::insertOrderToLevel(PriceLevel* pl, Order* order)
{
    order->price_level = pl;

    Order* old_tail = pl->dummy_tail.prev;

    old_tail->next = order;
    order->prev = old_tail;

    order->next = &pl->dummy_tail;
    pl->dummy_tail.prev = order;

    ++pl->order_count;
    pl->total_qty += order->qty_remaining;
    
    int s = 1;
    if (best_levels_[0] && pl <= best_levels_[0]) {
        s = 0;
    }
    l2.update(
        1, //symbol_id
        (Exchange::Side)s,
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

void OrderBook::removePriceLevel(PriceLevel *pl)
{
    if (!pl) return;

    const size_t price_idx = pl - price_array_.data();

    int side = -1;
    for (int s = 0; s < 2; ++s) {
        auto it = active_levels_[s].find(price_idx);
        if (it != active_levels_[s].end() && it->second == pl) {
            side = s;
            break;
        }
    }

    if (side == -1) {
        return;
    }

    if (pl->better) {
        pl->better->worse = pl->worse;
    } else {
        best_levels_[side] = pl->worse;
    }

    if (pl->worse) {
        pl->worse->better = pl->better;
    }

    active_levels_[side].erase(price_idx);
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
