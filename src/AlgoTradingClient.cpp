#include "AlgoTradingClient.hpp"
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <iostream>
#include <chrono>
#include <thread>
#include "PublicDataClient.hpp"

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = boost::asio::ip::tcp;

namespace Exchange {

AlgoTradingClient::AlgoTradingClient(const Config& config) : config_(config) {
    mgmt_client_ = SimpleWSClient::create(config_.host, config_.mgmt_port);
    md_client_ = SimpleWSClient::create(config_.host, config_.l2_port);
}

AlgoTradingClient::~AlgoTradingClient() {
    stop();
}

void AlgoTradingClient::new_limit_order(uint32_t symbol_id, Side side, int64_t p, uint64_t q, uint64_t visible_qty) {
    new_order(symbol_id, side, OrderType_Limit, p, q, visible_qty);
}

void AlgoTradingClient::new_market_order(uint32_t symbol_id, Side side, uint64_t q) {
    new_order(symbol_id, side, OrderType_Market, 0, q, q);
}

void AlgoTradingClient::new_order(uint32_t symbol_id, Side side, OrderType type, int64_t p, uint64_t q, uint64_t visible_qty) {
    OrderRequestT req;
    req.action = OrderAction_New;
    req.symbol_id = symbol_id;
    req.side = side;
    req.type = type;
    req.p = p;
    req.q = q;
    req.visible_qty = (visible_qty == 0) ? q : visible_qty;
    send_order_request(req);
}

void AlgoTradingClient::replace_order(uint64_t order_id, int64_t p, uint64_t q, uint32_t symbol_id, Side side) {
    OrderRequestT req;
    req.action = OrderAction_Modify;
    req.order_id = order_id;
    req.symbol_id = symbol_id;
    req.side = side;
    req.p = p;
    req.q = q;
    send_order_request(req);
}

void AlgoTradingClient::cancel_order(uint64_t order_id, uint32_t symbol_id, Side side) {
    OrderRequestT req;
    req.action = OrderAction_Cancel;
    req.order_id = order_id;
    req.symbol_id = symbol_id;
    req.side = side;
    send_order_request(req);
}

void AlgoTradingClient::send_order_request(OrderRequestT& order) {
    if (order.action == OrderAction_New && order.type != OrderType_Market) {
        std::string err;
        if (!validate_price(order.symbol_id, order.p, err)) {
            std::cerr << "[AlgoTradingClient] ERROR: Trying to send order with invalid price: " << err << std::endl;
            throw std::runtime_error("Invalid order price: " + err);
        }
    } else if (order.action == OrderAction_Modify) {
        std::string err;
        if (!validate_price(order.symbol_id, order.p, err)) {
            std::cerr << "[AlgoTradingClient] ERROR: Trying to modify order with invalid price: " << err << std::endl;
            throw std::runtime_error("Invalid order price: " + err);
        }
    }

    order.client_id = config_.client_id;
    if (order.action == OrderAction_New) {
        order.order_id = next_id_++;
        order.exec_id = order.order_id;
    } else {
        order.exec_id = next_id_++;
    }

    order.timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();

    flatbuffers::FlatBufferBuilder fbb(256);
    auto order_offset = OrderRequest::Pack(fbb, &order);
    auto client_req = CreateClientRequest(fbb, ClientRequestData_OrderRequest, order_offset.Union());
    fbb.Finish(client_req);
    mgmt_client_->send(fbb.GetBufferPointer(), fbb.GetSize());
}

void AlgoTradingClient::query_position(uint32_t symbol_id) {
    flatbuffers::FlatBufferBuilder fbb(128);
    auto pos_req = CreatePositionRequest(fbb, config_.client_id, symbol_id);
    auto client_req = CreateClientRequest(fbb, ClientRequestData_PositionRequest, pos_req.Union());
    fbb.Finish(client_req);
    mgmt_client_->send(fbb.GetBufferPointer(), fbb.GetSize());
}

bool AlgoTradingClient::validate_price(uint32_t symbol_id, int64_t p, std::string& err_msg) {
    auto it = symbols_info_.find(symbol_id);
    if (it == symbols_info_.end()) {
        err_msg = "Symbol " + std::to_string(symbol_id) + " info not found";
        return false;
    }
    const auto& info = it->second;
    if (p < info->price_min || p > info->price_max) {
        err_msg = "Price " + std::to_string(p) + " out of bounds [" +
                  std::to_string(info->price_min) + ", " + std::to_string(info->price_max) + "]";
        return false;
    }
    if (info->price_min_step > 0 && p % info->price_min_step != 0) {
        err_msg = "Price " + std::to_string(p) + " is not a multiple of step size " +
                  std::to_string(info->price_min_step);
        return false;
    }
    return true;
}

void AlgoTradingClient::on_order_response(const OrderResponse* response) {
    auto exec = response->exec_type();
    if ((exec == ExecType_New || exec == ExecType_Fill || 
         exec == ExecType_PartialFill || exec == ExecType_Replaced || exec == ExecType_OrderStatus) && 
        response->reject_code() == RejectCode_None) {
        std::string err;
        if (!validate_price(response->symbol_id(), response->p(), err)) {
            std::cerr << "[AlgoTradingClient] ERROR: OrderResponse has invalid price: " << err << std::endl;
            throw std::runtime_error("OrderResponse has invalid price: " + err);
        }
    }
    account_.handle_order_response(response);
}

void AlgoTradingClient::on_position_response(const PositionResponse* response) {
    account_.handle_position_response(response);
}

void AlgoTradingClient::wait_until_ready() {
    std::unique_lock<std::mutex> lock(ready_mtx_);
    ready_cv_.wait(lock, [this] { return ready_.load(); });
}

int AlgoTradingClient::run() {
    // Fetch symbol info from public-data service
    for (auto symbol_id : config_.symbol_ids) {
        try {
            auto info = PublicDataClient::getSymbolInfo(config_.host, "8081", symbol_id);
            if (info) {
                symbols_info_[symbol_id] = std::move(info);
            }
        } catch (const std::exception& e) {
            std::cerr << "[AlgoTradingClient] Warning: Failed to fetch symbol info for " << symbol_id << ": " << e.what() << std::endl;
        }
    }

    if (!mgmt_client_->connect()) {
        std::cerr << "Failed to connect to Management port " << config_.mgmt_port << std::endl;
        return 1;
    }
    if (!md_client_->connect()) {
        std::cerr << "Failed to connect to Market Data port " << config_.l2_port << std::endl;
        return 1;
    }

    mgmt_client_->run_async([this](const void* data, size_t size) {
        (void)size;
        auto resp = flatbuffers::GetRoot<ClientResponse>(data);
        if (resp->data_type() == ClientResponseData_OrderResponse) {
            auto order_resp = resp->data_as_OrderResponse();
            if (order_resp->exec_type() == ExecType_Complete) {
                {
                    std::lock_guard<std::mutex> lock(ready_mtx_);
                    ready_ = true;
                }
                ready_cv_.notify_all();
                return;
            }
            on_order_response(order_resp);
        } else if (resp->data_type() == ClientResponseData_PositionResponse) {
            on_position_response(resp->data_as_PositionResponse());
        }
    });

    md_client_->run_async([this](const void* data, size_t size) {
        (void)size;
        auto md_update = flatbuffers::GetRoot<MarketDataUpdate>(data);
        if (md_update->data_type() == MarketDataUpdateData_L2Update) {
            auto update = md_update->data_as_L2Update();
            std::string err;
            if (update->side() != Side_None && update->p() != 0 && !validate_price(update->symbol_id(), update->p(), err)) {
                std::cerr << "[AlgoTradingClient] ERROR: L2 Update has invalid price: " << err << std::endl;
                throw std::runtime_error("L2 Update has invalid price: " + err);
            }
            on_l2_update(update);
        } else if (md_update->data_type() == MarketDataUpdateData_L3Update) {
            auto update = md_update->data_as_L3Update();
            std::string err;
            if (update->side() != Side_None && update->p() != 0 && !validate_price(update->symbol_id(), update->p(), err)) {
                std::cerr << "[AlgoTradingClient] ERROR: L3 Update has invalid price: " << err << std::endl;
                throw std::runtime_error("L3 Update has invalid price: " + err);
            }
            on_l3_update(update);
        }
    });

    // Subscriptions
    {
        flatbuffers::FlatBufferBuilder fbb(128);
        auto admin_req = CreateAdminRequest(fbb, AdminAction_LogOn, config_.client_id);
        auto client_req = CreateClientRequest(fbb, ClientRequestData_AdminRequest, admin_req.Union());
        fbb.Finish(client_req);
        mgmt_client_->send(fbb.GetBufferPointer(), fbb.GetSize());
    }
    for (auto sym : config_.symbol_ids) {
        // L2 Subscription
        {
            flatbuffers::FlatBufferBuilder fbb(128);
            auto req = CreateMarketDataRequest(fbb, sym, MDType_L2, SubType_subscribe);
            fbb.Finish(req);
            md_client_->send(fbb.GetBufferPointer(), fbb.GetSize());
        }
        // L3 Subscription
        {
            flatbuffers::FlatBufferBuilder fbb(128);
            auto req = CreateMarketDataRequest(fbb, sym, MDType_L3, SubType_subscribe);
            fbb.Finish(req);
            md_client_->send(fbb.GetBufferPointer(), fbb.GetSize());
        }
    }

    while (running_) {
        on_timer();
        std::this_thread::sleep_for(std::chrono::milliseconds(config_.timer_interval_ms));
    }
    return 0;
}

void AlgoTradingClient::stop() {
    running_ = false;
    if (mgmt_client_) mgmt_client_->stop();
    if (md_client_) md_client_->stop();
}

} // namespace Exchange
