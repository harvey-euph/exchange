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
    , price_array_(max_price_levels, nullptr)
    , bid_head_(nullptr)
    , ask_head_(nullptr)
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
              << (price_offset / 10000.0) << " ~ " 
              << ((price_offset + max_price_levels * min_step) / 10000.0) 
              << "\n";
}

OrderBook::~OrderBook() {}

void OrderBook::processRequest(const OrderRequest* req)
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

void OrderBook::showL2(size_t depth)
{
    (void) depth;

}