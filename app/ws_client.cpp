#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/detached.hpp>
#include <iostream>
#include <string>
#include <thread>
#include <mutex>
#include "fbs/order_generated.h"
#include "L2Book.hpp"

namespace beast = boost::beast;         
namespace websocket = beast::websocket; 
namespace net = boost::asio;            
using tcp = boost::asio::ip::tcp;       

void print_book(const Exchange::L2Book& book) {
    std::lock_guard<std::mutex> lock(const_cast<Exchange::L2Book&>(book).mutex);
    std::cout << "\n--- L2 Book for Symbol " << book.symbol_id << " ---\n";
    std::cout << "  Asks:\n";
    // Print asks in descending order (top of book last)
    int count = 0;
    for (auto it = book.asks.rbegin(); it != book.asks.rend(); ++it) {
        if (book.asks.size() - count <= 5)
            std::cout << "    " << it->first << " : " << it->second << "\n";
        count++;
    }
    std::cout << "  ----------\n";
    std::cout << "  Bids:\n";
    count = 0;
    for (auto it = book.bids.rbegin(); it != book.bids.rend(); ++it) {
        if (count < 5)
            std::cout << "    " << it->first << " : " << it->second << "\n";
        count++;
    }
    std::cout << "------------------------------\n";
}

net::awaitable<void> run_client(std::string host, std::string port, uint32_t symbol_id, Exchange::L2Book& book) {
    auto executor = co_await net::this_coro::executor;
    tcp::resolver resolver{executor};
    websocket::stream<beast::tcp_stream> ws{executor};

    auto const results = co_await resolver.async_resolve(host, port, net::use_awaitable);
    co_await beast::get_lowest_layer(ws).async_connect(results, net::use_awaitable);

    co_await ws.async_handshake(host, "/", net::use_awaitable);
    std::cout << "[WS Client] Connected to " << host << ":" << port << std::endl;

    // Subscribe to symbol
    std::string sub_msg = "sub " + std::to_string(symbol_id);
    co_await ws.async_write(net::buffer(sub_msg), net::use_awaitable);
    std::cout << "[WS Client] Subscribed to symbol " << symbol_id << std::endl;

    for (;;) {
        beast::flat_buffer buffer;
        co_await ws.async_read(buffer, net::use_awaitable);
        
        auto l2_update = flatbuffers::GetRoot<Exchange::L2Update>(buffer.data().data());
        book.update(l2_update->side(), l2_update->p(), l2_update->q());
    }
}

int main(int argc, char** argv)
{
    try {
        std::string host = "127.0.0.1";
        std::string port = "9002";
        uint32_t symbol_id = 1;

        if (argc > 1) host = argv[1];
        if (argc > 2) port = argv[2];
        if (argc > 3) symbol_id = std::stoul(argv[3]);

        net::io_context ioc;

        Exchange::L2Book book;
        book.symbol_id = symbol_id;

        std::thread display_thread([&book]() {
            while (true) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                print_book(book);
            }
        });
        display_thread.detach();

        net::co_spawn(ioc, run_client(host, port, symbol_id, book), [](std::exception_ptr e) {
            if (e) {
                try {
                    std::rethrow_exception(e);
                } catch (std::exception const& ex) {
                    std::cerr << "[WS Client] Error in coroutine: " << ex.what() << std::endl;
                }
            }
        });

        ioc.run();
    }
    catch (std::exception const& e) {
        std::cerr << "[WS Client] Error: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
