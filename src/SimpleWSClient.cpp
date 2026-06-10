#include "SimpleWSClient.hpp"
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/strand.hpp>
#include <iostream>
#include <thread>
#include <queue>
#include <mutex>

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = boost::asio::ip::tcp;

namespace Exchange {

class SimpleWSClientImpl : public SimpleWSClient {
    net::io_context ioc_;
    tcp::resolver resolver_;
    websocket::stream<beast::tcp_stream> ws_;
    std::string host_;
    std::string port_;
    
    beast::flat_buffer buffer_;
    std::thread ioc_thread_;

    std::queue<std::pair<std::string, bool>> write_queue_;
    std::mutex write_mutex_;
    bool writing_ = false;

public:
    SimpleWSClientImpl(const std::string& host, const std::string& port)
        : resolver_(ioc_), ws_(ioc_), host_(host), port_(port) {}

    ~SimpleWSClientImpl() {
        stop();
    }

    bool connect() override {
        try {
            auto const results = resolver_.resolve(host_, port_);
            beast::get_lowest_layer(ws_).connect(results);
            
            boost::system::error_code ec_nodelay;
            beast::get_lowest_layer(ws_).socket().set_option(tcp::no_delay(true), ec_nodelay);
            
            ws_.handshake(host_, "/");
            return true;
        } catch (std::exception const& e) {
            std::cerr << "[SimpleWSClient] Connect error: " << e.what() << std::endl;
            return false;
        }
    }

    void run_async(MessageHandler on_message) override {
        ws_.binary(true);
        read_loop(on_message);
        ioc_thread_ = std::thread([this]() { ioc_.run(); });
    }

    void send(const void* data, size_t size) override {
        send_internal(std::string(static_cast<const char*>(data), size), true);
    }

    void send_text(const std::string& text) override {
        send_internal(text, false);
    }

    void stop() override {
        ioc_.stop();
        if (ioc_thread_.joinable()) ioc_thread_.join();
    }

private:
    void read_loop(MessageHandler on_message) {
        ws_.async_read(buffer_, [this, on_message](beast::error_code ec, std::size_t size) {
            if (!ec) {
                if (on_message) on_message(buffer_.data().data(), size);
                buffer_.consume(size);
                read_loop(on_message);
            }
        });
    }

    void send_internal(std::string data, bool is_binary) {
        net::post(ioc_, [this, data = std::move(data), is_binary]() mutable {
            {
                std::lock_guard<std::mutex> lock(write_mutex_);
                write_queue_.push({std::move(data), is_binary});
                if (writing_) return;
                writing_ = true;
            }
            do_write();
        });
    }

    void do_write() {
        std::string msg;
        bool is_binary;
        {
            std::lock_guard<std::mutex> lock(write_mutex_);
            if (write_queue_.empty()) {
                writing_ = false;
                return;
            }
            msg = std::move(write_queue_.front().first);
            is_binary = write_queue_.front().second;
            write_queue_.pop();
        }

        ws_.binary(is_binary);
        ws_.async_write(net::buffer(msg), [this](beast::error_code ec, std::size_t) {
            if (!ec) {
                do_write();
            } else {
                std::lock_guard<std::mutex> lock(write_mutex_);
                writing_ = false;
            }
        });
    }
};

std::unique_ptr<SimpleWSClient> SimpleWSClient::create(const std::string& host, const std::string& port) {
    return std::make_unique<SimpleWSClientImpl>(host, port);
}

} // namespace Exchange
