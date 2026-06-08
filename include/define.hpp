#pragma once
#include <thread>
#include <chrono>

#define L2_UPDATE_RING "L2_UPDATE_RING"
#define L3_UPDATE_RING "L3_UPDATE_RING"
#define ORDER_REQUEST  "ORDER_REQUEST"
#define ORDER_RESPONSE "ORDER_RESPONSE"

// Unified sleep duration in milliseconds for dev/test environment polling loops
#define POLL_SLEEP_MS 100

// Polling back-off strategy:
// In PRODUCTION_MODE, busy-wait using CPU pause instruction (or yield on non-x86) to minimize latency.
// In dev mode, sleep for POLL_SLEEP_MS to prevent CPU starvation.
#ifdef PRODUCTION_MODE
  #if defined(__x86_64__) || defined(_M_X64)
    #define POLL_BACKOFF() __builtin_ia32_pause()
  #else
    #define POLL_BACKOFF() std::this_thread::yield()
  #endif
#else
  #define POLL_BACKOFF() std::this_thread::sleep_for(std::chrono::milliseconds(POLL_SLEEP_MS))
#endif