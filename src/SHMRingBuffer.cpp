// SHMRing.cpp
#include "ring/SHMRingBuffer.hpp"
#include <sys/stat.h>
#include <iostream>

namespace Exchange {

template <bool ReadOnly>
SHMRingBufferImpl<ReadOnly>::SHMRingBufferImpl(const std::string& name, size_t capacity)
    : m_name("/" + name), m_capacity(capacity)
{
    if constexpr (ReadOnly) {
        // 唯讀 Observer 模式
        // 1. 以唯讀方式開啟現有 SHM 段
        m_fd = shm_open(m_name.c_str(), O_RDONLY, 0444);
        if (m_fd < 0) {
            throw std::runtime_error("Failed to open SHM in read-only mode. Does it exist?");
        }

        // 2. 先映射 sizeof(SHMRing) 的大小以讀取正確的 capacity 屬性
        void* temp_mmap = mmap(nullptr, sizeof(SHMRing), PROT_READ, MAP_SHARED, m_fd, 0);
        if (temp_mmap == MAP_FAILED) {
            close(m_fd);
            throw std::runtime_error("mmap failed for temp SHMRing in read-only mode");
        }

        SHMRing* temp_ring = reinterpret_cast<SHMRing*>(temp_mmap);
        
        // 自旋等待 creator 初始化完成
        while (temp_ring->ready.load(std::memory_order_acquire) != 1) {
            #if defined(__x86_64__) || defined(_M_X64)
                __builtin_ia32_pause();
            #else
                std::this_thread::yield();
            #endif
        }

        // 驗證 Magic Number
        if (temp_ring->magic.load(std::memory_order_relaxed) != SHM_RING_MAGIC) {
            munmap(temp_mmap, sizeof(SHMRing));
            close(m_fd);
            throw std::runtime_error("SHM Magic number mismatch in read-only mode!");
        }

        // 讀取真實的 capacity 並計算真正的總大小
        m_capacity = temp_ring->capacity;
        m_total_size = sizeof(SHMRing) + m_capacity + sizeof(uint32_t);

        // 解除臨時映射
        munmap(temp_mmap, sizeof(SHMRing));

        // 3. 以唯讀模式重新映射完整大小的記憶體
        m_mmap = mmap(nullptr, m_total_size, PROT_READ, MAP_SHARED, m_fd, 0);
        if (m_mmap == MAP_FAILED) {
            close(m_fd);
            throw std::runtime_error("mmap failed for full SHM in read-only mode");
        }

        m_ring = reinterpret_cast<SHMRing*>(m_mmap);
        m_data = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(m_mmap) + sizeof(SHMRing));
        
        std::cout << "[SHMRing] Connected to SHM in read-only mode. Name=" << m_name 
                  << " Capacity=" << m_capacity << std::endl;
    } else {
        // 一般讀寫 Creator/Attacher 模式
        m_total_size = sizeof(SHMRing) + m_capacity + sizeof(uint32_t); 

        // 嘗試以 O_EXCL 建立。如果檔案已存在，此呼叫會失敗並回傳 EEXIST
        m_fd = shm_open(m_name.c_str(), O_RDWR | O_CREAT | O_EXCL, 0666);
        
        bool is_creator = false;

        if (m_fd >= 0) {
            is_creator = true;
            
            // 調整 SHM 檔案大小
            if (ftruncate(m_fd, m_total_size) == -1) {
                shm_unlink(m_name.c_str());
                throw std::runtime_error("Failed to ftruncate SHM");
            }
        } else if (errno == EEXIST) {
            // 代表 SHM 檔案早就被另一個 Process 創好了
            m_fd = shm_open(m_name.c_str(), O_RDWR, 0666);
            if (m_fd < 0) {
                throw std::runtime_error("Failed to open existing SHM");
            }
        } else {
            throw std::runtime_error("shm_open failed with unknown error");
        }

        // 記憶體映射
        m_mmap = mmap(nullptr, m_total_size, PROT_READ | PROT_WRITE, MAP_SHARED, m_fd, 0);
        if (m_mmap == MAP_FAILED) {
            close(m_fd);
            throw std::runtime_error("mmap failed");
        }

        m_ring = reinterpret_cast<SHMRing*>(m_mmap);
        m_data = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(m_mmap) + sizeof(SHMRing));

        // 根據身份執行「初始化」或「自旋等待」
        if (is_creator) {
            std::cout << "[SHMRing] " << m_name << " not found. Creating and initializing SHM..." << std::endl;
            
            std::memset(m_mmap, 0, m_total_size);

            // 初始化內部結構 (使用 memory_order_relaxed 因為此時還沒有其他人會讀這塊區塊)
            m_ring->prod_head.store(0, std::memory_order_relaxed);
            m_ring->prod_tail.store(0, std::memory_order_relaxed);
            m_ring->cons_head.store(0, std::memory_order_relaxed);
            m_ring->capacity = m_capacity;
            m_ring->mask = m_capacity - 1; // 假設 capacity 是 2 的冪次方
            
            m_ring->magic.store(SHM_RING_MAGIC, std::memory_order_relaxed);
            
            m_ring->ready.store(1, std::memory_order_release);
        } else {
            std::cout << "[SHMRing] " << m_name << " already exists. Waiting for initialization..." << std::endl;
            
            while (m_ring->ready.load(std::memory_order_acquire) != 1) {
                #if defined(__x86_64__) || defined(_M_X64)
                    __builtin_ia32_pause(); // 減少 CPU 功耗與流水線阻塞
                #else
                    std::this_thread::yield();
                #endif
            }

            if (m_ring->magic.load(std::memory_order_relaxed) != SHM_RING_MAGIC) {
                throw std::runtime_error("SHM Magic number mismatch! Corrupted or unexpected memory segment.");
            }
            
            std::cout << "[SHMRing] SHM is ready and verified." << std::endl;
        }
    }
}

template <bool ReadOnly>
SHMRingBufferImpl<ReadOnly>::~SHMRingBufferImpl() {
    if (m_mmap && m_mmap != MAP_FAILED) {
        munmap(m_mmap, m_total_size);
    }
    if (m_fd >= 0) {
        close(m_fd);
    }
}

constexpr uint32_t WRAP_MARKER = 0xFFFFFFFF;

template <bool ReadOnly>
std::optional<ReserveToken> SHMRingBufferImpl<ReadOnly>::reserve(size_t size) requires (!ReadOnly)
{
    if (!size) return std::nullopt;

    const size_t required_space = sizeof(uint32_t) + size;
    if (required_space > m_capacity) return std::nullopt;

    uint64_t old_prod_head, new_prod_head;
    uint64_t tail_offset;
    bool wrapped;

    // 1. Reservation Phase
    while (true) {
        old_prod_head = m_ring->prod_head.load(std::memory_order_acquire);
        uint64_t current_cons_head = m_ring->cons_head.load(std::memory_order_acquire);

        tail_offset = old_prod_head & m_ring->mask;
        const size_t space_to_end = m_capacity - tail_offset;

        if (space_to_end >= required_space) {
            // 情況 A：末端空間足夠
            new_prod_head = old_prod_head + required_space;
            wrapped = false;
        } else {
            // 情況 B：末端空間不夠，需要繞回。總消耗 = 末端填充 + 實際空間
            new_prod_head = old_prod_head + space_to_end + required_space;
            wrapped = true;
        }

        // 檢查剩餘絕對空間是否足夠 (防止 Producer 追上 Consumer)
        if (new_prod_head - current_cons_head > m_capacity) {
            return std::nullopt;
        }

        if (m_ring->prod_head.compare_exchange_weak(old_prod_head, new_prod_head,
                                                    std::memory_order_acquire,
                                                    std::memory_order_relaxed)) {
            break;
        }
    }

    // 2. Slot Preparation Phase
    //    Decide where the payload header lands; write WRAP_MARKER at the tail
    //    boundary when the slot wraps around to the start of the buffer.
    uint8_t* base_ptr = static_cast<uint8_t*>(m_data);
    uint8_t* tail_ptr = base_ptr + tail_offset;

    uint8_t* payload_hdr_ptr;
    if (!wrapped) {
        payload_hdr_ptr = tail_ptr;
    } else {
        *reinterpret_cast<uint32_t*>(tail_ptr) = WRAP_MARKER;
        payload_hdr_ptr = base_ptr;
    }

    // Write the length header into SHM and hand the caller a direct pointer
    // to the payload region so they can write without an extra copy.
    *reinterpret_cast<uint32_t*>(payload_hdr_ptr) = static_cast<uint32_t>(size);

    return ReserveToken {
        .payload       = payload_hdr_ptr + sizeof(uint32_t),
        .size          = size,
        .old_prod_head = old_prod_head,
        .new_prod_head = new_prod_head,
    };
}

// 3. Commit Phase
//    Spin until all earlier reservations have committed, then advance
//    prod_tail to make this slot visible to consumers.
template <bool ReadOnly>
void SHMRingBufferImpl<ReadOnly>::commit(const ReserveToken& token) requires (!ReadOnly)
{
    while (m_ring->prod_tail.load(std::memory_order_acquire) != token.old_prod_head) {
        #if defined(__x86_64__) || defined(_M_X64)
            __builtin_ia32_pause();
        #else
            std::this_thread::yield();
        #endif
    }

    m_ring->prod_tail.store(token.new_prod_head, std::memory_order_release);
}

template <bool ReadOnly>
bool SHMRingBufferImpl<ReadOnly>::enqueue(void* data, size_t size) requires (!ReadOnly)
{
    if (!data) return false;

    auto token = reserve(size);
    if (!token) return false;

    std::memcpy(token->payload, data, size);
    commit(*token);
    return true;
}

template <bool ReadOnly>
std::optional<AcquireToken> SHMRingBufferImpl<ReadOnly>::acquire() requires (!ReadOnly)
{
    uint64_t current_head = m_ring->cons_head.load(std::memory_order_relaxed);
    uint64_t current_tail = m_ring->prod_tail.load(std::memory_order_acquire);

    if (current_head == current_tail) return std::nullopt;

    uint64_t head_offset = current_head & m_ring->mask;
    uint8_t* read_ptr = static_cast<uint8_t*>(m_data) + head_offset;

    uint32_t length = *reinterpret_cast<uint32_t*>(read_ptr);

    if (current_head != current_tail && (length == 0 || length > m_capacity) && length != WRAP_MARKER) {
        std::cerr << "[SHMRing] Corrupt length: " << length << " head=" << current_head << " tail=" << current_tail << " offset=" << head_offset << std::endl;
    }

    // 處理繞回標記：跳過末端 padding，資料在最開頭
    if (length == WRAP_MARKER) {
        uint64_t space_to_end = m_capacity - head_offset;
        uint64_t new_head = current_head + space_to_end;

        // 跳過 wrap padding 後如果追上 tail，代表後面沒有資料
        // 這是純內部 bookkeeping，直接推進 cons_head 並回傳 nullopt
        if (new_head == current_tail) {
            m_ring->cons_head.store(new_head, std::memory_order_relaxed);
            return std::nullopt;
        }

        read_ptr = static_cast<uint8_t*>(m_data);
        length = *reinterpret_cast<uint32_t*>(read_ptr);

        if (length == 0 || length > m_capacity) return std::nullopt;

        return AcquireToken {
            .payload       = read_ptr + sizeof(uint32_t),
            .size          = length,
            .new_cons_head = new_head + sizeof(uint32_t) + length,
        };
    }

    if (length == 0 || length > m_capacity) return std::nullopt;

    return AcquireToken {
        .payload       = read_ptr + sizeof(uint32_t),
        .size          = length,
        .new_cons_head = current_head + sizeof(uint32_t) + length,
    };
}

template <bool ReadOnly>
void SHMRingBufferImpl<ReadOnly>::release(const AcquireToken& token) requires (!ReadOnly)
{
    m_ring->cons_head.store(token.new_cons_head, std::memory_order_release);
}

template <bool ReadOnly>
bool SHMRingBufferImpl<ReadOnly>::dequeue(void** data, size_t* size) requires (!ReadOnly)
{
    if (!data || !size) return false;

    auto token = acquire();
    if (!token) return false;

    *data = const_cast<void*>(token->payload);
    *size = token->size;
    release(*token);
    return true;
}

template <bool ReadOnly>
uint64_t SHMRingBufferImpl<ReadOnly>::get_reserved_depth() const 
{
    if (!m_ring) return 0;
    uint64_t cons = m_ring->cons_head.load(std::memory_order_relaxed);
    uint64_t prod = m_ring->prod_head.load(std::memory_order_relaxed);
    return prod - cons;
}

template <bool ReadOnly>
uint64_t SHMRingBufferImpl<ReadOnly>::get_uncommitted_depth() const 
{
    if (!m_ring) return 0;
    uint64_t tail = m_ring->prod_tail.load(std::memory_order_relaxed);
    uint64_t head = m_ring->prod_head.load(std::memory_order_relaxed);
    return head - tail;
}

template <bool ReadOnly>
double SHMRingBufferImpl<ReadOnly>::get_occupancy_ratio() const
{
    if (!m_ring || m_capacity == 0) return 0.0;
    return static_cast<double>(get_reserved_depth()) / static_cast<double>(m_capacity);
}

template class SHMRingBufferImpl<false>;
template class SHMRingBufferImpl<true>;

} // namespace Exchange