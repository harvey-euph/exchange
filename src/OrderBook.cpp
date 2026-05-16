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

void OrderBook::send_reject(uint32_t client_id, std::string reason /* TODO: change to enum */)
{
    printf("[REJECT] client_id=%u reason=%s\n",
           client_id,
           reason.c_str());
}

void OrderBook::send_acked(const Order* incoming)
{
    printf("[ACK] order_id=%lu client_id=%u type=%d qty=%lu ts=%lu\n",
           incoming->order_id,
           incoming->client_id,
           static_cast<int>(incoming->type),
           incoming->qty_original,
           incoming->timestamp);
}

void OrderBook::send_fill(const Order* incoming,
                          const Order* existing,
                          size_t price_idx,
                          uint64_t qty_fill)
{
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
        // Unexpected situation
        return;
    }

    printOrderRequest(req);

    if (req->action() == OrderAction_Cancel) {
        handleCancelOrder(req);
        return;
    }

    if (price_invalid(req->price())) {
        send_reject(req->client_id(), "Price invalid");
        return;
    }

    if (req->action() == OrderAction_Modify) {
        
        // TODO: Manage modify order directly
        // handleModifyOrder(req);
        // return;

        handleCancelOrder(req); // temp
    }
    
    handleNewOrder(req);
}

void OrderBook::handleNewOrder(const OrderRequest* req)
{
    Order* incoming = createOrder(req);

    send_acked(incoming);

    const int side_int = static_cast<int>(req->side());
    const size_t price_idx = price_to_index(req->price());

    PriceLevel* oppo = best_levels_[1 - side_int];

    //
    // MATCH
    //
    while (oppo && incoming->qty_remaining)
    {
        bool crossed = false;

        if (req->side() == Side_Buy)
        {
            crossed = price_idx >= oppo->price_idx;
        }
        else
        {
            crossed = price_idx <= oppo->price_idx;
        }

        if (!crossed)
            break;

        Order* existing = oppo->dummy_head.next;

        //
        // walk this price level
        //
        while (existing != &oppo->dummy_tail &&
               incoming->qty_remaining)
        {
            Order* next = existing->next;

            const uint64_t qty_fill =
                std::min(existing->qty_remaining,
                         incoming->qty_remaining);

            //
            // apply fill
            //
            existing->qty_remaining -= qty_fill;
            incoming->qty_remaining -= qty_fill;

            oppo->total_qty -= qty_fill;

            send_fill(
                incoming,
                existing,
                oppo->price_idx,
                qty_fill);

            //
            // maker fully filled
            //
            if (existing->qty_remaining == 0)
            {
                active_orders_.erase(existing->order_id);

                removeOrderFromLevel(existing);
                printf("\n%s Deleting existing: %p\n", __func__, existing);
                delete existing;
                existing = next;
            }
        }

        best_levels_[1 - side_int] = oppo->worse;
        removePriceLevelIfEmpty(oppo);
        oppo = best_levels_[1 - side_int];
    }

    //
    // taker fully filled
    //
    if (incoming->qty_remaining == 0)
    {
        printf("\n%s Deleting incoming: %p\n", __func__, incoming);
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

void OrderBook::handleCancelOrder(const OrderRequest* req)
{
    auto it = active_orders_.find(req->order_id());
    if (it == active_orders_.end()) {
        send_reject(req->client_id(), "Order Not found");
        // TODO: Should reject request, neet client_id
        return;
    }

    Order* o = it->second;

    removeOrderFromLevel(o);
    removePriceLevelIfEmpty(o->price_level);
    
    printf("\n%s Deleting cancelled: %p\n", __func__, o);
    delete o;
}

void OrderBook::handleModifyOrder(const OrderRequest* req)
{
    (void) req;
}

void OrderBook::showL2(size_t depth)
{
    auto collect = [&](Side side)
    {
        std::vector<PriceLevel*> levels;

        const int s = static_cast<int>(side);
        PriceLevel* cur = best_levels_[s];

        while (cur && levels.size() < depth)
        {
            levels.push_back(cur);
            cur = cur->worse;
        }

        return levels;
    };

    //
    // ASK: want worst -> best
    //
    {
        auto asks = collect(Side_Sell);

        std::cout << "=== ASK ===\n";

        // reverse print
        for (int i = (int)asks.size() - 1; i >= 0; --i)
        {
            auto* lv = asks[i];

            std::cout << "[A" << (i + 1) << "] "
                      << "price_idx=" << lv->price_idx
                      << " qty=" << lv->total_qty
                      << " orders=" << lv->order_count
                      << "\n";
        }

        if (asks.empty())
            std::cout << "(empty)\n";

        std::cout << "\n";
    }

    //
    // BID: best -> worse
    //
    {
        auto bids = collect(Side_Buy);

        std::cout << "=== BID ===\n";

        for (size_t i = 0; i < bids.size(); ++i)
        {
            auto* lv = bids[i];

            std::cout << "[B" << (i + 1) << "] "
                      << "price_idx=" << lv->price_idx
                      << " qty=" << lv->total_qty
                      << " orders=" << lv->order_count
                      << "\n";
        }

        if (bids.empty())
            std::cout << "(empty)\n";

        std::cout << "\n";
    }
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

void OrderBook::removeOrderFromLevel(Order* order)
{
    order->prev->next = order->next;
    order->next->prev = order->prev;

    PriceLevel *pl = order->price_level;
    pl->order_count -= 1;
    pl->total_qty -= order->qty_remaining;
}

void OrderBook::removePriceLevelIfEmpty(PriceLevel* level)
{
    if (level->order_count) return; 

    if (level->worse) level->worse->better = level->worse;
    if (level->better) level->better->worse = level->better;
}

PriceLevel* OrderBook::GetOrCreatePriceLevel(size_t price_idx, Side side)
{
    PriceLevel* level = &price_array_[price_idx];

    if (level->order_count > 0) return level;

    level->price_idx   = price_idx;
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