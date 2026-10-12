#define FASTBEE_FINITE_REFERENCE_MAIN
#include "web_finite_progress_harness.cpp"

struct BasicFixture : Fixture {
    AsyncBasicResponse* basic;
    BasicFixture() {
        delete response;
        response=nullptr;
        basic=new AsyncBasicResponse;
        owner->_response=basic;
        basic->_state=RESPONSE_SETUP;
        basic->_content=std::string(1774,'J');
        basic->_assembled_headers=std::string(200,'H');
        owner->tcp.addAdapter=[this](const char* data,size_t length){return clock.add(data,length);};
    }
    void start(uint32_t at) {fakeMillis=at;basic->_respond(owner.get());}
    std::string expected() const {return std::string(200,'H')+std::string(1774,'J');}
};

void initialWriteFailure(uint32_t start) {
    BasicFixture f;f.owner->tcp.perAddLimit=0;f.start(start);
    for(uint32_t offset:{1U,1000U,5000U,9999U}) {
        f.poll(start+offset);
        assert(!f.owner->tcp.closed&&f.owner->tcp.accepted.empty());
        assert(f.clock._tx_last_packet==start+offset);
    }
    f.poll(start+10000U);
#if EXPECT_BOUNDED
    assert(f.basic->_state==RESPONSE_FAILED&&f.owner->tcp.closed);
    assert(f.closes==1&&f.disconnects==1);
    f.poll(start+11000U);assert(f.closes==1);
#else
    assert(!f.owner->tcp.closed&&f.owner->tcp.accepted.empty());
    for(uint32_t offset:{15000U,20000U,25000U,30000U})f.poll(start+offset);
    assert(!f.owner->tcp.closed&&f.clock._tx_last_packet==start+30000U);
    std::cout<<"OLD_BASIC_STALL_REPRODUCED: actual add fails, empty send refreshes clock, alive at 30000ms\n";
#endif
}

#if EXPECT_BOUNDED
void partialThenStall() {
    BasicFixture f;f.start(0);
    assert(f.owner->tcp.accepted.size()==1436&&f.basic->_state==RESPONSE_CONTENT);
    f.owner->tcp.window=TCP_SND_BUF; // Peer ACK arrived; callback was lost.
    f.owner->tcp.perAddLimit=0;
    for(uint32_t at:{1000U,5000U,10000U,10999U}) {
        f.poll(at);assert(!f.owner->tcp.closed);
    }
    f.poll(11000);assert(f.owner->tcp.closed&&f.basic->_state==RESPONSE_FAILED);
    assert(f.owner->tcp.accepted.size()==1436&&f.closes==1&&f.disconnects==1);
}

void recoveryBeforeDeadline() {
    BasicFixture f;f.owner->tcp.perAddLimit=0;f.start(0);
    f.poll(9000);assert(!f.owner->tcp.closed);
    f.owner->tcp.perAddLimit=TCP_SND_BUF;f.poll(9999);
    assert(f.owner->tcp.accepted.size()==1436&&!f.owner->tcp.closed);
    f.owner->tcp.window=TCP_SND_BUF;f.poll(19999);
    assert(f.owner->tcp.accepted==f.expected()&&f.basic->_state==RESPONSE_WAIT_ACK);
    f.owner->tcp.window=TCP_SND_BUF;f.poll(29999);
    assert(f.basic->_state==RESPONSE_END&&f.closes==1&&f.disconnects==1);
}

void pendingFlushIsNotProgress() {
    BasicFixture f;f.owner->tcp.failOutput=true;f.start(0);
    assert(f.basic->_fastbeePendingOutputBytes==1436);
    f.owner->tcp.failOutput=false;f.poll(9000);
    assert(f.basic->_fastbeePendingOutputBytes==0&&!f.owner->tcp.closed);
    assert(f.owner->tcp.accepted.size()==1436);
    f.poll(10000);assert(f.basic->_state==RESPONSE_FAILED&&f.closes==1);
}

void zeroSpaceAndDuplicateCallbacks() {
    BasicFixture zero;zero.owner->tcp.window=0;zero.start(0);
    zero.poll(9999);assert(!zero.owner->tcp.closed);
    zero.poll(10000);assert(zero.owner->tcp.closed&&zero.closes==1);
    BasicFixture duplicate;duplicate.start(0);
    for(uint32_t at:{1000U,5000U,9999U}) {
        fakeMillis=at;duplicate.owner->_onAck(static_cast<size_t>(-1),0);
        assert(!duplicate.owner->tcp.closed);
    }
    fakeMillis=10000;duplicate.owner->_onAck(static_cast<size_t>(-1),0);
    assert(duplicate.owner->tcp.closed&&duplicate.basic->_state==RESPONSE_FAILED);
}

void slowRealProgressAndOwnership() {
    BasicFixture f;f.start(0);uint32_t at=9000;
    for(size_t step=0;!f.owner->tcp.closed&&step<2200;++step) {
        f.owner->tcp.window=std::min<size_t>(TCP_SND_BUF,f.owner->tcp.window+1);
        // Real AsyncClient::_sent updates the TCP ACK timestamp before the
        // optional HTTP ACK callback. Model that transport side effect even
        // when the Request callback is deliberately omitted.
        f.clock._rx_last_ack=f.clock._rx_last_packet=at;
        f.poll(at);at+=9000;
    }
    assert(at>20000&&f.owner->tcp.accepted==f.expected());
    assert(f.basic->_state==RESPONSE_END&&f.closes==1&&f.disconnects==1);
    std::weak_ptr<AsyncWebServerRequest> weak=f.owner;
    f.owner.reset();assert(weak.expired()&&f.clientDestructions==1);
}
#endif

int main() {
    initialWriteFailure(0);initialWriteFailure(UINT32_MAX-5000U);
#if EXPECT_BOUNDED
    partialThenStall();recoveryBeforeDeadline();pendingFlushIsNotProgress();
    zeroSpaceAndDuplicateCallbacks();slowRealProgressAndOwnership();
    // Keep existing real Abstract/request/clock/close/ownership checks.
    finite_reference_main();
    assert(TransportClock::_fastbee_accepted_count==0);
    std::cout<<"BASIC_BOUNDED_PROGRESS_PASS: initial/partial write failure, zero space, output retry, duplicate ACK, recovery, wrap, slow real drain, ownership\n";
#endif
}
