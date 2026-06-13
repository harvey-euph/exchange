#include "L3Publisher.hpp"
#include <iostream>
#include "AsyncLogger.hpp"

namespace Exchange {

L3Publisher::L3Publisher(int port, SHMRingBuffer* ring_buffer)
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
        LOG("[L3Publisher] Sent snapshot (" << book->orders.size() << " orders) for symbol " << symbol_id << " to new subscriber.");
    };

    ws_adaptor_->set_subscribe_handler(subscribe_handler);
}

int L3Publisher::poll_client() {
    return ws_adaptor_->poll() > 0 ? 1 : 0;
}

int L3Publisher::poll_server() {
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

std::shared_ptr<L3Book> L3Publisher::get_or_create_book(uint32_t symbol_id) {
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

} // namespace Exchange
