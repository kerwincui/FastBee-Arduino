#include "utils/SystemTimeQuality.h"
#include "utils/SystemTimePlatform.h"
#include <cassert>
#include <cstring>
using Quality=FastBee::SystemTimeQuality;
static int64_t wall=0;
static uint64_t mono=1000;
static unsigned writes=0,delays=0;
static bool deliverSntp=false;
static FastBee::SystemTimePlatform::Observer observer=nullptr;
uint64_t FastBee::SystemTimePlatform::monotonicMs() { return mono; }
int64_t FastBee::SystemTimePlatform::wallMs() { return wall; }
bool FastBee::SystemTimePlatform::writeMs(int64_t value) { ++writes;wall=value;return true; }
void FastBee::SystemTimePlatform::installObserver(Observer value) { observer=value; }
static unsigned long consumerMillis() { return static_cast<unsigned long>(mono); }
static void consumerDelay(unsigned long elapsed) {
    ++delays;mono+=elapsed;wall+=elapsed;
    if(deliverSntp) { deliverSntp=false;assert(observer);observer(wall); }
}
class TimeUtils { public: static bool syncNTP(unsigned long,uint32_t); };
#define millis consumerMillis
#define delay consumerDelay
#include "rtc_sync_wait_under_test.inc"
#undef millis
#undef delay
int main(int argc,char** argv) {
    assert(argc==3);
    assert(Quality::claimRtcBootSeed());
    assert(Quality::seedFromRtcMilliseconds(1700000000000LL,0));
    const auto rtc=Quality::snapshot();
    assert(rtc.timeValid && !rtc.synced && rtc.sequence!=0);
    assert(std::strcmp(rtc.source,"rtc")==0 && writes==1);
    Quality::installSntpObserver();
    const bool network=std::strcmp(argv[1],"sntp_after_rtc")==0;
    assert(network || std::strcmp(argv[1],"rtc_only")==0);
    deliverSntp=network;
    // Starting before the RTC revision is deliberate: a changed revision by
    // itself cannot complete the production wait when synced remains false.
    assert(TimeUtils::syncNTP(300,0)==network);
    assert(writes==1 && delays==(network?1U:3U));
    const auto after=Quality::snapshot();
    assert(after.synced==network && after.timeValid && !after.transportAuthenticated);
    assert(std::strcmp(after.source,network?"sntp":"rtc")==0);
    assert(after.sequence==rtc.sequence+(network?1U:0U));
}
