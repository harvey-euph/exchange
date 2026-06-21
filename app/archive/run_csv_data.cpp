#include <cstdint>
#include <iostream>

#if defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#else
#error "run_csv_data.cpp profiling requires x86 rdtsc support"
#endif

#include "OrderBook.hpp"
#include "csv_util.hpp"

using namespace Exchange;

namespace {
inline uint64_t read_tsc_begin()
{
    _mm_lfence();
    return __rdtsc();
}

inline uint64_t read_tsc_end()
{
    unsigned aux = 0;
    const uint64_t tsc = __rdtscp(&aux);
    _mm_lfence();
    return tsc;
}
} // namespace

int main(int argc, char** argv)
{
    const char* csv_path = argc > 1 ? argv[1] : "data/basic.csv";

    // one-symbol for now
    OrderBook ob(10000, 2000, 8192);
    
    CSVDataReader reader;
    if (!reader.loadFromCSV(csv_path)) {
        return 1;
    }

    uint64_t total_cycles = 0;
    size_t request_count = 0;

    for (const OrderRequest* req : reader.getRequests()) {
        const uint64_t start = read_tsc_begin();
        OrderRequestT native_req; req->UnPackTo(&native_req); ob.processRequest(&native_req);
        const uint64_t end = read_tsc_end();
        total_cycles += (end - start);
        ++request_count;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    if (request_count) {
        std::cout << "[profile] processRequest total_cycles=" << total_cycles
                  << " avg_cycles=" << (total_cycles / request_count)
                  << " requests=" << request_count << '\n';
    }

    return 0;
}
