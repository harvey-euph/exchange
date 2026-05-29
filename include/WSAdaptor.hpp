#pragma once
#include "L2OutputAdaptor.hpp"
#include <memory>
#include <string>
#include <thread>
#include <boost/asio/io_context.hpp>

namespace Exchange {

class WSListener;

/**
 * @brief WebSocket 適配器實作，基於 Boost.Beast
 */
class WSAdaptor : public L2OutputAdaptor {
public:
    WSAdaptor(int port = 9002);
    virtual ~WSAdaptor();

    void publish(const Exchange::L2Update* l2_update, const void* raw_data, size_t raw_size) override;

private:
    boost::asio::io_context ioc_;
    std::shared_ptr<WSListener> listener_;
    std::thread ioc_thread_;
};

} // namespace Exchange
