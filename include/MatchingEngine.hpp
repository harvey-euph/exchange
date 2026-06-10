#pragma once

#include "ring/SHMRingBuffer.hpp"
#include "OrderBook.hpp"
#include "Worker.hpp"
#include <cstdint>

namespace Exchange {

class MatchingEngine : public Worker<MatchingEngine> {
public:
    MatchingEngine(SHMRingBuffer* request_ring, OrderBook* book);

    int poll_client();
    int poll_server();

private:
    SHMRingBuffer* request_ring_;
    OrderBook* book_;
};

} // namespace Exchange
