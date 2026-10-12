#include "core/PendingReportQueue.h"
#include "network/PendingReportStatus.h"
#include "protocols/MqttClientLease.h"
#include <cassert>
#include <condition_variable>
#include <stdexcept>
#include <thread>
#include <vector>
using Queue=FastBee::PendingReportQueue;
using Admission=Queue::Admission;
using Retry=Queue::Retry;
using Replay=FastBee::ReportReplayResult;
struct BoundClient {
    uint32_t id;
    bool connected=true, accepting=true;
    unsigned sent=0;
    uint32_t getReportInstanceId() const {return id;}
    bool getIsConnected() const {return connected;}
    bool queueReportData(const String&) {++sent;return accepting;}
};

struct BoundLease {
    FastBee::MqttClientLifetime::Read read;
    BoundClient* client;
    BoundLease(FastBee::MqttClientLifetime& gate, BoundClient* value) : read(gate), client(value) {}
    explicit operator bool() const { return bool(read) && client; }
    BoundClient* operator->() const { return client; }
};

int main(int argc,char** argv) {
    assert(argc==2);const std::string mode=argv[1];Queue q;
    if(mode=="unavailable") {
        pendingCreateFail=true;assert(!q.begin());assert(q.enqueue("a",0,true,47)==Admission::Unavailable);
        assert(!q.snapshot(0).available);return 0;
    }
    assert(q.begin() && q.begin());
    auto put=[&](const String& v,uint32_t now=1){return q.enqueue(v,now,true,47);};
    if(mode=="collection_release" || mode=="collection_busy" ||
       mode=="collection_absent" || mode=="collection_test_only") {
        FastBee::MqttClientLifetime gate;
        BoundClient client{mode=="collection_test_only"?0U:47U};
        FastBee::MqttClientLifetime::Write writer;
        if(mode=="collection_busy")writer=FastBee::MqttClientLifetime::Write(gate);
        unsigned calls=0;
        const uint32_t origin=FastBee::captureReportInstance([&] {
            ++calls;return BoundLease(gate,mode=="collection_absent"?nullptr:&client);
        });
        assert(calls==1 && origin==(mode=="collection_release"?47U:0U));
        writer.release();
        // Collection and callbacks can now replace the object; no read lease
        // leaks out of capture even on absent/test-only/busy paths.
        FastBee::MqttClientLifetime::Write replacement(gate);
        assert(bool(replacement) && client.sent==0);return 0;
    }
    if(mode=="collection_same" || mode=="collection_changed" || mode=="collection_unknown" ||
       mode=="collection_offline" || mode=="collection_refused" || mode=="collection_dropped" ||
       mode=="collection_batch") {
        using Result=FastBee::BoundReportAdmission;
        FastBee::MqttClientLifetime gate;BoundClient sampled{47},current{48};
        const uint32_t origin=mode=="collection_unknown"?0:
            FastBee::captureReportInstance([&]{return BoundLease(gate,&sampled);});
        BoundClient* client=(mode=="collection_changed" || mode=="collection_unknown" ||
                             mode=="collection_batch")?&current:&sampled;
        if(mode=="collection_offline" || mode=="collection_dropped")client=nullptr;
        if(mode=="collection_refused")sampled.accepting=false;
        unsigned retained=0;
        auto retain=[&](const String& payload,uint32_t id,uint32_t observed) {
            ++retained;assert(id==origin);
            assert(observed==(client?client->id:0));
            if(mode=="collection_dropped")return false;
            return q.enqueue(payload,1,true,id,observed)==Admission::Accepted;
        };
        auto result=FastBee::submitBoundReport(client,origin,String("original"),retain);
        if(mode=="collection_same") {
            assert(result==Result::Queued && sampled.sent==1 && retained==0 && q.snapshot(1).depth==0);
        } else if(mode=="collection_unknown") {
            assert(result==Result::Dropped && current.sent==0 && retained==1);
            assert(q.snapshot(1).rejectedUnknownInstance==1 && q.snapshot(1).depth==0);
        } else if(mode=="collection_dropped")assert(result==Result::Dropped && retained==1);
        else if(mode=="collection_changed" || mode=="collection_batch") {
            assert(result==Result::Dropped && retained==1 && current.sent==0);
            if(mode=="collection_batch") {
                assert(FastBee::submitBoundReport(client,origin,String("second"),retain)==Result::Dropped);
                assert(retained==2);
            }
            const auto state=q.snapshot(2);
            assert(state.depth==0 && state.admitted==0 && state.discardedInstanceChanged==retained);
            assert(state.lastRetry==Queue::LastRetry::None && state.retryDelayMs==0);
        }
        else {
            assert(result==Result::Retained && retained==1 && q.snapshot(1).depth==1);
            assert(sampled.sent==(mode=="collection_refused"?1U:0U));sampled.accepting=true;
            assert(q.retryOne(2,true,[&](const String& payload,uint32_t id) {
                assert(id==47 && payload=="original");return FastBee::replayBoundReport(sampled,id,payload);
            })==Retry::Accepted);
            assert(q.snapshot(3).depth==0);
        }
        return 0;
    }
    if(mode=="admission_changed_full") {
        for(unsigned i=0;i<Queue::Capacity;++i)assert(q.enqueue("valid",i,true,48,48)==Admission::Accepted);
        // No memory permission is needed to reject a known stale input. It
        // must not evict the live head, even though the queue is already full.
        assert(q.enqueue("stale",5,false,47,48)==Admission::InstanceChanged);
        const auto state=q.snapshot(6);
        assert(state.depth==Queue::Capacity && state.admitted==Queue::Capacity && state.evicted==0);
        assert(state.discardedInstanceChanged==1 && state.rejectedMemory==0 && state.rejectedFull==0);
        assert(state.oldestAgeMs==6 && state.oldestAttempts==0 && state.lastRetry==Queue::LastRetry::None);
        assert(q.retryOne(7,true,[](const String& payload,uint32_t id) {
            assert(payload=="valid" && id==48);return Replay::Accepted;
        })==Retry::Accepted);return 0;
    }
    if(mode=="admission_changed_inflight") {
        assert(put("pinned")==Admission::Accepted);
        assert(q.retryOne(2,true,[&](const String& payload,uint32_t id) {
            assert(q.enqueue("stale",3,true,46,47)==Admission::InstanceChanged);
            auto state=q.snapshot(3);
            assert(state.inFlight && state.depth==1 && state.discardedInstanceChanged==1);
            assert(state.lastRetry==Queue::LastRetry::None && payload=="pinned" && id==47);
            return Replay::Rejected;
        })==Retry::Rejected);
        auto state=q.snapshot(4);
        assert(state.depth==1 && state.retryRejected==1 && state.discardedInstanceChanged==1);
        assert(state.lastRetry==Queue::LastRetry::Rejected && state.retryDelayMs==4998);return 0;
    }
    if(mode=="admission_changed_busy") {
        assert(put("valid")==Admission::Accepted);pendingForceBusy=true;
        assert(q.enqueue("stale",2,true,47,48)==Admission::Busy);pendingForceBusy=false;
        auto state=q.snapshot(3);
        assert(state.depth==1 && state.rejectedBusy==1 && state.discardedInstanceChanged==0 && state.evicted==0);
        return 0;
    }
    if(mode=="admission_changed_history") {
        assert(put("old")==Admission::Accepted);
        assert(q.retryOne(2,true,[](const String&,uint32_t){return Replay::InstanceChanged;})==Retry::InstanceChanged);
        assert(q.enqueue("new",3,true,48,48)==Admission::Accepted);
        assert(q.enqueue("late",3,true,47,48)==Admission::InstanceChanged);
        auto state=q.snapshot(4);
        assert(state.discardedInstanceChanged==2 && state.retryAccepted==0 && state.retryRejected==0);
        assert(state.lastRetry==Queue::LastRetry::InstanceChanged && state.retryDelayMs==4998);
        const String json = FastBee::pendingReportStatusJson(state);
        JsonDocument doc;assert(!deserializeJson(doc,json.c_str()));
        assert(doc["discardedInstanceChanged"]==2 && doc["lastRetryResult"]=="instance_changed");return 0;
    }
    if(mode=="admission_identity_unknown") {
        assert(q.enqueue("unknown origin",1,true,0,48)==Admission::UnknownInstance);
        assert(q.enqueue("unknown current",2,true,47,0)==Admission::Accepted);
        auto state=q.snapshot(3);
        assert(state.rejectedUnknownInstance==1 && state.discardedInstanceChanged==0 && state.depth==1);
        return 0;
    }
    if(mode=="admission_disconnected_changed") {
        BoundClient client{48};client.connected=false;
        auto result=FastBee::submitBoundReport(&client,47,String("stale"),[&](const String& payload,uint32_t origin,uint32_t current) {
            assert(origin==47 && current==48);
            return q.enqueue(payload,1,true,origin,current)==Admission::Accepted;
        });
        assert(result==FastBee::BoundReportAdmission::Dropped && client.sent==0);
        assert(q.snapshot(2).depth==0 && q.snapshot(2).discardedInstanceChanged==1);return 0;
    }
    if(mode=="instance_unknown") {
        assert(q.enqueue("a",1,true,0)==Admission::UnknownInstance);
        auto state=q.snapshot(2);assert(state.depth==0 && state.rejectedUnknownInstance==1);
        JsonDocument doc;assert(!deserializeJson(doc,FastBee::pendingReportStatusJson(state).c_str()));
        assert(doc["bindingScope"]=="mqtt_instance" && doc["rejectedUnknownInstance"]==1);
        assert(doc["instanceId"].isNull());return 0;
    }
    if(mode=="instance_sequence") {
        FastBee::ReportInstanceSequence sequence(UINT32_MAX-1);
        assert(sequence.next()==UINT32_MAX && sequence.next()==0 && sequence.next()==0);return 0;
    }
    if(mode=="instance_reuse") {
        // Reusing storage/addresses must never reauthorize an old report.
        const auto first=FastBee::nextReportInstanceId(),second=FastBee::nextReportInstanceId();
        assert(first && second && first!=second); BoundClient client{second};
        assert(FastBee::replayBoundReport(client,first,String("a"))==Replay::InstanceChanged);
        assert(client.sent==0);return 0;
    }
    if(mode=="instance_sequence_threads") {
        FastBee::ReportInstanceSequence sequence;std::mutex mutex;std::vector<uint32_t> ids;
        std::vector<std::thread> threads;
        for(unsigned n=0;n<4;++n)threads.emplace_back([&]{
            for(unsigned i=0;i<100;++i) {const uint32_t id=sequence.next();
                if(id) {std::lock_guard<std::mutex> lock(mutex);
                    for(uint32_t previous:ids) {assert(previous!=id);}
                    ids.push_back(id);}}
        });
        for(auto& thread:threads) {thread.join();}
        assert(!ids.empty());return 0;
    }
    if(mode=="instance_same" || mode=="instance_mismatch" || mode=="instance_offline" || mode=="instance_rejected") {
        assert(put("a")==Admission::Accepted);
        BoundClient client{mode=="instance_mismatch"?48U:47U};
        client.connected=mode!="instance_offline";client.accepting=mode!="instance_rejected";
        const auto result=q.retryOne(2,true,[&](const String& payload,uint32_t instanceId) {
            assert(pendingLockDepth==0 && instanceId==47);return FastBee::replayBoundReport(client,instanceId,payload);
        });
        const auto state=q.snapshot(3);
        if(mode=="instance_same")assert(result==Retry::Accepted && state.depth==0 && state.retryAccepted==1 && client.sent==1);
        else if(mode=="instance_mismatch") {
            assert(result==Retry::InstanceChanged && state.depth==0 && state.discardedInstanceChanged==1 && client.sent==0);
            assert(state.retryAccepted==0 && state.retryRejected==0 && state.lastRetry==Queue::LastRetry::InstanceChanged);
            JsonDocument doc;assert(!deserializeJson(doc,FastBee::pendingReportStatusJson(state).c_str()));
            assert(doc["discardedInstanceChanged"]==1 && doc["lastRetryResult"]=="instance_changed");
        } else assert(result==Retry::Rejected && state.depth==1 && state.retryRejected==1 &&
                      client.sent==(mode=="instance_rejected"?1U:0U));
        return 0;
    }
    if(mode=="instance_mixed" || mode=="instance_completion_busy") {
        assert(put("a")==Admission::Accepted);BoundClient client{48};
        assert(q.retryOne(2,true,[&](const String& payload,uint32_t origin) {
            assert(q.enqueue("b",3,true,48)==Admission::Accepted);
            if(mode=="instance_completion_busy")pendingForceBusy=true;
            return FastBee::replayBoundReport(client,origin,payload);
        })==Retry::InstanceChanged);
        if(mode=="instance_completion_busy") {assert(!q.snapshot(3).available);pendingForceBusy=false;}
        assert(q.snapshot(3).depth==1 && q.snapshot(3).discardedInstanceChanged==1 && client.sent==0);
        assert(q.snapshot(4).discardedInstanceChanged==1); // settle just once
        assert(q.retryOne(5002,true,[&](const String& payload,uint32_t origin) {
            assert(origin==48 && payload=="b");return FastBee::replayBoundReport(client,origin,payload);
        })==Retry::Accepted);
        assert(q.snapshot(5003).depth==0 && q.snapshot(5003).discardedInstanceChanged==1 && client.sent==1);return 0;
    }
    if(mode=="clock_under_lock") {
        uint32_t current=10;unsigned reads=0;
        auto clock=[&](){assert(pendingLockDepth==1);++reads;return current;};
        assert(q.enqueueAt("a",clock,true,47)==Admission::Accepted);
        current=15;assert(q.snapshotAt(clock).oldestAgeMs==5);
        assert(q.retryOneAt(clock,true,[](const String&, uint32_t){return FastBee::ReportReplayResult::Rejected;})==Retry::Rejected);
        assert(q.snapshotAt(clock).retryDelayMs==5000 && reads==4);return 0;
    }
    if(mode=="retry_details") {
        assert(put("a",0)==Admission::Accepted);
        auto state=q.snapshot(0);assert(state.oldestAttempts==0 && state.retryDelayMs==0 && state.lastRetry==Queue::LastRetry::None);
        assert(q.retryOne(10,true,[&](const String&, uint32_t){
            auto in=q.snapshot(10);assert(in.inFlight && in.oldestAttempts==1 && in.retryDelayMs==0);return FastBee::ReportReplayResult::Rejected;
        })==Retry::Rejected);
        state=q.snapshot(11);assert(state.oldestAttempts==1 && state.retryDelayMs==4999 && state.lastRetry==Queue::LastRetry::Rejected);
        JsonDocument doc;assert(!deserializeJson(doc,FastBee::pendingReportStatusJson(state).c_str()));
        assert(doc["retryState"]=="cooldown" && doc["lastRetryResult"]=="rejected" && doc["oldestAttempts"]==1);
        assert(put("b",20)==Admission::Accepted);
        assert(q.retryOne(5010,true,[](const String& s, uint32_t){assert(s=="a");return FastBee::ReportReplayResult::Accepted;})==Retry::Accepted);
        state=q.snapshot(5011);assert(state.depth==1 && state.oldestAttempts==0 && state.retryDelayMs==4999 && state.lastRetry==Queue::LastRetry::Accepted);
        assert(q.retryOne(10010,true,[](const String& s, uint32_t){assert(s=="b");return FastBee::ReportReplayResult::Accepted;})==Retry::Accepted);
        state=q.snapshot(10011);assert(state.depth==0 && state.retryDelayMs==0 && state.oldestAttempts==0);return 0;
    }
    if(mode=="retry_detail_wrap") {
        assert(put("a",UINT32_MAX-10)==Admission::Accepted);
        assert(q.retryOne(UINT32_MAX-5,true,[](const String&, uint32_t){return FastBee::ReportReplayResult::Rejected;})==Retry::Rejected);
        auto state=q.snapshot(4);assert(state.oldestAgeMs==15 && state.retryDelayMs==4990 && state.oldestAttempts==1);return 0;
    }
    if(mode=="invalid") {
        assert(put("")==Admission::Invalid);assert(put(std::string(513,'x'))==Admission::Invalid);
        assert(put(String("a\0b",3))==Admission::Invalid);assert(q.snapshot(5).rejectedInvalid==3);return 0;
    }
    if(mode=="low_memory") {
        assert(q.enqueue("a",0,false,47)==Admission::LowMemory);
        assert(q.snapshot(5).rejectedMemory==1 && q.snapshot(5).depth==0);return 0;
    }
    if(mode=="busy") {
        pendingForceBusy=true;assert(put("a")==Admission::Busy);assert(!q.snapshot(0).available);
        pendingForceBusy=false;assert(q.snapshot(0).depth==0 && q.snapshot(0).contention==1 && q.snapshot(0).rejectedBusy==1);
        pendingForceBusy=true;assert(q.retryOne(0,true,[](const String&, uint32_t){assert(false);return FastBee::ReportReplayResult::Accepted;})==Retry::Busy);
        pendingForceBusy=false;assert(q.snapshot(0).contention==2 && q.snapshot(0).rejectedBusy==1);return 0;
    }
    if(mode=="empty") {assert(q.retryOne(0,true,[](const String&, uint32_t){assert(false);return FastBee::ReportReplayResult::Accepted;})==Retry::Idle);return 0;}
    assert(put(mode=="max_bytes"?String(std::string(512,'x')):String("a"))==Admission::Accepted);
    if(mode=="offline") {
        assert(q.retryOne(0,false,[](const String&, uint32_t){assert(false);return FastBee::ReportReplayResult::Accepted;})==Retry::Idle);
        assert(q.snapshot(3).depth==1);return 0;
    }
    if(mode=="failed_retained" || mode=="throttle" || mode=="wrap") {
        uint32_t first=mode=="wrap"?UINT32_MAX-100U:0U;
        assert(q.retryOne(first,true,[](const String& s, uint32_t){assert(pendingLockDepth==0 && s=="a");return FastBee::ReportReplayResult::Rejected;})==Retry::Rejected);
        assert(q.snapshot(first).depth==1 && q.snapshot(first).retryRejected==1);
        assert(q.snapshot(first).retryRejected==1); // completion reconciled exactly once
        assert(q.retryOne(first+4999U,true,[](const String&, uint32_t){assert(false);return FastBee::ReportReplayResult::Accepted;})==Retry::Idle);
        assert(q.retryOne(first+5000U,true,[](const String& s, uint32_t){assert(s=="a");return FastBee::ReportReplayResult::Accepted;})==Retry::Accepted);
        assert(q.snapshot(first+5000U).depth==0 && q.snapshot(first+5000U).retryAccepted==1);return 0;
    }
    if(mode=="full_evict") {
        for(int n=1;n<=5;++n)assert(put(String(n))==(n==5?Admission::ReplacedOldest:Admission::Accepted));
        assert(q.snapshot(10).depth==5 && q.snapshot(10).evicted==1);
        assert(q.retryOne(1,true,[](const String& s, uint32_t){assert(s=="1");return FastBee::ReportReplayResult::Accepted;})==Retry::Accepted);return 0;
    }
    if(mode=="reentrant") {
        assert(q.retryOne(0,true,[&](const String& s, uint32_t){
            assert(pendingLockDepth==0 && s=="a" && q.snapshot(0).inFlight);
            assert(q.retryOne(6000,true,[](const String&, uint32_t){assert(false);return FastBee::ReportReplayResult::Accepted;})==Retry::Busy);
            for(int n=0;n<4;++n)assert(put(String(n))==Admission::Accepted);
            assert(put("extra")==Admission::Full && s=="a");return FastBee::ReportReplayResult::Rejected;
        })==Retry::Rejected);
        auto state=q.snapshot(1);assert(state.depth==5 && state.rejectedFull==1 && state.retryRejected==1 && !state.inFlight);return 0;
    }
    if(mode=="completion_busy") {
        assert(q.retryOne(0,true,[](const String&, uint32_t){pendingForceBusy=true;return FastBee::ReportReplayResult::Accepted;})==Retry::Accepted);
        assert(!q.snapshot(1).available);pendingForceBusy=false;
        assert(q.snapshot(2).depth==0 && q.snapshot(2).retryAccepted==1);return 0;
    }
    if(mode=="callback_exception") {
        try {q.retryOne(0,true,[](const String&, uint32_t)->FastBee::ReportReplayResult{throw std::runtime_error("send");});assert(false);}
        catch(const std::runtime_error&) {}
        assert(!q.snapshot(1).inFlight && q.snapshot(1).depth==1 && q.snapshot(1).retryRejected==1);return 0;
    }
    if(mode=="age") {assert(q.snapshot(51).oldestAgeMs==50);return 0;}
    if(mode=="json" || mode=="json_unavailable") {
        if(mode=="json_unavailable")pendingForceBusy=true;
        JsonDocument doc;assert(!deserializeJson(doc,FastBee::pendingReportStatusJson(q.snapshot(51)).c_str()));
        assert(!doc["deliveryVerified"].as<bool>() && doc["payload"].isNull());
        if(mode=="json")assert(doc["available"]==true && doc["depth"]==1 && doc["oldestAgeMs"]==50);
        else assert(doc["available"]==false && doc["depth"].isNull());
        pendingForceBusy=false;return 0;
    }
    if(mode=="concurrent_pin") {
        std::mutex barrier;std::condition_variable cv;bool entered=false,release=false;
        std::thread consumer([&]{assert(q.retryOne(0,true,[&](const String& value, uint32_t){
            std::unique_lock<std::mutex> lock(barrier);entered=true;cv.notify_all();
            cv.wait(lock,[&]{return release;});assert(value=="a" && pendingLockDepth==0);return FastBee::ReportReplayResult::Rejected;
        })==Retry::Rejected);});
        {std::unique_lock<std::mutex> lock(barrier);cv.wait(lock,[&]{return entered;});}
        std::vector<std::thread> producers;
        std::atomic<unsigned> admitted{0},rejected{0};
        for(int p=0;p<3;++p)producers.emplace_back([&,p]{
            for(int i=0;i<100;++i){auto r=put(String(p)+":"+String(i));
                if(r==Admission::Accepted)++admitted;
                else {assert(r==Admission::Full || r==Admission::Busy);++rejected;}}
        });
        for(auto& producer:producers)producer.join();
        assert(admitted==4 && rejected==296);
        assert(q.retryOne(9000,true,[](const String&, uint32_t){assert(false);return FastBee::ReportReplayResult::Accepted;})==Retry::Busy);
        {std::lock_guard<std::mutex> lock(barrier);release=true;}cv.notify_all();consumer.join();
        auto state=q.snapshot(10);assert(state.depth==5 && state.retryRejected==1 && state.evicted==0);
        assert(q.retryOne(10000,true,[](const String& s, uint32_t){assert(s=="a");return FastBee::ReportReplayResult::Accepted;})==Retry::Accepted);
        assert(q.snapshot(10001).depth==4);return 0;
    }
    assert(q.retryOne(0,true,[&](const String& s, uint32_t){
        assert(pendingLockDepth==0);assert(s.length()==(mode=="max_bytes"?512U:1U));return FastBee::ReportReplayResult::Accepted;
    })==Retry::Accepted);
    assert(q.snapshot(2).depth==0 && q.snapshot(2).retryAccepted==1);
}
