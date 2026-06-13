#pragma once

#include "ring/SHMRingBuffer.hpp"
#include <string>
#include <sstream>

namespace Exchange {

class AsyncLogger {
public:
    static AsyncLogger& get() {
        static AsyncLogger instance;
        return instance;
    }

    void init(const std::string& name);

    void log(const std::string& msg);

    ~AsyncLogger();

private:
    AsyncLogger();
    SHMRingBuffer* ring_buffer_;
};

#define LOG(msg) do { \
    std::ostringstream _oss; \
    _oss << msg; \
    Exchange::AsyncLogger::get().log(_oss.str()); \
} while(0)

} // namespace Exchange
