#include "mmap_log.h"
#include "fbs/exchange_generated.h"
#include <iostream>
#include <thread>
#include <chrono>

using namespace Exchange;

int main() {
    std::string tmp_dir = "./execution_journals";
    std::cout << "[Live Reader] Starting to listen on " << tmp_dir << "...\n";
    std::cout << "[Live Reader] Press Ctrl+C to stop.\n";
    
    mmaplog::MmapReader reader(tmp_dir);
    int count = 0;
    
    while (true) {
        const void* data = nullptr;
        uint32_t len = 0;
        
        if (reader.read_next(data, len)) {
            if (len >= sizeof(OrderResponseT)) {
                count++;
                const OrderResponseT* resp = reinterpret_cast<const OrderResponseT*>(data);
                std::cout << "[" << count << "] >>> Received Live OrderResponse:\n"
                          << "  exec_id: " << resp->exec_id << "\n"
                          << "  order_id: " << resp->order_id << "\n"
                          << "  client_id: " << resp->client_id << "\n"
                          << "  side: " << (int)resp->side << "\n"
                          << "  price: " << resp->p << "\n"
                          << "  qty: " << resp->q << "\n"
                          << "  exec_type: " << (int)resp->exec_type << "\n"
                          << "-----------------------------------\n";
            } else {
                std::cout << "[Live Reader] Received unknown message of length " << len << "\n";
            }
        } else {
            // No data, wait a bit before polling again
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    return 0;
}
