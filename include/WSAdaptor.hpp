#pragma once
#include "L2OutputAdaptor.hpp"
#include "L3OutputAdaptor.hpp"
#include <memory>
#include <string>
#include <thread>
#include <functional>

namespace Exchange {

/**
 * @brief 抽象的 WebSocket 用戶端控制句柄 (Opaque Handle)
 * 應用層不需要知道具體的 Session 實作
 */
class WSClient {
public:
    virtual ~WSClient() = default;
    virtual void send(const void* data, size_t size) = 0;
};

using WSClientPtr = std::shared_ptr<WSClient>;

/**
 * @brief WebSocket 適配器實作
 */
class WSAdaptor : public L2OutputAdaptor, public L3OutputAdaptor {
public:
    WSAdaptor(int port);
    virtual ~WSAdaptor();

    size_t poll();

    // Market Data Broadcasting
    void publish(const Exchange::L2Update* l2_update, const void* raw_data, size_t raw_size) override;
    void publish(const Exchange::L3Update* l3_update, const void* raw_data, size_t raw_size) override;

    // Subscription & Binary Message Handlers
    using SubscribeHandler = std::function<void(WSClientPtr client, uint32_t id, bool is_subscribe)>;
    void set_subscribe_handler(SubscribeHandler handler);

    using MessageHandler = std::function<void(WSClientPtr client, const void* data, size_t size)>;
    void set_message_handler(MessageHandler handler);

    using CloseHandler = std::function<void(WSClientPtr client)>;
    void set_close_handler(CloseHandler handler);

    // Direct Sending (if app logic needs it)
    void send(WSClientPtr client, const void* data, size_t size);
    
    // Broadcast to all (ignoring symbol_id filters)
    void broadcast(const void* data, size_t size);

private:
    struct Impl;
    std::unique_ptr<Impl> pimpl_;
};

} // namespace Exchange
