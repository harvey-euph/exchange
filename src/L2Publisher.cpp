#include "L2Publisher.hpp"
#include <iostream>
#include "AsyncLogger.hpp"

namespace Exchange {

L2Publisher::L2Publisher(int port, SHMRingBuffer* ring_buffer)
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
        LOG("[L2Publisher] Sent snapshot for symbol " << symbol_id << " to new subscriber.");
    };

    ws_adaptor_->set_subscribe_handler(subscribe_handler);
}

int L2Publisher::poll_client() {
    return ws_adaptor_->poll() > 0 ? 1 : 0;
}

int L2Publisher::poll_server() {
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

std::shared_ptr<L2Book> L2Publisher::get_or_create_book(uint32_t symbol_id) {
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

} // namespace Exchange
