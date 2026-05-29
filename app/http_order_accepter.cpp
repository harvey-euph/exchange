#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/strand.hpp>
#include <boost/config.hpp>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <map>
#include <set>
#include <mutex>
#include <condition_variable>
#include "ring/SHMRingBuffer.hpp"
#include "fbs/order_generated.h"

namespace beast = boost::beast;         
namespace http = beast::http;           
namespace net = boost::asio;            
using tcp = boost::asio::ip::tcp;       

class ResponseTracker {
    std::mutex mutex_;
    std::condition_variable cv_;
    std::map<uint64_t, std::string> responses_;
    std::set<uint64_t> pending_;
public:
    void expect(uint64_t exec_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.insert(exec_id);
    }
    void deliver(uint64_t exec_id, const std::string& data) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pending_.count(exec_id)) {
            responses_[exec_id] = data;
            pending_.erase(exec_id);
            cv_.notify_all();
        }
    }
    std::string wait_for(uint64_t exec_id, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (cv_.wait_for(lock, timeout, [this, exec_id] { return responses_.count(exec_id); })) {
            std::string res = std::move(responses_[exec_id]);
            responses_.erase(exec_id);
            return res;
        }
        pending_.erase(exec_id);
        return "";
    }
};

ResponseTracker g_tracker;

void response_listener_thread() {
    try {
        Exchange::SHMRingBuffer response_ring("OrderResponse", 16384);
        void* data_ptr = nullptr;
        size_t data_size = 0;
        while (true) {
            if (response_ring.dequeue(&data_ptr, &data_size)) {
                if (data_ptr && data_size >= 8) {
                    auto resp = flatbuffers::GetRoot<Exchange::OrderResponse>(data_ptr);
                    g_tracker.deliver(resp->exec_id(), std::string(static_cast<const char*>(data_ptr), data_size));
                }
            } else {
                std::this_thread::sleep_for(std::chrono::microseconds(10));
            }
        }
    } catch (...) {}
}

class session : public std::enable_shared_from_this<session> {
    beast::tcp_stream stream_;
    beast::flat_buffer buffer_;
    Exchange::SHMRingBuffer& request_ring_;
public:
    session(tcp::socket&& socket, Exchange::SHMRingBuffer& ring) : stream_(std::move(socket)), request_ring_(ring) {}
    void run() { do_read(); }
    void do_read() {
        auto req = std::make_shared<http::request<http::vector_body<char>>>();
        http::async_read(stream_, buffer_, *req, beast::bind_front_handler(&session::on_read, shared_from_this(), req));
    }
    void on_read(std::shared_ptr<http::request<http::vector_body<char>>> req, beast::error_code ec, std::size_t) {
        if (ec) return;
        handle_request(std::move(*req));
    }
    void handle_request(http::request<http::vector_body<char>>&& req) {
        auto const version = req.version();
        auto const set_cors = [](auto& res) {
            res.set(http::field::access_control_allow_origin, "*");
            res.set(http::field::access_control_allow_methods, "POST, OPTIONS");
            res.set(http::field::access_control_allow_headers, "Content-Type");
        };
        if (req.method() == http::verb::options) {
            auto res = std::make_shared<http::response<http::empty_body>>(http::status::ok, version);
            set_cors(*res); res->prepare_payload();
            http::async_write(stream_, *res, [this, self=shared_from_this(), res](beast::error_code ec, std::size_t){ if(!ec) do_read(); });
            return;
        }
        if (req.method() == http::verb::post && req.target() == "/order" && req.body().size() >= 8) {
            auto order_req = flatbuffers::GetRoot<Exchange::OrderRequest>(req.body().data());
            uint64_t exec_id = order_req->exec_id();
            std::cout << "[Accepter] Request exec_id=" << exec_id << " order_id=" << order_req->order_id() << std::endl;
            g_tracker.expect(exec_id);
            request_ring_.enqueue(req.body().data(), req.body().size());
            std::string resp_data = g_tracker.wait_for(exec_id, std::chrono::seconds(2));
            if (!resp_data.empty()) {
                auto res = std::make_shared<http::response<http::vector_body<char>>>(http::status::ok, version);
                res->set(http::field::content_type, "application/x-flatbuffers");
                set_cors(*res); res->body().assign(resp_data.begin(), resp_data.end()); res->prepare_payload();
                std::cout << "[Accepter] Response sent for exec_id=" << exec_id << std::endl;
                http::async_write(stream_, *res, [this, self=shared_from_this(), res](beast::error_code ec, std::size_t){ if(!ec) do_read(); });
                return;
            } else { std::cerr << "[Accepter] Timeout for exec_id=" << exec_id << std::endl; }
        }
        auto res = std::make_shared<http::response<http::string_body>>(http::status::not_found, version);
        set_cors(*res); res->body() = "Error or Timeout"; res->prepare_payload();
        http::async_write(stream_, *res, [this, self=shared_from_this(), res](beast::error_code ec, std::size_t){ if(!ec) do_read(); });
    }
};

int main() {
    try {
        net::io_context ioc{1};
        Exchange::SHMRingBuffer request_ring("OrderRequest", 16384);
        std::thread(response_listener_thread).detach();
        tcp::acceptor acceptor{ioc, {net::ip::make_address("0.0.0.0"), 8080}};
        std::cout << "[Accepter] Listening on 0.0.0.0:8080" << std::endl;
        std::function<void()> do_accept = [&](){
            acceptor.async_accept([&](beast::error_code ec, tcp::socket socket){
                if(!ec) std::make_shared<session>(std::move(socket), request_ring)->run();
                do_accept();
            });
        };
        do_accept(); ioc.run();
    } catch (const std::exception& e) { std::cerr << "[Accepter] Main error: " << e.what() << std::endl; }
    return 0;
}
