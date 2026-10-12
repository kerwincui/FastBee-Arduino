#define main batch_scenarios_main
#include "web_response_batch_harness.cpp"
#undef main
#define async_ws_log_v(...) ((void)0)
using err_t=int;
constexpr err_t ERR_OK=0,ERR_CONN=-11,ERR_MEM=-1;
using AsyncClient=Client;
struct tcpip_api_call_data {};
struct tcp_pcb {Client* client;bool aborted=false;size_t discarded=0;int state=4;};
struct tcp_api_call_t:tcpip_api_call_data {
    tcp_pcb** pcb;Client* close;err_t err=ERR_CONN;
};
err_t closeResult=ERR_OK;
void _reset_tcp_callbacks(tcp_pcb*,Client*) {}
bool _remove_events_for_client(Client*) {return false;}
err_t tcp_close(tcp_pcb*) {return closeResult;}
void tcp_abort(tcp_pcb* pcb) {
    pcb->aborted=true;pcb->discarded=TCP_SND_BUF-pcb->client->window;
}
#include "production_peer_drain.inc"
template<typename Response> void finalPacket(bool chunked,bool lostAck,bool closeOom) {
    Response response;
    if constexpr(std::is_same_v<Response,AsyncBasicResponse>) response._content=std::string(10000,'B');
    else {
        response.body=std::string(10000,'A');response._contentLength=response.body.size();
        response._chunked=chunked;
        if(chunked) {response._contentLength=0;response._sendContentLength=false;}
    }
    auto request=std::make_shared<AsyncWebServerRequest>();request->_this=request;
    request->_response=&response;
    std::weak_ptr<AsyncWebServerRequest> weak=request;
    tcp_pcb pcb{&request->tcp};tcp_pcb* active=&pcb;
    unsigned closes=0,disconnects=0;
    closeResult=closeOom?ERR_MEM:ERR_OK;
    request->_onDisconnectfn=[&]{++disconnects;};
    request->tcp.onClose=[&]{
        ++closes;
        assert(request->tcp.window==TCP_SND_BUF); // No unacknowledged data discarded.
        tcp_api_call_t msg;msg.pcb=&active;msg.close=&request->tcp;
        _tcp_close_api(&msg);
        request->_onDisconnect();
    };
    response._ack(request.get(),0,0);
    for(int step=0;response._state!=RESPONSE_WAIT_ACK&&step<1000;++step) {
        const size_t acked=TCP_SND_BUF-request->tcp.window;
        request->tcp.window=TCP_SND_BUF;
        request->_onAck(acked,1);
        assert(!request->tcp.closed);
    }
    assert(response._state==RESPONSE_WAIT_ACK);
    assert(response._writtenLength==request->tcp.accepted.size());
    assert(request->tcp.window<TCP_SND_BUF);
    // Duplicate/oversized callback cannot turn queued bytes into peer delivery.
    request->_onAck(static_cast<size_t>(-1),1);
    request->_onAck(static_cast<size_t>(-1),1);
    assert(response._ackedLength<=response._writtenLength);
    assert(!request->tcp.closed&&response._state==RESPONSE_WAIT_ACK);
    for(int i=0;i<10;++i) request->_onPoll();
    assert(!request->tcp.closed);
    const size_t finalBytes=TCP_SND_BUF-request->tcp.window;
    request->tcp.window+=finalBytes/2;
    request->_onAck(finalBytes/2,1);
    assert(!request->tcp.closed&&response._state==RESPONSE_WAIT_ACK);
    request->tcp.window=TCP_SND_BUF;
    if(lostAck) request->_onPoll();else request->_onAck(finalBytes-finalBytes/2,1);
    assert(response._finished()&&request->tcp.closed);
    assert(closes==1&&disconnects==1&&active==nullptr&&pcb.discarded==0);
    assert(pcb.aborted==closeOom);
    assert(!request->_this);request.reset();assert(weak.expired());
}
void timeoutAndCancel() {
    for(bool cancel:{false,true}) {
        AsyncAbstractResponse response;response.body="payload";response._contentLength=7;
        auto owner=std::make_shared<AsyncWebServerRequest>();owner->_this=owner;
        owner->_response=&response;
        auto* raw=owner.get();std::weak_ptr<AsyncWebServerRequest> weak=owner;
        unsigned closes=0,disconnects=0;
        owner->_onDisconnectfn=[&]{++disconnects;};
        owner->tcp.onClose=[&]{++closes;raw->_onDisconnect();};
        response._ack(raw,0,0);assert(response._state==RESPONSE_WAIT_ACK);
        owner.reset(); // Request now owns itself, as in production.
        if(cancel) raw->_onDisconnect(); // Peer cancel tears down even mid-response.
        else raw->_onTimeout(10000); // Existing timeout remains bounded.
        assert(weak.expired()&&disconnects==1&&closes==unsigned(!cancel));
    }
}
#ifndef FASTBEE_PEER_DRAIN_NO_MAIN
int main() {
    for(bool lost:{false,true}) for(bool oom:{false,true}) {
        finalPacket<AsyncBasicResponse>(false,lost,oom);
        finalPacket<AsyncAbstractResponse>(false,lost,oom);
        finalPacket<AsyncAbstractResponse>(true,lost,oom);
    }
    timeoutAndCancel();
    std::cout<<"peer drain: final packet, lost/duplicate ACK, ERR_MEM, timeout and cancellation passed\n";
}
#endif
