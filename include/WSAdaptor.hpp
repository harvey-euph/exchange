#pragma once
#include <memory>
#include <string>
#include <thread>
#include <functional>
#include <atomic>

namespace Exchange {

class WSClient {
public:
    virtual ~WSClient() = default;
    virtual void send(const void* data, size_t size) = 0;
    
    virtual void close() = 0;

    // per-session handlers
    virtual void set_message_handler(std::function<void(const void*, size_t)> handler) = 0;
    virtual void set_close_handler(std::function<void()> handler) = 0;
};

using WSClientPtr = std::shared_ptr<WSClient>;

class WSAdaptor {
public:
    WSAdaptor(int port);
    virtual ~WSAdaptor();

    size_t poll();

    using OpenHandler = std::function<void(WSClientPtr client)>;
    using MessageHandler = std::function<void(WSClientPtr client, const void* data, size_t size)>;
    using CloseHandler = std::function<void(WSClientPtr client)>;
    
    void set_open_handler(OpenHandler handler);
    void set_message_handler(MessageHandler handler);
    void set_close_handler(CloseHandler handler);
    void send(WSClientPtr client, const void* data, size_t size);
    void broadcast(const void* data, size_t size);

private:
    struct Impl;
    std::unique_ptr<Impl> pimpl_;
};

} // namespace Exchange
