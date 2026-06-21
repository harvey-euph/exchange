#include "csv_util.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <random>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace {
constexpr int INITIAL_PRICE_LEVELS = 8;
constexpr int INITIAL_ORDERS_PER_LEVEL = 4;
constexpr int64_t MIN_PRICE_STEP = 10000;
constexpr int64_t PRICE_INDEX_OFFSET = 2000;
constexpr size_t MAX_PRICE_LEVELS = 8192;
constexpr int64_t BASE_PRICE = 50000000;
constexpr uint32_t SYMBOL_ID = 1;
}

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

CSVDataReader::~CSVDataReader() {
    clear();
}

bool CSVDataReader::loadFromCSV(const std::string& csv_filename) {
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

    while (std::getline(file, line)) 
    {
        line_num++;
        if (line.empty() || line[0] == '#') continue;

        std::vector<std::string> fields;
        std::stringstream ss(line);
        std::string field;

        while (std::getline(ss, field, ',')) {
            fields.push_back(field);
        }

        if (fields.size() < 10) {
            std::cerr << "[Warning] Line " << line_num << " skipped.\n";
            continue;
        }

        try {
            auto builder = std::make_unique<flatbuffers::FlatBufferBuilder>(512);

            // 解析 Action
            OrderAction action = OrderAction_New;
            if (fields[3] == "Cancel") action = OrderAction_Cancel;
            else if (fields[3] == "Modify") action = OrderAction_Modify;

            Side side = (fields[4] == "Buy" || fields[4] == "buy") ? Side_Buy : Side_Sell;
            OrderType type = OrderType_Limit;
            if (fields[5] == "Market") type = OrderType_Market;

            uint64_t req_id       = safe_stoull(fields[0]);
            uint64_t order_id     = safe_stoull(fields[1]);
            uint32_t client_id    = safe_stoul(fields[2]);
            uint32_t symbol_id    = (fields.size() > 10) ? safe_stoul(fields[10]) : 1;
            
            int64_t  price        = safe_stoll(fields[6]);
            uint64_t quantity     = safe_stoull(fields[7]);
            uint64_t visible_qty  = (fields.size() > 8) ? safe_stoull(fields[8]) : 0;
            uint64_t timestamp    = safe_stoull(fields[9]);

            if (action == OrderAction_Cancel) {
                price = 0;
                quantity = 0;
            }

            auto req_offset = CreateOrderRequest(
                *builder,
                action,
                req_id,
                order_id,
                client_id,
                symbol_id,
                side,
                type,
                price,
                quantity,
                visible_qty,
                timestamp
            );

            builder->Finish(req_offset);

            const OrderRequest* req = flatbuffers::GetRoot<OrderRequest>(builder->GetBufferPointer());

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

const std::vector<const OrderRequest*>& CSVDataReader::getRequests() const {
    return requests_;
}

void CSVDataReader::clear() {
    builders_.clear();
    requests_.clear();
}

size_t CSVDataReader::size() const {
    return requests_.size();
}

CSVDataGen::CSVDataGen(size_t target_rows)
    : CSVDataGen(target_rows, "data/" + std::to_string(target_rows) + ".csv")
{
}

CSVDataGen::CSVDataGen(size_t target_rows, std::string out_path)
    : target_rows_(target_rows),
      out_path_(std::move(out_path)),
      book_(MIN_PRICE_STEP, PRICE_INDEX_OFFSET, MAX_PRICE_LEVELS),
      rng_(std::random_device{}())
{
}

void CSVDataGen::run()
{
    const std::filesystem::path output_path(out_path_);
    if (output_path.has_parent_path()) {
        std::filesystem::create_directories(output_path.parent_path());
    }

    std::ofstream out(out_path_, std::ios::trunc);
    if (!out.is_open()) {
        throw std::runtime_error("cannot open output file: " + out_path_);
    }

    out << "req_id,order_id,client_id,action,side,type,price,quantity,visible_qty,timestamp,symbol_id\n";

    writeInitialBook(out);
    while (rows_written_ < target_rows_) {
        writeRandomRequest(out);
    }

    std::cout << "[csv_data_gen] wrote " << rows_written_ << " rows to " << out_path_ << '\n';
}

const OrderBook& CSVDataGen::book() const
{
    return book_;
}

void CSVDataGen::writeInitialBook(std::ofstream& out)
{
    for (int level = INITIAL_PRICE_LEVELS; level >= 1 && rows_written_ < target_rows_; --level) {
        const int64_t price = BASE_PRICE - level * MIN_PRICE_STEP;
        for (int i = 0; i < INITIAL_ORDERS_PER_LEVEL && rows_written_ < target_rows_; ++i) {
            writeNewPassive(out, Side_Buy, price);
        }
    }

    for (int level = 1; level <= INITIAL_PRICE_LEVELS && rows_written_ < target_rows_; ++level) {
        const int64_t price = BASE_PRICE + level * MIN_PRICE_STEP;
        for (int i = 0; i < INITIAL_ORDERS_PER_LEVEL && rows_written_ < target_rows_; ++i) {
            writeNewPassive(out, Side_Sell, price);
        }
    }
}

void CSVDataGen::writeRandomRequest(std::ofstream& out)
{
    if (book().active_orders_.empty()) {
        const Side side = randomSide();
        writeNewPassive(out, side, passivePrice(side));
        return;
    }

    const int roll = randomInt(1, 100);
    if (roll <= 12 && canWriteFill()) {
        writeCrossingFill(out);
    } else if (roll <= 42) {
        writeModify(out);
    } else if (roll <= 67) {
        writeCancel(out);
    } else {
        const Side side = randomSide();
        writeNewPassive(out, side, passivePrice(side));
    }
}

void CSVDataGen::writeNewPassive(std::ofstream& out, Side side, int64_t price)
{
    const uint64_t order_id = next_order_id_++;
    const uint32_t client_id = randomClient();
    const uint64_t quantity = randomQuantity();

    writeRow(out, order_id, client_id, "New", side, price, quantity);
    processRequest(OrderAction_New, order_id, client_id, side, price, quantity);
}

void CSVDataGen::writeCrossingFill(std::ofstream& out)
{
    const Side side = (book().best_levels_[Side_Sell] && book().best_levels_[Side_Buy])
        ? randomSide()
        : (book().best_levels_[Side_Sell] ? Side_Buy : Side_Sell);
    const int opposite_side = 1 - static_cast<int>(side);
    const PriceLevel* best_opposite = book().best_levels_[opposite_side];

    if (!best_opposite || best_opposite->total_qty == 0) {
        const Side passive_side = randomSide();
        writeNewPassive(out, passive_side, passivePrice(passive_side));
        return;
    }

    const uint64_t order_id = next_order_id_++;
    const uint32_t client_id = randomClient();
    const int64_t price = static_cast<int64_t>(
        book().index_to_price(best_opposite - book().price_array_.data()));
    const uint64_t quantity = randomInt<uint64_t>(1, std::min<uint64_t>(best_opposite->total_qty, 500));
    const auto before = activeOrderTargets();

    writeRow(out, order_id, client_id, "New", side, price, quantity);
    processRequest(OrderAction_New, order_id, client_id, side, price, quantity);
    recordFilledOrders(before);
    if (!hasActiveOrder(order_id)) {
        filled_orders_.push_back({order_id, client_id, side, price, quantity});
    }
}

void CSVDataGen::writeCancel(std::ofstream& out)
{
    const bool use_filled_order = shouldUseFilledOrder();
    const Target target = use_filled_order ? randomFilledOrder() : randomActiveOrder();

    writeRow(out, target.order_id, target.client_id, "Cancel", target.side, 0, 0);
    processRequest(OrderAction_Cancel, target.order_id, target.client_id, target.side, 0, 0);
}

void CSVDataGen::writeModify(std::ofstream& out)
{
    const bool use_filled_order = shouldUseFilledOrder();
    const Target target = use_filled_order ? randomFilledOrder() : randomActiveOrder();
    const int64_t new_price = passivePrice(target.side);
    const uint64_t new_quantity = randomQuantity();

    writeRow(out, target.order_id, target.client_id, "Modify", target.side, new_price, new_quantity);
    processRequest(OrderAction_Modify, target.order_id, target.client_id, target.side, new_price, new_quantity);
}

bool CSVDataGen::shouldUseFilledOrder()
{
    return !filled_orders_.empty() && randomInt(1, 100) > 95;
}

bool CSVDataGen::canWriteFill() const
{
    return book().best_levels_[Side_Buy] || book().best_levels_[Side_Sell];
}

std::unordered_map<uint64_t, CSVDataGen::Target> CSVDataGen::activeOrderTargets() const
{
    std::unordered_map<uint64_t, Target> targets;
    targets.reserve(book().active_orders_.size());

    for (const auto& [order_id, order_ptr] : book().active_orders_) {
        const Order* order = order_ptr;
        const PriceLevel* level = order->price_level;
        targets.emplace(order_id, Target{
            order->order_id,
            order->client_id,
            sideForLevel(level),
            static_cast<int64_t>(book().index_to_price(level - book().price_array_.data())),
            order->qty_remaining
        });
    }

    return targets;
}

void CSVDataGen::recordFilledOrders(const std::unordered_map<uint64_t, Target>& before)
{
    for (const auto& [order_id, target] : before) {
        if (!hasActiveOrder(order_id)) {
            filled_orders_.push_back(target);
        }
    }
}

bool CSVDataGen::hasActiveOrder(uint64_t order_id) const
{
    return book().active_orders_.find(order_id) != book().active_orders_.end();
}

CSVDataGen::Target CSVDataGen::randomActiveOrder()
{
    const size_t offset = randomInt<size_t>(0, book().active_orders_.size() - 1);
    auto it = book().active_orders_.begin();
    std::advance(it, offset);

    const Order* order = it->second;
    const PriceLevel* level = order->price_level;
    return {
        order->order_id,
        order->client_id,
        sideForLevel(level),
        static_cast<int64_t>(book().index_to_price(level - book().price_array_.data())),
        order->qty_remaining
    };
}

CSVDataGen::Target CSVDataGen::randomFilledOrder()
{
    return filled_orders_[randomInt<size_t>(0, filled_orders_.size() - 1)];
}

Side CSVDataGen::sideForLevel(const PriceLevel* level) const
{
    const size_t price_index = level - book().price_array_.data();
    const auto bid = book().active_levels_[Side_Buy].find(price_index);
    if (bid != book().active_levels_[Side_Buy].end() && bid->second == level) {
        return Side_Buy;
    }
    return Side_Sell;
}

int64_t CSVDataGen::passivePrice(Side side)
{
    const int level = randomInt(1, INITIAL_PRICE_LEVELS);
    return side == Side_Buy
        ? BASE_PRICE - level * MIN_PRICE_STEP
        : BASE_PRICE + level * MIN_PRICE_STEP;
}

Side CSVDataGen::randomSide()
{
    return randomInt(0, 1) == 0 ? Side_Buy : Side_Sell;
}

uint32_t CSVDataGen::randomClient()
{
    return static_cast<uint32_t>(randomInt(1001, 9999));
}

uint64_t CSVDataGen::randomQuantity()
{
    return randomInt<uint64_t>(100, 5000);
}

void CSVDataGen::processRequest(
    OrderAction action,
    uint64_t order_id,
    uint32_t client_id,
    Side side,
    int64_t price,
    uint64_t quantity)
{
    fbb_.Clear();
    const auto req = CreateOrderRequest(
        fbb_,
        action,
        req_id_ - 1,
        order_id,
        client_id,
        SYMBOL_ID,
        side,
        OrderType_Limit,
        price,
        quantity,
        0,
        timestamp_ - 1);

    fbb_.Finish(req);
    OrderRequestT native_req; flatbuffers::GetRoot<OrderRequest>(fbb_.GetBufferPointer())->UnPackTo(&native_req); book_.processRequest(&native_req);
}

void CSVDataGen::writeRow(
    std::ofstream& out,
    uint64_t order_id,
    uint32_t client_id,
    const char* action,
    Side side,
    int64_t price,
    uint64_t quantity)
{
    out << req_id_++ << ','
        << order_id << ','
        << client_id << ','
        << action << ','
        << (side == Side_Buy ? "Buy" : "Sell") << ','
        << "Limit" << ','
        << price << ','
        << quantity << ','
        << 0 << ','
        << timestamp_++ << ','
        << SYMBOL_ID << '\n';
    ++rows_written_;
}

} // namespace Exchange
