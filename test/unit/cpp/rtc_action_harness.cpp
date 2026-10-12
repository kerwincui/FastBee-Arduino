#include "core/PeriphExecExecutor.h"
#include "core/RtcAction.h"
#include "core/RtcAccess.h"
#include "systems/LoggerSystem.h"
#include <cassert>
#include <cstring>
#if FASTBEE_ENABLE_DS1302
#include "utils/TimeUtils.h"
#include "utils/SystemTimeQuality.h"
#include "utils/SystemTimeZone.h"
static bool rtcTimeSynced=true;
static bool rtcBootSource=false;
static uint32_t rtcTimeSequence=1;
static unsigned rtcTimeReads=0, rtcTimeFaultAt=0;
static const char* rtcTimeFault="";
FastBee::SystemTimeQuality::Snapshot FastBee::SystemTimeQuality::snapshot() {
    Snapshot result;result.synced=rtcTimeSynced;result.timeValid=rtcTimeSynced;
    if (rtcBootSource) {
        result.timeValid=true;result.synced=false;result.source="rtc";
        result.quality="rtc_unverified";result.transportAuthenticated=false;
    }
    result.sequence=rtcTimeSequence;result.epochMs=1709210096000LL;
    if (++rtcTimeReads == rtcTimeFaultAt) {
        if (std::strcmp(rtcTimeFault,"expired")==0) result.synced=false;
        if (std::strcmp(rtcTimeFault,"delayed")==0) {result.ageMs=1001;result.epochMs+=1001;}
        if (std::strcmp(rtcTimeFault,"jump")==0) result.epochMs-=1;
        if (std::strcmp(rtcTimeFault,"source")==0) ++result.sequence;
    }
    return result;
}
uint32_t FastBee::SystemTimeQuality::sequence() {return rtcTimeSequence;}
#endif
PeriphExecExecutor::PeriphExecExecutor()=default;
PeriphExecExecutor::~PeriphExecExecutor()=default;
static ExecAction action(const char* command="start") {
    ExecAction a; a.actionType=static_cast<uint8_t>(ExecActionType::ACTION_RTC_CONTROL);
    a.targetPeriphId="rtc_01"; a.actionValue=command; a.useReceivedValue=false; return a;
}
static void literalValidation() {
    for(const char* value:{"","START","start ","$value","on","set-datetime","{\"action\":\"start\"}"}) {
        assert(FastBee::validateRtcAction(action(value)));
    }
    auto a=action(); a.targetPeriphId=""; assert(FastBee::validateRtcAction(a));
    a=action(); a.useReceivedValue=true; assert(FastBee::validateRtcAction(a));
#if FASTBEE_ENABLE_DS1302
    assert(!FastBee::validateRtcAction(action()));
    assert(!FastBee::validateRtcAction(action("stop")));
#else
    PeriphExecExecutor e; assert(!e.executeRtcAction(action()));
    assert(rtcLastError=="DS1302_NOT_COMPILED");
#endif
}
static ExecAction readAction() {
    auto a = action("");
    a.actionType = static_cast<uint8_t>(ExecActionType::ACTION_RTC_READ);
    return a;
}
static void readValidation() {
    auto a = readAction();
    PeriphExecExecutor e; ActionExecResult result;
    assert(FastBee::isRtcAction(35) && FastBee::isRtcAction(36));
    assert(FastBee::isRtcAction(37));
    assert(!FastBee::isRtcAction(34) && !FastBee::isRtcAction(38));
    for (const char* value : {"start", "stop", "$value", "{}", " "}) {
        a.actionValue = value; assert(FastBee::validateRtcAction(a));
    }
    a = readAction(); a.targetPeriphId = ""; assert(FastBee::validateRtcAction(a));
    a = readAction(); a.useReceivedValue = true; assert(FastBee::validateRtcAction(a));
    a = readAction();
#if FASTBEE_ENABLE_DS1302
    assert(!FastBee::validateRtcAction(a));
#else
    result.success=true; result.actualValue="old";
    assert(!e.executeRtcReadAction(a,result));
    assert(!result.success && result.actualValue.isEmpty());
    assert(result.remark=="DS1302_NOT_COMPILED");
#endif
    const auto writes = rtcWrites;
    assert(!e.executeRtcAction(readAction()));
    assert(!e.executeRtcReadAction(action(), result));
    assert(rtcWrites == writes);
}
static ExecAction setAction(const char* command = "system-local") {
    auto a=action(command);a.actionType=static_cast<uint8_t>(ExecActionType::ACTION_RTC_SET);return a;
}
static void setValidation() {
    PeriphExecExecutor e;ActionExecResult result;auto a=setAction();
    for(const char* value:{"","start","stop","ntp","SYSTEM-LOCAL"," system-local","$value","2024-02-29"}) {
        assert(FastBee::validateRtcAction(setAction(value)));
        assert(!e.executeRtcSetAction(setAction(value),result));
    }
    a.useReceivedValue=true;assert(FastBee::validateRtcAction(a));
    a=setAction();a.targetPeriphId="";assert(FastBee::validateRtcAction(a));
    assert(!e.executeRtcAction(setAction()));
    assert(!e.executeRtcReadAction(setAction(),result));
    assert(!e.executeRtcSetAction(readAction(),result));
#if FASTBEE_ENABLE_DS1302
    assert(!FastBee::validateRtcAction(setAction()));
#else
    result.success=true;result.actualValue="old";
    assert(!e.executeRtcSetAction(setAction(),result));
    assert(!result.success && result.actualValue.isEmpty() && result.remark=="DS1302_NOT_COMPILED");
#endif
    assert(rtcWrites==0);
}
#if FASTBEE_ENABLE_DS1302
static void setCalendar(const char* mode) {
    auto& rtc=DS1302Driver::getInstance();assert(rtc.begin(5,6,7));
    PeriphExecExecutor e;auto a=setAction();ActionExecResult result;
    rtcIoObserver=[](){
        assert(rtcLockDepth>0);
        FastBee::TimeZoneGuard competingWriter; assert(!competingWriter.held());
    };
    if(std::strcmp(mode,"set-timezone-busy")==0) {
        FastBee::TimeZoneGuard writer;assert(writer.held());const auto writes=rtcWrites;
        assert(!e.executeRtcSetAction(a,result));
        assert(result.remark=="DS1302_TIMEZONE_BUSY" && rtcWrites==writes && rtcBitIndex==0);
    } else if(std::strcmp(mode,"set-expired-before")==0 || std::strcmp(mode,"set-delayed-before")==0 ||
              std::strcmp(mode,"set-jump-before")==0) {
        rtcTimeFaultAt=2;rtcTimeFault=std::strcmp(mode,"set-expired-before")==0?"expired":
            std::strcmp(mode,"set-delayed-before")==0?"delayed":"jump";
        rtcLoad({0x56,0x34,0x12,0x29,0x02,0x04,0x24,0});
        assert(!e.executeRtcSetAction(a,result));
        assert(result.remark=="DS1302_SYSTEM_TIME_CHANGED" && result.actualValue.isEmpty());
        assert(rtcOutputBytes()==std::vector<uint8_t>{0xbf});
    } else if(std::strcmp(mode,"set-untracked-source")==0 || std::strcmp(mode,"set-rtc-source")==0) {
        rtcTimeSynced=false;rtcBootSource=std::strcmp(mode,"set-rtc-source")==0;const auto writes=rtcWrites;
        assert(!e.executeRtcSetAction(a,result));
        assert(result.remark=="DS1302_SYSTEM_TIME_UNSYNCED" && rtcWrites==writes && rtcBitIndex==0);
    } else if(std::strcmp(mode,"set-source-changed")==0) {
        rtcLoad({0x56,0x34,0x12,0x29,0x02,0x04,0x24,0});
        rtcIoObserver=[](){assert(rtcLockDepth>0);++rtcTimeSequence;};
        assert(!e.executeRtcSetAction(a,result));
        assert(result.remark=="DS1302_SYSTEM_TIME_CHANGED");
        assert(rtcOutputBytes()==std::vector<uint8_t>{0xbf});
    } else if(std::strcmp(mode,"set-invalid-source")==0) {
        const struct tm good=rtcSystemTime;
        int tm::*fields[]={&tm::tm_year,&tm::tm_mon,&tm::tm_mday,&tm::tm_hour,&tm::tm_min,&tm::tm_sec,&tm::tm_wday};
        const int bad[][2]={{99,200},{-1,12},{0,32},{-1,24},{-1,60},{-1,60},{-1,7}};
        for(int i=0;i<7;++i)for(int j=0;j<2;++j) {
            rtcSystemTime=good;rtcSystemTime.*fields[i]=bad[i][j];
            const auto writes=rtcWrites;assert(!e.executeRtcSetAction(a,result));
            assert(result.remark=="DS1302_SYSTEM_TIME_INVALID" && rtcWrites==writes && rtcBitIndex==0);
        }
        rtcSystemTime=good;rtcSystemTime.tm_year=123;
        assert(!e.executeRtcSetAction(a,result));assert(result.remark=="DS1302_SYSTEM_TIME_INVALID");
    } else if(std::strcmp(mode,"set-busy")==0) {
        rtcMutexBusy=true;const auto writes=rtcWrites;assert(!e.executeRtcSetAction(a,result));
        assert(result.remark=="DS1302_BUSY" && rtcWrites==writes);rtcMutexBusy=false;
    } else if(std::strcmp(mode,"set-invalid-rtc")==0) {
        rtcLoad({255,255,255,255,255,255,255,255});
        assert(!e.executeRtcSetAction(a,result));assert(result.remark=="DS1302_INVALID_DATA");
        assert(rtcOutputBytes()==std::vector<uint8_t>{0xbf});
    } else {
        const bool paused=std::strcmp(mode,"set-paused")==0;
        const bool mismatch=std::strcmp(mode,"set-mismatch")==0;
        const bool haltMismatch=std::strcmp(mode,"set-halt-mismatch")==0;
        const bool badMode=std::strcmp(mode,"set-mode-mismatch")==0;
        const bool midnight=std::strcmp(mode,"set-midnight")==0;
        const bool sunday=std::strcmp(mode,"set-sunday")==0;
        const bool sourceAfter=std::strcmp(mode,"set-source-after")==0;
        const bool expiredAfter=std::strcmp(mode,"set-expired-after")==0;
        const bool delayedAfter=std::strcmp(mode,"set-delayed-after")==0;
        const bool changedAfter=sourceAfter||expiredAfter||delayedAfter;
        if(changedAfter){rtcTimeFaultAt=3;rtcTimeFault=sourceAfter?"source":expiredAfter?"expired":"delayed";}
        if(midnight){rtcSystemTime.tm_hour=23;rtcSystemTime.tm_min=59;rtcSystemTime.tm_sec=59;}
        if(sunday){rtcSystemTime.tm_mon=2;rtcSystemTime.tm_mday=3;rtcSystemTime.tm_wday=0;}
        const uint8_t afterSeconds=static_cast<uint8_t>(midnight?0:paused||haltMismatch?0xd6:0x56);
        rtcLoad({static_cast<uint8_t>(paused?0x80:0),0,0,0x01,0x01,0x03,0x20,0,
            afterSeconds,static_cast<uint8_t>(midnight?0:mismatch?0x35:0x34),
            static_cast<uint8_t>(midnight?0:badMode?0xb2:0x12),
            static_cast<uint8_t>(midnight?1:sunday?3:0x29),static_cast<uint8_t>(midnight||sunday?3:2),
            static_cast<uint8_t>(midnight?5:sunday?7:4),0x24,0});
        const bool ok=!mismatch&&!haltMismatch&&!badMode&&!changedAfter;
        result.actualValue="old";result.dataField="old";result.success=true;
        assert(e.executeRtcSetAction(a,result)==ok);assert(result.success==ok && result.dataField.isEmpty());
        assert(result.remark==(ok?"rtc_copied_system_local_unverified":changedAfter?
            "DS1302_SOURCE_CHANGED_AFTER_WRITE":"DS1302_WRITE_VERIFY_FAILED"));
        assert(result.actualValue==(ok?(midnight?"2024-03-01 00:00:00":sunday?"2024-03-03 12:34:56":"2024-02-29 12:34:56"):""));
        const auto bytes=rtcOutputBytes();
        assert(bytes.size()==13 && bytes[0]==0xbf && bytes[1]==0x8e && bytes[2]==0 && bytes[3]==0xbe && bytes[12]==0xbf);
        assert(bytes[4]==(midnight?0x59:paused?0xd6:0x56));
        assert(bytes[9]==(sunday?7:4));
        assert(rtcBitIndex==128);
    }
    assert(rtcLockDepth==0);rtcIoObserver=nullptr;rtc.end();
}
static void readCalendar(const char* mode) {
    auto& rtc = DS1302Driver::getInstance(); assert(rtc.begin(5,6,7));
    auto& pm = PeripheralManager::getInstance();
    rtcIoObserver = [](){assert(rtcLockDepth>0);};
    PeriphExecExecutor e; auto a = readAction(); ActionExecResult result;
    auto reset = [&]() { result.success=true; result.actualValue="old"; result.dataField="old_field"; };
    if (std::strcmp(mode, "read-guards")==0) {
        auto reject = [&](const char* reason) {
            reset(); const auto writes=rtcWrites; assert(!e.executeRtcReadAction(a,result));
            assert(rtcWrites==writes && rtcBitIndex==0 && rtcLockDepth==0);
            assert(result.remark==reason && !result.success && result.actualValue.isEmpty());
            assert(result.dataField.isEmpty() && result.targetPeriphId==a.targetPeriphId);
        };
        rtcMutexBusy=true;reject("DS1302_BUSY");rtcMutexBusy=false;
        pm.config.enabled=false;reject("DS1302_NOT_READY");pm.config.enabled=true;
        pm.config.pins[0]=8;reject("DS1302_OWNER_MISMATCH");pm.config.pins[0]=5;
        pm.found=false;reject("DS1302_TARGET_INVALID");pm.found=true;
        a.actionValue="start";reject("DS1302_READ_ACTION_INVALID: select a target and no value");
    } else {
        const bool invalid=std::strcmp(mode,"read-invalid")==0;
        const bool halted=std::strcmp(mode,"read-halted")==0;
        const bool twelve=std::strcmp(mode,"read-12hour")==0;
        for(int i=0;i<100;++i) {
            reset();
            if(invalid) rtcLoad({0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff});
            else rtcLoad({static_cast<uint8_t>(halted?0xd6:0x56),0x34,
                          static_cast<uint8_t>(twelve?0xb2:0x12),0x29,0x02,0x04,0x24,0});
            assert(e.executeRtcReadAction(a,result)==(!invalid && !halted));
            assert(result.dataField.isEmpty() && result.targetPeriphId=="rtc_01");
            assert(result.actualValue==(invalid || halted ? "" : "2024-02-29 12:34:56"));
            assert(result.remark==(invalid?"DS1302_INVALID_DATA":halted?"DS1302_HALTED":"rtc_calendar_unzoned"));
            assert(result.success==(!invalid && !halted));
            assert(rtcBitIndex==64 && rtcOutputBytes()==std::vector<uint8_t>{0xbf});
            assert(rtcLockDepth==0);
        }
    }
    rtcIoObserver=nullptr; rtc.end();
}
static void guards() {
    auto& pm=PeripheralManager::getInstance(); auto& rtc=DS1302Driver::getInstance();
    PeriphExecExecutor e; auto a=action(); assert(rtc.begin(5,6,7));
    auto reject=[&](const char* code) {
        const auto writes=rtcWrites; assert(!e.executeRtcAction(a));
        assert(rtcWrites==writes && rtcBitIndex==0 && rtcLockDepth==0);
        assert(rtcLastError==code);
    };
    rtcMutexBusy=true; reject("DS1302_BUSY"); assert(rtcLockTimeout==pdMS_TO_TICKS(50));rtcMutexBusy=false;
    pm.hasMutex=false;reject("DS1302_BUSY");pm.hasMutex=true;
    pm.found=false;reject("DS1302_TARGET_INVALID");pm.found=true;
    pm.config.type=PeripheralType::GPIO_DIGITAL_OUTPUT;reject("DS1302_TARGET_INVALID");pm.config.type=PeripheralType::DS1302_RTC;
    pm.config.enabled=false;reject("DS1302_NOT_READY");pm.config.enabled=true;
    pm.stateFound=false;reject("DS1302_NOT_READY");pm.stateFound=true;
    pm.state.memorySuspended=true;reject("DS1302_NOT_READY");pm.state.memorySuspended=false;
    for(auto state:{PeripheralStatus::PERIPHERAL_ENABLED,PeripheralStatus::PERIPHERAL_DISABLED,PeripheralStatus::PERIPHERAL_ERROR}) {
        pm.state.status=state;reject("DS1302_NOT_READY");
    }
    pm.state.status=PeripheralStatus::PERIPHERAL_INITIALIZED;
    FastBee::RuntimeFeatureManager::instance().allowed=false;reject("DS1302_NOT_READY");
    FastBee::RuntimeFeatureManager::instance().allowed=true;
    for(int i=0;i<3;++i){auto pin=pm.config.pins[i];pm.config.pins[i]=8;reject("DS1302_OWNER_MISMATCH");pm.config.pins[i]=pin;}
    pm.config.pinCount=2;reject("DS1302_OWNER_MISMATCH");pm.config.pinCount=3;
    a.useReceivedValue=true;reject("DS1302_ACTION_INVALID: select a target and literal start or stop");
    a=action(); rtc.end(); reject("DS1302_NOT_INITIALIZED");
}
static void runControl(const char* mode) {
    auto& rtc=DS1302Driver::getInstance();assert(rtc.begin(5,6,7));
    PeriphExecExecutor e;
    rtcIoObserver=[](){assert(rtcLockDepth>0);};
    if(std::strcmp(mode,"invalid-time")==0) {
        rtcLoad({0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff});
        assert(!e.executeRtcAction(action())); assert(rtcLastError=="DS1302_INVALID_DATA");
        assert(rtcOutputBytes()==std::vector<uint8_t>{0xbf}); // Only the read command.
    } else {
        const bool start=std::strcmp(mode,"stop")!=0;
        const bool unchanged=std::strcmp(mode,"idempotent")==0;
        const bool fail=std::strcmp(mode,"verify-failed")==0;
        uint8_t before=unchanged ? 0x56 : (start?0xd6:0x56);
        uint8_t after=fail?before:(start?0x56:0xd6);
        rtcLoad({before,0x34,0x12,0x29,0x02,0x04,0x24,0, before,before,after});
        assert(e.executeRtcAction(action(start?"start":"stop"))==!fail);
        if(unchanged) {
            assert(rtcBitIndex==72); assert(rtcOutputBytes()==std::vector<uint8_t>({0xbf,0x81}));
        } else {
            assert(rtcBitIndex==88);
            assert(rtcOutputBytes()==std::vector<uint8_t>({0xbf,0x81,0x81,0x80,static_cast<uint8_t>(start?0x56:0xd6),0x81}));
        }
        if(fail)assert(rtcLastError=="DS1302_RUNNING_VERIFY_FAILED");
    }
    assert(rtcLockDepth==0);rtcIoObserver=nullptr;rtc.end();
}
#endif
int main(int argc,char** argv) {
    assert(argc==2);
    if(std::strcmp(argv[1],"validate")==0)literalValidation();
    else if(std::strcmp(argv[1],"read-validate")==0)readValidation();
    else if(std::strcmp(argv[1],"set-validate")==0)setValidation();
#if FASTBEE_ENABLE_DS1302
    else if(std::strcmp(argv[1],"guards")==0)guards();
    else if(std::strncmp(argv[1],"read-",5)==0)readCalendar(argv[1]);
    else if(std::strncmp(argv[1],"set-",4)==0)setCalendar(argv[1]);
    else runControl(argv[1]);
#endif
}
