#pragma once
#include <thread>
#include <chrono>

#define ORDER_REQUEST  "ORDER_REQUEST"
#define ORDER_RESPONSE "ORDER_RESPONSE"
// #define EXCHANGE_TELEMETRY "EXCHANGE_TELEMETRY"

#define ORDER_REQUEST_SIZE  65536
#define ORDER_RESPONSE_SIZE 131072

// Service Ports
#define PORT_CLIENT_MANAGER 9001
#define PORT_MARKET_DATA_SERVER 9002
#define PORT_HTTP_ACCEPTER  8080
#define PORT_PUBLIC_DATA    8081

// Unified sleep duration in milliseconds for dev/test environment polling loops
#define POLL_SLEEP_MS 1

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

namespace Exchange {

// Bitmask to identify executions that update client position (Fill & PartialFill)
constexpr uint8_t EXEC_MASK_POSITION_UPDATE = 0b00000110;

// Bitmask to identify execution reports where the order remains active/open (New, PartialFill, & Replaced)
constexpr uint8_t EXEC_MASK_UPSERT_OPEN = 0b00100011;

// Bitmask to identify execution reports that terminate/remove the open order (Fill & Cancelled)
constexpr uint8_t EXEC_MASK_REMOVE_OPEN = 0b00010100;

// Bitmask to identify immediate client request responses for E2E latency tracking (New, Replaced, & Cancelled)
constexpr uint8_t EXEC_MASK_LATENCY_TRACK = 0b00110001;

// Cancel & Replace
constexpr uint8_t EXEC_MASK_CHANGE_OPEN = 0b00110001;

// NEW & Replace
constexpr uint8_t EXEC_MASK_END_UP_OPEN = 0b00100001;

// Execution
constexpr uint8_t EXEC_MASK_EXECUTIONS = 0b00111111;

// Non-Execution
constexpr uint8_t EXEC_MASK_NOT_EXECUTIONS = ~EXEC_MASK_EXECUTIONS;

#define check_exec(type, mask) ((mask) >> (type)) & 1


} // namespace Exchange