#include "ring/SHMRingBuffer.hpp"
#include "fbs/order_generated.h"
#include "WSAdaptor.hpp"
#include "L3OutputAdaptor.hpp"
#include "L3Book.hpp"
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

class L3Publisher : public Worker<L3Publisher> {
public:
    L3Publisher(int port, SHMRingBuffer* ring_buffer)
        : ws_adaptor_(std::make_shared<WSAdaptor>(port))
        , ring_buffer_(ring_buffer)
    {
        auto subscribe_handler = [this](WSClientPtr client, uint32_t symbol_id, bool is_subscribe) {
            if (!is_subscribe) return;

            auto book = get_or_create_book(symbol_id);
            
            flatbuffers::FlatBufferBuilder fbb(1024);

            // 1. Send empty frame (ExecType=Complete, Side=None) to clear old data
            {
                auto l3_update = CreateL3Update(fbb, symbol_id, ExecType_Complete, 0, 0, Side_None, 0, 0, 0);
                fbb.Finish(l3_update);
                client->send(fbb.GetBufferPointer(), fbb.GetSize());
            }

            // 2. Send snapshots
            {
                std::lock_guard<std::mutex> lock(book->mutex);
                // Bids (highest to lowest)
                for (auto it = book->bids.rbegin(); it != book->bids.rend(); ++it) {
                    for (uint64_t order_id : it->second) {
                        auto const& order = book->orders.at(order_id);
                        fbb.Clear();
                        auto l3_update = CreateL3Update(fbb, symbol_id, ExecType_New, 0, order.order_id, order.side, order.price, order.qty, 0);
                        fbb.Finish(l3_update);
                        client->send(fbb.GetBufferPointer(), fbb.GetSize());
                    }
                }
                // Asks (lowest to highest)
                for (auto const& [price, queue] : book->asks) {
                    for (uint64_t order_id : queue) {
                        auto const& order = book->orders.at(order_id);
                        fbb.Clear();
                        auto l3_update = CreateL3Update(fbb, symbol_id, ExecType_New, 0, order.order_id, order.side, order.price, order.qty, 0);
                        fbb.Finish(l3_update);
                        client->send(fbb.GetBufferPointer(), fbb.GetSize());
                    }
                }
            }
            std::cout << "[L3Publisher] Sent snapshot (" << book->orders.size() << " orders) for symbol " << symbol_id << " to new subscriber." << std::endl;
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

            auto l3_update = flatbuffers::GetRoot<L3Update>(data_ptr);
            
            auto book = get_or_create_book(l3_update->symbol_id());
            book->update(l3_update->exec_type(), l3_update->order_id(), l3_update->side(), l3_update->p(), l3_update->q());

            ws_adaptor_->publish(l3_update, data_ptr, data_size);
            return 1;
        }
        return 0;
    }

private:
    std::shared_ptr<L3Book> get_or_create_book(uint32_t symbol_id) {
        std::lock_guard<std::mutex> lock(books_mutex_);
        auto it = books_.find(symbol_id);
        if (it == books_.end()) {
            auto book = std::make_shared<L3Book>();
            book->symbol_id = symbol_id;
            books_[symbol_id] = book;
            return book;
        }
        return it->second;
    }

    std::shared_ptr<WSAdaptor> ws_adaptor_;
    SHMRingBuffer* ring_buffer_;
    std::map<uint32_t, std::shared_ptr<L3Book>> books_;
    std::mutex books_mutex_;
};

} // namespace Exchange

int main() 
{
    setup_signals();
    std::cout << "\033[2J\033[H" << std::flush;

    std::cout << "[L3Publisher] Connecting to SHMRingBuffer: " << L3_UPDATE_RING << "..." << std::endl;

    Exchange::SHMRingBuffer* ring_buffer = nullptr;
    try {
        ring_buffer = new Exchange::SHMRingBuffer(L3_UPDATE_RING, L3_UPDATE_RING_SIZE);
    } catch (const std::exception& e) {
        std::cerr << "[L3Publisher] Failed to connect to SHMRingBuffer: " << e.what() << std::endl;
        return -1;
    }

    int main_core = L3_MAIN_CORE;
    if (main_core >= 0) {
        Exchange::set_thread_affinity(main_core, "L3Publisher_Main");
    }

    std::cout << "[L3Publisher] Connected successfully. Start consuming..." << std::endl;

    Exchange::L3Publisher publisher(9003, ring_buffer);
    publisher.run();

    std::cout << "[L3Publisher] Exiting and cleaning up..." << std::endl;
    delete ring_buffer;

    return 0;
}
