#include "OrderRequestReader.hpp"
#include <fstream>
#include <sstream>
#include <iostream>
#include <limits>

namespace Exchange {

// 安全轉換函數：轉換失敗時回傳 0
static int64_t  safe_stoll(const std::string& str) {
    if (str.empty()) return 0;
    try {
        return std::stoll(str);
    } catch (...) {
        return 0;
    }
}

static uint64_t safe_stoull(const std::string& str) {
    if (str.empty()) return 0;
    try {
        return std::stoull(str);
    } catch (...) {
        return 0;
    }
}

static uint32_t safe_stoul(const std::string& str) {
    if (str.empty()) return 0;
    try {
        return std::stoul(str);
    } catch (...) {
        return 0;
    }
}

OrderRequestReader::~OrderRequestReader() {
    clear();
}

bool OrderRequestReader::loadFromCSV(const std::string& csv_filename) {
    std::ifstream file(csv_filename);
    if (!file.is_open()) {
        std::cerr << "[Error] Cannot open CSV file: " << csv_filename << std::endl;
        return false;
    }

    clear();

    std::string line;
    size_t line_num = 0;

    std::getline(file, line); // 跳過標題
    line_num++;

    while (std::getline(file, line)) {
        line_num++;
        if (line.empty() || line[0] == '#') continue;

        std::vector<std::string> fields;
        std::stringstream ss(line);
        std::string field;

        while (std::getline(ss, field, ',')) {
            fields.push_back(field);
        }

        if (fields.size() < 9) {
            std::cerr << "[Warning] Line " << line_num << " skipped.\n";
            continue;
        }

        try {
            auto builder = std::make_unique<flatbuffers::FlatBufferBuilder>(512);

            // 解析 Action
            OrderAction action = OrderAction_New;
            if (fields[2] == "Cancel") action = OrderAction_Cancel;
            else if (fields[2] == "Modify") action = OrderAction_Modify;

            Side side = (fields[3] == "Buy" || fields[3] == "buy") ? Side_Buy : Side_Sell;

            uint64_t order_id     = safe_stoull(fields[0]);
            uint32_t client_id    = safe_stoul(fields[1]);
            uint32_t symbol_id    = (fields.size() > 9) ? safe_stoul(fields[9]) : 1;
            
            int64_t  price        = safe_stoll(fields[5]);
            uint64_t quantity     = safe_stoull(fields[6]);
            uint64_t visible_qty  = (fields.size() > 7) ? safe_stoull(fields[7]) : 0;
            uint64_t timestamp    = safe_stoull(fields[8]);

            // 對於 Cancel/Modify，price 和 quantity 通常為 0（或可忽略）
            if (action == OrderAction_Cancel) {
                price = 0;
                quantity = 0;
            }

            auto req_offset = CreateOrderRequest(
                *builder,
                action,
                order_id,
                client_id,
                symbol_id,
                side,
                OrderType_Limit,
                price,
                quantity,
                visible_qty,
                timestamp
            );

            builder->Finish(req_offset);

            const OrderRequest* req = GetOrderRequest(builder->GetBufferPointer());

            builders_.push_back(std::move(builder));
            requests_.push_back(req);

        } catch (const std::exception& e) {
            std::cerr << "[Error] Line " << line_num << ": " << e.what() << std::endl;
        }
    }

    std::cout << "[Info] Successfully loaded " << requests_.size() 
              << " orders from " << csv_filename << std::endl;
    return true;
}

const std::vector<const OrderRequest*>& OrderRequestReader::getRequests() const {
    return requests_;
}

void OrderRequestReader::clear() {
    builders_.clear();
    requests_.clear();
}

size_t OrderRequestReader::size() const {
    return requests_.size();
}

} // namespace Exchange