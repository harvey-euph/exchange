#pragma once
#include "define.hpp"
#include "SignalHandler.hpp"

namespace Exchange {

template <typename Derived>
class Worker {
public:
    void run() {
        Derived* derived = static_cast<Derived*>(this);
        while (g_running.load(std::memory_order_relaxed)) {
#ifdef PRODUCTION_MODE
            derived->poll_client();
            derived->poll_server();
            POLL_BACKOFF();
#else
            int has_task = 0;
            has_task |= derived->poll_client();
            has_task |= derived->poll_server();
            if (!has_task) {
                POLL_BACKOFF();
            }
#endif
        }
    }
};

} // namespace Exchange
