#include "gtest/gtest.h"
#include "../include/OrderBook.hpp"

namespace Exchange {

class OrderBookTest : public ::testing::Test
{
protected:
    void SetUp() override {
        orderbook = std::make_unique<OrderBook>(1, 10000, 65536);
    }

    std::unique_ptr<OrderBook> orderbook;

    const OrderRequest* CreateRequest(
        OrderAction action,
        uint64_t exec_id,
        uint64_t order_id,
        uint32_t client_id,
        Side side,
        OrderType type,
        int64_t price,
        uint64_t quantity,
        uint64_t visible_qty = 0)
    {
        static flatbuffers::FlatBufferBuilder fbb(1024);
        fbb.Clear();

        auto req = CreateOrderRequest(
            fbb,
            action,
            exec_id,
            order_id,
            client_id,
            1,                    // symbol_id
            side,
            type,
            price,
            quantity,
            visible_qty,
            1000000000ULL
        );

        fbb.Finish(req);
        return GetOrderRequest(fbb.GetBufferPointer());
    }

    void SetupInitialBook() 
    {
        std::vector<int64_t> bid_prices = {10500, 10400, 10300, 10200, 10100};
        for (int64_t p : bid_prices) {
            for (int i = 0; i < 3; ++i) {
                const OrderRequest* req = CreateRequest(
                    OrderAction_New, 10000+i, 20000+p+i, 1,
                    Side_Buy, OrderType_Limit, p, 100);
                orderbook->processRequest(req);
            }
        }

        std::vector<int64_t> ask_prices = {10600, 10700, 10800, 10900, 11000};
        for (int64_t p : ask_prices) {
            for (int i = 0; i < 3; ++i) {
                const OrderRequest* req = CreateRequest(
                    OrderAction_New, 30000+i, 40000+p+i, 2,
                    Side_Sell, OrderType_Limit, p, 100);
                orderbook->processRequest(req);
            }
        }
        orderbook->showL2();
    }
};

// ==================== InsertBidAsk ====================

TEST_F(OrderBookTest, InsertBidAsk)
{
    SetupInitialBook();

    EXPECT_EQ(orderbook->active_levels_[0].size(), 5);
    EXPECT_EQ(orderbook->active_levels_[1].size(), 5);

    EXPECT_NE(orderbook->best_levels_[0], nullptr);
    EXPECT_NE(orderbook->best_levels_[1], nullptr);

    EXPECT_EQ(orderbook->best_levels_[0]->total_qty, 300);
    EXPECT_EQ(orderbook->best_levels_[1]->total_qty, 300);

    std::cout << "Initial OrderBook setup completed successfully.\n";
}

// ==================== CancelAndModify ====================

TEST_F(OrderBookTest, CancelAndModify)
{
    SetupInitialBook();

    // Cancel Best Bid 的其中一筆訂單 (order_id = 20000)
    const OrderRequest* cancel_req = CreateRequest(
        OrderAction_Cancel, 0, 20000, 1, Side_Buy, OrderType_Limit, 0, 0);
    orderbook->processRequest(cancel_req);

    EXPECT_EQ(orderbook->active_orders_.count(20000), 0);

    // Modify 另一筆訂單：改價 + 改量
    const OrderRequest* modify_req = CreateRequest(
        OrderAction_Modify, 0, 20001, 1, Side_Buy, OrderType_Limit, 10450, 250);
    orderbook->processRequest(modify_req);

    EXPECT_EQ(orderbook->active_orders_.count(20001), 1);
}

// ==================== MatchSingleLayer ====================

TEST_F(OrderBookTest, MatchSingleLayer)
{
    SetupInitialBook();

    // 在 Best Ask 價格 (10600) 掛一個大買單，應該只匹配單一價格層
    const OrderRequest* big_bid = CreateRequest(
        OrderAction_New, 9991, 9991, 1, Side_Buy, OrderType_Limit, 10600, 500);
    orderbook->processRequest(big_bid);

    // 檢查是否發生部分成交
    EXPECT_EQ(orderbook->active_orders_.count(9991), 1);  // 應該剩下部分數量
}

// ==================== MatchingMultiLayer ====================

TEST_F(OrderBookTest, MatchingMultiLayer)
{
    SetupInitialBook();

    // 發一個大賣單，價格很低，應該吃穿多層 Bid
    const OrderRequest* big_ask = CreateRequest(
        OrderAction_New, 9992, 9992, 2, Side_Sell, OrderType_Limit, 10100, 2000);
    orderbook->processRequest(big_ask);

    // 預期最上面幾層 Bid 應該被吃掉
    // (因為每個 Bid 層只有 300，總共 5 層 = 1500，所以會吃穿 4 層以上)
    EXPECT_LT(orderbook->active_levels_[0].size(), 5);  // Bid 層數應該減少
}

} // namespace Exchange