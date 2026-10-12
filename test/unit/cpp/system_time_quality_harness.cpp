#include "utils/SystemTimeQuality.h"
#include "utils/SystemTimePlatform.h"
#include <cassert>
#include <cstring>
#include <string>
using Quality=FastBee::SystemTimeQuality;
using Source=Quality::Source;
static int64_t wall=1760000000000LL;
static uint64_t mono=1000;
static bool writesOk=true, duringWriteSntp=false, observedBusy=false;
static bool duringMonotonicSntp=false, duringWallSntp=false;
static bool invalidateDuringWrite=false, rollbackDuringWrite=false;
static unsigned writes=0;
static FastBee::SystemTimePlatform::Observer observer=nullptr;
uint64_t FastBee::SystemTimePlatform::monotonicMs(){
    if(duringMonotonicSntp){duringMonotonicSntp=false;assert(observer);observer(wall);}
    return mono;
}
int64_t FastBee::SystemTimePlatform::wallMs(){
    const auto sampled=wall;
    if(duringWallSntp){duringWallSntp=false;assert(observer);observer(wall);}
    return sampled;
}
void FastBee::SystemTimePlatform::installObserver(Observer value){observer=value;}
bool FastBee::SystemTimePlatform::writeMs(int64_t value){
    ++writes;observedBusy=std::strcmp(Quality::snapshot().quality,"busy")==0;
    if(!writesOk)return false;
    wall=value;
    if(invalidateDuringWrite)mono=UINT64_MAX;
    if(rollbackDuringWrite)--mono;
    if(duringWriteSntp){assert(observer);wall+=10000;observer(wall);}
    return true;
}
static void expect(const char* quality,bool synced){
    const auto status=Quality::snapshot();
    assert(std::strcmp(status.quality,quality)==0 && status.synced==synced);
}
static void apply(){assert(Quality::applyMilliseconds(wall,Source::Https,true,Quality::sequence()));}
int main(int argc,char** argv){
    assert(argc==2);const std::string mode=argv[1];Quality::installSntpObserver();
    if(mode=="unknown") {expect("unknown",false);wall=0;expect("invalid",false);}
    else if(mode=="apply"){
        apply();expect("synced_tls",true);assert(observedBusy&&writes==1);
        const auto status=Quality::snapshot();assert(status.epochMs==wall&&status.ageMs==0&&status.sequence==1);
        mono+=86400000;wall+=86400000;expect("synced_tls",true);
        ++mono;++wall;expect("stale",false);
    } else if(mode=="revision"){
        apply();const auto saved=Quality::sequence();
        assert(!Quality::applyMilliseconds(wall,Source::Http,false,saved-1));assert(writes==1);
        assert(Quality::applyMilliseconds(wall,Source::Mqtt,false,saved));expect("synced_unverified",true);
        assert(std::strcmp(Quality::snapshot().source,"mqtt")==0);
    } else if(mode=="range"){
        const int64_t values[]={-1,0,1577836799999LL,4102444800000LL,INT64_MAX};
        for(int64_t value:values)
            assert(!Quality::applyMilliseconds(value,Source::Http,false,0));
        assert(!Quality::applyMilliseconds(wall,Source::Http,true,0));
        assert(!Quality::applyMilliseconds(wall,Source::Sntp,false,0));assert(writes==0);
    } else if(mode=="write_failure"){
        apply();const auto sequence=Quality::sequence();writesOk=false;
        assert(!Quality::applyMilliseconds(wall+1000,Source::Http,false,sequence));
        assert(Quality::sequence()==sequence);expect("synced_tls",true);
    } else if(mode=="clock_jump"){
        apply();wall+=5001;expect("clock_changed",false);wall-=10002;expect("clock_changed",false);
    } else if(mode=="clock_return"){
        apply();const auto captured=Quality::snapshot();const auto original=wall;
        wall+=5001;expect("clock_changed",false);wall=original;
        expect("clock_changed",false);assert(!Quality::stillCurrent(captured,1000));
        assert(Quality::sequence()==captured.sequence);apply();expect("synced_tls",true);
        assert(!Quality::stillCurrent(captured,1000));
    } else if(mode=="invalid_return"){
        apply();const auto original=wall;wall=0;expect("invalid",false);
        wall=original;expect("clock_changed",false);
        observer(wall);expect("synced_unverified",true);
    } else if(mode=="monotonic_return"){
        apply();mono+=2000;wall+=2000;expect("synced_tls",true);
        mono-=1000;wall-=1000;expect("clock_changed",false);
        mono+=1000;wall+=1000;expect("clock_changed",false);
        assert(Quality::applyMilliseconds(wall,Source::Http,false,Quality::sequence()));
        expect("synced_unverified",true);
    } else if(mode=="monotonic_before_sync"){
        apply();--mono;--wall;expect("clock_changed",false);
        ++mono;++wall;expect("clock_changed",false);
    } else if(mode=="stale_rollback"){
        apply();mono+=86400001;wall+=86400001;expect("stale",false);
        mono-=86400000;wall-=86400000;expect("clock_changed",false);
        mono+=86400000;wall+=86400000;expect("clock_changed",false);
    } else if(mode=="failed_resync_latched"){
        apply();const auto original=wall;wall+=5001;expect("clock_changed",false);wall=original;
        writesOk=false;assert(!Quality::applyMilliseconds(wall,Source::Http,false,Quality::sequence()));
        expect("clock_changed",false);writesOk=true;
        assert(!Quality::applyMilliseconds(wall,Source::Http,false,Quality::sequence()-1));
        expect("clock_changed",false);apply();expect("synced_tls",true);
    } else if(mode=="drift_tolerance"){
        apply();wall+=5000;expect("synced_tls",true);
        wall-=10000;expect("synced_tls",true);--wall;expect("clock_changed",false);
    } else if(mode=="long_uptime"){
        mono=100000000000ULL;apply();mono+=60000;wall+=60000;
        expect("synced_tls",true);assert(Quality::snapshot().ageMs==60000);
    } else if(mode=="sntp"){
        observer(wall);expect("synced_unverified",true);
        assert(std::strcmp(Quality::snapshot().source,"sntp")==0);
        observer(0);expect("unknown",false);
    } else if(mode=="sntp_race"){
        duringWriteSntp=true;assert(!Quality::applyMilliseconds(wall,Source::Http,false,0));
        expect("unknown",false);duringWriteSntp=false;apply();expect("synced_tls",true);
    } else if(mode=="sntp_nested"){
        apply();duringMonotonicSntp=true;observer(wall);
        expect("unknown",false);expect("unknown",false);
        observer(wall);expect("synced_unverified",true);
    } else if(mode=="apply_capture_race"){
        apply();const auto before=Quality::sequence();duringMonotonicSntp=true;
        assert(!Quality::applyMilliseconds(wall,Source::Http,false,before));
        assert(Quality::sequence()==before+1);expect("unknown",false);
        apply();expect("synced_tls",true);
    } else if(mode=="snapshot_wall_race"){
        apply();duringWallSntp=true;expect("unknown",false);
        expect("unknown",false);observer(wall);expect("synced_unverified",true);
    } else if(mode=="snapshot_mono_race"){
        apply();duringMonotonicSntp=true;expect("unknown",false);
        expect("unknown",false);apply();expect("synced_tls",true);
    } else if(mode=="failed_write_keeps_unknown"){
        apply();observer(0);expect("unknown",false);writesOk=false;
        assert(!Quality::applyMilliseconds(wall,Source::Http,false,Quality::sequence()));
        expect("unknown",false);writesOk=true;apply();expect("synced_tls",true);
    } else if(mode=="monotonic_conversion"){
        using Platform=FastBee::SystemTimePlatform;
        for(const int64_t value:{INT64_MIN,int64_t(-1001),int64_t(-1)}) {
            assert(Platform::monotonicFromMicroseconds(value)==UINT64_MAX);
            assert(!Platform::monotonicAvailable(Platform::monotonicFromMicroseconds(value)));
        }
        assert(Platform::monotonicFromMicroseconds(0)==0);
        assert(Platform::monotonicFromMicroseconds(1999)==1);
        assert(Platform::monotonicFromMicroseconds(INT64_MAX)==Platform::MAX_MONOTONIC_MS);
        assert(Platform::monotonicAvailable(Platform::MAX_MONOTONIC_MS));
        assert(!Platform::monotonicAvailable(Platform::MAX_MONOTONIC_MS+1));
    } else if(mode=="monotonic_zero"){
        mono=0;apply();expect("synced_tls",true);
    } else if(mode=="monotonic_invalid_before_write"){
        apply();const auto captured=Quality::snapshot();mono=UINT64_MAX;
        assert(!Quality::applyMilliseconds(wall,Source::Http,false,Quality::sequence()));
        assert(writes==1);mono=1000;expect("clock_changed",false);
        assert(!Quality::stillCurrent(captured,1000));apply();expect("synced_tls",true);
    } else if(mode=="monotonic_invalid_after_write" || mode=="monotonic_rollback_after_write"){
        apply();const auto before=Quality::sequence();
        invalidateDuringWrite=mode=="monotonic_invalid_after_write";
        rollbackDuringWrite=mode=="monotonic_rollback_after_write";
        assert(!Quality::applyMilliseconds(wall+1000,Source::Http,false,before));
        assert(writes==2 && Quality::sequence()==before+1);mono=2000;
        expect("unknown",false);invalidateDuringWrite=rollbackDuringWrite=false;
        apply();expect("synced_tls",true);
    } else if(mode=="monotonic_invalid_sntp"){
        apply();const auto before=Quality::sequence();mono=UINT64_MAX;
        observer(wall);mono=1000;expect("unknown",false);
        assert(Quality::sequence()==before+1);observer(wall);expect("synced_unverified",true);
    } else if(mode=="monotonic_invalid_snapshot"){
        apply();const auto captured=Quality::snapshot();mono=UINT64_MAX;
        expect("clock_changed",false);mono=1000;expect("clock_changed",false);
        assert(!Quality::stillCurrent(captured,1000));
    } else if(mode=="binding_window"){
        apply();const auto captured=Quality::snapshot();
        assert(Quality::stillCurrent(captured,1000));
        mono+=1000;wall+=1000;assert(Quality::stillCurrent(captured,1000));
        ++mono;++wall;assert(!Quality::stillCurrent(captured,1000));
    } else if(mode=="binding_revision"){
        apply();const auto captured=Quality::snapshot();apply();
        assert(!Quality::stillCurrent(captured,1000));
    } else if(mode=="binding_expiry"){
        apply();mono+=86400000;wall+=86400000;const auto captured=Quality::snapshot();
        ++mono;++wall;assert(!Quality::stillCurrent(captured,1000));
    } else if(mode=="binding_clock"){
        apply();const auto captured=Quality::snapshot();
        --wall;assert(!Quality::stillCurrent(captured,1000));
        wall+=1002;assert(!Quality::stillCurrent(captured,1000));
    } else if(mode=="binding_unknown"){
        const auto captured=Quality::snapshot();apply();
        assert(!Quality::stillCurrent(captured,1000));
    } else assert(false);
}
