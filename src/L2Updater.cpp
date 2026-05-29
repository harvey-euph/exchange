#include "L2Updater.hpp"
#include <flatbuffers/flatbuffers.h>
#include <chrono>
#include <iostream>

namespace Exchange {

L2Updater::L2Updater(const std::string& ring_name, unsigned int ring_size)
    : fbb(128) 
{
    try {
        m_ring = new SHMRingBuffer(ring_name, ring_size);
    } catch (const std::exception& e) {
        std::cerr << "[L2Updater] Failed to create SHMRingBuffer: " << e.what() << std::endl;
        m_ring = nullptr;
    }
}

L2Updater::~L2Updater()
{
    if (m_ring) {
        delete m_ring; // 釋放 SHMRingBuffer 物件，其析構子會負責 munmap/close
        m_ring = nullptr;
    }
}

bool L2Updater::update(uint32_t symbol_id, Side side, int64_t price, uint64_t qty)
{
    auto now = std::chrono::duration_cast<std::chrono::microseconds>(
                   std::chrono::system_clock::now().time_since_epoch()
               ).count();

    return update(symbol_id, side, price, qty, static_cast<uint64_t>(now));
}

bool L2Updater::update(uint32_t symbol_id, Side side, int64_t price, uint64_t qty, uint64_t timestamp)
{
    if (m_ring == nullptr) return false;

    // 清理上一次的 FlatBuffer 殘留狀態，重複利用記憶體，避免隱式配置
    fbb.Clear();

    uint64_t seq = m_seq_num.fetch_add(1, std::memory_order_relaxed);

    auto l2_update = CreateL2Update(fbb, symbol_id, seq, side, price, qty, timestamp);
    fbb.Finish(l2_update);

    size_t size = fbb.GetSize();
    void* buf_ptr = fbb.GetBufferPointer();

    // 直接將 FlatBuffer 的序列化資料 enqueue 進入自製的共享記憶體 Ring
    // 自製 ring 的 enqueue 內部應會處理：檢查剩餘空間、memcpy、更新 tail
    if (!m_ring->enqueue(buf_ptr, size)) {
        // Ring 已滿或寫入失敗
        return false; 
    }
    return true;
}

} // namespace Exchange