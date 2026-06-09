#include "ring/SHMRingBuffer.hpp"
#include "fbs/order_generated.h"
#include "WSAdaptor.hpp"
#include "L3OutputAdaptor.hpp"
#include "L3Book.hpp"
#include <iostream>
#include <vector>
#include <memory>
#include <atomic>
#include <map>
#include <mutex>
#include "define.hpp"
#include "SignalHandler.hpp"


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

    auto ws_adaptor = std::make_shared<Exchange::WSAdaptor>(9003);

    std::map<uint32_t, std::shared_ptr<Exchange::L3Book>> books;
    std::mutex books_mutex;

    auto get_or_create_book = [&books_mutex, &books](uint32_t symbol_id) {
        std::lock_guard<std::mutex> lock(books_mutex);
        auto it = books.find(symbol_id);
        if (it == books.end()) {
            auto book = std::make_shared<Exchange::L3Book>();
            book->symbol_id = symbol_id;
            books[symbol_id] = book;
            return book;
        }
        return it->second;
    };

    auto subscribe_handler = [&get_or_create_book](Exchange::WSClientPtr client, uint32_t symbol_id, bool is_subscribe) {
        if (!is_subscribe) return;

        auto book = get_or_create_book(symbol_id);
        
        flatbuffers::FlatBufferBuilder fbb(1024);

        // 1. Send empty frame (Side=None) to clear old data
        {
            auto l3_update = Exchange::CreateL3Update(fbb, symbol_id, Exchange::ExecType_New, 0, 0, Exchange::Side_None, 0, 0, 0);
            fbb.Finish(l3_update);
            client->send(fbb.GetBufferPointer(), fbb.GetSize());
        }

        // 2. Send snapshots (all active orders in priority order)
        {
            std::lock_guard<std::mutex> lock(book->mutex);
            // Bids first (highest to lowest)
            for (auto const& [price, queue] : book->bids) {
                for (uint64_t order_id : queue) {
                    auto const& order = book->orders.at(order_id);
                    fbb.Clear();
                    auto l3_update = Exchange::CreateL3Update(fbb, symbol_id, Exchange::ExecType_New, 0, order.order_id, order.side, order.price, order.qty, 0);
                    fbb.Finish(l3_update);
                    client->send(fbb.GetBufferPointer(), fbb.GetSize());
                }
            }
            // Asks (lowest to highest)
            for (auto const& [price, queue] : book->asks) {
                for (uint64_t order_id : queue) {
                    auto const& order = book->orders.at(order_id);
                    fbb.Clear();
                    auto l3_update = Exchange::CreateL3Update(fbb, symbol_id, Exchange::ExecType_New, 0, order.order_id, order.side, order.price, order.qty, 0);
                    fbb.Finish(l3_update);
                    client->send(fbb.GetBufferPointer(), fbb.GetSize());
                }
            }
        }
        std::cout << "[L3Publisher] Sent snapshot (" << book->orders.size() << " orders) for symbol " << symbol_id << " to new subscriber." << std::endl;
    };

    ws_adaptor->set_subscribe_handler(subscribe_handler);

    std::cout << "[L3Publisher] Connected successfully. Start consuming..." << std::endl;

    void* data_ptr = nullptr;
    size_t data_size = 0;

    while (g_running.load(std::memory_order_relaxed))
    {
        if (ring_buffer->dequeue(&data_ptr, &data_size))
        {
            if (data_ptr == nullptr || data_size == 0) {
                continue;
            }

            auto l3_update = flatbuffers::GetRoot<Exchange::L3Update>(data_ptr);
            
            auto book = get_or_create_book(l3_update->symbol_id());
            book->update(l3_update->exec_type(), l3_update->order_id(), l3_update->side(), l3_update->p(), l3_update->q());
            // book->display();

            ws_adaptor->publish(l3_update, data_ptr, data_size);
        } 
        else 
        {
            POLL_BACKOFF();
        }
    }

    std::cout << "[L3Publisher] Exiting and cleaning up..." << std::endl;
    delete ring_buffer;

    return 0;
}
