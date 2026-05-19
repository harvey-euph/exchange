#include <gtest/gtest.h>
#include "../include/OrderBook.hpp"   // 根據你的目錄結構調整

namespace Exchange {

class OrderBookTest : public ::testing::Test {
protected:
    OrderBook book{1LL, 0LL, 2048};   // min_step=1, offset=10

    void SetUp() override {
        // 如有需要可在此 reset
    }

    // ==================== Helper ====================
    void NewLimit(uint64_t oid, Side side, int64_t price, uint64_t qty) {
        flatbuffers::FlatBufferBuilder fbb(1024);
        auto req_offset = CreateOrderRequest(fbb,
            OrderAction_New,
            0, oid, 1, fbb.CreateString("TEST"),
            side, OrderType_Limit, price, qty, 0, 0
        );
        fbb.Finish(req_offset);
        const OrderRequest* req = GetOrderRequest(fbb.GetBufferPointer());
        book.processRequest(req);
    }

    void NewMarket(uint64_t oid, Side side, uint64_t qty) {
        flatbuffers::FlatBufferBuilder fbb(1024);
        auto req_offset = CreateOrderRequest(fbb,
            OrderAction_New,
            0, oid, 1, fbb.CreateString("TEST"),
            side, OrderType_Market, 0, qty, 0, 0
        );
        fbb.Finish(req_offset);
        book.processRequest(GetOrderRequest(fbb.GetBufferPointer()));
    }

    void Cancel(uint64_t oid) {
        flatbuffers::FlatBufferBuilder fbb(1024);
        auto req_offset = CreateOrderRequest(fbb,
            OrderAction_Cancel, 0, oid, 1, fbb.CreateString("TEST"),
            Side_Buy, OrderType_Limit, 0, 0, 0, 0
        );
        fbb.Finish(req_offset);
        book.processRequest(GetOrderRequest(fbb.GetBufferPointer()));
    }

    void Modify(uint64_t oid, int64_t new_price, uint64_t new_qty) {
        flatbuffers::FlatBufferBuilder fbb(1024);
        auto req_offset = CreateOrderRequest(fbb,
            OrderAction_Modify, 0, oid, 1, fbb.CreateString("TEST"),
            Side_Buy, OrderType_Limit, new_price, new_qty, 0, 0
        );
        fbb.Finish(req_offset);
        book.processRequest(GetOrderRequest(fbb.GetBufferPointer()));
    }
};

// ====================== Tests ======================

TEST_F(OrderBookTest, MultiLevelAndLinkedListStructure)
{
    NewLimit(1, Side_Buy, 100, 10);
    NewLimit(2, Side_Buy, 100, 15);
    NewLimit(3, Side_Buy, 99,  20);
    NewLimit(4, Side_Buy, 99,  25);
    NewLimit(5, Side_Buy, 98,  30);

    EXPECT_EQ(book.active_levels_[Side_Buy].size(), 3);

    // std::vector<int64_t> expected = {100, 99, 98};
    // size_t i = 0;
    // for (auto it = book.active_levels_[Side_Buy].rbegin(); 
    //      it != book.active_levels_[Side_Buy].rend(); ++it) {
    //     EXPECT_EQ(book.index_to_price(it->first), expected[i++]);
    // }

    // for (auto& [idx, pl] : book.active_levels_[Side_Buy]) {
    //     auto orders = OrderBook::getOrdersInLevel(pl);  // 我們會在下面定義
    //     EXPECT_GE(orders.size(), 1);
    // }
}

// TEST_F(OrderBookTest, MatchingBehavior) {
//     NewLimit(10, Side_Buy, 100, 50);
//     NewLimit(11, Side_Buy, 100, 30);

//     NewLimit(12, Side_Sell, 100, 70);  // taker

//     auto* pl = book.getPriceLevelInternal(Side_Buy, 100); // 我們會在 test 裡實作
//     ASSERT_NE(pl, nullptr);
//     EXPECT_EQ(pl->total_qty, 30);
//     EXPECT_EQ(pl->order_count, 1);

//     auto orders = OrderBook::getOrdersInLevel(pl);
//     ASSERT_EQ(orders.size(), 1);
//     EXPECT_EQ(orders[0]->order_id, 11);
// }

// TEST_F(OrderBookTest, MultiLevelSweep) {
//     NewLimit(20, Side_Buy, 100, 40);
//     NewLimit(21, Side_Buy, 99,  50);
//     NewLimit(22, Side_Buy, 98,  60);

//     NewLimit(23, Side_Sell, 100, 200);

//     EXPECT_EQ(book.best_levels_[Side_Buy], nullptr);
// }

// TEST_F(OrderBookTest, ModifyCancelFIFOPreservation) {
//     NewLimit(30, Side_Buy, 100, 10);   // A
//     NewLimit(31, Side_Buy, 100, 20);   // B
//     NewLimit(32, Side_Buy, 100, 30);   // C

//     Cancel(31);

//     auto* pl = book.getPriceLevelInternal(Side_Buy, 100);
//     auto orders = OrderBook::getOrdersInLevel(pl);
//     ASSERT_EQ(orders.size(), 2);
//     EXPECT_EQ(orders[0]->order_id, 30);
//     EXPECT_EQ(orders[1]->order_id, 32);

//     Modify(30, 100, 15);
//     orders = OrderBook::getOrdersInLevel(pl);
//     EXPECT_EQ(orders[0]->order_id, 30);
//     EXPECT_EQ(orders[0]->qty_remaining, 15);
// }

// // ==================== Test 專用靜態函數 ====================
// std::vector<Order*> OrderBook::getOrdersInLevel(PriceLevel* pl) {
//     std::vector<Order*> orders;
//     if (!pl) return orders;
//     Order* cur = pl->dummy_head.next;
//     while (cur != &pl->dummy_tail) {
//         orders.push_back(cur);
//         cur = cur->next;
//     }
//     return orders;
// }

// // 如果你不想在 OrderBook 加 getPriceLevelInternal，可以在 test 裡直接這樣寫：
// PriceLevel* OrderBookTest::getPriceLevelInternal(Side side, int64_t price) {
//     size_t idx = book.price_to_index(price);
//     auto it = book.active_levels_[side].find(idx);
//     return it != book.active_levels_[side].end() ? it->second : nullptr;
// }

} // namespace Exchange

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}