#include "MatchingEngine.hpp"
#include "DbUtil.hpp"
#include "ThreadUtil.hpp"
#include "AffinityConfig.hpp"
#include "SignalHandler.hpp"
#include "AsyncLogger.hpp"
#include <iostream>


int main()
{
    setup_signals();
    Exchange::AsyncLogger::get().init(LOG_RING_ME);

    int main_core = ME_MAIN_CORE;
    if (main_core >= 0) {
        Exchange::set_thread_affinity(main_core, "MatchingEngine");
    }

    std::cout << "[OrderCore] Starting matching engine..." << std::endl;

    // Query DB for symbol 1 parameters
    int64_t min_step = 1;
    int64_t price_offset = 2000;
    size_t max_price_levels = 8192;
    try {
        auto conn = Exchange::DbUtil::getDbConnection();
        pqxx::work w(*conn);
        pqxx::result r = w.exec(
            "SELECT min_step_raw, min_price_raw, max_price_raw FROM symbols WHERE symbol_id = 1"
        );
        if (!r.empty()) {
            int64_t min_step_raw = r[0][0].as<int64_t>();
            int64_t min_price_raw = r[0][1].as<int64_t>();
            int64_t max_price_raw = r[0][2].as<int64_t>();
            
            min_step = min_step_raw;
            price_offset = min_price_raw / min_step_raw;
            max_price_levels = (max_price_raw - min_price_raw) / min_step_raw + 1;
            std::cout << "[OrderCore] Loaded Symbol 1 config from DB: min_step=" << min_step 
                      << ", price_offset=" << price_offset << ", max_price_levels=" << max_price_levels << std::endl;
        } else {
            std::cerr << "[OrderCore] WARNING: Symbol 1 not found in DB, using default parameters" << std::endl;
        }
    } catch (const std::exception& e) {
        std::cerr << "[OrderCore] ERROR querying DB: " << e.what() << ", using default parameters" << std::endl;
    }

    Exchange::SHMRingBuffer response_ring(ORDER_RESPONSE, ORDER_RESPONSE_SIZE);
    Exchange::OrderBook book(1, min_step, price_offset, max_price_levels, &response_ring);

    Exchange::SHMRingBuffer request_ring(ORDER_REQUEST, ORDER_REQUEST_SIZE);

    std::cout << "[OrderCore] Listening for requests on OrderRequest ring..." << std::endl;

    Exchange::MatchingEngine engine(&request_ring, &book);
    engine.run();

    std::cout << "[OrderCore] Shutting down..." << std::endl;
    return 0;
}
