#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <new>
#include <set>
#include <string>
#include <vector>

using err_t = int8_t;
using esp_err_t = int8_t;
constexpr int8_t ERR_OK = 0, ERR_ABRT = -13, ERR_CONN = -11;
constexpr int LWIP_TCP_ERROR = 1, LWIP_TCP_ACCEPT = 2;
#define ARDUINO 1
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
#define async_tcp_log_e(...) ((void)0)
#define async_tcp_log_elapsed(label, call) call
class AsyncClient;
class AsyncServer;
static std::set<AsyncClient*> liveClients;
static bool failEventAllocation = false;
static unsigned abortCallbacks = 0, disconnects = 0;
struct tcp_pcb {
    void* arg = nullptr;
    void (*error)(void*, int8_t) = nullptr;
    bool alive = true;
};
class AsyncClient {
public:
    static uint32_t _fastbee_accepted_count;
    bool _fastbee_server_accepted = false;
    tcp_pcb* _pcb;
    size_t _rx_ack_len = 0;
    std::function<void(void*, AsyncClient*, int8_t)> _error_cb;
    std::function<void(void*, AsyncClient*)> _discard_cb;
    void* _error_cb_arg = nullptr;
    void* _discard_cb_arg = nullptr;
    explicit AsyncClient(tcp_pcb* pcb);
    ~AsyncClient();
    void _fastbee_release_accepted();
    void close();
    int8_t abort();
    int8_t _close();
    void _error(int8_t);
    tcp_pcb* pcb() const { return _pcb; }
    void setNoDelay(bool) {}
    static void operator delete(void* ptr) noexcept {
        liveClients.erase(static_cast<AsyncClient*>(ptr));
        ::operator delete(ptr);
    }
    static void operator delete(void* ptr, const std::nothrow_t&) noexcept {
        operator delete(ptr);
    }
    static void* operator new(size_t n, const std::nothrow_t&) noexcept {
        return ::operator new(n, std::nothrow);
    }
};
uint32_t AsyncClient::_fastbee_accepted_count = 0;
class AsyncServer {
public:
    std::function<void(void*, AsyncClient*)> _connect_cb = [](void*, AsyncClient*) {};
    bool _noDelay = false;
};
class AsyncTCP_detail {
public:
    static void tcp_error(void*, int8_t);
    static int8_t tcp_accept(void*, tcp_pcb*, int8_t);
};
struct lwip_tcp_event_packet_t {
    int event;
    AsyncClient* client;
    struct { int8_t err = 0; } error;
    struct { AsyncServer* server = nullptr; } accept;
    bool pooled = false;
    lwip_tcp_event_packet_t(int type, AsyncClient* c) : event(type), client(c) {}
    static void* operator new(size_t n, const std::nothrow_t&) noexcept {
        return failEventAllocation ? nullptr : ::operator new(n, std::nothrow);
    }
    static void operator delete(void* p) noexcept { ::operator delete(p); }
};
static std::vector<lwip_tcp_event_packet_t*> events;
struct queue_mutex_guard { queue_mutex_guard() {} ~queue_mutex_guard() {} };
static lwip_tcp_event_packet_t emergency(LWIP_TCP_ERROR, nullptr);
static bool emergencyUsed = false;
static lwip_tcp_event_packet_t* _fastbee_alloc_error_event(AsyncClient* c) {
    if (emergencyUsed) return nullptr;
    emergencyUsed = true; emergency.client = c; emergency.pooled = true;
    return &emergency;
}
static void freeEvent(lwip_tcp_event_packet_t* event) {
    if (event->pooled) { emergencyUsed = false; emergency.client = nullptr; }
    else delete event;
}
static size_t _remove_events_for_client(AsyncClient* client) {
    size_t removed = 0;
    for (auto it = events.begin(); it != events.end();) {
        if ((*it)->client == client) { freeEvent(*it); it = events.erase(it); ++removed; }
        else ++it;
    }
    return removed;
}
static void _send_async_event(lwip_tcp_event_packet_t* event) { events.push_back(event); }
static void _prepend_async_event(lwip_tcp_event_packet_t* event) { events.insert(events.begin(), event); }
static uint32_t heap_caps_get_free_size(int) { return 50000; }
static uint32_t heap_caps_get_largest_free_block(int) { return 30000; }
static uint32_t millis() { return 10000; }
static void tcp_arg(tcp_pcb* p, void* arg) { p->arg = arg; }
static void tcp_sent(tcp_pcb*, void*) {}
static void tcp_recv(tcp_pcb*, void*) {}
static void tcp_poll(tcp_pcb*, void*, int) {}
static void tcp_err(tcp_pcb* p, void (*callback)(void*, int8_t)) { p->error = callback; }
static void tcp_abort(tcp_pcb* p) {
    p->alive = false;
    if (p->error) { ++abortCallbacks; p->error(p->arg, ERR_ABRT); }
}
static int8_t tcp_close(tcp_pcb* p) { p->alive = false; return ERR_OK; }
struct tcpip_api_call_data {};
struct tcp_api_call_t {
    tcpip_api_call_data call;
    tcp_pcb** pcb;
    AsyncClient* close;
    err_t err;
};
static void tcpip_api_call(err_t (*fn)(tcpip_api_call_data*), tcpip_api_call_data* data) { fn(data); }
static void _tcp_recved(tcp_pcb**, size_t) {}
AsyncClient::AsyncClient(tcp_pcb* pcb) : _pcb(pcb) {
    liveClients.insert(this);
    pcb->arg = this;
    pcb->error = AsyncTCP_detail::tcp_error;
}

#include "tcp_cleanup_methods.inc"

static void require(bool condition, const char* error) {
    if (!condition) { std::cerr << error << '\n'; std::exit(86); }
}
static AsyncClient* accepted(tcp_pcb& pcb) {
    auto* client = new (std::nothrow) AsyncClient(&pcb);
    client->_fastbee_server_accepted = true;
    ++AsyncClient::_fastbee_accepted_count;
    return client;
}
static void drainEvents() {
    while (!events.empty()) {
        auto* event = events.front(); events.erase(events.begin());
        require(liveClients.count(event->client), "queued event references deleted client");
        if (event->event == LWIP_TCP_ERROR) event->client->_error(event->error.err);
        freeEvent(event);
    }
}
int main(int argc, char** argv) {
    require(argc == 2, "provide mode");
    if (std::string(argv[1]) == "abort") {
        for (bool hasCallback : {false, true}) {
            tcp_pcb pcb;
            auto* client = accepted(pcb);
            if (hasCallback) client->_discard_cb = [](void*, AsyncClient*) { ++disconnects; };
            client->abort();
            require(!pcb.alive && client->_pcb == nullptr, "abort must close PCB");
            require(AsyncClient::_fastbee_accepted_count == 0, "abort retained admission slot");
            require(abortCallbacks == 0, "explicit abort must detach raw callback");
            client->abort(); client->close(); client->_fastbee_release_accepted();
            require(AsyncClient::_fastbee_accepted_count == 0, "repeated teardown underflow");
            delete client;
            require(AsyncClient::_fastbee_accepted_count == 0, "destructor double release");
        }
        require(disconnects == 1, "disconnect callback must occur exactly once");
        tcp_pcb pcb;
        auto* client = accepted(pcb);
        failEventAllocation = true;
        AsyncTCP_detail::tcp_error(client, ERR_ABRT);
        require(AsyncClient::_fastbee_accepted_count == 0, "error path release");
        client->abort(); client->close(); drainEvents(); delete client;
        require(AsyncClient::_fastbee_accepted_count == 0 && events.empty(), "null PCB cleanup");
    } else {
        for (unsigned repeat = 0; repeat < 20; ++repeat) {
            tcp_pcb pcb;
            AsyncServer server;
            failEventAllocation = true;
            const int8_t result = AsyncTCP_detail::tcp_accept(&server, &pcb, ERR_OK);
            require(result == ERR_ABRT && !pcb.alive, "failed accept must abort PCB");
            require(AsyncClient::_fastbee_accepted_count == 0, "accept rollback leaked slot");
            require(liveClients.empty(), "accept rollback leaked client");
            drainEvents();
            require(!emergencyUsed && events.empty(), "accept rollback stranded pooled event");
        }
        require(abortCallbacks == 0, "unpublished client must detach callbacks before abort");
        tcp_pcb pcb; AsyncServer server;
        failEventAllocation = false;
        require(AsyncTCP_detail::tcp_accept(&server, &pcb, ERR_OK) == ERR_OK, "normal accept failed");
        require(AsyncClient::_fastbee_accepted_count == 1 && events.size() == 1, "normal admission missing");
        auto* client = events.front()->client;
        drainEvents(); client->close(); delete client;
        require(AsyncClient::_fastbee_accepted_count == 0 && liveClients.empty(), "normal close leak");
    }
}
