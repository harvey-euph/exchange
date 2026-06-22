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

namespace Exchange {

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
    std::pair<std::shared_ptr<L3Book>, OrderResponseT*> get_or_create_book(uint32_t symbol_id);
    void setup_handlers();
    void handle_market_data_request(WSClientPtr client, const MarketDataRequest* req);
    void process_market_update(const OrderResponseT* resp);

    std::shared_ptr<WSAdaptor> ws_adaptor_;
    mmaplog::MmapReader* response_ring_;
    
    struct BookState {
        std::shared_ptr<L3Book> book;
        OrderResponseT *pending;
    };

    std::mutex books_mutex_;
    std::map<uint32_t, BookState> books_;

    std::mutex subs_mutex_;
    std::unordered_map<uint32_t, std::unordered_set<WSClientPtr>> l2_subscribers_;
    std::unordered_map<uint32_t, std::unordered_set<WSClientPtr>> l3_subscribers_;

    bool crosses(Side side, int64_t price, const std::shared_ptr<L3Book>& book) const;
    void validated_update(std::shared_ptr<L3Book> book, const OrderResponseT* resp, uint64_t timestamp);
    void publish_l3_update(uint32_t symbol_id, ExecType exec_type, uint64_t order_id, Side side, int64_t p, uint64_t q, uint64_t msg_seq_num, uint64_t timestamp);
    void publish_l2_update(uint32_t symbol_id, Side side, int64_t price, uint64_t new_qty, uint64_t msg_seq_num, uint64_t timestamp);
};

} // namespace Exchange
