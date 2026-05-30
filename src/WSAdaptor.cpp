#include "WSAdaptor.hpp"
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/detached.hpp>
#include <boost/core/ignore_unused.hpp>
#include <set>
#include <mutex>
#include <queue>
#include <cstdio>
#include <iostream>

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = boost::asio::ip::tcp;

namespace Exchange {

class WSSession : public std::enable_shared_from_this<WSSession> {
    websocket::stream<beast::tcp_stream> ws_;
    beast::flat_buffer buffer_;
    std::set<uint32_t> subscriptions_;
    bool subscribe_all_ = false;
    std::mutex sub_mutex_;
    
    std::queue<std::string> write_queue_;
    std::mutex write_mutex_;
    std::atomic<bool> closed_{false};
    std::string remote_info_;

    WSAdaptor::SubscribeHandler sub_handler_;
    
public:
    explicit WSSession(tcp::socket&& socket, WSAdaptor::SubscribeHandler sub_handler) 
        : ws_(std::move(socket)), sub_handler_(sub_handler) 
    {
        try {
            auto ep = ws_.next_layer().socket().remote_endpoint();
            remote_info_ = ep.address().to_string() + ":" + std::to_string(ep.port());
        } catch (...) {
            remote_info_ = "unknown";
        }
    }

    bool is_closed() const { return closed_; }

    net::awaitable<void> start() {
        try {
            ws_.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));
            ws_.binary(true);

            co_await ws_.async_accept(net::use_awaitable);

            // Spawn read loop
            net::co_spawn(ws_.get_executor(), read_loop(), net::detached);
        } catch (std::exception const& e) {
            closed_ = true;
        }
    }

    net::awaitable<void> read_loop() {
        try {
            for (;;) {
                co_await ws_.async_read(buffer_, net::use_awaitable);
                
                std::string msg = beast::buffers_to_string(buffer_.data());
                buffer_.consume(buffer_.size());
                
                {
                    std::lock_guard<std::mutex> lock(sub_mutex_);
                    if (msg.find("sub ") == 0) {
                        try {
                            uint32_t sym = std::stoul(msg.substr(4));
                            subscriptions_.insert(sym);
                            if (sub_handler_) sub_handler_(shared_from_this(), sym, true);
                        } catch (...) {}
                    } else if (msg.find("unsub ") == 0) {
                        try {
                            uint32_t sym = std::stoul(msg.substr(6));
                            subscriptions_.erase(sym);
                            if (sub_handler_) sub_handler_(shared_from_this(), sym, false);
                        } catch (...) {}
                    }
                }
            }
        } catch (std::exception const& e) {
            closed_ = true;
        }
    }

    void send(std::string data, uint32_t symbol_id, bool bypass_sub_check = false) {
        if (!bypass_sub_check) {
            std::lock_guard<std::mutex> lock(sub_mutex_);
            if (!subscribe_all_ && subscriptions_.find(symbol_id) == subscriptions_.end()) {
                return;
            }
        }

        net::post(ws_.get_executor(), [this, self = shared_from_this(), data = std::move(data)]() mutable {
            bool write_in_progress = false;
            {
                std::lock_guard<std::mutex> lock(write_mutex_);
                write_in_progress = !write_queue_.empty();
                write_queue_.push(std::move(data));
            }
            if (!write_in_progress) {
                net::co_spawn(ws_.get_executor(), write_loop(), net::detached);
            }
        });
    }

    net::awaitable<void> write_loop() {
        try {
            for (;;) {
                std::string msg;
                {
                    std::lock_guard<std::mutex> lock(write_mutex_);
                    if (write_queue_.empty()) co_return;
                    msg = std::move(write_queue_.front());
                    write_queue_.pop();
                }

                co_await ws_.async_write(net::buffer(msg), net::use_awaitable);
            }
        } catch (std::exception const& e) {
            closed_ = true;
        }
    }
};

class WSListener : public std::enable_shared_from_this<WSListener> {
    net::io_context& ioc_;
    tcp::acceptor acceptor_;
    std::set<std::shared_ptr<WSSession>> sessions_;
    std::mutex session_mutex_;
    WSAdaptor::SubscribeHandler sub_handler_;

public:
    WSListener(net::io_context& ioc, tcp::endpoint endpoint)
        : ioc_(ioc), acceptor_(net::make_strand(ioc)) 
    {
        beast::error_code ec;
        acceptor_.open(endpoint.protocol(), ec);
        acceptor_.set_option(net::socket_base::reuse_address(true), ec);
        acceptor_.bind(endpoint, ec);
        acceptor_.listen(net::socket_base::max_listen_connections, ec);
    }

    void set_subscribe_handler(WSAdaptor::SubscribeHandler handler) {
        sub_handler_ = handler;
    }

    net::awaitable<void> run() {
        try {
            for (;;) {
                tcp::socket socket = co_await acceptor_.async_accept(net::use_awaitable);
                
                auto session = std::make_shared<WSSession>(std::move(socket), sub_handler_);
                {
                    std::lock_guard<std::mutex> lock(session_mutex_);
                    sessions_.insert(session);
                }
                net::co_spawn(ioc_, session->start(), net::detached);
            }
        } catch (std::exception const& e) {
            // Log or handle error
        }
    }

    void broadcast(const std::string& data, uint32_t symbol_id) {
        std::lock_guard<std::mutex> lock(session_mutex_);
        for (auto it = sessions_.begin(); it != sessions_.end();) {
            if ((*it)->is_closed()) {
                it = sessions_.erase(it);
            } else {
                (*it)->send(data, symbol_id);
                ++it;
            }
        }
    }
};

WSAdaptor::WSAdaptor(int port) {
    auto const address = net::ip::make_address("0.0.0.0");
    listener_ = std::make_shared<WSListener>(ioc_, tcp::endpoint{address, static_cast<unsigned short>(port)});
    
    net::co_spawn(ioc_, listener_->run(), net::detached);
    
    ioc_thread_ = std::thread([this]() { ioc_.run(); });
}

WSAdaptor::~WSAdaptor() {
    ioc_.stop();
    if (ioc_thread_.joinable()) ioc_thread_.join();
}

void WSAdaptor::publish(const Exchange::L2Update* l2_update, const void* raw_data, size_t raw_size) {
    std::string data(static_cast<const char*>(raw_data), raw_size);
    listener_->broadcast(data, l2_update->symbol_id());
}

void WSAdaptor::set_subscribe_handler(SubscribeHandler handler) {
    listener_->set_subscribe_handler(handler);
}

void WSAdaptor::send_to_session(WSSessionPtr session, const void* data, size_t size) {
    std::string msg(static_cast<const char*>(data), size);
    session->send(std::move(msg), 0, true);
}

} // namespace Exchange
