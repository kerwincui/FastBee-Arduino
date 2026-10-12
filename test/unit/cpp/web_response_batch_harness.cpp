#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <new>
#include <string>
#include <type_traits>
#include <vector>
using String=std::string;
#define ESP32 1
#define ASYNCWEBSERVER_USE_CHUNK_INFLIGHT 1
#ifndef CONFIG_LWIP_TCP_MSS
#define CONFIG_LWIP_TCP_MSS 1460
#endif
#define CONFIG_LWIP_TCP_WND_DEFAULT 5760
#ifndef TCP_SND_BUF
#define TCP_SND_BUF 65536
#endif
#define ASYNC_RESPONCE_BUFF_SIZE 2872
constexpr int MALLOC_CAP_INTERNAL=1,MALLOC_CAP_8BIT=2;
constexpr size_t RESPONSE_TRY_AGAIN=static_cast<size_t>(-1);
size_t freeInternal=28000,largestInternal=24000;
size_t heap_caps_get_free_size(int) {return freeInternal;}
size_t heap_caps_get_largest_free_block(int) {return largestInternal;}
size_t allocationLimit=static_cast<size_t>(-1),allocationFailures=0,liveArrays=0;
void* operator new[](size_t size,const std::nothrow_t&) noexcept {
    if(size>allocationLimit) {++allocationFailures;return nullptr;}
    void* result=std::malloc(size);if(result)++liveArrays;return result;
}
void operator delete[](void* pointer) noexcept {if(pointer)--liveArrays;std::free(pointer);}
void operator delete[](void* pointer,size_t) noexcept {if(pointer)--liveArrays;std::free(pointer);}
[[maybe_unused]] struct {void printf(const char*,...) {}} Serial;
uint32_t fakeMillis=0;
uint32_t millis() {return fakeMillis;}
constexpr const char* T_Connection="Connection";
constexpr const char* T_close="close";
enum {RESPONSE_SETUP,RESPONSE_HEADERS,RESPONSE_CONTENT,RESPONSE_WAIT_ACK,RESPONSE_END,RESPONSE_FAILED};
struct Client {
    std::string accepted;
    size_t window=TCP_SND_BUF, perAddLimit=TCP_SND_BUF, batchBytes=0, maxBatch=0;
    bool closed=false,failOutput=false;
    std::function<void()> onClose;
    std::function<bool()> outputAdapter;
    std::function<size_t()> spaceAdapter;
    std::function<void()> closeAdapter;
    size_t* destructionCount=nullptr;
    ~Client() {if(destructionCount)++*destructionCount;}
    void onDisconnect(std::nullptr_t) {onClose=nullptr;}
    size_t sends=0,closes=0;
    uint32_t lastSuccessfulSend=0;
    size_t space() {return spaceAdapter?spaceAdapter():window;}
    std::function<size_t(const char*,size_t)> addAdapter;
    size_t add(const char* data,size_t length) {
        if(addAdapter)return addAdapter(data,length);
        const size_t written=std::min({length,window,perAddLimit});
        accepted.append(data,written);window-=written;batchBytes+=written;
        return written;
    }
    bool send() {++sends;maxBatch=std::max(maxBatch,batchBytes);batchBytes=0;const bool ok=outputAdapter?outputAdapter():!failOutput;if(ok)lastSuccessfulSend=millis();return ok;}
    size_t write(const char* data,size_t length) {const size_t count=add(data,length);return send()?count:0;}
    bool canSend() {return !closed && window>0;}
    void close() {++closes;closed=true;if(closeAdapter){closeAdapter();return;}if(onClose)onClose();}
};
class AsyncWebServerResponse;
using ArDisconnectHandler=std::function<void()>;
struct CountedFile {
    bool opened=false;
    size_t* closeCount=nullptr;
    explicit operator bool() const {return opened;}
    void close() {if(opened&&closeCount)++*closeCount;opened=false;}
};
struct AsyncWebServerRequest {
#ifdef FASTBEE_REAL_REQUEST_OWNERSHIP
    Client* _client=new Client;
    Client& tcp=*_client;
    void* _tempObject=nullptr;
    void* _itemBuffer=nullptr;
    CountedFile _tempFile;
    ~AsyncWebServerRequest();
#else
    Client tcp;
    Client* _client=&tcp;
#endif
    Client* client() {return _client;}
    AsyncWebServerResponse* _response=nullptr;
    std::shared_ptr<AsyncWebServerRequest> _this;
    std::function<void()> _onDisconnectfn;
    bool _ackTimeoutRecoveryAttempted=false;
    std::string _url;
    void _onAck(size_t,uint32_t);
    void _onPoll();
    void _onTimeout(uint32_t);
    void _onDisconnect();
    void onDisconnect(ArDisconnectHandler);
    int version() {return 1;}
};
class AsyncWebServerResponse {
public:
    virtual ~AsyncWebServerResponse()=default;
    int _state=RESPONSE_HEADERS;
    bool _chunked=false,_sendContentLength=true;
    size_t _in_flight_credit=2,_in_flight=0,_ackedLength=0,_writtenLength=0,_writtenHeadersLength=0;
    size_t _sentLength=0,_contentLength=0,_send_buffer_len=0,_send_buffer_offset=0,_send_buffer_cap=0;
    size_t _fastbeePendingOutputBytes=0;
    std::unique_ptr<uint8_t[]> _send_buffer;
    String _assembled_headers="HTTP/1.1 200 OK\r\n\r\n";
    std::string body;
    size_t sourcePosition=0,maxAllocation=0;
    bool transientRead=false,persistentTryAgain=false;
    void addHeader(const char*,const char*,bool) {}
    void _assembleHead(String&,int) {}
    bool _sourceValid() {return true;}
    size_t _fillBufferAndProcessTemplates(uint8_t* dest,size_t length) {
        maxAllocation=std::max(maxAllocation,_send_buffer_cap);
        if(persistentTryAgain)return RESPONSE_TRY_AGAIN;
        if(transientRead){transientRead=false;return RESPONSE_TRY_AGAIN;}
        const size_t copied=std::min(length,body.size()-sourcePosition);
        std::memcpy(dest,body.data()+sourcePosition,copied);sourcePosition+=copied;return copied;
    }
    bool _finished() const {return _state>RESPONSE_WAIT_ACK;}
    virtual size_t _ack(AsyncWebServerRequest*,size_t,uint32_t)=0;
#include "production_progress_base.inc"
};
class AsyncAbstractResponse : public AsyncWebServerResponse {
public:
#include "production_progress_fields.inc"
    void _respond(AsyncWebServerRequest*);
    size_t write_send_buffs(AsyncWebServerRequest*,size_t,uint32_t);
#include "production_abstract_ack.inc"
};
class AsyncBasicResponse : public AsyncWebServerResponse {
public:
    String _content;
#ifdef FASTBEE_TEST_BASIC_RESPOND
    void _respond(AsyncWebServerRequest*);
#endif
#include "production_basic_progress_fields.inc"
    size_t write_send_buffs(AsyncWebServerRequest*,size_t,uint32_t);
#include "production_basic_ack.inc"
};
#include "production_response_batch.inc"
void verifyBasic(bool lowMemory,bool partial) {
    freeInternal=lowMemory?28000:80000;largestInternal=24000;
    AsyncBasicResponse response;
    response._content=std::string(9213,'C');
    const auto expected=response._assembled_headers+response._content;
    AsyncWebServerRequest request;
    for(int step=0;response._state!=RESPONSE_END&&step<1000;++step) {
        request.tcp.window=65536;
        request.tcp.perAddLimit=partial?(step%4==0?0:217):65536;
        response.write_send_buffs(&request,step?217:0,0);
    }
    assert(response._state==RESPONSE_END&&!request.tcp.closed);
    assert(request.tcp.accepted==expected);
    assert(response._writtenLength==request.tcp.accepted.size());
    if(lowMemory) assert(request.tcp.maxBatch<=CONFIG_LWIP_TCP_MSS);
    else if(!partial) assert(request.tcp.maxBatch>CONFIG_LWIP_TCP_MSS);
}
void verify(bool lowMemory,bool partial,bool transient,bool unknownLength=false,bool chunked=false) {
    freeInternal=lowMemory?28000:80000;
    AsyncAbstractResponse response;
    response.body=std::string(6235,'A')+std::string(24092,'B');
    response._contentLength=response.body.size();
    if(unknownLength) {response._contentLength=0;response._sendContentLength=false;}
    if(chunked) {response._chunked=true;response._contentLength=0;response._sendContentLength=false;}
    response.transientRead=transient;
    const std::string expected=response._assembled_headers+response.body;
    AsyncWebServerRequest request;
    for(int step=0;response._state!=RESPONSE_END && step<500;++step) {
        request.tcp.window=65536;
        request.tcp.perAddLimit=partial?(step%4==0?0:217):65536;
        response.write_send_buffs(&request,step?217:0,0);
    }
    assert(response._state==RESPONSE_END && !request.tcp.closed);
    assert(response._writtenLength==request.tcp.accepted.size());
    if(chunked) {
        const auto separator=request.tcp.accepted.find("\r\n\r\n");
        assert(separator!=std::string::npos);
        size_t position=separator+4;
        std::string decoded;
        for(;;) {
            assert(position+6<=request.tcp.accepted.size());
            const size_t length=std::stoul(request.tcp.accepted.substr(position,4),nullptr,16);
            assert(request.tcp.accepted.substr(position+4,2)=="\r\n");
            position+=6;
            assert(position+length+2<=request.tcp.accepted.size());
            decoded+=request.tcp.accepted.substr(position,length);
            position+=length;
            assert(request.tcp.accepted.substr(position,2)=="\r\n");
            position+=2;
            if(!length) break;
        }
        assert(position==request.tcp.accepted.size());
        assert(decoded==response.body);
    } else assert(request.tcp.accepted==expected);
    if(lowMemory || largestInternal<8192) {
        assert(request.tcp.maxBatch<=CONFIG_LWIP_TCP_MSS);
        assert(response.maxAllocation<=768);
    } else if(!partial) {
        assert(request.tcp.maxBatch>CONFIG_LWIP_TCP_MSS);
        assert(response.maxAllocation==ASYNC_RESPONCE_BUFF_SIZE);
    }
}
template<typename Response> void verifyOutstanding(bool longHeaders, bool lostAck) {
    freeInternal=28000;largestInternal=24000;
    Response response;
    if(longHeaders) response._assembled_headers=std::string(4097,'H');
    const std::string headers=response._assembled_headers;
    const std::string payload(30011,'P');
    if constexpr(std::is_same_v<Response,AsyncBasicResponse>) response._content=payload;
    else {response.body=payload;response._contentLength=payload.size();}
    AsyncWebServerRequest request;
    size_t unacked=0;
    for(int step=0;response._state!=RESPONSE_END&&step<1000;++step) {
        // Only an actual peer ACK returns socket space. Polls are not credits.
        const size_t before=request.tcp.accepted.size();
        response.write_send_buffs(&request,0,0);
        unacked+=request.tcp.accepted.size()-before;
        assert(unacked<=CONFIG_LWIP_TCP_MSS);
        const size_t queued=request.tcp.accepted.size();
        for(int poll=0;poll<12;++poll) response.write_send_buffs(&request,0,0);
        unacked+=request.tcp.accepted.size()-queued;
        assert(unacked<=CONFIG_LWIP_TCP_MSS);
        // Include short ACKs and swallowed/coalesced ACK notifications. Real
        // lwIP space recovers in both cases, so the next poll must make progress.
        const size_t acked=std::min<size_t>(unacked,step%3==0?211:CONFIG_LWIP_TCP_MSS);
        request.tcp.window+=acked;
        unacked-=acked;
        const size_t prior=request.tcp.accepted.size();
        response.write_send_buffs(&request,lostAck?0:acked,0);
        unacked+=request.tcp.accepted.size()-prior;
        assert(unacked<=CONFIG_LWIP_TCP_MSS);
    }
    assert(response._state==RESPONSE_END&&!request.tcp.closed);
    assert(request.tcp.accepted==headers+payload);
    assert(response._writtenLength==request.tcp.accepted.size());
}
template<typename Response> void verifyAckCompletion() {
    freeInternal=28000;largestInternal=24000;
    Response response;
    AsyncWebServerRequest request;
    request.tcp.window=TCP_SND_BUF;
    response._state=RESPONSE_WAIT_ACK;
    response._writtenLength=123;
    response.write_send_buffs(&request,123,0);
    assert(response._ackedLength==123&&response._state==RESPONSE_END);
    request.tcp.window=TCP_SND_BUF-CONFIG_LWIP_TCP_MSS;
    response._state=RESPONSE_CONTENT;
    response._in_flight_credit=0;
    response._sentLength=CONFIG_LWIP_TCP_WND_DEFAULT+1;
    response._writtenLength+=17;
    response.write_send_buffs(&request,17,0);
    assert(response._ackedLength==140); // even a flow-control early return records ACK
}
void verifyBasicOutputFailure() {
    freeInternal=28000;largestInternal=24000;
    AsyncBasicResponse response;
    response._content=std::string(9001,'X');
    const std::string expected=response._assembled_headers+response._content;
    AsyncWebServerRequest request;
    for(int step=0;response._state!=RESPONSE_END&&step<100;++step) {
        request.tcp.window=TCP_SND_BUF;
        request.tcp.failOutput=step%2==0;
        response.write_send_buffs(&request,step?CONFIG_LWIP_TCP_MSS:0,0);
    }
    assert(response._state==RESPONSE_END&&!request.tcp.closed);
    assert(request.tcp.accepted==expected);
    AsyncBasicResponse empty;
    AsyncWebServerRequest emptyRequest;
    const auto headers=empty._assembled_headers;
    empty.write_send_buffs(&emptyRequest,0,0);
    assert(empty._state==RESPONSE_WAIT_ACK&&emptyRequest.tcp.sends==1);
    assert(emptyRequest.tcp.accepted==headers);
    emptyRequest.tcp.window=TCP_SND_BUF;
    empty.write_send_buffs(&emptyRequest,headers.size(),0);
    assert(empty._state==RESPONSE_END);
}
template<typename Response> void verifyQueuedOutputRecovery(bool longHeaders) {
    freeInternal=28000;largestInternal=24000;
    Response response;
    if(longHeaders) response._assembled_headers=std::string(4097,'H');
    const auto headers=response._assembled_headers;
    const std::string payload(6001,'R');
    if constexpr(std::is_same_v<Response,AsyncBasicResponse>) response._content=payload;
    else {response.body=payload;response._contentLength=payload.size();}
    AsyncWebServerRequest request;
    request.tcp.failOutput=true;
    response.write_send_buffs(&request,0,0);
    assert(!request.tcp.closed);
    const auto first=request.tcp.accepted;
    assert(first.size()==CONFIG_LWIP_TCP_MSS);
    const auto sends=request.tcp.sends;
    // Preserve actual occupied sndbuf. No ACK can arrive while output fails.
    response.write_send_buffs(&request,0,0);
    assert(request.tcp.sends==sends+1 && request.tcp.accepted==first);
    request.tcp.failOutput=false;
    response.write_send_buffs(&request,0,0);
    assert(request.tcp.sends==sends+2 && request.tcp.accepted==first);
    // Successful output without ACK must not repeatedly refresh the timeout.
    response.write_send_buffs(&request,0,0);
    assert(request.tcp.sends==sends+2 && request.tcp.accepted==first);
    for(int step=0;response._state!=RESPONSE_END&&step<100;++step) {
        const auto acked=TCP_SND_BUF-request.tcp.window;
        request.tcp.window+=acked;
        response.write_send_buffs(&request,acked,0);
    }
    assert(response._state==RESPONSE_END&&!request.tcp.closed);
    assert(request.tcp.accepted==headers+payload);
}
template<typename Response> void verifyFinalOutputRecovery(bool lowMemory) {
    freeInternal=lowMemory?28000:80000;largestInternal=24000;
    Response response;
    const std::string payload(17,'Z');
    const auto expected=response._assembled_headers+payload;
    if constexpr(std::is_same_v<Response,AsyncBasicResponse>) response._content=payload;
    else {response.body=payload;response._contentLength=payload.size();}
    AsyncWebServerRequest request;
    request.tcp.failOutput=true;
    response.write_send_buffs(&request,0,0);
    assert(response._state!=RESPONSE_END&&!request.tcp.closed);
    assert(request.tcp.accepted==expected);
    const auto sends=request.tcp.sends;
    for(int i=0;i<5;++i) {
        assert(response.write_send_buffs(&request,0,0)==0);
        assert(request.tcp.sends==sends+i+1);
        assert(response._state!=RESPONSE_END&&request.tcp.accepted==expected);
    }
    request.tcp.failOutput=false;
    assert(response.write_send_buffs(&request,0,0)==expected.size());
    assert(request.tcp.accepted==expected);
    // No repeated output after one successful recovery, even without ACK.
    const auto recoveredSends=request.tcp.sends;
    response.write_send_buffs(&request,0,0);
    assert(response._state==RESPONSE_WAIT_ACK);
    request.tcp.window=TCP_SND_BUF;
    response.write_send_buffs(&request,expected.size(),0);
    assert(response._state==RESPONSE_END&&!request.tcp.closed);
    assert(request.tcp.sends==recoveredSends&&request.tcp.accepted==expected);
}
void verifyTerminalChunk(size_t contentSize, size_t tailAcceptance, bool zeroFirst) {
    freeInternal=28000;largestInternal=24000;
    AsyncAbstractResponse response;
    response._chunked=true;response._sendContentLength=false;
    response._contentLength=0;response.body=std::string(contentSize,'Z');
    const std::string headers=response._assembled_headers;
    AsyncWebServerRequest request;
    bool injectedZero=false;
    size_t acked=0;
    for(int step=0;response._state!=RESPONSE_END&&step<10000;++step) {
        request.tcp.window=TCP_SND_BUF;
        request.tcp.perAddLimit=tailAcceptance;
        if(zeroFirst&&!injectedZero&&response.sourcePosition==contentSize) {
            request.tcp.perAddLimit=0;injectedZero=true;
        }
        const size_t before=request.tcp.accepted.size();
        response.write_send_buffs(&request,acked,0);
        acked=request.tcp.accepted.size()-before;
    }
    assert(response._state==RESPONSE_END&&!request.tcp.closed&&!response._send_buffer_len);
    const auto& bytes=request.tcp.accepted;
    assert(bytes.compare(0,headers.size(),headers)==0);
    size_t cursor=headers.size();std::string decoded;
    for(;;) {
        assert(cursor+6<=bytes.size());
        const size_t length=std::stoul(bytes.substr(cursor,4),nullptr,16);
        assert(bytes.substr(cursor+4,2)=="\r\n");cursor+=6;
        assert(cursor+length+2<=bytes.size());
        decoded+=bytes.substr(cursor,length);cursor+=length;
        assert(bytes.substr(cursor,2)=="\r\n");cursor+=2;
        if(!length) break;
    }
    assert(cursor==bytes.size()&&decoded==response.body);
}
void verifyAllocationFallback() {
    freeInternal=28000;largestInternal=24000;
    AsyncAbstractResponse response;
    response.body=std::string(15331,'F');response._contentLength=response.body.size();
    const auto expected=response._assembled_headers+response.body;
    AsyncWebServerRequest request;
    allocationLimit=0;allocationFailures=0;
    response.write_send_buffs(&request,0,0);
    assert(response._state==RESPONSE_CONTENT&&!request.tcp.closed);
    assert(response._sentLength==0&&!response._send_buffer&&allocationFailures==2);
    // Once any allocation becomes possible, the same request must resume.
    allocationLimit=256;
    for(int step=0;response._state!=RESPONSE_END&&step<1000;++step) {
        request.tcp.window=TCP_SND_BUF;
        response.write_send_buffs(&request,step?217:0,0);
    }
    assert(response._state==RESPONSE_END&&!request.tcp.closed);
    assert(request.tcp.accepted==expected&&response.maxAllocation==256);
    assert(allocationFailures>2); // 768-byte attempt failed before 256 fallback
    // The fallback also preserves framing and payload when the response has
    // no content length or uses chunked encoding, not only a known-size file.
    verify(true,true,true,true);
    verify(true,true,true,false,true);
    allocationLimit=static_cast<size_t>(-1);
}
int main() {
    verifyBasic(true,false);verifyBasic(true,true);verifyBasic(false,false);verifyBasic(false,true);
    verify(true,false,false);verify(true,true,false);verify(true,false,true);
    verify(false,false,false);verify(false,true,true);verify(true,true,true,true);
    verify(true,false,true,false,true);verify(false,false,false,false,true);
    verify(true,true,true,false,true);
    largestInternal=7000;verify(false,false,false); // fragmentation also invokes limit
    verifyOutstanding<AsyncAbstractResponse>(false,false);
    verifyOutstanding<AsyncAbstractResponse>(true,false);
    verifyOutstanding<AsyncAbstractResponse>(false,true);
    verifyOutstanding<AsyncAbstractResponse>(true,true);
    verifyOutstanding<AsyncBasicResponse>(false,false);
    verifyOutstanding<AsyncBasicResponse>(true,false);
    verifyOutstanding<AsyncBasicResponse>(false,true);
    verifyOutstanding<AsyncBasicResponse>(true,true);
    verifyAckCompletion<AsyncAbstractResponse>();
    verifyAckCompletion<AsyncBasicResponse>();
    verifyBasicOutputFailure();
    verifyQueuedOutputRecovery<AsyncAbstractResponse>(false);
    verifyQueuedOutputRecovery<AsyncAbstractResponse>(true);
    verifyQueuedOutputRecovery<AsyncBasicResponse>(false);
    verifyQueuedOutputRecovery<AsyncBasicResponse>(true);
    verifyFinalOutputRecovery<AsyncAbstractResponse>(false);
    verifyFinalOutputRecovery<AsyncAbstractResponse>(true);
    verifyFinalOutputRecovery<AsyncBasicResponse>(false);
    verifyFinalOutputRecovery<AsyncBasicResponse>(true);
    verifyAllocationFallback();
    // Every possible partial acceptance of the final eight-byte chunk marker,
    // with empty/nonempty payloads and a temporary zero-progress TCP write.
    for(size_t length:{size_t(0),size_t(1),size_t(29),size_t(3000)})
      for(size_t acceptance=1;acceptance<=7;++acceptance)
        for(bool zero:{false,true}) verifyTerminalChunk(length,acceptance,zero);
    std::cout<<"response batch scenarios passed\n";
    return 0;
}
