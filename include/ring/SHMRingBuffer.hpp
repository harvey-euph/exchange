// SHMRing.hpp
#pragma once
#include <cstdint>
#include <atomic>
#include <optional>
#include <string>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <stdexcept>
#include <thread>

namespace Exchange {

// 定義 Magic Number 用於識別與驗證
constexpr uint32_t SHM_RING_MAGIC = 0x52494E47; // "RING" 的 ASCII

struct SHMRing {
    std::atomic<uint32_t> magic{0};    // 驗證是否為正確的 shm 格式
    std::atomic<uint32_t> ready{0};    // 0: 初始化中, 1: 初始化完成可安全使用
    
    alignas(64) std::atomic<uint64_t> prod_head{0}; // Producer 預約進度
    alignas(64) std::atomic<uint64_t> prod_tail{0}; // Producer 寫入完成進度
    
    alignas(64) std::atomic<uint64_t> cons_head{0}; // Consumer 讀取進度
    
    uint64_t capacity;
    uint64_t mask;
};

// Token returned by reserve().
// - payload  : pointer to write your data into directly (zero-copy)
// - size     : payload capacity you requested
// The token must be passed unmodified to commit() after you finish writing.
struct ReserveToken {
    void*    payload;        // writable slot for the caller's data
    size_t   size;           // payload size (bytes)
    uint64_t old_prod_head;  // CAS snapshot needed by commit()
    uint64_t new_prod_head;  // advanced head written by reserve()
};

template <bool ReadOnly = false>
class SHMRingBufferImpl {
public:
    SHMRingBufferImpl(const std::string& name, size_t capacity = 16384);
    ~SHMRingBufferImpl();

    // Zero-copy two-phase API ------------------------------------------------
    // Phase 1: atomically claim a slot in the ring.
    //   On success returns a token whose .payload points directly into SHM.
    //   Write up to token.size bytes into token.payload, then call commit().
    //   Returns std::nullopt when the ring is full or the request is too large.
    std::optional<ReserveToken> reserve(size_t size) requires (!ReadOnly);

    // Phase 2: publish the previously reserved slot to consumers.
    //   Must be called exactly once per successful reserve(), with the
    //   unmodified token that was returned.
    void commit(const ReserveToken& token) requires (!ReadOnly);

    // Convenience wrapper: reserve + memcpy + commit in one call.
    bool enqueue(void* data, size_t size) requires (!ReadOnly);

    bool dequeue(void** data, size_t* size) requires (!ReadOnly);

    // 監控與統計指標 API
    constexpr bool is_read_only() const { return ReadOnly; }
    uint64_t get_capacity() const { return m_capacity; }
    uint64_t get_reserved_depth() const;
    uint64_t get_uncommitted_depth() const;
    double get_occupancy_ratio() const;

private:
    std::string m_name;
    int m_fd = -1;
    void* m_mmap = nullptr;
    SHMRing* m_ring = nullptr;
    void* m_data = nullptr;
    size_t m_capacity = 0;
    size_t m_total_size = 0;
};

using SHMRingBuffer = SHMRingBufferImpl<false>;
using SHMObserver = SHMRingBufferImpl<true>;

} // namespace Exchange