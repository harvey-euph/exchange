#pragma once
#include <cstdint>
#include <fstream>
#include <random>
#include <unordered_map>
#include <vector>
#include <string>
#include <memory>
#include "fbs/order_generated.h"
#include "OrderBook.hpp"

namespace Exchange {

class CSVDataReader {
public:
    CSVDataReader() = default;
    ~CSVDataReader();

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

class CSVDataGen {
public:
    explicit CSVDataGen(size_t target_rows);
    CSVDataGen(size_t target_rows, std::string out_path);

    CSVDataGen(const CSVDataGen&) = delete;
    CSVDataGen& operator=(const CSVDataGen&) = delete;

    void run();

private:
    static constexpr uint64_t kBaseTimestamp = 1747238400000000000ULL;

    struct Target {
        uint64_t order_id;
        uint32_t client_id;
        Side side;
        int64_t price;
        uint64_t quantity;
    };

    const OrderBook& book() const;
    void writeInitialBook(std::ofstream& out);
    void writeRandomRequest(std::ofstream& out);
    void writeNewPassive(std::ofstream& out, Side side, int64_t price);
    void writeCrossingFill(std::ofstream& out);
    void writeCancel(std::ofstream& out);
    void writeModify(std::ofstream& out);

    bool shouldUseFilledOrder();
    bool canWriteFill() const;
    std::unordered_map<uint64_t, Target> activeOrderTargets() const;
    void recordFilledOrders(const std::unordered_map<uint64_t, Target>& before);
    bool hasActiveOrder(uint64_t order_id) const;
    Target randomActiveOrder();
    Target randomFilledOrder();
    Side sideForLevel(const PriceLevel* level) const;

    int64_t passivePrice(Side side);
    Side randomSide();
    uint32_t randomClient();
    uint64_t randomQuantity();

    template <typename T>
    T randomInt(T low, T high)
    {
        std::uniform_int_distribution<T> dist(low, high);
        return dist(rng_);
    }

    void processRequest(
        OrderAction action,
        uint64_t order_id,
        uint32_t client_id,
        Side side,
        int64_t price,
        uint64_t quantity);

    void writeRow(
        std::ofstream& out,
        uint64_t order_id,
        uint32_t client_id,
        const char* action,
        Side side,
        int64_t price,
        uint64_t quantity);

    const size_t target_rows_;
    const std::string out_path_;
    OrderBook book_;
    flatbuffers::FlatBufferBuilder fbb_;
    std::mt19937_64 rng_;

    uint64_t req_id_ = 1;
    uint64_t next_order_id_ = 1;
    uint64_t timestamp_ = kBaseTimestamp;
    size_t rows_written_ = 0;
    std::vector<Target> filled_orders_;
};

} // namespace Exchange
