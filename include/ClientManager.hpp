#pragma once

#include "ring/SHMRingBuffer.hpp"
#include "fbs/exchange_generated.h"
#include "mmap_log.h"
#include "WSAdaptor.hpp"
#include "ClientDatabase.hpp"
#include "Worker.hpp"
#include <memory>
#include <map>
#include <set>
#include <mutex>
#include <vector>
#include <unordered_map>
#include <atomic>

namespace Exchange {

class CMClient : public std::enable_shared_from_this<CMClient> 
{
public:
    using MessageHandler = std::function<void(std::shared_ptr<CMClient>, const void*, size_t)>;

    explicit CMClient(WSClientPtr ws, uint32_t client_id = 0) 
        : conn_(ws), client_id_(client_id) {}

    void set_message_handler(MessageHandler handler) { msg_handler_ = handler; }

    void on_message(const void* data, size_t size) {
        if (msg_handler_) {
            msg_handler_(this->shared_from_this(), data, size);
        }
    }

    void send(const void* data, size_t size) {
        if (conn_) {
            conn_->send(data, size);
        }
    }

    uint32_t client_id() const { return client_id_; }
    void set_client_id(uint32_t id) { client_id_ = id; }

    uint64_t inbound_seq_num() const { return inbound_seq_num_.load(std::memory_order_relaxed); }
    void set_inbound_seq_num(uint64_t seq) { inbound_seq_num_.store(seq, std::memory_order_relaxed); }
    uint64_t increment_inbound_seq_num() { return inbound_seq_num_.fetch_add(1, std::memory_order_relaxed) + 1; }

    uint64_t outbound_seq_num() const { return outbound_seq_num_.load(std::memory_order_relaxed); }
    void set_outbound_seq_num(uint64_t seq) { outbound_seq_num_.store(seq, std::memory_order_relaxed); }
    uint64_t increment_outbound_seq_num() { return outbound_seq_num_.fetch_add(1, std::memory_order_relaxed) + 1; }

    WSClientPtr get_conn() const { return conn_; }

    bool is_ready() const { return ready_; }
    void set_ready(bool ready) { ready_ = ready; }

    static void bind_adaptor(std::shared_ptr<WSAdaptor> adaptor, 
                             std::function<void(std::shared_ptr<CMClient>)> on_open,
                             std::function<void(std::shared_ptr<CMClient>)> on_close,
                             std::function<void(std::shared_ptr<CMClient>, const void*, size_t)> on_message) 
    {
        adaptor->set_open_handler([on_open, on_close, on_message](WSClientPtr ws) {
            auto client = std::make_shared<CMClient>(ws);

            if (on_message) {
                client->set_message_handler(on_message);
                ws->set_message_handler([client](const void* data, size_t size) {
                    client->on_message(data, size);
                });
            }

            if (on_close) {
                ws->set_close_handler([client, on_close]() {
                    on_close(client);
                });
            }

            if (on_open) {
                on_open(client);
            }
        });
    }

private:
    WSClientPtr conn_;
    MessageHandler msg_handler_;
    
    uint32_t client_id_{0};
    std::atomic<uint64_t> inbound_seq_num_{0};
    std::atomic<uint64_t> outbound_seq_num_{0};
    bool ready_{false};
};

using CMClientPtr = std::shared_ptr<CMClient>;

class ClientManager : public Worker<ClientManager>
{
public:
    ClientManager(int port, SHMRingBuffer* request_ring, mmaplog::MmapReader* response_ring, std::shared_ptr<ClientDatabase> db);

    __attribute__((noinline)) void handle_execution_response(const OrderResponseT* resp);
    __attribute__((noinline)) void process_client_request(CMClientPtr client, const void* data, size_t size);
    __attribute__((noinline)) void handle_client_logon(CMClientPtr client, const AdminRequest* admin_req);
    __attribute__((noinline)) void handle_client_logout(CMClientPtr client);
    
    int poll_client();
    int poll_server();

private:

    std::shared_ptr<WSAdaptor> ws_adaptor_;
    SHMRingBuffer* request_ring_;
    mmaplog::MmapReader* response_ring_;
    std::shared_ptr<ClientDatabase> db_;

    std::mutex clients_mutex_;
    std::map<uint32_t, CMClientPtr> clients_;
};

} // namespace Exchange
