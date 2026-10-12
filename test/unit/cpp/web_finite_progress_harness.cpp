#define FASTBEE_PEER_DRAIN_NO_MAIN
#define FASTBEE_REAL_REQUEST_OWNERSHIP
#include "web_peer_drain_harness.cpp"
#define async_tcp_log_d(...) ((void)0)
#define async_tcp_log_elapsed(label, expression) (expression)
#define ESTABLISHED 4
size_t tcp_sndbuf(tcp_pcb* pcb) {return pcb->client->window;}

// Actual AsyncTCP::send/_poll bodies are compiled below with only the class
// qualifier renamed. Callback scheduling is controlled, not claimed real-time.
struct TransportClock {
    tcp_pcb* _pcb;
    Client* peer;
    static uint32_t _fastbee_accepted_count;
    bool _fastbee_server_accepted=true;
    uint32_t _tx_last_packet=0,_rx_last_ack=0,_ack_timeout=10000;
    uint32_t _rx_timeout=0,_rx_last_packet=0;
    void* _timeout_cb_arg=nullptr;
    void* _poll_cb_arg=nullptr;
    void* _discard_cb_arg=nullptr;
    std::function<void(void*,TransportClock*,uint32_t)> _timeout_cb;
    std::function<void(void*,TransportClock*)> _poll_cb;
    std::function<void(void*,TransportClock*)> _discard_cb;
    explicit TransportClock(tcp_pcb* pcb):_pcb(pcb),peer(pcb->client) {++_fastbee_accepted_count;}
    ~TransportClock() {_fastbee_release_accepted();}
    size_t add(const char*,size_t,uint8_t apiflags=1);
    bool send();
    size_t space() const;
    int8_t _poll(tcp_pcb*);
    int8_t _close();
    void _fastbee_release_accepted();
};
uint32_t TransportClock::_fastbee_accepted_count=0;
int _tcp_output(tcp_pcb** pcb) {return (*pcb)->client->failOutput?ERR_MEM:ERR_OK;}
int8_t _tcp_close(tcp_pcb** pcb,TransportClock* client) {
    tcp_api_call_t message;message.pcb=pcb;message.close=client->peer;
    return _tcp_close_api(&message);
}
int8_t _tcp_write(tcp_pcb** pcb,const char* data,size_t length,uint8_t) {
    auto* peer=(*pcb)->client;
    if(peer->perAddLimit==0)return ERR_MEM;
    peer->accepted.append(data,length);peer->window-=length;peer->batchBytes+=length;
    return ERR_OK;
}
#include "production_transport_clock.inc"
#include "production_owned_request.inc"
class AsyncFileResponse:public AsyncAbstractResponse {
public:
    CountedFile _content;
#include "production_file_destructor.inc"
};

constexpr uint8_t STATIC_MAX_INFLIGHT=3;
#include "production_lease_types.inc"
StaticInFlightSlot s_staticInFlightSlots[STATIC_MAX_INFLIGHT]{};
#define portENTER_CRITICAL(...) ((void)0)
#define portEXIT_CRITICAL(...) ((void)0)
#include "production_lease_release.inc"

struct Fixture {
    size_t closes=0,disconnects=0,fileCloses=0,clientDestructions=0;
    std::shared_ptr<AsyncWebServerRequest> owner=std::make_shared<AsyncWebServerRequest>();
    AsyncFileResponse* response=new AsyncFileResponse;
    tcp_pcb pcb{&owner->tcp};
    TransportClock clock{&pcb};
    Fixture() {
        allocationLimit=static_cast<size_t>(-1);freeInternal=28000;largestInternal=24000;
        owner->_this=owner;owner->_response=response;
        response->_state=RESPONSE_CONTENT;response->body="finite payload";
        response->_contentLength=response->body.size();response->_assembled_headers.clear();
        response->_content.opened=true;response->_content.closeCount=&fileCloses;
        owner->tcp.destructionCount=&clientDestructions;
        auto* request=owner.get();
        owner->tcp.closeAdapter=[this]{++closes;clock._close();};
        owner->_onDisconnectfn=[this]{++disconnects;};
        owner->tcp.outputAdapter=[this]{return clock.send();};
        owner->tcp.spaceAdapter=[this]{return clock.space();};
        clock._discard_cb=[request](void*,TransportClock*){request->_onDisconnect();};
        clock._timeout_cb=[request](void*,TransportClock*,uint32_t t){request->_onTimeout(t);};
        clock._poll_cb=[request](void*,TransportClock*){request->_onPoll();};
    }
    ~Fixture() {
        if(owner&&owner->_this)owner->_this.reset();
        owner.reset();
        allocationLimit=static_cast<size_t>(-1);
    }
    void poll(uint32_t at) {fakeMillis=at;clock._poll(&pcb);}
    void bodyOnlyStart(uint32_t at) {fakeMillis=at;response->_ack(owner.get(),0,0);}
};

void stalled(bool allocationFailure,uint32_t start) {
    Fixture f;
    if(allocationFailure)allocationLimit=0;else f.response->persistentTryAgain=true;
    f.bodyOnlyStart(start);
    for(uint32_t offset:{1U,1000U,5000U,9999U}) {
        f.poll(start+offset);
        assert(!f.owner->tcp.closed&&f.owner->tcp.accepted.empty());
        assert(f.clock._tx_last_packet==start+offset); // Real send refreshed old ACK clock.
    }
    f.poll(start+10000U);
    assert(f.response->_state==RESPONSE_FAILED&&f.owner->tcp.closed);
    assert(f.closes==1&&f.disconnects==1&&f.owner->tcp.closes==1);
    assert(f.owner->tcp.accepted.empty());
    const auto sends=f.owner->tcp.sends,position=f.response->sourcePosition;
    assert(f.response->_ack(f.owner.get(),123,0)==0);
    assert(f.owner->tcp.sends==sends&&f.response->sourcePosition==position&&f.owner->tcp.accepted.empty());
    f.poll(start+11000U);assert(f.closes==1&&f.disconnects==1);
}

void zeroSpacePollAndTimeout() {
    Fixture f;fakeMillis=0;f.owner->tcp.window=0;
    assert(f.owner->tcp.space()==0);
    f.owner->tcp.window=TCP_SND_BUF;f.pcb.state=0;assert(f.owner->tcp.space()==0);
    f.pcb.state=ESTABLISHED;f.clock._pcb=nullptr;assert(f.owner->tcp.space()==0);
    f.clock._pcb=&f.pcb;f.owner->tcp.window=0;
    f.response->_fastbeeCheckProgress(f.owner.get());
    // The actual ACK-timeout predicate is false: tx == ack == 0.
    f.poll(9999);assert(!f.owner->tcp.closed);
    f.poll(10000);assert(f.owner->tcp.closed&&f.closes==1);
    Fixture timeout;timeout.response->persistentTryAgain=true;timeout.bodyOnlyStart(0);
    fakeMillis=10000;timeout.owner->_onTimeout(10000);
    assert(timeout.owner->tcp.closed&&timeout.closes==1);
    assert(timeout.response->_state==RESPONSE_FAILED);
}

void setupAndRespond() {
    Fixture f;f.response->_state=RESPONSE_SETUP;fakeMillis=50000;
    assert(f.response->_fastbeeCheckProgress(f.owner.get()));
    assert(!f.response->_fastbeeProgressInitialized);
    f.response->_assembled_headers="HTTP/1.1 200 OK\r\n\r\n";
    f.owner->tcp.perAddLimit=0;f.response->_respond(f.owner.get());
    assert(f.response->_fastbeeProgressInitialized&&f.response->_fastbeeLastProgressMs==50000);
    f.poll(59999);assert(!f.owner->tcp.closed);
    f.poll(60000);assert(f.owner->tcp.closed);
}

void repeatedAckAndPendingOutput() {
    Fixture f;fakeMillis=0;f.owner->tcp.perAddLimit=1;
    f.bodyOnlyStart(0);assert(f.response->_writtenLength==1);
    f.owner->tcp.perAddLimit=0;
    for(uint32_t at:{1000U,5000U,9999U}) {
        fakeMillis=at;f.owner->_onAck(static_cast<size_t>(-1),0);
        assert(!f.owner->tcp.closed&&f.response->_fastbeeLastProgressMs==0);
    }
    fakeMillis=10000;f.owner->_onAck(static_cast<size_t>(-1),0);
    assert(f.owner->tcp.closed&&f.closes==1);
    Fixture pending;pending.owner->tcp.perAddLimit=1;pending.owner->tcp.failOutput=true;
    pending.bodyOnlyStart(0);assert(pending.response->_fastbeePendingOutputBytes==1);
    pending.owner->tcp.failOutput=false;pending.owner->tcp.perAddLimit=0;
    pending.poll(9999);assert(!pending.owner->tcp.closed);
    assert(!pending.response->_fastbeePendingOutputBytes&&pending.response->_fastbeeLastProgressMs==0);
    pending.poll(10000);assert(pending.owner->tcp.closed&&pending.closes==1);
}

void recoveryAndSlowProgress() {
    Fixture f;f.response->persistentTryAgain=true;f.bodyOnlyStart(0);
    f.poll(9000);f.response->persistentTryAgain=false;f.owner->tcp.perAddLimit=1;
    uint32_t at=9999;std::string expected=f.response->body;
    for(size_t step=0;step<expected.size()+2&&!f.owner->tcp.closed;++step) {
        fakeMillis=at;f.owner->tcp.window=TCP_SND_BUF;f.owner->_onPoll();at+=9000;
    }
    assert(at>20000&&f.owner->tcp.accepted==expected);
    assert(f.response->_state==RESPONSE_END&&f.closes==1);
    Fixture allocation;allocationLimit=0;allocation.bodyOnlyStart(0);
    allocation.poll(9999);allocationLimit=static_cast<size_t>(-1);
    fakeMillis=9999;allocation.owner->_onPoll();assert(!allocation.owner->tcp.closed);
    allocation.owner->tcp.window=TCP_SND_BUF;fakeMillis=19999;allocation.owner->_onPoll();
    assert(allocation.response->_state==RESPONSE_END&&allocation.closes==1);
}

void peerHighWaterAndFinalFlush() {
    Fixture f;f.bodyOnlyStart(0);assert(f.response->_state==RESPONSE_WAIT_ACK);
    const auto queued=f.response->_writtenLength;
    assert(queued>1);f.owner->tcp.window+=1;fakeMillis=9999;f.owner->_onPoll();
    assert(f.response->_fastbeeLastProgressMs==9999&&!f.owner->tcp.closed);
    fakeMillis=19998;f.owner->_onAck(static_cast<size_t>(-1),0);assert(!f.owner->tcp.closed);
    // A fully drained response succeeds at its deadline, with no lost final byte.
    fakeMillis=19999;f.owner->tcp.window=TCP_SND_BUF;f.owner->_onPoll();
    assert(f.response->_state==RESPONSE_END&&f.closes==1);
    Fixture pending;pending.owner->tcp.failOutput=true;pending.bodyOnlyStart(0);
    assert(pending.response->_fastbeePendingOutputBytes>0);
    pending.owner->tcp.window=TCP_SND_BUF;pending.owner->tcp.failOutput=false;
    fakeMillis=9000;pending.owner->_onPoll();
    assert(!pending.response->_fastbeePendingOutputBytes&&!pending.owner->tcp.closed);
    assert(pending.owner->tcp.sends==2);
    fakeMillis=19000;pending.owner->_onPoll();
    assert(pending.response->_state==RESPONSE_END&&pending.closes==1);
}

void excludedTypes() {
    for(int variant=0;variant<3;++variant) {
        Fixture f;f.response->_chunked=variant!=0;f.response->_sendContentLength=variant==2;
        f.response->persistentTryAgain=true;f.bodyOnlyStart(0);
        for(uint32_t at:{9000U,18000U,27000U})f.poll(at);
        assert(!f.owner->tcp.closed&&!f.response->_fastbeeProgressInitialized);
    }
    AsyncBasicResponse basic;AsyncWebServerRequest request;fakeMillis=100000;
    assert(basic._fastbeeCheckProgress(&request));
    fakeMillis+=20000;assert(!basic._fastbeeCheckProgress(&request));
    assert(basic._state==RESPONSE_FAILED);
}

void releaseOwnershipAndLease() {
    Fixture f;f.owner->tcp.perAddLimit=0;f.bodyOnlyStart(0);
    assert(f.response->_send_buffer&&liveArrays>0);
    StaticInFlightLease lease{41,0};s_staticInFlightSlots[0]={41,0,true};
    registerStaticInFlightRelease(f.owner.get(),lease);
    auto release=f.owner->_onDisconnectfn;
    f.owner->_onDisconnectfn=[&]{++f.disconnects;release();};
    auto* raw=f.owner.get();std::weak_ptr<AsyncWebServerRequest> weak=f.owner;
    const auto acceptedBefore=TransportClock::_fastbee_accepted_count;
    f.owner.reset();fakeMillis=10000;raw->_onPoll();
    assert(weak.expired()&&f.closes==1&&f.disconnects==1);
    assert(f.fileCloses==1&&f.clientDestructions==1&&liveArrays==0);
    assert(TransportClock::_fastbee_accepted_count+1==acceptedBefore&&!f.clock._fastbee_server_accepted);
    assert(!s_staticInFlightSlots[0].active);
    s_staticInFlightSlots[0]={42,10000,true};release();
    assert(s_staticInFlightSlots[0].active&&s_staticInFlightSlots[0].generation==42);
}

#ifdef FASTBEE_FINITE_REFERENCE_MAIN
int finite_reference_main() {
#else
int main() {
#endif
    stalled(true,0);stalled(false,0);stalled(false,UINT32_MAX-5000U);
    zeroSpacePollAndTimeout();setupAndRespond();repeatedAckAndPendingOutput();
    recoveryAndSlowProgress();peerHighWaterAndFinalFlush();excludedTypes();
    releaseOwnershipAndLease();
    assert(liveArrays==0);
    assert(TransportClock::_fastbee_accepted_count==0);
    std::cout<<"finite progress: callback reachability, real empty-send clock, deadlines, recovery, flush and ownership passed\n";
    return 0;
}
