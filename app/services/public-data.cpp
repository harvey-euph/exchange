#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include "fbs/exchange_generated.h"
#include "LogUtil.hpp"
#include "define.hpp"
#include "ThreadUtil.hpp"
#include "AffinityConfig.hpp"
#include "DbUtil.hpp"
#include "SymbolDatabase.hpp"
#include "HttpServer.hpp"

using namespace Exchange;

int main() {
    try {
        boost::asio::io_context ioc{1};
        int main_core = PD_CORE;
        if (main_core >= 0) {
            set_thread_affinity(main_core, "PublicData");
        }
        
#ifdef USE_PGSQL
        auto db = std::make_shared<PostgresSymbolDatabase>(DbUtil::getConnectionString());
#else
        auto db = std::make_shared<InMemorySymbolDatabase>();
#endif

        auto handler = [db](const http::request<http::vector_body<char>>& req) -> http::response<http::string_body> {
            auto const version = req.version();
            
            if (req.method() == http::verb::get) {
                std::string target = std::string(req.target());
                std::string prefix = "/v1/symbol/";
                if (target.rfind(prefix, 0) == 0) {
                    std::string id_str = target.substr(prefix.length());
                    try {
                        uint32_t symbol_id = std::stoul(id_str);
                        DbSymbolInfo info;
                        if (db && db->getSymbolInfo(symbol_id, info)) {
                            flatbuffers::FlatBufferBuilder builder(256);
                            auto name_offset = builder.CreateString(info.name);
                            SymbolInfoBuilder symbol_builder(builder);
                            symbol_builder.add_symbol_id(symbol_id);
                            symbol_builder.add_name(name_offset);
                            symbol_builder.add_price_exp(info.price_exp);
                            symbol_builder.add_price_min_step(info.min_step);
                            symbol_builder.add_price_min(info.min_price);
                            symbol_builder.add_price_max(info.max_price);
                            auto symbol_offset = symbol_builder.Finish();
                            builder.Finish(symbol_offset);

                            http::response<http::string_body> res{http::status::ok, version};
                            res.set(http::field::content_type, "application/octet-stream");
                            res.body() = std::string(reinterpret_cast<const char*>(builder.GetBufferPointer()), builder.GetSize());
                            return res;
                        }
                    } catch (...) {}
                }
            }

            http::response<http::string_body> res{http::status::not_found, version};
            res.body() = "Not Found";
            return res;
        };

        HttpServer server(
            {boost::asio::ip::make_address("0.0.0.0"), PORT_PUBLIC_DATA},
            "GET, OPTIONS",
            handler
        );
        server.run(ioc);

        std::cout << "[PublicData] Listening on 0.0.0.0:" << PORT_PUBLIC_DATA << std::endl;
        ioc.run();
    } catch (const std::exception& e) {
        std::cerr << "[PublicData] Main error: " << e.what() << std::endl;
    }
    return 0;
}
