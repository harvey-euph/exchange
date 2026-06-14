#include "WSAdaptor.hpp"
#include "ring/SHMRingBuffer.hpp"
#include "define.hpp"
#include "L3Book.hpp"
#include "SignalHandler.hpp"
#include "ThreadUtil.hpp"
#include "AffinityConfig.hpp"
#include "Worker.hpp"
#include <iostream>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <memory>
#include <atomic>
#include <vector>

using namespace Exchange;

class MarketDataServer : public Worker<MarketDataServer> {
public:
    MarketDataServer(int port, SHMRingBuffer* ring_buffer)
        : ws_adaptor_(std::make_shared<WSAdaptor>(port))
        , ring_buffer_(ring_buffer)
    {
        setup_handlers();
    }

    ~MarketDataServer() {
        std::cout << "[MarketDataServer] Shutdown complete." << std::endl;
    }

    int poll_client() {
        return ws_adaptor_->poll() > 0 ? 1 : 0;
    }

    int poll_server() {
        void* data_ptr = nullptr;
        size_t data_size = 0;
        if (ring_buffer_->dequeue(&data_ptr, &data_size)) {
            if (data_ptr != nullptr && data_size > 0) {
                process_market_update(data_ptr, data_size);
            }
            return 1;
        }
        return 0;
    }

private:
    std::shared_ptr<L3Book> get_or_create_book(uint32_t symbol_id) {
        std::lock_guard<std::mutex> lock(books_mutex_);
        auto it = books_.find(symbol_id);
        if (it == books_.end()) {
            auto book = std::make_shared<L3Book>();
            book->symbol_id = symbol_id;
            books_[symbol_id] = book;
            return book;
        }
        return it->second;
    }

    void setup_handlers() {
        // Set up the unified market data request handler
        ws_adaptor_->set_market_data_request_handler([this](WSClientPtr client, const MarketDataRequest* req) {
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
                auto book = get_or_create_book(symbol_id);

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

    void process_market_update(const void* data_ptr, size_t data_size) {
        auto l3_update_root = flatbuffers::GetRoot<L3Update>(data_ptr);
        uint32_t symbol_id = l3_update_root->symbol_id();
        auto book = get_or_create_book(symbol_id);

        // Update the single local L3Book
        book->update(
            l3_update_root->exec_type(),
            l3_update_root->order_id(),
            l3_update_root->side(),
            l3_update_root->p(),
            l3_update_root->q()
        );

        // Check L3 Subscribers
        bool has_l3_subs = false;
        {
            std::lock_guard<std::mutex> lock(subs_mutex_);
            auto it = l3_subscribers_.find(symbol_id);
            if (it != l3_subscribers_.end() && !it->second.empty()) {
                has_l3_subs = true;
            }
        }
        if (has_l3_subs) {
            flatbuffers::FlatBufferBuilder fbb(512);
            auto l3_update = CreateL3Update(
                fbb, symbol_id,
                l3_update_root->exec_type(),
                l3_update_root->seq_num(),
                l3_update_root->order_id(),
                l3_update_root->side(),
                l3_update_root->p(),
                l3_update_root->q(),
                l3_update_root->timestamp()
            );
            auto md_update = CreateMarketDataUpdate(fbb, MarketDataUpdateData_L3Update, l3_update.Union());
            fbb.Finish(md_update);

            std::lock_guard<std::mutex> lock(subs_mutex_);
            for (auto& client : l3_subscribers_[symbol_id]) {
                client->send(fbb.GetBufferPointer(), fbb.GetSize());
            }
        }

        // Check L2 Subscribers
        bool has_l2_subs = false;
        {
            std::lock_guard<std::mutex> lock(subs_mutex_);
            auto it = l2_subscribers_.find(symbol_id);
            if (it != l2_subscribers_.end() && !it->second.empty()) {
                has_l2_subs = true;
            }
        }
        if (has_l2_subs) {
            flatbuffers::FlatBufferBuilder fbb(512);

            if (l3_update_root->side() == Side_None) {
                // Handle complete/clear
                auto l2_update = CreateL2Update(fbb, symbol_id, l3_update_root->seq_num(), Side_None, 0, 0, l3_update_root->timestamp());
                auto md_update = CreateMarketDataUpdate(fbb, MarketDataUpdateData_L2Update, l2_update.Union());
                fbb.Finish(md_update);

                std::lock_guard<std::mutex> lock(subs_mutex_);
                for (auto& client : l2_subscribers_[symbol_id]) {
                    client->send(fbb.GetBufferPointer(), fbb.GetSize());
                }
            } else {
                // Standard update: get the price level total qty
                int64_t price = l3_update_root->p();
                Side side = l3_update_root->side();
                uint64_t new_qty = 0;
                {
                    std::lock_guard<std::mutex> lock(book->mutex);
                    if (side == Side_Buy) {
                        auto level_it = book->bids.find(price);
                        if (level_it != book->bids.end()) {
                            new_qty = level_it->second.total_qty;
                        }
                    } else if (side == Side_Sell) {
                        auto level_it = book->asks.find(price);
                        if (level_it != book->asks.end()) {
                            new_qty = level_it->second.total_qty;
                        }
                    }
                }

                auto l2_update = CreateL2Update(
                    fbb, symbol_id,
                    l3_update_root->seq_num(),
                    l3_update_root->side(),
                    price,
                    new_qty,
                    l3_update_root->timestamp()
                );
                auto md_update = CreateMarketDataUpdate(fbb, MarketDataUpdateData_L2Update, l2_update.Union());
                fbb.Finish(md_update);

                std::lock_guard<std::mutex> lock(subs_mutex_);
                for (auto& client : l2_subscribers_[symbol_id]) {
                    client->send(fbb.GetBufferPointer(), fbb.GetSize());
                }
            }
        }
    }

    std::shared_ptr<WSAdaptor> ws_adaptor_;
    SHMRingBuffer* ring_buffer_;
    
    std::mutex books_mutex_;
    std::map<uint32_t, std::shared_ptr<L3Book>> books_;

    std::mutex subs_mutex_;
    std::unordered_map<uint32_t, std::unordered_set<WSClientPtr>> l2_subscribers_;
    std::unordered_map<uint32_t, std::unordered_set<WSClientPtr>> l3_subscribers_;
};

int main() {
    setup_signals();
    
    int main_core = MD_MAIN_CORE;
    if (main_core >= 0) {
        set_thread_affinity(main_core, "MarketDataServer");
    }

    std::cout << "[MarketDataServer] Connecting to SHMRingBuffer: " << MARKET_DATA_RING << " (size: " << MARKET_DATA_RING_SIZE << ")..." << std::endl;
    SHMRingBuffer* ring_buffer = nullptr;
    try {
        ring_buffer = new SHMRingBuffer(MARKET_DATA_RING, MARKET_DATA_RING_SIZE);
    } catch (const std::exception& e) {
        std::cerr << "[MarketDataServer] FATAL: " << e.what() << std::endl;
        return -1;
    }

    std::cout << "[MarketDataServer] Polling ring buffer and WebSocket events..." << std::endl;
    MarketDataServer server(PORT_MARKET_DATA_SERVER, ring_buffer);
    server.run();

    delete ring_buffer;
    return 0;
}
