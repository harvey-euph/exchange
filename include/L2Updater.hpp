#pragma once
#include "fbs/order_generated.h"
#include "ring/SHMRingBuffer.hpp"
#include "define.hpp"
#include <atomic>
#include <string>

namespace Exchange {

class L2Updater {
public:
    // 改為初始化自製認的 SHMRingBuffer
    L2Updater(const std::string& ring_name, unsigned int ring_size = L2_UPDATE_RING_SIZE);
    ~L2Updater();

    bool update(uint32_t symbol_id, Side side, int64_t price, uint64_t qty);
    bool update(uint32_t symbol_id, Side side, int64_t price, uint64_t qty, uint64_t timestamp);

    SHMRingBuffer* get_ring() const { return m_ring; }

private:
    SHMRingBuffer* m_ring = nullptr;
    flatbuffers::FlatBufferBuilder fbb;
    std::atomic<uint64_t> m_seq_num{1};
};

} // namespace Exchange