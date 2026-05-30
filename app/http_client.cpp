#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <iostream>
#include <string>
#include <vector>
#include <chrono>
#include "csv_util.hpp"
#include "fbs/order_generated.h"

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = boost::asio::ip::tcp;

net::awaitable<void> do_client(std::string csv_path) {
    auto executor = co_await net::this_coro::executor;
    tcp::resolver resolver(executor);
    beast::tcp_stream stream(executor);

    std::string host = "127.0.0.1";
    std::string port = "8080";

    Exchange::CSVDataReader reader;
    if (!reader.loadFromCSV(csv_path)) {
        std::cerr << "Failed to load CSV: " << csv_path << std::endl;
        co_return;
    }

    const auto& requests = reader.getRequests();
    std::cout << "Loaded " << requests.size() << " orders from " << csv_path << std::endl;

    auto const results = co_await resolver.async_resolve(host, port, net::use_awaitable);
    co_await stream.async_connect(results, net::use_awaitable);

    for (const auto* order : requests) {
        flatbuffers::FlatBufferBuilder fbb;

        auto or_offset = Exchange::CreateOrderRequest(fbb, 
            order->action(), order->exec_id(), order->order_id(), 
            order->client_id(), order->symbol_id(), 
            order->side(), order->type(), order->p(), 
            order->q(), 0, order->timestamp()
        );
        fbb.Finish(or_offset);

        http::request<http::vector_body<char>> req{http::verb::post, "/order", 11};
        req.set(http::field::host, host);
        req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
        req.set(http::field::content_type, "application/octet-stream");
        
        auto const* data = reinterpret_cast<const char*>(fbb.GetBufferPointer());
        req.body().assign(data, data + fbb.GetSize());
        req.prepare_payload();

        co_await http::async_write(stream, req, net::use_awaitable);

        beast::flat_buffer buffer;
        http::response<http::string_body> res;
        co_await http::async_read(stream, buffer, res, net::use_awaitable);

        std::cout << "[HTTP Client] Sent exec_id=" << order->exec_id() << " Response: " << res.body() << std::endl;

        // 1 second stop for each order
        net::steady_timer timer(executor);
        timer.expires_after(std::chrono::milliseconds(100));
        co_await timer.async_wait(net::use_awaitable);
    }

    beast::error_code ec;
    stream.socket().shutdown(tcp::socket::shutdown_both, ec);
}

int main(int argc, char** argv) {
    try {
        std::string csv_path = "data/basic.csv";
        if (argc > 1) {
            csv_path = argv[1];
        }

        net::io_context ioc{1};
        net::co_spawn(ioc, do_client(csv_path), net::detached);
        ioc.run();
    } catch (std::exception const& e) {
        std::cerr << "[HTTP Client] Error: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
