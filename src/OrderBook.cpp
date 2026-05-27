#include "OrderBook.hpp"
#include <iostream>

using namespace Exchange;

OrderBook::OrderBook(
    int64_t min_step, 
    int64_t price_index_offset, 
    size_t max_price_levels
)   : min_step_(min_step)
    , price_index_offset_(price_index_offset)
    , max_price_levels_(max_price_levels)
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

void OrderBook::send_reject(const OrderRequest* req, std::string reason /* TODO: change to enum */)
{
    printf("[REJECT] client_id=%u reason=%s\n", req->client_id(), reason.c_str());
}

void OrderBook::send_acked(const OrderRequest* req)
{
    printf("[ACK] order_id=%lu client_id=%u type=%d price_idx=%lu qty=%lu ts=%lu\n",
           req->order_id(),
           req->client_id(),
           static_cast<int>(req->type()),
           price_to_index(req->price()),
           req->quantity(),
           req->timestamp());
}

void OrderBook::send_fill(const Order* incoming,
                          const Order* existing,
                          uint64_t qty_fill)
{
    size_t price_idx = existing->price_level - price_array_.data();

    printf("[FILL] taker_order=%lu maker_order=%lu "
           "price_idx=%zu qty=%lu "
           "taker_remaining=%lu maker_remaining=%lu\n",
           incoming->order_id,
           existing->order_id,
           price_idx,
           qty_fill,
           incoming->qty_remaining,
           existing->qty_remaining);
}

Order* OrderBook::createOrder(const OrderRequest* req)
{
    return new Order {
        req->order_id(),
        req->client_id(),
        req->quantity(),
        req->quantity(),
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

    printOrderRequest(req);

    if (req->action() == OrderAction_Cancel) {
        handleCancelOrder<Mode::Normal>(req);
        return;
    }

    if (price_invalid(req->price())) {
        send_reject(req, "Price invalid");
        return;
    }

    if (req->action() == OrderAction_Modify) {
        handleModifyOrder(req);
        return;
    }
    
    handleNewOrder<Mode::Normal>(req);
}

template <OrderBook::Mode m>
void OrderBook::handleNewOrder(const OrderRequest* req)
{
    send_acked(req);

    Order* incoming = createOrder(req);

    const int side_int = static_cast<int>(req->side());
    const size_t price_idx = price_to_index(req->price());

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

            send_fill(incoming, existing, qty_fill);

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

template <OrderBook::Mode m>
void OrderBook::handleCancelOrder(const OrderRequest* req)
{
    auto it = active_orders_.find(req->order_id());
    if (it == active_orders_.end()) {
        send_reject(req, "Order Not found");
        return;
    }
    // send_cancelled(req);

    Order *o = it->second;

    active_orders_.erase(o->order_id);
    removeOrderFromLevel(o);

    PriceLevel *pl = o->price_level;
    pl->total_qty -= o->qty_remaining;
   
    if (!pl->order_count) 
        removePriceLevel(pl);
    
    delete o;
}

void OrderBook::handleModifyOrder(const OrderRequest* req)
{
    auto it = active_orders_.find(req->order_id());
    if (it == active_orders_.end()) {
        send_reject(req, "Order Not found");
        return;
    }

    Order *o = it->second;
    PriceLevel *pl = o->price_level;
    PriceLevel *target = req->price() ? &price_array_[price_to_index(req->price())] : pl;
    int64_t qty_diff = req->quantity()? 
                        (int64_t)req->quantity() - (int64_t)o->qty_original : 0;

    if (pl == target)
    {
        if (!qty_diff)
        {
            std::cout << "Condition unchanged.\n";
        } 
        else if (qty_diff < 0) 
        {
            if ((uint64_t)-qty_diff < o->qty_remaining) {
                // cancel remaining
                return;
            }
            o->qty_remaining += qty_diff;
            o->qty_original  += qty_diff;
        }
        return;
    }
    handleCancelOrder<Mode::Modify>(req);
    handleNewOrder<Mode::Modify>(req);
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

void OrderBook::printOrderRequest(const OrderRequest* req)
{
    if (!req) return;

    std::cout << "[Order " << req->order_id() << "] "
              << EnumNameSide(req->side()) << " "
              << EnumNameOrderAction(req->action()) << " "
              << "Price:" << req->price() 
              << " Qty:" << req->quantity()
              << " Vis:" << req->visible_qty()
              << " Client:" << req->client_id()
              << "\n";
}


void OrderBook::insertOrderToLevel(PriceLevel* level, Order* order)
{
    order->price_level = level;

    Order* old_tail = level->dummy_tail.prev;

    old_tail->next = order;
    order->prev = old_tail;

    order->next = &level->dummy_tail;
    level->dummy_tail.prev = order;

    ++level->order_count;
    level->total_qty += order->qty_remaining;
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

// void OrderBook::checkPriceLevelConsistent(size_t price_index)
// {
//     PriceLevel *pl = &price_array_[price_index];
//     if (pl->dummy_tail.next || pl->dummy_head.prev) {
//         std::cerr << "Error, dummy_tail.next or dummy_head.prev exists.\n";
//     }
//     if (pl->order_count || pl->total_qty ||
//         pl->dummy_head.next != &pl->dummy_tail ||
//         pl->dummy_tail.prev != &pl->dummy_head ||
//     ) {}
// }

PriceLevel* OrderBook::GetOrCreatePriceLevel(size_t price_idx, Side side)
{
    PriceLevel* level = &price_array_[price_idx];

    if (level->order_count > 0) return level;

    // level->total_qty   = 0;
    // level->order_count = 0;

    // level->dummy_head.next = &level->dummy_tail;
    // level->dummy_tail.prev = &level->dummy_head;

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
