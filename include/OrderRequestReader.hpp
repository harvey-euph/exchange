#pragma once
#include <vector>
#include <string>
#include <memory>
#include "generated/order_generated.h"

namespace Exchange {

class OrderRequestReader {
public:
    OrderRequestReader() = default;
    ~OrderRequestReader();

    // 從 CSV 檔案讀取所有 OrderRequest
    bool loadFromCSV(const std::string& csv_filename);

    // 取得所有讀取到的 requests（pointer 有效直到 reader 被銷毀或 clear）
    const std::vector<const OrderRequest*>& getRequests() const;

    // 清空目前載入的資料
    void clear();

    // 取得載入的筆數
    size_t size() const;

private:
    // 每個 FlatBufferBuilder 對應一筆 OrderRequest
    std::vector<std::unique_ptr<flatbuffers::FlatBufferBuilder>> builders_;
    std::vector<const OrderRequest*> requests_;
};

} // namespace Exchange