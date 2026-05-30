#pragma once
#include "L2OutputAdaptor.hpp"
#include <memory>
#include <string>
#include <thread>
#include <functional>
#include <boost/asio/io_context.hpp>

namespace Exchange {

class WSSession;
using WSSessionPtr = std::shared_ptr<WSSession>;

/**
 * @brief WebSocket 適配器實作，基於 Boost.Beast
 */
class WSAdaptor : public L2OutputAdaptor {
public:
    WSAdaptor(int port = 9002);
    virtual ~WSAdaptor();

    void publish(const Exchange::L2Update* l2_update, const void* raw_data, size_t raw_size) override;

    using SubscribeHandler = std::function<void(WSSessionPtr, uint32_t symbol_id, bool is_subscribe)>;
    void set_subscribe_handler(SubscribeHandler handler);

    void send_to_session(WSSessionPtr session, const void* data, size_t size);

private:
    boost::asio::io_context ioc_;
    std::shared_ptr<class WSListener> listener_;
    std::thread ioc_thread_;
};

} // namespace Exchange
