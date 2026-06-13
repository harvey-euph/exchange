#include "AsyncLogger.hpp"
#include "define.hpp"
#include <iostream>

namespace Exchange {

AsyncLogger::AsyncLogger() : ring_buffer_(nullptr) {
}

AsyncLogger::~AsyncLogger() {
    if (ring_buffer_) {
        delete ring_buffer_;
    }
}

void AsyncLogger::init(const std::string& name) {
    try {
        ring_buffer_ = new SHMRingBuffer(name, LOG_RING_SIZE);
    } catch (const std::exception& e) {
        std::cerr << "[AsyncLogger] Failed to init SHM: " << e.what() << "\n";
    }
}

void AsyncLogger::log(const std::string& msg) {
    if (ring_buffer_) {
        ring_buffer_->enqueue(const_cast<char*>(msg.c_str()), msg.size());
    } else {
        std::cout << msg << "\n";
    }
}

} // namespace Exchange
