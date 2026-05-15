#include "OrderBook.hpp"
#include <iostream>

using namespace Exchange;

OrderBook::OrderBook(
    int64_t min_step, 
    int64_t price_offset, 
    size_t max_price_levels
)   : min_step_(min_step)
    , price_offset_(price_offset)
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
              << "  min_step       = " << min_step << "\n"
              << "  price_offset   = " << price_offset << "\n"
              << "  max_levels     = " << max_price_levels << "\n"
              << "  price range    ≈ " 
              << (price_offset * min_step) << " ~ " 
              << ((price_offset + max_price_levels) * min_step)
              << "\n";
}

OrderBook::~OrderBook() {}

void OrderBook::send_reject(uint32_t client_id, std::string reason /* TODO: change to enum */)
{
    (void) client_id;
    (void) reason;
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
        handleCancelOrder(req->client_id(), req->order_id());
        return;
    }

    if (price_invalid(req->price())) {
        send_reject(req->client_id(), "Price invalid");
        return;
    }

    size_t price_idx = price_to_index(req->price());

    if (req->action() == OrderAction_Modify) {
        
        // TODO: Manage modify order directly
        // handleModifyOrder(req->client_id(), req->order_id(), price_idx, req->quantity());
        // return;

        handleCancelOrder(req->client_id(), req->order_id()); // temp
    }
    
    handleNewOrder(req->side(), price_idx, createOrder(req));
}

void OrderBook::handleNewOrder(Side side, size_t price_idx, const Order* incoming)
{
    send_acked(incoming);

    int side_int = static_cast<int>(side);

    PriceLevel* oppo = best_levels_[1 - side_int];
    
    if (oppo) 
    {
        while (((ssize_t)price_idx - (ssize_t)oppo->price_idx) * side_int >= 0)
        {
            while (oppo->order_count)
            {
                Order *existing = oppo->dummy_head.next;
                // trade logic
                if (existing->qty_remaining > incoming->qty_remaining) {
                    // new Order is completely filled
                    existing->qty_remaining -= incoming->qty_remaining;
                    send_fill(incoming, existing, incoming->qty_remaining);
                    break;
                }
                // Need to clear existing node and 
                send_fill(incoming, existing, existing->qty_remaining);
                
                // clear
                --oppo->order_count;
                oppo->total_qty -= existing->qty_remaining;
                oppo->dummy_head.next = existing->next;
                delete existing;
                existing->prev = &oppo->dummy_head;
            }
            if (oppo->order_count)
            {
                oppo->dummy_head.next = existing;
                existing->prev = &oppo->dummy_head;
            }
            oppo = oppo->worse;
        }
    }
    
    PriceLevel* same = best_levels_[side_int];
    (void) same;
    // Insert into same
    if (incoming->qty_remaining) {

    } else {

    }
}

void OrderBook::handleCancelOrder(uint32_t client_id, uint64_t order_id)
{
    auto it = active_orders_.find(order_id);
    if (it == active_orders_.end()) {
        send_reject(client_id, "Order Not found");
        // TODO: Should reject request, neet client_id
        return;
    }

    Order* o = it->second;

    removeOrderFromLevel(o);
}

void OrderBook::handleModifyOrder(uint32_t client_id, uint64_t order_id, size_t new_price_idx, uint64_t new_qty)
{
    (void) client_id;
    (void) order_id;
    (void) new_price_idx;
    (void) new_qty;
}

void OrderBook::showL2(size_t depth)
{
    (void) depth;

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
    Order* old_tail = level->dummy_tail.prev;

    old_tail->next = order;
    order->prev = old_tail;

    order->next = &level->dummy_tail;
    level->dummy_tail.prev = order;
}

void OrderBook::removeOrderFromLevel(Order* order)
{
    order->prev->next = order->next;
    order->next->prev = order->prev;

    PriceLevel *pl = order->price_level;
    pl->order_count -= 1;
    pl->total_qty -= order->qty_remaining;
    
    removePriceLevelIfEmpty(order->price_level);
    delete order;
}

void OrderBook::removePriceLevelIfEmpty(PriceLevel* level)
{
    if (level->order_count) return; 

    if (level->worse) level->worse->better = level->worse;
    if (level->better) level->better->worse = level->better;
}
