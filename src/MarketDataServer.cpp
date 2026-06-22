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
    std::cout << "[MarketDataServer] Shutdown complete." << std::endl;
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

std::pair<std::shared_ptr<L3Book>, OrderResponseT*> MarketDataServer::get_or_create_book(uint32_t symbol_id) {
    std::lock_guard<std::mutex> lock(books_mutex_);
    auto it = books_.find(symbol_id);
    if (it == books_.end()) {
        auto book = std::make_shared<L3Book>();
        book->symbol_id = symbol_id;
        auto& state = books_[symbol_id];
        state.book = book;
        return {state.book, nullptr};
    }
    return {it->second.book, it->second.pending};
}

void MarketDataServer::setup_handlers() {
    // Set up the raw message handler
    ws_adaptor_->set_message_handler([this](WSClientPtr client, const void* data, size_t size) {
        flatbuffers::Verifier verifier(reinterpret_cast<const uint8_t*>(data), size);
        if (VerifyMarketDataRequestBuffer(verifier)) {
            auto req = GetMarketDataRequest(data);
            this->handle_market_data_request(client, req);
        }
    });

    // Set up close handler to clean up closed connections
    ws_adaptor_->set_close_handler([this](WSClientPtr client) {
        std::lock_guard<std::mutex> lock(subs_mutex_);
        for (auto& [sym, set] : l2_subscribers_) {
            set.erase(client);
        }
        for (auto& [sym, set] : l3_subscribers_) {
            set.erase(client);
        }
    });
}

void MarketDataServer::handle_market_data_request(WSClientPtr client, const MarketDataRequest* req) {
    uint32_t symbol_id = req->symbol_id();
    MDType md_type = req->md_type();
    SubType sub_type = req->sub_type();

    if (sub_type == SubType_unsubscribe) {
        std::lock_guard<std::mutex> lock(subs_mutex_);
        if (md_type == MDType_L2) {
            l2_subscribers_[symbol_id].erase(client);
            std::cout << "[MarketDataServer] Client unsubscribed L2 for symbol " << symbol_id << std::endl;
        } else if (md_type == MDType_L3) {
            l3_subscribers_[symbol_id].erase(client);
            std::cout << "[MarketDataServer] Client unsubscribed L3 for symbol " << symbol_id << std::endl;
        }
        return;
    }

    if (sub_type == SubType_subscribe) {
        auto [book, pending_ptr] = get_or_create_book(symbol_id);

        if (md_type == MDType_L2) {
            {
                std::lock_guard<std::mutex> lock(subs_mutex_);
                l2_subscribers_[symbol_id].insert(client);
            }
            std::cout << "[MarketDataServer] Client subscribed L2 for symbol " << symbol_id << std::endl;

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
            {
                std::lock_guard<std::mutex> lock(subs_mutex_);
                l3_subscribers_[symbol_id].insert(client);
            }
            std::cout << "[MarketDataServer] Client subscribed L3 for symbol " << symbol_id << std::endl;

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
                        auto l3_update = CreateL3Update(fbb, symbol_id, ExecType_New, 0, order.order_id, order.side, order.price, order.qty, 0);
                        auto md_update = CreateMarketDataUpdate(fbb, MarketDataUpdateData_L3Update, l3_update.Union());
                        fbb.Finish(md_update);
                        client->send(fbb.GetBufferPointer(), fbb.GetSize());
                    }
                }
                // Asks (lowest to highest)
                for (auto const& [price, level] : book->asks) {
                    for (uint64_t order_id : level.queue) {
                        auto const& order = book->orders.at(order_id);
                        fbb.Clear();
                        auto l3_update = CreateL3Update(fbb, symbol_id, ExecType_New, 0, order.order_id, order.side, order.price, order.qty, 0);
                        auto md_update = CreateMarketDataUpdate(fbb, MarketDataUpdateData_L3Update, l3_update.Union());
                        fbb.Finish(md_update);
                        client->send(fbb.GetBufferPointer(), fbb.GetSize());
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
    bool has_l3_subs = false;
    {
        std::lock_guard<std::mutex> lock(subs_mutex_);
        auto it = l3_subscribers_.find(symbol_id);
        if (it != l3_subscribers_.end() && !it->second.empty()) {
            has_l3_subs = true;
        }
    }
    if (!has_l3_subs) return;

    flatbuffers::FlatBufferBuilder fbb(512);
    auto l3_update = CreateL3Update(fbb, symbol_id, exec_type, msg_seq_num, order_id, side, p, q, timestamp);
    auto md_update = CreateMarketDataUpdate(fbb, MarketDataUpdateData_L3Update, l3_update.Union());
    fbb.Finish(md_update);

    std::lock_guard<std::mutex> lock(subs_mutex_);
    for (auto& client : l3_subscribers_[symbol_id]) {
        client->send(fbb.GetBufferPointer(), fbb.GetSize());
    }
}

void MarketDataServer::publish_l2_update(uint32_t symbol_id, Side side, int64_t price, uint64_t new_qty, uint64_t msg_seq_num, uint64_t timestamp) {
    bool has_l2_subs = false;
    {
        std::lock_guard<std::mutex> lock(subs_mutex_);
        auto it = l2_subscribers_.find(symbol_id);
        if (it != l2_subscribers_.end() && !it->second.empty()) { 
            has_l2_subs = true;
        }
    }
    if (!has_l2_subs) return;

    flatbuffers::FlatBufferBuilder fbb(512);
    auto l2_update = CreateL2Update(fbb, symbol_id, msg_seq_num, side, price, new_qty, timestamp);
    auto md_update = CreateMarketDataUpdate(fbb, MarketDataUpdateData_L2Update, l2_update.Union());
    fbb.Finish(md_update);

    std::lock_guard<std::mutex> lock(subs_mutex_);
    for (auto& client : l2_subscribers_[symbol_id]) {
        client->send(fbb.GetBufferPointer(), fbb.GetSize());
    }
}

void MarketDataServer::process_market_update(const OrderResponseT* resp)
{
    auto [book, pending_ptr] = get_or_create_book(resp->symbol_id);

    if (check_exec(resp->exec_type, EXEC_MASK_NOT_EXECUTIONS)) {
        return;
    }

    uint64_t timestamp = 0; // Or whatever timestamp we have
    
    if (pending_ptr) {
        if (check_exec(resp->exec_type, EXEC_MASK_LATENCY_TRACK)) {
            std::cerr << "[MarketDataServer] FATAL: Received new crossing order " << resp->order_id 
                        << " while pending_order " << pending_ptr->order_id << " is still active!" << std::endl;
            throw std::runtime_error("Multiple pending orders");
        } else if (check_exec(resp->exec_type, EXEC_MASK_REMOVE_OPEN) && resp->order_id == pending_ptr->order_id) {
            delete pending_ptr;
            pending_ptr = nullptr;
            return;
        }
    } else if ((resp->exec_type == ExecType_New /* TODO: Should support ExecType_Replaced here */) && crosses(resp->side, resp->p, book)) {
        pending_ptr = new OrderResponseT {*resp};
        return;
    }

    validated_update(book, resp, timestamp);

    if (!pending_ptr) return;
    if ((pending_ptr->exec_type == ExecType_New) && !crosses(pending_ptr->side, pending_ptr->p, book)) {
        validated_update(book, pending_ptr, timestamp);
        delete pending_ptr;
        pending_ptr = nullptr;
    }
}

void MarketDataServer::validated_update(std::shared_ptr<L3Book> book, const OrderResponseT* resp, uint64_t timestamp)
{
    uint64_t qty_new = book->update(resp->exec_type, resp->order_id, resp->side, resp->p, resp->q);
    publish_l3_update(resp->symbol_id, resp->exec_type, resp->order_id, resp->side, resp->p, resp->q, resp->msg_seq_num, timestamp);
    publish_l2_update(resp->symbol_id, resp->side, resp->p, qty_new, resp->msg_seq_num, timestamp);
}

} // namespace Exchange
