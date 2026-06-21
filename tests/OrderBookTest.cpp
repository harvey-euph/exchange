#include "gtest/gtest.h"
#include "OrderBook.hpp"

namespace Exchange {

class OrderBookTest : public ::testing::Test
{
protected:
    void SetUp() override {
        orderbook = std::make_unique<OrderBook>(1, 1, 10000, 1024);
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
        uint64_t quantity,
        uint64_t visible_qty = 0)
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
        req.visible_qty = visible_qty;
        req.timestamp = 1000000000ULL;
        req.msg_seq_num = 0;
        return req;
    }

    void SetupInitialBook() 
    {
        // int64_t basePrice = 10000;
        std::vector<int64_t> bid_prices = {10500, 10400, 10300, 10200, 10100};
        for (int64_t p : bid_prices) {
            for (int i = 0; i < 3; ++i) {
                auto req = CreateRequest(
                    OrderAction_New, 20000+p+i, 1,
                    Side_Buy, OrderType_Limit, p, 100);
                orderbook->processRequest(&req);
            }
        }

        std::vector<int64_t> ask_prices = {10600, 10700, 10800, 10900, 11000};
        for (int64_t p : ask_prices) {
            for (int i = 0; i < 3; ++i) {
                auto req = CreateRequest(
                    OrderAction_New, 40000+p+i, 2,
                    Side_Sell, OrderType_Limit, p, 100);
                orderbook->processRequest(&req);
            }
        }
    }
    
protected:

    void PerformMixedOperations(int count, uint64_t base_order_id)
    {
        std::vector<uint64_t> active_order_ids;  // 追蹤目前存在的訂單

        for (int i = 0; i < count; ++i)
        {
            int op_type = rand() % 100;  // 機率控制

            if (op_type < 45 || active_order_ids.empty()) 
            {
                // 45% 機會 New Order
                uint64_t oid = base_order_id + i;
                int64_t price = 10000 + (rand() % 400) * 10;  // 10000 ~ 14000 之間
                Side side = (rand() % 2 == 0) ? Side_Buy : Side_Sell;
                uint64_t qty = 50 + (rand() % 200) * 10;     // 50 ~ 2050

                auto req = CreateRequest(
                    OrderAction_New, oid, 1, side, OrderType_Limit, price, qty);

                orderbook->processRequest(&req);
                active_order_ids.push_back(oid);
            }
            else if (op_type < 75)
            {
                // 30% 機會 Cancel
                size_t idx = rand() % active_order_ids.size();
                uint64_t oid = active_order_ids[idx];

                auto req = CreateRequest(
                    OrderAction_Cancel, oid, 1, Side_Buy, OrderType_Limit, 0, 0);

                orderbook->processRequest(&req);
                active_order_ids.erase(active_order_ids.begin() + idx);
            }
            else
            {
                // 25% 機會 Modify（改價或改量）
                if (active_order_ids.empty()) continue;

                size_t idx = rand() % active_order_ids.size();
                uint64_t oid = active_order_ids[idx];

                // 隨機決定改價還是改量
                if (rand() % 2 == 0)
                {
                    // Modify Price + Qty
                    int64_t new_price = 10000 + (rand() % 400) * 10;
                    uint64_t new_qty = 50 + (rand() % 150) * 10;

                    auto req = CreateRequest(
                        OrderAction_Modify, oid, 1, Side_Buy, OrderType_Limit, new_price, new_qty);

                    orderbook->processRequest(&req);
                }
                else
                {
                    // 只改數量
                    uint64_t new_qty = 50 + (rand() % 200) * 10;
                    auto req = CreateRequest(
                        OrderAction_Modify, oid  , 1, Side_Buy, OrderType_Limit, 0, new_qty);

                    orderbook->processRequest(&req);
                }
            }
        }
    }

    void CheckMemoryConsistency()
    {
        size_t orders_in_levels = 0;

        for (int side = 0; side < 2; ++side)
        {
            for (const auto& [idx, pl] : orderbook->active_levels_[side])
            {
                size_t count = 0;
                uint64_t qty_sum = 0;

                for (Order* ord = pl->dummy_head.next; ord != &pl->dummy_tail; ord = ord->next)
                {
                    count++;
                    qty_sum += ord->qty_remaining;
                    EXPECT_EQ(orderbook->active_orders_.count(ord->order_id), 1)
                        << "訂單 " << ord->order_id << " 在 PriceLevel 中但不在 active_orders_";
                    EXPECT_EQ(ord->price_level, pl);
                }

                EXPECT_EQ(count, pl->order_count);
                EXPECT_EQ(qty_sum, pl->total_qty);
                orders_in_levels += count;
            }
        }

        EXPECT_EQ(orders_in_levels, orderbook->active_orders_.size())
            << "PriceLevel 與 active_orders_ 訂單數量不一致";
    }

    void CheckNoDanglingPointers()
    {
        for (const auto& [id, order] : orderbook->active_orders_)
        {
            ASSERT_NE(order, nullptr);
            ASSERT_NE(order->price_level, nullptr);
            EXPECT_GT(order->qty_remaining, 0ULL);
        }
    }
};

// ==================== InsertBidAsk ====================

TEST_F(OrderBookTest, InsertBidAsk)
{
    SetupInitialBook();

    EXPECT_EQ(orderbook->active_levels_[0].size(), 5) << "Bid should have 5 PriceLevel";
    EXPECT_EQ(orderbook->active_levels_[1].size(), 5) << "Ask should have 5 PriceLevel";

    EXPECT_NE(orderbook->best_levels_[0], nullptr) << "Best Bid should not be nullptr";
    EXPECT_NE(orderbook->best_levels_[1], nullptr) << "Best Ask should not be nullptr";

    const std::vector<int64_t> expected_bid_prices = {10500, 10400, 10300, 10200, 10100};
    
    auto it_bid = orderbook->active_levels_[0].rbegin();  // map 反向迭代，從最高價開始
    
    for (size_t i = 0; i < expected_bid_prices.size(); ++i)
    {
        ASSERT_NE(it_bid, orderbook->active_levels_[0].rend()) << "Bid 價格層數量不足";

        size_t price_index = it_bid->first;
        PriceLevel* pl = it_bid->second;
        int64_t price = orderbook->index_to_price(price_index);

        EXPECT_EQ(price, expected_bid_prices[i]) 
            << "Bid 第 " << i+1 << " 層價格錯誤，預期 " << expected_bid_prices[i] << "，實際 " << price;

        EXPECT_EQ(pl->total_qty, 300ULL) 
            << "Bid 價格 " << price << " total_qty 應為 300";

        EXPECT_EQ(pl->order_count, 3ULL) 
            << "Bid 價格 " << price << " 應有 3 筆訂單";

        EXPECT_NE(pl->dummy_head.next, &pl->dummy_tail) << "Bid 價格 " << price << " 應有實際訂單";

        // 檢查訂單鏈結數量
        size_t order_count_in_list = 0;
        for (Order* ord = pl->dummy_head.next; ord != &pl->dummy_tail; ord = ord->next)
        {
            ++order_count_in_list;
            EXPECT_EQ(ord->price_level, pl);
            EXPECT_EQ(ord->qty_remaining, 100ULL);
        }
        EXPECT_EQ(order_count_in_list, 3ULL);

        ++it_bid;
    }

    // ==================== Ask 詳細檢查 (由低到高) ====================
    const std::vector<int64_t> expected_ask_prices = {10600, 10700, 10800, 10900, 11000};
    
    auto it_ask = orderbook->active_levels_[1].begin();  // map 正向迭代，從最低價開始
    
    for (size_t i = 0; i < expected_ask_prices.size(); ++i)
    {
        ASSERT_NE(it_ask, orderbook->active_levels_[1].end()) << "Ask 價格層數量不足";

        size_t price_index = it_ask->first;
        PriceLevel* pl = it_ask->second;
        int64_t price = orderbook->index_to_price(price_index);

        EXPECT_EQ(price, expected_ask_prices[i]) 
            << "Ask 第 " << i+1 << " 層價格錯誤，預期 " << expected_ask_prices[i] << "，實際 " << price;

        EXPECT_EQ(pl->total_qty, 300ULL);
        EXPECT_EQ(pl->order_count, 3ULL);

        // 檢查訂單鏈結
        size_t order_count_in_list = 0;
        for (Order* ord = pl->dummy_head.next; ord != &pl->dummy_tail; ord = ord->next)
        {
            ++order_count_in_list;
            EXPECT_EQ(ord->price_level, pl);
            EXPECT_EQ(ord->qty_remaining, 100ULL);
        }
        EXPECT_EQ(order_count_in_list, 3ULL);

        ++it_ask;
    }

    // ==================== Best Bid & Best Ask 檢查 ====================
    // int64_t best_bid_price = orderbook->index_to_price(orderbook->best_levels_[0]->dummy_head.next->price_level->dummy_head.next->price_level ? 
    //     /* wait, better way */ 0 : 0); // 改用下面更乾淨的方式

    EXPECT_EQ(orderbook->best_levels_[0], orderbook->active_levels_[0].rbegin()->second)
        << "Best Bid 應指向最高 bid 價格層 (10500)";

    EXPECT_EQ(orderbook->best_levels_[1], orderbook->active_levels_[1].begin()->second)
        << "Best Ask 應指向最低 ask 價格層 (10600)";

    EXPECT_EQ(orderbook->best_levels_[0]->total_qty, 300ULL);
    EXPECT_EQ(orderbook->best_levels_[1]->total_qty, 300ULL);

    // ==================== active_orders_ 數量檢查 ====================
    EXPECT_EQ(orderbook->active_orders_.size(), 30ULL) 
        << "總共應有 5*3 + 5*3 = 30 筆活躍訂單";
}

TEST_F(OrderBookTest, CancelAndModify)
{
    SetupInitialBook();

    // ==================== 初始狀態確認 (重點價格層) ====================
    EXPECT_EQ(orderbook->active_levels_[0].size(), 5);  // Bid
    EXPECT_EQ(orderbook->active_levels_[1].size(), 5);  // Ask

    // ==================== Step 1: Cancel 一筆 Best Bid 之外的訂單 ====================
    // 取消 10300 價格層的其中一筆 (order_id = 30301)
    auto cancel_req = CreateRequest(
        OrderAction_Cancel, 30301, 1, Side_Buy, OrderType_Limit, 0, 0);

    orderbook->processRequest(&cancel_req);

    // --- 針對取消後的變動進行詳細檢查 ---
    EXPECT_EQ(orderbook->active_orders_.count(30301), 0) << "訂單 30301 應已被移除";

    // 檢查 10300 價格層的變化
    {
        auto it = orderbook->active_levels_[0].find(orderbook->price_to_index(10300));
        ASSERT_NE(it, orderbook->active_levels_[0].end()) << "10300 價格層不應被移除 (還有 2 筆)";

        PriceLevel* pl_10300 = it->second;
        EXPECT_EQ(pl_10300->order_count, 2ULL);
        EXPECT_EQ(pl_10300->total_qty, 200ULL);

        // 檢查剩餘訂單
        size_t count = 0;
        for (Order* ord = pl_10300->dummy_head.next; ord != &pl_10300->dummy_tail; ord = ord->next)
        {
            EXPECT_NE(ord->order_id, 30301ULL);
            count++;
        }
        EXPECT_EQ(count, 2ULL);
    }

    // Best Bid 不應改變
    EXPECT_EQ(orderbook->best_levels_[0], orderbook->active_levels_[0].rbegin()->second);
    EXPECT_EQ(orderbook->best_levels_[0]->total_qty, 300ULL);  // 10500 仍為 300

    // ==================== Step 2: Modify 訂單 (改價 + 改量) ====================
    // 修改 10300 的另一筆訂單 → 改到 10450，數量改為 250
    auto modify_req = CreateRequest(
        OrderAction_Modify, 30302, 1, Side_Buy, OrderType_Limit, 10450, 250);

    orderbook->processRequest(&modify_req);

    // --- 針對修改後的變動進行詳細檢查 ---

    // 原 10300 價格層應只剩 1 筆
    {
        auto it = orderbook->active_levels_[0].find(orderbook->price_to_index(10300));
        ASSERT_NE(it, orderbook->active_levels_[0].end());
        PriceLevel* pl = it->second;
        EXPECT_EQ(pl->order_count, 1ULL);
        EXPECT_EQ(pl->total_qty, 100ULL);
    }

    // 新價格層 10450 應該被建立
    {
        auto it = orderbook->active_levels_[0].find(orderbook->price_to_index(10450));
        ASSERT_NE(it, orderbook->active_levels_[0].end()) << "10450 價格層應被建立";

        PriceLevel* pl_10450 = it->second;
        EXPECT_EQ(pl_10450->order_count, 1ULL);
        EXPECT_EQ(pl_10450->total_qty, 250ULL);

        // 檢查訂單內容
        Order* modified_order = pl_10450->dummy_head.next;
        ASSERT_NE(modified_order, &pl_10450->dummy_tail);
        EXPECT_EQ(modified_order->order_id, 30302ULL);
        EXPECT_EQ(modified_order->qty_remaining, 250ULL);
        EXPECT_EQ(modified_order->qty_original, 250ULL);  // 注意：依你的實作是否更新 original
        // EXPECT_EQ(index_to_price(modified_order->price_level-> /* 價格檢查 */), 10450);
    }

    // 檢查 active_orders_ 是否更新
    EXPECT_EQ(orderbook->active_orders_.count(30302), 1);

    // Best Bid 是否改變？（目前不會，因為 10500 仍是最高）
    EXPECT_EQ(orderbook->best_levels_[0]->total_qty, 300ULL);

    // ==================== 整體一致性檢查 ====================
    EXPECT_EQ(orderbook->active_levels_[0].size(), 6) << "Bid 價格層應從 5 增加到 6 (新增 10450)";
}

TEST_F(OrderBookTest, CancelFullLevelMaintainsPriceLinks)
{
    SetupInitialBook();

    for (uint64_t order_id : {51000ULL, 51001ULL, 51002ULL}) {
        auto cancel_req = CreateRequest(
            OrderAction_Cancel, order_id, 2, Side_Sell, OrderType_Limit, 0, 0);

        orderbook->processRequest(&cancel_req);
    }

    const size_t ask_10900_idx = orderbook->price_to_index(10900);
    auto ask_10900 = orderbook->active_levels_[1].find(ask_10900_idx);
    ASSERT_NE(ask_10900, orderbook->active_levels_[1].end());
    EXPECT_EQ(ask_10900->second->worse, nullptr);
    EXPECT_NE(ask_10900->second->better, ask_10900->second);
    EXPECT_NE(ask_10900->second->worse, ask_10900->second);

    for (uint64_t order_id : {50600ULL, 50601ULL, 50602ULL}) {
        auto cancel_req = CreateRequest(
            OrderAction_Cancel, order_id, 2, Side_Sell, OrderType_Limit, 0, 0);

        orderbook->processRequest(&cancel_req);
    }

    const size_t ask_10700_idx = orderbook->price_to_index(10700);
    auto ask_10700 = orderbook->active_levels_[1].find(ask_10700_idx);
    ASSERT_NE(ask_10700, orderbook->active_levels_[1].end());
    EXPECT_EQ(orderbook->best_levels_[1], ask_10700->second);
    EXPECT_EQ(ask_10700->second->better, nullptr);
    EXPECT_NE(ask_10700->second->better, ask_10700->second);
    EXPECT_NE(ask_10700->second->worse, ask_10700->second);
}

TEST_F(OrderBookTest, MatchSingleLayer)
{
    SetupInitialBook();

    const int64_t match_price = 10600;
    const uint64_t incoming_qty = 500;

    // ==================== 送入大買單 (Limit Buy @ 10600) ====================
    auto big_bid = CreateRequest(
        OrderAction_New, 9991, 9991, Side_Buy, OrderType_Limit, match_price, incoming_qty);

    orderbook->processRequest(&big_bid);

    // ==================== 詳細驗證 ====================

    // 1. Ask 10600 價格層應被完全移除
    {
        auto it = orderbook->active_levels_[1].find(orderbook->price_to_index(match_price));
        EXPECT_EQ(it, orderbook->active_levels_[1].end()) << "10600 Ask 價格層應被完全清除";

        EXPECT_EQ(orderbook->active_orders_.count(40000 + match_price + 0), 0);
        EXPECT_EQ(orderbook->active_orders_.count(40000 + match_price + 1), 0);
        EXPECT_EQ(orderbook->active_orders_.count(40000 + match_price + 2), 0);
    }

    // 2. 新增的 Bid 訂單應部分成交並掛簿
    {
        auto it = orderbook->active_levels_[0].find(orderbook->price_to_index(match_price));
        ASSERT_NE(it, orderbook->active_levels_[0].end()) << "Bid 側應新增 10600 價格層";

        PriceLevel* pl = it->second;
        EXPECT_EQ(pl->total_qty, 200ULL);
        EXPECT_EQ(pl->order_count, 1ULL);

        Order* ord = pl->dummy_head.next;
        ASSERT_NE(ord, &pl->dummy_tail);
        EXPECT_EQ(ord->order_id, 9991ULL);
        EXPECT_EQ(ord->qty_remaining, 200ULL);
        EXPECT_EQ(ord->qty_original, 500ULL);
    }

    // 3. Best 價格更新
    EXPECT_EQ(orderbook->best_levels_[0]->total_qty, 200ULL) << "Best Bid 應更新為 10600 的剩餘量";
    EXPECT_EQ(orderbook->best_levels_[1]->total_qty, 300ULL) << "Best Ask 應上移至 10700";

    // 4. 整體數量檢查
    EXPECT_EQ(orderbook->active_levels_[0].size(), 6);
    EXPECT_EQ(orderbook->active_levels_[1].size(), 4);
    EXPECT_EQ(orderbook->active_orders_.size(), 28ULL);
}

// // ==================== MatchingMultiLayer ====================
TEST_F(OrderBookTest, MatchingMultiLayer)
{
    SetupInitialBook();

    const uint64_t incoming_qty = 850;
    const int64_t sell_price = 10300;

    // ==================== Step 1: 送入大賣單 (Limit Sell @ 10300) ====================
    auto big_sell = CreateRequest(
        OrderAction_New, 8888, 8888, Side_Sell, OrderType_Limit, sell_price, incoming_qty);

    orderbook->processRequest(&big_sell);

    // ==================== 詳細驗證 ====================

    // 1. 完全被吃掉的 Bid 價格層
    for (int64_t price : {10500, 10400})
    {
        auto it = orderbook->active_levels_[0].find(orderbook->price_to_index(price));
        EXPECT_EQ(it, orderbook->active_levels_[0].end()) 
            << "Bid 價格 " << price << " 應被完全吃掉";

        for (int i = 0; i < 3; ++i)
        {
            uint64_t oid = 20000 + price + i;
            EXPECT_EQ(orderbook->active_orders_.count(oid), 0);
        }
    }

    // 2. 部分成交的 10300 價格層（剩下 50）
    {
        auto it = orderbook->active_levels_[0].find(orderbook->price_to_index(10300));
        ASSERT_NE(it, orderbook->active_levels_[0].end()) 
            << "10300 價格層應剩下部分數量";

        PriceLevel* pl_10300 = it->second;
        EXPECT_EQ(pl_10300->total_qty, 50ULL) << "10300 應剩下 50";
        EXPECT_EQ(pl_10300->order_count, 1ULL);   // 原有 3 筆，其中 1 筆被吃掉部分，其餘 2 筆完整

        // 檢查訂單剩餘量總和
        uint64_t total_remaining = 0;
        for (Order* ord = pl_10300->dummy_head.next; ord != &pl_10300->dummy_tail; ord = ord->next)
        {
            total_remaining += ord->qty_remaining;
        }
        EXPECT_EQ(total_remaining, 50ULL);
    }

    // 3. 未被吃到的 Bid 價格層 (10200, 10100)
    for (int64_t price : {10200, 10100})
    {
        auto it = orderbook->active_levels_[0].find(orderbook->price_to_index(price));
        ASSERT_NE(it, orderbook->active_levels_[0].end());
        EXPECT_EQ(it->second->total_qty, 300ULL);
        EXPECT_EQ(it->second->order_count, 3ULL);
    }

    // 4. 大賣單應已完全成交
    EXPECT_EQ(orderbook->active_orders_.count(8888), 0) 
        << "賣單 8888 已完全成交，不應留在 active_orders_";

    // 5. Best Bid 更新檢查
    {
        auto best_bid_it = orderbook->active_levels_[0].rbegin();  // 目前最高 Bid
        int64_t new_best_bid = orderbook->index_to_price(best_bid_it->first);
        EXPECT_EQ(new_best_bid, 10300) << "Best Bid 應下移至 10300（剩下 50）";
        EXPECT_EQ(orderbook->best_levels_[0]->total_qty, 50ULL);
    }

    // 6. 整體結構檢查
    EXPECT_EQ(orderbook->active_levels_[0].size(), 3) 
        << "Bid 價格層: 5 - 2(完全吃掉) = 3 層剩下";

    EXPECT_EQ(orderbook->active_levels_[1].size(), 5) 
        << "Ask 價格層數量不變";

    EXPECT_EQ(orderbook->active_orders_.size(), 30 - 8)  
        << "30 - 8(完全成交的訂單) = 22";
}

} // namespace Exchange
