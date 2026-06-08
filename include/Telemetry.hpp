#pragma once
#include <atomic>
#include <cstdint>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <string>
#include <stdexcept>
#include <iostream>
#include <cstring>
#include <thread>

namespace Exchange {

struct TelemetryData {
    std::atomic<uint32_t> magic{0};
    std::atomic<uint32_t> ready{0};

    // 撮合核心指標 (Core matching engine)
    std::atomic<uint64_t> core_count{0};
    std::atomic<uint64_t> core_cycles_sum{0};

    // Client Manager 處理指標 (handle_execution_response)
    std::atomic<uint64_t> mgmt_count{0};
    std::atomic<uint64_t> mgmt_cycles_sum{0};

    // 端到端（E2E）總指標 (接收 OrderRequest 到送出 OrderResponse)
    std::atomic<uint64_t> e2e_count{0};
    std::atomic<uint64_t> e2e_cycles_sum{0};
};

class TelemetryProvider {
public:
    TelemetryProvider(const std::string& name = "EXCHANGE_TELEMETRY", bool read_only = false)
        : m_name("/" + name), m_read_only(read_only)
    {
        size_t size = sizeof(TelemetryData);

        if (m_read_only) {
            m_fd = shm_open(m_name.c_str(), O_RDONLY, 0444);
            if (m_fd < 0) {
                throw std::runtime_error("Failed to open Telemetry SHM in read-only mode");
            }

            m_mmap = mmap(nullptr, size, PROT_READ, MAP_SHARED, m_fd, 0);
            if (m_mmap == MAP_FAILED) {
                close(m_fd);
                throw std::runtime_error("mmap failed for Telemetry in read-only mode");
            }
            
            m_data = reinterpret_cast<TelemetryData*>(m_mmap);

            // 等待初始化完成
            while (m_data->ready.load(std::memory_order_acquire) != 1) {
                std::this_thread::yield();
            }
        } else {
            // 嘗試以 O_EXCL 建立
            m_fd = shm_open(m_name.c_str(), O_RDWR | O_CREAT | O_EXCL, 0666);
            bool is_creator = false;

            if (m_fd >= 0) {
                is_creator = true;
                if (ftruncate(m_fd, size) == -1) {
                    shm_unlink(m_name.c_str());
                    close(m_fd);
                    throw std::runtime_error("ftruncate failed for Telemetry");
                }
            } else if (errno == EEXIST) {
                m_fd = shm_open(m_name.c_str(), O_RDWR, 0666);
                if (m_fd < 0) {
                    throw std::runtime_error("Failed to open existing Telemetry SHM");
                }
            } else {
                throw std::runtime_error("shm_open failed for Telemetry with unknown error");
            }

            m_mmap = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, m_fd, 0);
            if (m_mmap == MAP_FAILED) {
                close(m_fd);
                throw std::runtime_error("mmap failed for Telemetry");
            }

            m_data = reinterpret_cast<TelemetryData*>(m_mmap);

            if (is_creator) {
                std::memset(m_mmap, 0, size);
                m_data->magic.store(0x54454C4D, std::memory_order_relaxed); // "TELM"
                m_data->ready.store(1, std::memory_order_release);
            } else {
                while (m_data->ready.load(std::memory_order_acquire) != 1) {
                    std::this_thread::yield();
                }
            }
        }
    }

    ~TelemetryProvider() {
        if (m_mmap && m_mmap != MAP_FAILED) {
            munmap(m_mmap, sizeof(TelemetryData));
        }
        if (m_fd >= 0) {
            close(m_fd);
        }
    }

    TelemetryData* data() { return m_data; }
    const TelemetryData* data() const { return m_data; }

private:
    std::string m_name;
    bool m_read_only;
    int m_fd = -1;
    void* m_mmap = nullptr;
    TelemetryData* m_data = nullptr;
};

} // namespace Exchange
