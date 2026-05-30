#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <iostream>
#include <string>
#include <thread>
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
        tcp::resolver resolver{ioc};
        websocket::stream<beast::tcp_stream> ws{ioc};

        auto const results = resolver.resolve(host, port);
        beast::get_lowest_layer(ws).connect(results);

        ws.handshake(host, "/");
        std::cout << "[WS Client] Connected to " << host << ":" << port << std::endl;

        // Subscribe to symbol
        std::string sub_msg = "sub " + std::to_string(symbol_id);
        ws.write(net::buffer(sub_msg));
        std::cout << "[WS Client] Subscribed to symbol " << symbol_id << std::endl;

        Exchange::L2Book book;
        book.symbol_id = symbol_id;

        std::thread display_thread([&book]() {
            while (true) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                print_book(book);
            }
        });
        display_thread.detach();

        for (;;) {
            beast::flat_buffer buffer;
            ws.read(buffer);
            
            auto l2_update = flatbuffers::GetRoot<Exchange::L2Update>(buffer.data().data());
            book.update(l2_update->side(), l2_update->p(), l2_update->q());
        }
    }
    catch (std::exception const& e) {
        std::cerr << "[WS Client] Error: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
