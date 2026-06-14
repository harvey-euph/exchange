#pragma once
#include "fbs/exchange_generated.h"
#include "ring/SHMRingBuffer.hpp"
#include "define.hpp"
#include <atomic>
#include <string>

namespace Exchange {

class L3Updater {
public:
    L3Updater(const std::string& ring_name, unsigned int ring_size = MARKET_DATA_RING_SIZE);
    ~L3Updater();

    bool update(uint32_t symbol_id, ExecType exec_type, uint64_t order_id, Side side, int64_t price, uint64_t qty);
    bool update(uint32_t symbol_id, ExecType exec_type, uint64_t order_id, Side side, int64_t price, uint64_t qty, uint64_t timestamp);

    SHMRingBuffer* get_ring() const { return m_ring; }

private:
    SHMRingBuffer* m_ring = nullptr;
    flatbuffers::FlatBufferBuilder fbb;
    std::atomic<uint64_t> m_seq_num{1};
};

} // namespace Exchange
