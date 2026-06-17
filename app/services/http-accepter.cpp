#include <iostream>
#include <memory>
#include <string>
#include "ring/SHMRingBuffer.hpp"
#include "fbs/exchange_generated.h"
#include "LogUtil.hpp"
#include "define.hpp"
#include "ThreadUtil.hpp"
#include "AffinityConfig.hpp"
#include "HttpServer.hpp"

using namespace Exchange;

int main() {
    try {
        boost::asio::io_context ioc{1};
        int main_core = OH_CORE;
        if (main_core >= 0) {
            set_thread_affinity(main_core, "HttpAccepter");
        }
        
        SHMRingBuffer request_ring(ORDER_REQUEST, ORDER_REQUEST_SIZE);
        
        auto handler = [&request_ring](const http::request<http::vector_body<char>>& req) -> http::response<http::string_body> {
            auto const version = req.version();
            
            if (req.method() == http::verb::post && req.target() == "/order" && req.body().size() >= 8) {
                auto order_req = flatbuffers::GetRoot<OrderRequest>(req.body().data());
                uint64_t exec_id = order_req->exec_id();
                
                logOrderRequest(order_req, "[Accepter] Received Request:");

                if (request_ring.enqueue(const_cast<char*>(req.body().data()), req.body().size())) {
                    std::cout << "[Accepter] Enqueued Request exec_id=" << exec_id << " size=" << req.body().size() << std::endl;
                    http::response<http::string_body> res{http::status::ok, version};
                    res.set(http::field::content_type, "text/plain");
                    res.body() = "Order received: exec_id=" + std::to_string(exec_id);
                    return res;
                } else {
                    std::cerr << "[Accepter] Failed to enqueue request for exec_id=" << exec_id << std::endl;
                    http::response<http::string_body> res{http::status::internal_server_error, version};
                    res.body() = "Internal Server Error: Queue Full";
                    return res;
                }
            }

            http::response<http::string_body> res{http::status::not_found, version};
            res.body() = "Not Found";
            return res;
        };

        HttpServer server(
            {boost::asio::ip::make_address("0.0.0.0"), PORT_HTTP_ACCEPTER},
            "POST, OPTIONS",
            handler
        );
        server.run(ioc);

        std::cout << "[Accepter] Listening on 0.0.0.0:" << PORT_HTTP_ACCEPTER << " (Coroutine mode via HttpServer)" << std::endl;
        ioc.run();
    } catch (const std::exception& e) {
        std::cerr << "[Accepter] Main error: " << e.what() << std::endl;
    }
    return 0;
}
