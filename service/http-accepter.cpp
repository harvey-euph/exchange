#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/config.hpp>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include "ring/SHMRingBuffer.hpp"
#include "fbs/order_generated.h"
#include "LogUtil.hpp"
#include "define.hpp"

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = boost::asio::ip::tcp;

net::awaitable<void> do_session(tcp::socket socket, Exchange::SHMRingBuffer& request_ring) {
    beast::tcp_stream stream(std::move(socket));
    beast::flat_buffer buffer;

    auto set_cors = [](auto& res) {
        res.set(http::field::access_control_allow_origin, "*");
        res.set(http::field::access_control_allow_methods, "POST, OPTIONS");
        res.set(http::field::access_control_allow_headers, "Content-Type");
    };

    try {
        for (;;) {
            http::request<http::vector_body<char>> req;
            co_await http::async_read(stream, buffer, req, net::use_awaitable);

            auto const version = req.version();

            if (req.method() == http::verb::options) {
                http::response<http::empty_body> res{http::status::ok, version};
                set_cors(res);
                res.prepare_payload();
                co_await http::async_write(stream, res, net::use_awaitable);
                continue;
            }

            if (req.method() == http::verb::post && req.target() == "/order" && req.body().size() >= 8) {
                auto order_req = flatbuffers::GetRoot<Exchange::OrderRequest>(req.body().data());
                uint64_t exec_id = order_req->exec_id();
                
                Exchange::logOrderRequest(order_req, "[Accepter] Received Request:");

                if (request_ring.enqueue(req.body().data(), req.body().size())) {
                    std::cout << "[Accepter] Enqueued Request exec_id=" << exec_id << " size=" << req.body().size() << std::endl;
                    http::response<http::string_body> res{http::status::ok, version};
                    res.set(http::field::content_type, "text/plain");
                    set_cors(res);
                    res.body() = "Order received: exec_id=" + std::to_string(exec_id);
                    res.prepare_payload();
                    std::cout << "[Accepter] Async response sent for exec_id=" << exec_id << std::endl;
                    co_await http::async_write(stream, res, net::use_awaitable);
                } else {
                    std::cerr << "[Accepter] Failed to enqueue request for exec_id=" << exec_id << std::endl;
                    http::response<http::string_body> res{http::status::internal_server_error, version};
                    set_cors(res);
                    res.body() = "Internal Server Error: Queue Full";
                    res.prepare_payload();
                    co_await http::async_write(stream, res, net::use_awaitable);
                }
                continue;
            }

            http::response<http::string_body> res{http::status::not_found, version};
            set_cors(res);
            res.body() = "Not Found";
            res.prepare_payload();
            co_await http::async_write(stream, res, net::use_awaitable);
        }
    } catch (const boost::system::system_error& e) {
        if (e.code() != beast::error::timeout && e.code() != http::error::end_of_stream) {
            std::cerr << "[Accepter] Session error: " << e.what() << std::endl;
        }
    }
}

net::awaitable<void> do_listen(tcp::endpoint endpoint, Exchange::SHMRingBuffer& request_ring) {
    auto executor = co_await net::this_coro::executor;
    tcp::acceptor acceptor(executor, endpoint);

    for (;;) {
        tcp::socket socket = co_await acceptor.async_accept(net::use_awaitable);
        net::co_spawn(executor, do_session(std::move(socket), request_ring), net::detached);
    }
}

int main() {
    try {
        net::io_context ioc{1};
        Exchange::SHMRingBuffer request_ring(ORDER_REQUEST, ORDER_REQUEST_SIZE);
        
        net::co_spawn(ioc, do_listen({net::ip::make_address("0.0.0.0"), 8080}, request_ring), net::detached);

        std::cout << "[Accepter] Listening on 0.0.0.0:8080 (Coroutine mode)" << std::endl;
        ioc.run();
    } catch (const std::exception& e) {
        std::cerr << "[Accepter] Main error: " << e.what() << std::endl;
    }
    return 0;
}
