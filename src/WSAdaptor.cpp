#include "WSAdaptor.hpp"
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/core/ignore_unused.hpp>
#include <set>
#include <mutex>
#include <queue>
#include <cstdio>

namespace beast = boost::beast;         // from <boost/beast.hpp>
namespace websocket = beast::websocket; // from <boost/beast/websocket.hpp>
namespace net = boost::asio;            // from <boost/asio.hpp>
using tcp = boost::asio::ip::tcp;       // from <boost/asio/ip/tcp.hpp>

namespace Exchange {

// -----------------------------------------------------------------------------
// WSSession Implementation
// -----------------------------------------------------------------------------

class WSSession : public std::enable_shared_from_this<WSSession> {
    websocket::stream<beast::tcp_stream> ws_;
    beast::flat_buffer buffer_;
    std::set<uint32_t> subscriptions_;
    bool subscribe_all_ = true;
    std::mutex sub_mutex_;
    
    std::queue<std::string> write_queue_;
    std::mutex write_mutex_;
    std::atomic<bool> closed_{false};

public:
    explicit WSSession(tcp::socket&& socket) : ws_(std::move(socket)) {}

    bool is_closed() const { return closed_; }

    void run() {
        fprintf(stderr, "[WS] Session starting run\n");
        net::dispatch(ws_.get_executor(),
            beast::bind_front_handler(&WSSession::on_run, shared_from_this()));
    }

    void on_run() {
        ws_.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));
        ws_.binary(true);
        ws_.async_accept(beast::bind_front_handler(&WSSession::on_accept, shared_from_this()));
    }

    void on_accept(beast::error_code ec) {
        if (ec) {
            closed_ = true;
            return fail(ec, "accept");
        }
        fprintf(stderr, "[WS] Client connected and handshaked\n");
        do_read();
    }

    void do_read() {
        ws_.async_read(buffer_, beast::bind_front_handler(&WSSession::on_read, shared_from_this()));
    }

    void on_read(beast::error_code ec, std::size_t bytes_transferred) {
        boost::ignore_unused(bytes_transferred);
        if (ec == websocket::error::closed) {
            closed_ = true;
            fprintf(stderr, "[WS] Client closed connection gracefully\n");
            return;
        }
        if (ec) {
            closed_ = true;
            return fail(ec, "read");
        }

        std::string msg = beast::buffers_to_string(buffer_.data());
        buffer_.consume(buffer_.size());
        
        fprintf(stderr, "[WS] Received message: %s\n", msg.c_str());

        {
            std::lock_guard<std::mutex> lock(sub_mutex_);
            if (msg.find("sub ") == 0) {
                try {
                    size_t first_digit = msg.find_first_of("0123456789", 4);
                    if (first_digit != std::string::npos) {
                        uint32_t sym = std::stoul(msg.substr(first_digit));
                        subscriptions_.insert(sym);
                        subscribe_all_ = false;
                        fprintf(stderr, "[WS] Session subscribed to symbol %u\n", sym);
                    }
                } catch (...) {
                    fprintf(stderr, "[WS] Failed to parse sub message\n");
                }
            } else if (msg.find("unsub ") == 0) {
                try {
                    size_t first_digit = msg.find_first_of("0123456789", 6);
                    if (first_digit != std::string::npos) {
                        uint32_t sym = std::stoul(msg.substr(first_digit));
                        subscriptions_.erase(sym);
                        if (subscriptions_.empty()) subscribe_all_ = true;
                        fprintf(stderr, "[WS] Session unsubscribed from symbol %u\n", sym);
                    }
                } catch (...) {
                    fprintf(stderr, "[WS] Failed to parse unsub message\n");
                }
            }
        }

        do_read();
    }

    void send(std::string data, uint32_t symbol_id) {
        {
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
                do_write();
            }
        });
    }

    void do_write() {
        std::string* next_msg = nullptr;
        {
            std::lock_guard<std::mutex> lock(write_mutex_);
            if (write_queue_.empty()) return;
            next_msg = &write_queue_.front();
        }

        ws_.async_write(net::buffer(*next_msg), [this, self = shared_from_this()](beast::error_code ec, std::size_t) {
            if (ec) {
                closed_ = true;
                return fail(ec, "write");
            }
            {
                std::lock_guard<std::mutex> lock(write_mutex_);
                write_queue_.pop();
            }
            do_write();
        });
    }

private:
    void fail(beast::error_code ec, char const* what) {
        fprintf(stderr, "[WS] %s: %s\n", what, ec.message().c_str());
    }
};

// -----------------------------------------------------------------------------
// WSListener Implementation
// -----------------------------------------------------------------------------

class WSListener : public std::enable_shared_from_this<WSListener> {
    net::io_context& ioc_;
    tcp::acceptor acceptor_;
    std::set<std::shared_ptr<WSSession>> sessions_;
    std::mutex session_mutex_;

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
            fprintf(stderr, "[WS] Listen error: %s\n", ec.message().c_str());
        } else {
            fprintf(stderr, "[WS] Listening on %s:%u\n", 
                endpoint.address().to_string().c_str(), endpoint.port());
        }
    }

    void run() { do_accept(); }

    void do_accept() {
        acceptor_.async_accept(net::make_strand(ioc_),
            beast::bind_front_handler(&WSListener::on_accept, shared_from_this()));
    }

    void on_accept(beast::error_code ec, tcp::socket socket) {
        if (ec) {
            fail(ec, "accept");
        } else {
            auto session = std::make_shared<WSSession>(std::move(socket));
            {
                std::lock_guard<std::mutex> lock(session_mutex_);
                sessions_.insert(session);
            }
            session->run();
        }
        do_accept();
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

private:
    void fail(beast::error_code ec, char const* what) {
        fprintf(stderr, "[WS] %s: %s\n", what, ec.message().c_str());
    }
};

// -----------------------------------------------------------------------------
// WSAdaptor Implementation
// -----------------------------------------------------------------------------

WSAdaptor::WSAdaptor(int port) {
    auto const address = net::ip::make_address("0.0.0.0");
    listener_ = std::make_shared<WSListener>(ioc_, tcp::endpoint{address, static_cast<unsigned short>(port)});
    listener_->run();
    
    ioc_thread_ = std::thread([this]() {
        ioc_.run();
    });
}

WSAdaptor::~WSAdaptor() {
    ioc_.stop();
    if (ioc_thread_.joinable()) ioc_thread_.join();
}

void WSAdaptor::publish(const Exchange::L2Update* l2_update, const void* raw_data, size_t raw_size) {
    std::string data(static_cast<const char*>(raw_data), raw_size);
    listener_->broadcast(data, l2_update->symbol_id());
}

} // namespace Exchange
