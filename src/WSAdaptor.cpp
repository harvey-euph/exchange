#include "WSAdaptor.hpp"
#include "ThreadUtil.hpp"
#include <cstdlib>
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
#include <chrono>

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = boost::asio::ip::tcp;

namespace Exchange {

class WSSession : public WSClient, public std::enable_shared_from_this<WSSession> {
    websocket::stream<beast::tcp_stream> ws_;
    beast::flat_buffer buffer_;
    std::set<uint32_t> subscriptions_;
    bool subscribe_all_ = false;
    std::mutex sub_mutex_;
    
    std::queue<std::string> write_queue_;
    std::mutex write_mutex_;
    bool writing_ = false;
    std::atomic<bool> closed_{false};
    std::string remote_info_;

    WSAdaptor::SubscribeHandler sub_handler_;
    WSAdaptor::MessageHandler msg_handler_;
    WSAdaptor::CloseHandler close_handler_;
    
public:
    explicit WSSession(tcp::socket&& socket, 
                       WSAdaptor::SubscribeHandler sub_handler, 
                       WSAdaptor::MessageHandler msg_handler,
                       WSAdaptor::CloseHandler close_handler) 
        : ws_(std::move(socket)), 
          sub_handler_(sub_handler), 
          msg_handler_(msg_handler),
          close_handler_(close_handler)
    {
        try {
            auto ep = ws_.next_layer().socket().remote_endpoint();
            remote_info_ = ep.address().to_string() + ":" + std::to_string(ep.port());
        } catch (...) {
            remote_info_ = "unknown";
        }
    }

    bool is_closed() const { return closed_; }

    void on_close() {
        if (!closed_.exchange(true)) {
            if (close_handler_) {
                close_handler_(shared_from_this());
            }
        }
    }

    net::awaitable<void> start() {
        try {
            // Configure heartbeat and timeouts
            websocket::stream_base::timeout opt;
            opt.handshake_timeout = std::chrono::seconds(20);
            opt.idle_timeout = std::chrono::seconds(30);
            opt.keep_alive_pings = true; // Server will send Pings
            ws_.set_option(opt);

            // Control callback to handle pings/pongs
            ws_.control_callback(
                [this](websocket::frame_type kind, beast::string_view payload) {
                    boost::ignore_unused(payload);
                    if (kind == websocket::frame_type::pong) {
                        // Successfully received pong from client browser
                    }
                });

            ws_.binary(true);

            co_await ws_.async_accept(net::use_awaitable);
            std::cout << "[WSSession] WebSocket Handshake successful for " << remote_info_ << std::endl;

            // Spawn read loop
            net::co_spawn(ws_.get_executor(), read_loop(), net::detached);
        } catch (std::exception const& e) {
            std::cerr << "[WSSession] Handshake error for " << remote_info_ << ": " << e.what() << std::endl;
            on_close();
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
                    } else {
                        if (msg_handler_) msg_handler_(shared_from_this(), msg.data(), msg.size());
                    }
                }
            }
        } catch (std::exception const& e) {
            std::cout << "[WSSession] Read loop ending for " << remote_info_ << " (" << e.what() << ")" << std::endl;
            on_close();
        }
    }

    // WSClient implementation
    void send(const void* data, size_t size) override {
        send_internal(std::string(static_cast<const char*>(data), size), 0, true);
    }

    void send_market_data(const std::string& data, uint32_t symbol_id) {
        send_internal(data, symbol_id, false);
    }

private:
    void send_internal(std::string data, uint32_t symbol_id, bool bypass_sub_check) {
        if (!bypass_sub_check) {
            std::lock_guard<std::mutex> lock(sub_mutex_);
            if (!subscribe_all_ && subscriptions_.find(symbol_id) == subscriptions_.end()) {
                return;
            }
        }

        net::post(ws_.get_executor(), [this, self = shared_from_this(), data = std::move(data)]() mutable {
            {
                std::lock_guard<std::mutex> lock(write_mutex_);
                write_queue_.push(std::move(data));
                if (writing_) return;
                writing_ = true;
            }
            net::co_spawn(ws_.get_executor(), write_loop(), net::detached);
        });
    }

    net::awaitable<void> write_loop() {
        try {
            for (;;) {
                std::string msg;
                {
                    std::lock_guard<std::mutex> lock(write_mutex_);
                    if (write_queue_.empty()) {
                        writing_ = false;
                        co_return;
                    }
                    msg = std::move(write_queue_.front());
                    write_queue_.pop();
                }

                co_await ws_.async_write(net::buffer(msg), net::use_awaitable);
            }
        } catch (std::exception const& e) {
            std::cerr << "[WSSession] Write error for " << remote_info_ << ": " << e.what() << std::endl;
            {
                std::lock_guard<std::mutex> lock(write_mutex_);
                writing_ = false;
            }
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
    WSAdaptor::MessageHandler msg_handler_;
    WSAdaptor::CloseHandler close_handler_;

public:
    WSListener(net::io_context& ioc, tcp::endpoint endpoint)
        : ioc_(ioc), acceptor_(net::make_strand(ioc)) 
    {
        beast::error_code ec;
        acceptor_.open(endpoint.protocol(), ec);
        acceptor_.set_option(net::socket_base::reuse_address(true), ec);
        acceptor_.bind(endpoint, ec);
        acceptor_.listen(net::socket_base::max_listen_connections, ec);
        if (ec) {
            std::cerr << "[WSListener] Failed to listen on " << endpoint << ": " << ec.message() << std::endl;
        } else {
            std::cout << "[WSListener] Listening on " << endpoint << std::endl;
        }
    }

    void set_subscribe_handler(WSAdaptor::SubscribeHandler handler) {
        sub_handler_ = handler;
    }

    void set_message_handler(WSAdaptor::MessageHandler handler) {
        msg_handler_ = handler;
    }

    void set_close_handler(WSAdaptor::CloseHandler handler) {
        close_handler_ = handler;
    }

    net::awaitable<void> run() {
        try {
            for (;;) {
                tcp::socket socket = co_await acceptor_.async_accept(net::use_awaitable);
                
                boost::system::error_code ec_nodelay;
                socket.set_option(tcp::no_delay(true), ec_nodelay);
                
                auto session = std::make_shared<WSSession>(std::move(socket), sub_handler_, msg_handler_, close_handler_);
                std::cout << "[WSListener] Accepted new connection. Starting session..." << std::endl;
                {
                    std::lock_guard<std::mutex> lock(session_mutex_);
                    sessions_.insert(session);
                }
                net::co_spawn(ioc_, session->start(), net::detached);
            }
        } catch (std::exception const& e) {
            std::cerr << "[WSListener] Accept loop error: " << e.what() << std::endl;
        }
    }

    void broadcast_market_data(const std::string& data, uint32_t symbol_id) {
        std::lock_guard<std::mutex> lock(session_mutex_);
        for (auto it = sessions_.begin(); it != sessions_.end();) {
            if ((*it)->is_closed()) {
                it = sessions_.erase(it);
            } else {
                (*it)->send_market_data(data, symbol_id);
                ++it;
            }
        }
    }

    void broadcast_all(const void* data, size_t size) {
        std::string msg(static_cast<const char*>(data), size);
        std::lock_guard<std::mutex> lock(session_mutex_);
        for (auto it = sessions_.begin(); it != sessions_.end();) {
            if ((*it)->is_closed()) {
                it = sessions_.erase(it);
            } else {
                (*it)->send(data, size);
                ++it;
            }
        }
    }
};

struct WSAdaptor::Impl {
    net::io_context ioc;
    std::shared_ptr<WSListener> listener;

    Impl(int port) {
        auto const address = net::ip::make_address("0.0.0.0");
        listener = std::make_shared<WSListener>(ioc, tcp::endpoint{address, static_cast<unsigned short>(port)});
        net::co_spawn(ioc, listener->run(), net::detached);
    }

    ~Impl() {
        ioc.stop();
    }

    size_t poll() {
        if (ioc.stopped()) {
            ioc.restart();
        }
        return ioc.poll();
    }
};

WSAdaptor::WSAdaptor(int port) : pimpl_(std::make_unique<Impl>(port)) {}

WSAdaptor::~WSAdaptor() = default;

size_t WSAdaptor::poll() {
    return pimpl_->poll();
}

void WSAdaptor::publish(const Exchange::L2Update* l2_update, const void* raw_data, size_t raw_size) {
    std::string data(static_cast<const char*>(raw_data), raw_size);
    pimpl_->listener->broadcast_market_data(data, l2_update->symbol_id());
}

void WSAdaptor::publish(const Exchange::L3Update* l3_update, const void* raw_data, size_t raw_size) {
    std::string data(static_cast<const char*>(raw_data), raw_size);
    pimpl_->listener->broadcast_market_data(data, l3_update->symbol_id());
}

void WSAdaptor::set_subscribe_handler(SubscribeHandler handler) {
    pimpl_->listener->set_subscribe_handler(handler);
}

void WSAdaptor::set_message_handler(MessageHandler handler) {
    pimpl_->listener->set_message_handler(handler);
}

void WSAdaptor::set_close_handler(CloseHandler handler) {
    pimpl_->listener->set_close_handler(handler);
}

void WSAdaptor::send(WSClientPtr client, const void* data, size_t size) {
    if (client) client->send(data, size);
}

void WSAdaptor::broadcast(const void* data, size_t size) {
    pimpl_->listener->broadcast_all(data, size);
}

} // namespace Exchange
