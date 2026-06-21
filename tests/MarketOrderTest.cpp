#include "gtest/gtest.h"
#include "OrderBook.hpp"

namespace Exchange {

class MarketOrderTest : public ::testing::Test
{
protected:
    void SetUp() override {
        // symbol_id=1, min_step=10, price_offset=1000, max_levels=1000
        // Price range: 10000 ~ 20000
        orderbook = std::make_unique<OrderBook>(1, 10, 1000, 1000);
    }

    std::unique_ptr<OrderBook> orderbook;
    uint64_t exec_id = 0;

    OrderRequestT CreateRequest(
        OrderAction act,
        uint64_t order_id,
        uint32_t client_id,
        Side side,
        OrderType type,
        int64_t price,
        uint64_t quantity)
    {
        OrderRequestT req;
        req.action = act;
        req.exec_id = exec_id++;
        req.order_id = order_id;
        req.client_id = client_id;
        req.symbol_id = 1;
        req.side = side;
        req.type = type;
        req.p = price;
        req.q = quantity;
        req.visible_qty = 0;
        req.timestamp = 1000000000ULL;
        req.msg_seq_num = 0;
        return req;
    }

    void SetupInitialBook() 
    {
        // Bids: 10500, 10400, 10300
        std::vector<int64_t> bid_prices = {10500, 10400, 10300};
        for (int64_t p : bid_prices) {
            auto req = CreateRequest(
                OrderAction_New, 20000+p, 1,
                Side_Buy, OrderType_Limit, p, 100);
            orderbook->processRequest(&req);
        }

        // Asks: 10600, 10700, 10800
        std::vector<int64_t> ask_prices = {10600, 10700, 10800};
        for (int64_t p : ask_prices) {
            auto req = CreateRequest(
                OrderAction_New, 40000+p, 2,
                Side_Sell, OrderType_Limit, p, 100);
            orderbook->processRequest(&req);
        }
    }
};

TEST_F(MarketOrderTest, MarketBuyMatchesAllAsks)
{
    SetupInitialBook();

    // Market Buy 250 shares
    // Should match:
    // 100 @ 10600
    // 100 @ 10700
    // 50  @ 10800
    auto market_buy = CreateRequest(
        OrderAction_New, 9999, 3, Side_Buy, OrderType_Market, 0, 250);
    
    orderbook->processRequest(&market_buy);

    // Verify:
    // Asks 10600, 10700 should be gone.
    // Ask 10800 should have 50 left.
    EXPECT_EQ(orderbook->active_levels_[1].count(orderbook->price_to_index(10600)), 0);
    EXPECT_EQ(orderbook->active_levels_[1].count(orderbook->price_to_index(10700)), 0);
    
    auto it = orderbook->active_levels_[1].find(orderbook->price_to_index(10800));
    ASSERT_NE(it, orderbook->active_levels_[1].end());
    EXPECT_EQ(it->second->total_qty, 50ULL);

    // Market order should not be in book (fully filled)
    EXPECT_EQ(orderbook->active_orders_.count(9999), 0);
}

TEST_F(MarketOrderTest, MarketSellMatchesAllBids)
{
    SetupInitialBook();

    // Market Sell 250 shares
    // Should match:
    // 100 @ 10500
    // 100 @ 10400
    // 50  @ 10300
    auto market_sell = CreateRequest(
        OrderAction_New, 8888, 4, Side_Sell, OrderType_Market, 0, 250);
    
    orderbook->processRequest(&market_sell);

    // Verify:
    // Bids 10500, 10400 should be gone.
    // Bid 10300 should have 50 left.
    EXPECT_EQ(orderbook->active_levels_[0].count(orderbook->price_to_index(10500)), 0);
    EXPECT_EQ(orderbook->active_levels_[0].count(orderbook->price_to_index(10400)), 0);
    
    auto it = orderbook->active_levels_[0].find(orderbook->price_to_index(10300));
    ASSERT_NE(it, orderbook->active_levels_[0].end());
    EXPECT_EQ(it->second->total_qty, 50ULL);

    // Market order should not be in book (fully filled)
    EXPECT_EQ(orderbook->active_orders_.count(8888), 0);
}

TEST_F(MarketOrderTest, MarketOrderRestInBookAtExtremePrice)
{
    SetupInitialBook();

    // Market Buy 400 shares (only 300 available in asks)
    auto market_buy = CreateRequest(
        OrderAction_New, 7777, 5, Side_Buy, OrderType_Market, 0, 400);
    
    orderbook->processRequest(&market_buy);

    // All asks should be gone
    EXPECT_EQ(orderbook->active_levels_[1].size(), 0);

    // Market order should be in book at price_idx = max_price_levels_ - 1
    EXPECT_EQ(orderbook->active_orders_.count(7777), 1);
    Order* o = orderbook->active_orders_[7777];
    EXPECT_EQ(o->qty_remaining, 100ULL);
    
    size_t expected_idx = 1000 - 1; // max_levels - 1
    EXPECT_EQ(o->price_level - &orderbook->price_array_[0], expected_idx);
}

} // namespace Exchange
