#include "ring/SHMRingBuffer.hpp"
#include "fbs/order_generated.h"
#include "WSAdaptor.hpp"
#include "L2Book.hpp"
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

    auto ws_adaptor = std::make_shared<Exchange::WSAdaptor>(9002);
    
    std::map<uint32_t, std::shared_ptr<Exchange::L2Book>> books;
    std::mutex books_mutex;

    auto get_or_create_book = [&books_mutex, &books](uint32_t symbol_id) {
        std::lock_guard<std::mutex> lock(books_mutex);
        auto it = books.find(symbol_id);
        if (it == books.end()) {
            auto book = std::make_shared<Exchange::L2Book>();
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
            auto l2_update = Exchange::CreateL2Update(fbb, symbol_id, 0, Exchange::Side_None, 0, 0, 0);
            fbb.Finish(l2_update);
            client->send(fbb.GetBufferPointer(), fbb.GetSize());
        }

        // 2. Send snapshots
        {
            std::lock_guard<std::mutex> lock(book->mutex);
            for (auto const& [price, qty] : book->bids) {
                fbb.Clear();
                auto l2_update = Exchange::CreateL2Update(fbb, symbol_id, 0, Exchange::Side_Buy, price, qty, 0);
                fbb.Finish(l2_update);
                client->send(fbb.GetBufferPointer(), fbb.GetSize());
            }
            for (auto const& [price, qty] : book->asks) {
                fbb.Clear();
                auto l2_update = Exchange::CreateL2Update(fbb, symbol_id, 0, Exchange::Side_Sell, price, qty, 0);
                fbb.Finish(l2_update);
                client->send(fbb.GetBufferPointer(), fbb.GetSize());
            }
        }
        std::cout << "[L2Publisher] Sent snapshot for symbol " << symbol_id << " to new subscriber." << std::endl;
    };

    ws_adaptor->set_subscribe_handler(subscribe_handler);

    std::cout << "[L2Publisher] Connected successfully. Start consuming..." << std::endl;

    void* data_ptr = nullptr;
    size_t data_size = 0;

    while (g_running.load(std::memory_order_relaxed))
    {
        if (ring_buffer->dequeue(&data_ptr, &data_size))
        {
            if (data_ptr == nullptr || data_size == 0) {
                continue;
            }

            auto l2_update = flatbuffers::GetRoot<Exchange::L2Update>(data_ptr);
            
            auto book = get_or_create_book(l2_update->symbol_id());
            book->update(l2_update->side(), l2_update->p(), l2_update->q());
            // book->display();

            ws_adaptor->publish(l2_update, data_ptr, data_size);
        } 
        else 
        {
            POLL_BACKOFF();
        }
    }

    std::cout << "[L2Publisher] Exiting and cleaning up..." << std::endl;
    delete ring_buffer;

    return 0;
}
