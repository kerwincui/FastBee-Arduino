#include <Arduino.h>
#include "core/DailyTriggerState.h"
#include "core/UartEventOrigin.h"
#include <cassert>
#include <climits>
#include <ctime>
#include <set>
#include <thread>
static tm localCalendar{};
static bool localAvailable=true,lateJump=false,lockAllowed=true;
static unsigned localReads=0,lockDepth=0;
static std::atomic<bool> queueEntered{false},releaseQueue{false};
static bool getLocalTime(tm* output,unsigned long) {
    ++localReads;*output=localCalendar;
    if(lateJump && localReads>=2) ++output->tm_min;
    return localAvailable;
}
static void calendar(int year,int month,int day,int hour=6,int minute=0,int second=0) {
    localCalendar={};localCalendar.tm_year=year-1900;localCalendar.tm_mon=month-1;
    localCalendar.tm_mday=day;localCalendar.tm_hour=hour;localCalendar.tm_min=minute;localCalendar.tm_sec=second;
}
#define pdMS_TO_TICKS(x) (x)
constexpr int portMAX_DELAY=-1;
struct MutexGuard {
    explicit MutexGuard(int,int=0) : held(lockAllowed) { if(held) ++lockDepth; }
    ~MutexGuard() { if(held) --lockDepth; }
    bool isLocked() const { return held; }
    bool held;
};
struct Logger { template<class... T> void infof(T...) {} template<class... T> void warningf(T...) {} } LOGGER;
namespace FastBee { namespace MemoryBudget { static size_t available=100000;size_t dramFree() {return available;} } }
namespace FastBee {
bool hasThermostatAction(const PeriphExecRule&) {return false;}
bool hasSdRecorderAction(const PeriphExecRule&) {return false;}
}
struct HealthMonitor { bool isMemoryCritical() const {return false;} };
struct FastBeeFramework {
    static FastBeeFramework* getInstance() {return nullptr;}
    HealthMonitor* getHealthMonitor() const {return nullptr;}
};
struct AsyncExecContextPool { static unsigned poolSize() {return 4;} unsigned usedCount() const {return 0;} };
struct PeriphExecManager {
    std::map<String,PeriphExecRule> rules;
    std::set<String> _runningRuleIds;
    std::map<String,unsigned long> _failureBackoff;
    unsigned long _lastBackoffCleanupTime=0,_lastDiagLogTime=0;
    String _lastTimerDispatchRuleId;
    int _rulesMutex=1,_runningRulesMutex=2;
    AsyncExecContextPool _contextPool;
    std::atomic_flag _timerDispatchBusy=ATOMIC_FLAG_INIT;
    unsigned attempts=0,accepted=0;
    bool reject=false,changeRevision=false,blockQueue=false;
    void checkTimerTriggers(unsigned long,bool);
    bool dispatchByRuleId(const String& id,const String&,String* reason,
        const FastBee::UartEventOrigin& origin,uint64_t revision) {
        assert(lockDepth==0 && origin.unbound());++attempts;
        auto& rule=rules.at(id);
        if(reject || revision!=rule.runtimeRevision) { *reason="queue unavailable";return false; }
        if(blockQueue) {
            queueEntered.store(true,std::memory_order_release);
            while(!releaseQueue.load(std::memory_order_acquire)) std::this_thread::yield();
        }
        ++accepted;
        if(changeRevision) ++rule.runtimeRevision;
        return true;
    }
};
#include "daily_timer_under_test.inc"
static PeriphExecRule daily() {
    PeriphExecRule rule;rule.id="daily";rule.name="Daily";rule.enabled=true;rule.runtimeRevision=9;
    ExecTrigger trigger;trigger.triggerType=static_cast<uint8_t>(ExecTriggerType::TIMER_TRIGGER);
    trigger.timerMode=1;trigger.timePoint="06:00";trigger._cachedHour=6;trigger._cachedMinute=0;
    rule.triggers.push_back(trigger);return rule;
}
int main(int argc,char** argv) {
    assert(argc==2);const std::string mode=argv[1];
    PeriphExecManager manager;manager.rules["daily"]=daily();
    auto& rule=manager.rules.at("daily");
    auto& trigger=rule.triggers[0];
    calendar(2026,10,4);
    if(mode=="same_day") {
        manager.checkTimerTriggers(1000,true);assert(manager.accepted==1);
        manager.checkTimerTriggers(61000,true);manager.checkTimerTriggers(121000,true);
        assert(manager.accepted==1 && trigger.lastCalendarDay==20261004 && trigger.triggerCount==1);
    } else if(mode=="rollback") {
        manager.checkTimerTriggers(1000,true);
        calendar(2026,10,3);manager.checkTimerTriggers(121000,true);
        calendar(2026,10,4,5,59);manager.checkTimerTriggers(181000,true);
        calendar(2026,10,4);manager.checkTimerTriggers(241000,true);
        assert(manager.accepted==1 && trigger.lastCalendarDay==20261004);
    } else if(mode=="advance_day") {
        calendar(2026,12,31);manager.checkTimerTriggers(1000,true);
        calendar(2027,1,1);manager.checkTimerTriggers(121000,true);
        assert(manager.accepted==2 && trigger.lastCalendarDay==20270101);
    } else if(mode=="forward_skip") {
        calendar(2026,10,4,5,59);manager.checkTimerTriggers(1000,true);
        calendar(2026,10,4,6,1);manager.checkTimerTriggers(121000,true);
        assert(manager.accepted==0 && trigger.lastCalendarDay==0);
        calendar(2026,10,5);manager.checkTimerTriggers(241000,true);
        assert(manager.accepted==1 && trigger.lastCalendarDay==20261005);
    } else if(mode=="future_correction") {
        calendar(2099,12,31);manager.checkTimerTriggers(1000,true);
        calendar(2026,10,4);manager.checkTimerTriggers(121000,true);
        calendar(2099,12,31);manager.checkTimerTriggers(241000,true);
        assert(manager.accepted==1 && trigger.lastCalendarDay==20991231);
    } else if(mode=="rejected_retry" || mode=="rejected_window") {
        manager.reject=true;manager.checkTimerTriggers(1000,true);
        assert(manager.attempts==1 && manager.accepted==0 && trigger.lastCalendarDay==0);
        assert(!trigger.triggerCount && !trigger.lastTriggerTime && manager._lastTimerDispatchRuleId.isEmpty());
        manager.reject=false;
        if(mode=="rejected_window") calendar(2026,10,4,6,1);
        manager.checkTimerTriggers(2000,true);
        assert(manager.accepted==(mode=="rejected_retry"?1U:0U));
        assert(trigger.lastCalendarDay==(mode=="rejected_retry"?20261004U:0U));
    } else if(mode=="late_jump") {
        lateJump=true;manager.checkTimerTriggers(1000,true);
        assert(manager.attempts==0 && trigger.lastCalendarDay==0);
        lateJump=false;manager.checkTimerTriggers(2000,true);assert(manager.accepted==1);
    } else if(mode=="invalid_calendar") {
        const tm valid=localCalendar;
        int tm::*members[]={&tm::tm_year,&tm::tm_mon,&tm::tm_mday,&tm::tm_hour,&tm::tm_min,&tm::tm_sec};
        const int invalid[][2]={{INT_MIN,INT_MAX},{-1,12},{0,32},{-1,24},{-1,60},{-1,61}};
        for(unsigned i=0;i<6;++i) for(unsigned j=0;j<2;++j) {
            localCalendar=valid;localCalendar.*members[i]=invalid[i][j];manager.checkTimerTriggers(1000,true);
        }
        calendar(2025,2,29);manager.checkTimerTriggers(1000,true);
        calendar(2100,2,29);manager.checkTimerTriggers(1000,true);
        assert(!manager.attempts && !trigger.lastCalendarDay);
        calendar(2024,2,29);manager.checkTimerTriggers(1000,true);
        assert(manager.accepted==1 && trigger.lastCalendarDay==20240229);
        assert(FastBee::DailyTriggerState::dateKey(tm{})==0);
    } else if(mode=="clock_unavailable") {
        localAvailable=false;manager.checkTimerTriggers(1000,true);
        assert(manager.attempts==0 && !trigger.lastCalendarDay);
        localAvailable=true;manager.checkTimerTriggers(2000,true);assert(manager.accepted==1);
    } else if(mode=="interval") {
        trigger.timerMode=0;trigger.intervalSec=60;localAvailable=false;
        manager.checkTimerTriggers(1000,true);manager.checkTimerTriggers(60000,true);
        calendar(2099,12,31);manager.checkTimerTriggers(61000,true);
        assert(manager.accepted==2 && !trigger.lastCalendarDay && localReads==0);
    } else if(mode=="duplicate") {
        rule.triggers.push_back(trigger);
        manager.checkTimerTriggers(1000,true);manager.checkTimerTriggers(121000,true);
        assert(manager.accepted==1 && rule.triggers[0].lastCalendarDay==20261004);
        assert(rule.triggers[1].lastCalendarDay==20261004);
    } else if(mode=="revision_change") {
        manager.changeRevision=true;manager.checkTimerTriggers(1000,true);
        assert(manager.accepted==1 && trigger.lastCalendarDay==0 && trigger.lastTriggerTime==0);
        manager.changeRevision=false;manager.checkTimerTriggers(2000,true);
        assert(manager.accepted==2 && trigger.lastCalendarDay==20261004);
    } else if(mode=="busy_tick") {
        manager.blockQueue=true;
        std::thread first([&] {manager.checkTimerTriggers(1000,true);});
        while(!queueEntered.load(std::memory_order_acquire)) std::this_thread::yield();
        manager.checkTimerTriggers(121000,true);
        assert(manager.attempts==1 && manager.accepted==0);
        releaseQueue.store(true,std::memory_order_release);first.join();
        manager.blockQueue=false;manager.checkTimerTriggers(241000,true);
        assert(manager.accepted==1 && trigger.lastCalendarDay==20261004);
    } else {
        assert(mode=="guards_release");
        FastBee::MemoryBudget::available=100;manager.checkTimerTriggers(1000,true);
        FastBee::MemoryBudget::available=100000;lockAllowed=false;manager.checkTimerTriggers(1000,true);
        lockAllowed=true;rule._cachedNeedsModbus=true;manager.checkTimerTriggers(1000,false);
        assert(!manager.attempts && !trigger.lastCalendarDay);
        manager.checkTimerTriggers(2000,true);assert(manager.accepted==1);
    }
    assert(lockDepth==0);
}
