#pragma once

#include "WSAdaptor.hpp"
#include "mmap_log.h"
#include "Worker.hpp"
#include "L3Book.hpp"
#include "fbs/exchange_generated.h"
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <memory>
#include <atomic>
#include <vector>
#include <optional>
#include <stdexcept>
#include <memory>
#include <atomic>
#include <vector>
#include <optional>
#include <stdexcept>

namespace Exchange {

class MDClient : public std::enable_shared_from_this<MDClient> {
public:
    using MessageHandler = std::function<void(std::shared_ptr<MDClient>, const void*, size_t)>;

    explicit MDClient(WSClientPtr ws) : conn_(ws) {}

    void set_message_handler(MessageHandler handler) {
        msg_handler_ = handler;
    }

    void on_message(const void* data, size_t size) {
        if (msg_handler_) {
            msg_handler_(this->shared_from_this(), data, size);
        }
    }

    void send(const void* data, size_t size) {
        if (conn_) {
            conn_->send(data, size);
        }
    }

    static void bind_adaptor(std::shared_ptr<WSAdaptor> adaptor, 
                             std::function<void(std::shared_ptr<MDClient>)> on_open,
                             std::function<void(std::shared_ptr<MDClient>)> on_close,
                             std::function<void(std::shared_ptr<MDClient>, const void*, size_t)> on_message) 
    {
        adaptor->set_open_handler([on_open, on_close, on_message](WSClientPtr ws) {
            auto client = std::make_shared<MDClient>(ws);

            if (on_message) {
                client->set_message_handler(on_message);
                ws->set_message_handler([client](const void* data, size_t size) {
                    client->on_message(data, size);
                });
            }

            if (on_close) {
                ws->set_close_handler([client, on_close]() {
                    on_close(client);
                });
            }

            if (on_open) {
                on_open(client);
            }
        });
    }

private:
    WSClientPtr conn_;
    MessageHandler msg_handler_;
};

using MDClientPtr = std::shared_ptr<MDClient>;

struct PendingOrder {
    uint64_t order_id;
    Side side;
    int64_t p;
    uint64_t q;
    uint64_t msg_seq_num;
};

class MarketDataServer : public Worker<MarketDataServer> {
public:
    MarketDataServer(int port, mmaplog::MmapReader* response_ring);
    ~MarketDataServer();

    int poll_client();
    int poll_server();

private:
    std::pair<std::shared_ptr<L3Book>, OrderResponseT> get_or_create_book(uint32_t symbol_id);
    void setup_handlers();
    void handle_market_data_request(MDClientPtr client, const MarketDataRequest* req);
    void process_market_update(const OrderResponseT* resp);

    std::shared_ptr<WSAdaptor> ws_adaptor_;
    mmaplog::MmapReader* response_ring_;
    
    std::mutex books_mutex_;
    std::map<uint32_t, std::pair<std::shared_ptr<L3Book>, OrderResponseT>> books_;

    std::mutex subs_mutex_;
    
    std::unordered_map<uint32_t, std::unordered_set<MDClientPtr>> l2_clients_;
    std::unordered_map<uint32_t, std::unordered_set<MDClientPtr>> l3_clients_;
    std::unordered_map<MDClientPtr, std::vector<std::pair<MDType, uint32_t>>> client_subs_;

    bool crosses(Side side, int64_t price, const std::shared_ptr<L3Book>& book) const;
    void __update(std::shared_ptr<L3Book> book, const OrderResponseT* resp, uint64_t timestamp);
    void publish_l3_update(uint32_t symbol_id, ExecType exec_type, uint64_t order_id, Side side, int64_t p, uint64_t q, uint64_t msg_seq_num, uint64_t timestamp);
    void publish_l2_update(uint32_t symbol_id, const std::vector<L2UpdateT>& updates, uint64_t msg_seq_num, uint64_t timestamp);
};

} // namespace Exchange
