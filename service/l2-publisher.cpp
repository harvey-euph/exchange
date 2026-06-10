#include "ring/SHMRingBuffer.hpp"
#include "fbs/order_generated.h"
#include "WSAdaptor.hpp"
#include "L2Book.hpp"
#include "ThreadUtil.hpp"
#include "AffinityConfig.hpp"
#include <cstdlib>
#include <iostream>
#include <vector>
#include <memory>
#include <atomic>
#include <map>
#include <mutex>
#include "define.hpp"
#include "SignalHandler.hpp"
#include "Worker.hpp"

namespace Exchange {

class L2Publisher : public Worker<L2Publisher> {
public:
    L2Publisher(int port, SHMRingBuffer* ring_buffer)
        : ws_adaptor_(std::make_shared<WSAdaptor>(port))
        , ring_buffer_(ring_buffer)
    {
        auto subscribe_handler = [this](WSClientPtr client, uint32_t symbol_id, bool is_subscribe) {
            if (!is_subscribe) return;

            auto book = get_or_create_book(symbol_id);
            
            flatbuffers::FlatBufferBuilder fbb(1024);

            // 1. Send empty frame (Side=None) to clear old data
            {
                auto l2_update = CreateL2Update(fbb, symbol_id, 0, Side_None, 0, 0, 0);
                fbb.Finish(l2_update);
                client->send(fbb.GetBufferPointer(), fbb.GetSize());
            }

            // 2. Send snapshots
            {
                std::lock_guard<std::mutex> lock(book->mutex);
                for (auto const& [price, qty] : book->bids) {
                    fbb.Clear();
                    auto l2_update = CreateL2Update(fbb, symbol_id, 0, Side_Buy, price, qty, 0);
                    fbb.Finish(l2_update);
                    client->send(fbb.GetBufferPointer(), fbb.GetSize());
                }
                for (auto const& [price, qty] : book->asks) {
                    fbb.Clear();
                    auto l2_update = CreateL2Update(fbb, symbol_id, 0, Side_Sell, price, qty, 0);
                    fbb.Finish(l2_update);
                    client->send(fbb.GetBufferPointer(), fbb.GetSize());
                }
            }
            std::cout << "[L2Publisher] Sent snapshot for symbol " << symbol_id << " to new subscriber." << std::endl;
        };

        ws_adaptor_->set_subscribe_handler(subscribe_handler);
    }

    int poll_client() {
        return ws_adaptor_->poll() > 0 ? 1 : 0;
    }

    int poll_server() {
        void* data_ptr = nullptr;
        size_t data_size = 0;
        if (ring_buffer_->dequeue(&data_ptr, &data_size)) {
            if (data_ptr == nullptr || data_size == 0) {
                return 0;
            }

            auto l2_update = flatbuffers::GetRoot<L2Update>(data_ptr);
            
            auto book = get_or_create_book(l2_update->symbol_id());
            book->update(l2_update->side(), l2_update->p(), l2_update->q());

            ws_adaptor_->publish(l2_update, data_ptr, data_size);
            return 1;
        }
        return 0;
    }

private:
    std::shared_ptr<L2Book> get_or_create_book(uint32_t symbol_id) {
        std::lock_guard<std::mutex> lock(books_mutex_);
        auto it = books_.find(symbol_id);
        if (it == books_.end()) {
            auto book = std::make_shared<L2Book>();
            book->symbol_id = symbol_id;
            books_[symbol_id] = book;
            return book;
        }
        return it->second;
    }

    std::shared_ptr<WSAdaptor> ws_adaptor_;
    SHMRingBuffer* ring_buffer_;
    std::map<uint32_t, std::shared_ptr<L2Book>> books_;
    std::mutex books_mutex_;
};

} // namespace Exchange

int main() 
{
    setup_signals();

    // Use a small trick to clear screen initially
    std::cout << "\033[2J\033[H" << std::flush;

    std::cout << "[L2Publisher] Connecting to SHMRingBuffer: " << L2_UPDATE_RING << "..." << std::endl;

    Exchange::SHMRingBuffer* ring_buffer = nullptr;
    try {
        ring_buffer = new Exchange::SHMRingBuffer(L2_UPDATE_RING, L2_UPDATE_RING_SIZE);
    } catch (const std::exception& e) {
        std::cerr << "[L2Publisher] Failed to connect to SHMRingBuffer: " << e.what() << std::endl;
        return -1;
    }

    int main_core = L2_MAIN_CORE;
    if (main_core >= 0) {
        Exchange::set_thread_affinity(main_core, "L2Publisher_Main");
    }

    std::cout << "[L2Publisher] Connected successfully. Start consuming..." << std::endl;

    Exchange::L2Publisher publisher(9002, ring_buffer);
    publisher.run();

    std::cout << "[L2Publisher] Exiting and cleaning up..." << std::endl;
    delete ring_buffer;

    return 0;
}
