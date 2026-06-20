#include "mmap_log.h"
#include "fbs/exchange_generated.h"
#include <iostream>
#include <unistd.h>
#include <sys/wait.h>
#include <thread>
#include <chrono>

using namespace Exchange;

int main() {
    std::string tmp_dir = "./tmp_ipc_test";
    std::system(("mkdir -p " + tmp_dir).c_str());
    std::system(("rm -f " + tmp_dir + "/*").c_str());

    std::cout << "[Main] Forking process...\n";
    pid_t pid = fork();

    if (pid < 0) {
        std::cerr << "Fork failed\n";
        return 1;
    }

    if (pid == 0) {
        // Child Process: Reader
        std::cout << "[Reader] Starting...\n";
        mmaplog::MmapReader reader(tmp_dir);
        
        int messages_received = 0;
        while (messages_received < 3) {
            const void* data = nullptr;
            uint32_t len = 0;
            if (reader.read_next(data, len)) {
                if (len >= sizeof(OrderResponseT)) {
                    const OrderResponseT* resp = reinterpret_cast<const OrderResponseT*>(data);
                    std::cout << "[Reader] Received OrderResponse:\n"
                              << "  exec_id: " << resp->exec_id << "\n"
                              << "  order_id: " << resp->order_id << "\n"
                              << "  client_id: " << resp->client_id << "\n"
                              << "  side: " << (int)resp->side << "\n"
                              << "  price: " << resp->p << "\n"
                              << "  qty: " << resp->q << "\n"
                              << "  exec_type: " << (int)resp->exec_type << "\n"
                              << "-----------------------------------\n";
                    messages_received++;
                } else {
                    std::cout << "[Reader] Received unexpected length: " << len << "\n";
                }
            } else {
                // Yield
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        std::cout << "[Reader] Finished.\n";
        return 0;
    } else {
        // Parent Process: Writer
        std::cout << "[Writer] Starting... (Waiting 100ms to let Reader try opening file)\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        mmaplog::MmapWriter writer(tmp_dir, 1024 * 1024); // Use 1MB for quick test
        
        for (int i = 1; i <= 3; ++i) {
            uint64_t offset;
            void* ptr = writer.reserve(sizeof(OrderResponseT), offset);
            if (ptr) {
                OrderResponseT* resp_ptr = new (ptr) OrderResponseT {
                    .exec_type = ExecType_New,
                    .order_id = static_cast<uint64_t>(1000 + i),
                    .client_id = 42,
                    .exec_id = static_cast<uint64_t>(2000 + i),
                    .side = Side_Buy,
                    .p = 50000 + i * 10,
                    .q = 100,
                    .reject_code = RejectCode_None
                };
                writer.commit(ptr);
                std::cout << "[Writer] Wrote message " << i << " at offset " << offset << "\n";
            } else {
                std::cerr << "[Writer] Reserve failed!\n";
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        // Wait for reader to finish
        waitpid(pid, nullptr, 0);
        std::cout << "[Writer] Reader exited, writer finished.\n";
    }

    return 0;
}
