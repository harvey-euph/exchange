#pragma once

#include "ring/SHMRingBuffer.hpp"
#include "fbs/order_generated.h"
#include "WSAdaptor.hpp"
#include "L3Book.hpp"
#include "Worker.hpp"
#include <memory>
#include <map>
#include <mutex>

namespace Exchange {

class L3Publisher : public Worker<L3Publisher> {
public:
    L3Publisher(int port, SHMRingBuffer* ring_buffer);

    int poll_client();
    int poll_server();

private:
    std::shared_ptr<L3Book> get_or_create_book(uint32_t symbol_id);

    std::shared_ptr<WSAdaptor> ws_adaptor_;
    SHMRingBuffer* ring_buffer_;
    std::map<uint32_t, std::shared_ptr<L3Book>> books_;
    std::mutex books_mutex_;
};

} // namespace Exchange
