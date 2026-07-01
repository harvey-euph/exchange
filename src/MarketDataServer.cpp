#include "LogUtil.hpp"
#include "MarketDataServer.hpp"
#include <iostream>
#include "define.hpp"

namespace Exchange {

MarketDataServer::MarketDataServer(int port, mmaplog::MmapReader* response_ring)
    : ws_adaptor_(std::make_shared<WSAdaptor>(port))
    , response_ring_(response_ring)
{
    setup_handlers();
}

MarketDataServer::~MarketDataServer() {
    LOG_INFO("[MarketDataServer] Shutdown complete.");
}

int MarketDataServer::poll_client() {
    return ws_adaptor_->poll();
}

int MarketDataServer::poll_server()
{
    const void* data = nullptr;
    uint32_t len = 0;
    if (response_ring_->read_next(data, len)) {
        if (len >= sizeof(OrderResponseT)) {
            const OrderResponseT* resp = reinterpret_cast<const OrderResponseT*>(data);
            process_market_update(resp);
        }
        return 1;
    }
    return 0;
}

std::pair<std::shared_ptr<L3Book>, OrderResponseT> MarketDataServer::get_or_create_book(uint32_t symbol_id) {
    std::lock_guard<std::mutex> lock(books_mutex_);
    auto it = books_.find(symbol_id);
    if (it == books_.end()) {
        auto book = std::make_shared<L3Book>();
        book->symbol_id = symbol_id;
        auto& state = books_[symbol_id];
        state.first = book;
        state.second.order_id = 0;
        return state;
    }
    return it->second;
}

void MarketDataServer::setup_handlers()
{
    MDClient::bind_adaptor(
        ws_adaptor_,
        nullptr, // on_open
        [this](MDClientPtr client) { // on_close
            std::lock_guard<std::mutex> lock(subs_mutex_);
            auto it = client_subs_.find(client);
            if (it != client_subs_.end()) {
                for (const auto& [md_type, symbol_id] : it->second) {
                    if (md_type == MDType_L2) {
                        l2_clients_[symbol_id].erase(client);
                    } else if (md_type == MDType_L3) {
                        l3_clients_[symbol_id].erase(client);
                    }
                }
                client_subs_.erase(it);
            }
        },
        [this](MDClientPtr client, const void* data, size_t size) { // on_message
            flatbuffers::Verifier verifier(reinterpret_cast<const uint8_t*>(data), size);
            if (VerifyMarketDataRequestBuffer(verifier)) {
                auto req = GetMarketDataRequest(data);
                this->handle_market_data_request(client, req);
            }
        }
    );
}

void MarketDataServer::handle_market_data_request(MDClientPtr client, const MarketDataRequest* req)
{
    uint32_t symbol_id = req->symbol_id();
    MDType md_type = req->md_type();
    SubType sub_type = req->sub_type();
    if (sub_type == SubType_unsubscribe) {
        std::lock_guard<std::mutex> lock(subs_mutex_);
        if (md_type == MDType_L2) {
            l2_clients_[symbol_id].erase(client);
        } else if (md_type == MDType_L3) {
            l3_clients_[symbol_id].erase(client);
        }

        auto it = client_subs_.find(client);
        if (it != client_subs_.end()) {
            auto& subs = it->second;
            subs.erase(std::remove(subs.begin(), subs.end(), std::make_pair(md_type, symbol_id)), subs.end());
        }
        LOG_INFO("[MarketDataServer] Client unsubscribed %d for symbol %d", (int)md_type, symbol_id);
        return;
    }

    if (sub_type == SubType_subscribe) {
        auto [book, pending] = get_or_create_book(symbol_id);

        {
            std::lock_guard<std::mutex> lock(subs_mutex_);
            if (md_type == MDType_L2) {
                l2_clients_[symbol_id].insert(client);
            } else if (md_type == MDType_L3) {
                l3_clients_[symbol_id].insert(client);
            }

            auto& subs = client_subs_[client];
            auto tag = std::make_pair(md_type, symbol_id);
            if (std::find(subs.begin(), subs.end(), tag) == subs.end()) {
                subs.push_back(tag);
            }
        }
        LOG_INFO("[MarketDataServer] Client subscribed %d for symbol %d", (int)md_type, symbol_id);

        if (md_type == MDType_L2) {

            // Send snapshot
            flatbuffers::FlatBufferBuilder fbb(1024);

            // 1. Send empty L2 frame (Side=None) to clear old data
            {
                auto l2_update = CreateL2Update(fbb, symbol_id, 0, Side_None, 0, 0, 0);
                auto md_update = CreateMarketDataUpdate(fbb, MarketDataUpdateData_L2Update, l2_update.Union());
                fbb.Finish(md_update);
                client->send(fbb.GetBufferPointer(), fbb.GetSize());
            }

            // 2. Send active levels
            {
                std::lock_guard<std::mutex> lock(book->mutex);
                for (auto const& [price, level] : book->bids) {
                    fbb.Clear();
                    auto l2_update = CreateL2Update(fbb, symbol_id, 0, Side_Buy, price, level.total_qty, 0);
                    auto md_update = CreateMarketDataUpdate(fbb, MarketDataUpdateData_L2Update, l2_update.Union());
                    fbb.Finish(md_update);
                    client->send(fbb.GetBufferPointer(), fbb.GetSize());
                }
                for (auto const& [price, level] : book->asks) {
                    fbb.Clear();
                    auto l2_update = CreateL2Update(fbb, symbol_id, 0, Side_Sell, price, level.total_qty, 0);
                    auto md_update = CreateMarketDataUpdate(fbb, MarketDataUpdateData_L2Update, l2_update.Union());
                    fbb.Finish(md_update);
                    client->send(fbb.GetBufferPointer(), fbb.GetSize());
                }
            }
        } else if (md_type == MDType_L3) {

            // Send snapshot
            flatbuffers::FlatBufferBuilder fbb(1024);

            // 1. Send empty L3 frame (ExecType=Complete, Side=None) to clear old data
            {
                auto l3_update = CreateL3Update(fbb, symbol_id, ExecType_Complete, 0, 0, Side_None, 0, 0, 0);
                auto md_update = CreateMarketDataUpdate(fbb, MarketDataUpdateData_L3Update, l3_update.Union());
                fbb.Finish(md_update);
                client->send(fbb.GetBufferPointer(), fbb.GetSize());
            }

            // 2. Send active orders
            {
                std::lock_guard<std::mutex> lock(book->mutex);
                // Bids (highest to lowest)
                for (auto it = book->bids.rbegin(); it != book->bids.rend(); ++it) {
                    for (uint64_t order_id : it->second.queue) {
                        auto const& order = book->orders.at(order_id);
                        fbb.Clear();
                        auto l3_update = CreateL3Update(fbb, symbol_id, ExecType_New, 0, order.order_id, order.side, order.price, order.qty_req, 0);
                        auto md_update = CreateMarketDataUpdate(fbb, MarketDataUpdateData_L3Update, l3_update.Union());
                        fbb.Finish(md_update);
                        client->send(fbb.GetBufferPointer(), fbb.GetSize());

                        if (order.qty_req > order.qty_rem) {
                            fbb.Clear();
                            auto fill_update = CreateL3Update(fbb, symbol_id, ExecType_PartialFill, 0, order.order_id, order.side, order.price, order.qty_req - order.qty_rem, 0);
                            auto md_fill = CreateMarketDataUpdate(fbb, MarketDataUpdateData_L3Update, fill_update.Union());
                            fbb.Finish(md_fill);
                            client->send(fbb.GetBufferPointer(), fbb.GetSize());
                        }
                    }
                }
                // Asks (lowest to highest)
                for (auto const& [price, level] : book->asks) {
                    for (uint64_t order_id : level.queue) {
                        auto const& order = book->orders.at(order_id);
                        fbb.Clear();
                        auto l3_update = CreateL3Update(fbb, symbol_id, ExecType_New, 0, order.order_id, order.side, order.price, order.qty_req, 0);
                        auto md_update = CreateMarketDataUpdate(fbb, MarketDataUpdateData_L3Update, l3_update.Union());
                        fbb.Finish(md_update);
                        client->send(fbb.GetBufferPointer(), fbb.GetSize());

                        if (order.qty_req > order.qty_rem) {
                            fbb.Clear();
                            auto fill_update = CreateL3Update(fbb, symbol_id, ExecType_PartialFill, 0, order.order_id, order.side, order.price, order.qty_req - order.qty_rem, 0);
                            auto md_fill = CreateMarketDataUpdate(fbb, MarketDataUpdateData_L3Update, fill_update.Union());
                            fbb.Finish(md_fill);
                            client->send(fbb.GetBufferPointer(), fbb.GetSize());
                        }
                    }
                }
            }
        }
    }
}

bool MarketDataServer::crosses(Side side, int64_t price, const std::shared_ptr<L3Book>& book) const {
    if (side == Side_Buy) {
        if (book->asks.empty()) return false;
        return price == 0 || price >= book->asks.begin()->first;
    } else if (side == Side_Sell) {
        if (book->bids.empty()) return false;
        return price == 0 || price <= book->bids.begin()->first;
    }
    return false;
}

void MarketDataServer::publish_l3_update(uint32_t symbol_id, ExecType exec_type, uint64_t order_id, Side side, int64_t p, uint64_t q, uint64_t msg_seq_num, uint64_t timestamp) {
    std::vector<MDClientPtr> target_clients;
    {
        std::lock_guard<std::mutex> lock(subs_mutex_);
        auto it = l3_clients_.find(symbol_id);
        if (it != l3_clients_.end()) {
            target_clients.assign(it->second.begin(), it->second.end());
        }
    }
    if (target_clients.empty()) return;

    flatbuffers::FlatBufferBuilder fbb(512);
    auto l3_update = CreateL3Update(fbb, symbol_id, exec_type, msg_seq_num, order_id, side, p, q, timestamp);
    auto md_update = CreateMarketDataUpdate(fbb, MarketDataUpdateData_L3Update, l3_update.Union());
    fbb.Finish(md_update);

    for (auto& client : target_clients) {
        client->send(fbb.GetBufferPointer(), fbb.GetSize());
    }
}

void MarketDataServer::publish_l2_update(uint32_t symbol_id, const std::vector<L2UpdateT>& updates, uint64_t msg_seq_num, uint64_t timestamp) {
    std::vector<MDClientPtr> target_clients;
    {
        std::lock_guard<std::mutex> lock(subs_mutex_);
        auto it = l2_clients_.find(symbol_id);
        if (it != l2_clients_.end()) { 
            target_clients.assign(it->second.begin(), it->second.end());
        }
    }
    if (target_clients.empty() || updates.empty()) return;

    for (const auto& up : updates) {
        flatbuffers::FlatBufferBuilder fbb(512);
        auto l2_update = CreateL2Update(fbb, symbol_id, msg_seq_num, up.side, up.p, up.q, timestamp);
        auto md_update = CreateMarketDataUpdate(fbb, MarketDataUpdateData_L2Update, l2_update.Union());
        fbb.Finish(md_update);

        for (auto& client : target_clients) {
            client->send(fbb.GetBufferPointer(), fbb.GetSize());
        }
    }
}

void MarketDataServer::process_market_update(const OrderResponseT* resp)
{
    if (!check_exec(resp->exec_type, EXEC_MD)) return;

    auto [book, pending] = get_or_create_book(resp->symbol_id);

    uint64_t timestamp = 0; // Or whatever timestamp we have
    
    if (pending.order_id) {
        if (check_exec(resp->exec_type, EXEC_RESP)) {
            LOG_ERROR("[MarketDataServer] FATAL: Received new crossing order %d while pending_order %d is still active!", resp->order_id, pending.order_id);
            throw std::runtime_error("Multiple pending orders");
        } else if (check_exec(resp->exec_type, EXEC_ANN) && resp->order_id == pending.order_id) {
            pending.order_id = 0;
            return;
        }
    } else if (check_exec(resp->exec_type, EXEC_ME) && crosses(resp->side, resp->p, book)) {
        pending = *resp;
        return;
    }

    __update(book, resp, timestamp);

    if (!pending.order_id) return;
    if (crosses(pending.side, pending.p, book)) return;

    __update(book, &pending, timestamp);
    pending.order_id = 0;
}

void MarketDataServer::__update(std::shared_ptr<L3Book> book, const OrderResponseT* resp, uint64_t timestamp)
{
    auto updates = book->update(resp->exec_type, resp->order_id, resp->side, resp->p, resp->q);
    publish_l3_update(resp->symbol_id, resp->exec_type, resp->order_id, resp->side, resp->p, resp->q, resp->msg_seq_num, timestamp);
    publish_l2_update(resp->symbol_id, updates, resp->msg_seq_num, timestamp);
}

} // namespace Exchange
