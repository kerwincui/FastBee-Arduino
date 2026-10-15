"""Prepared real device-time rendering regressions; no browser/device access."""
from pathlib import Path
import shutil
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = r'''
const fs=require('fs'),vm=require('vm'),assert=require('assert/strict');
const elements={};
const app={registerModule(name,methods){Object.assign(this,methods);this.setupDeviceConfigEvents=undefined;}};
vm.runInNewContext(fs.readFileSync('web-src/modules/runtime/device-config.js','utf8'),{
    AppState:app,window:{},document:{getElementById:id=>elements[id]||(elements[id]={})}});
const sample=()=>({datetime:'2026-10-02 18:00:00',timeDisplayAvailable:true,
    activeTimezoneAvailable:true,activeTimezone:'CST-8',timeQuality:'synced_tls',synced:true,
    timeValid:true,timeSource:'https',timeSourceAuthenticated:true,timeSyncAgeMs:1500,timeSyncSequence:1});
const rtcSample=()=>({...sample(),timeQuality:'rtc_unverified',synced:false,
    timeSource:'rtc',timeSourceAuthenticated:false,rtcBootStatus:'applied'});
const unknown=data=>{app._renderDeviceTime(data);assert(elements['dev-time-synced'].innerHTML.includes('来源未知'));
    assert(!elements['dev-time-synced'].innerHTML.includes('badge-success'));
    assert(!elements['dev-time-synced'].title.includes('距最近同步'));};
const cases={
    rtc_calendar_is_available_without_sync_claim(){
        app._renderDeviceTime(rtcSample());
        assert.equal(elements['dev-time-datetime'].textContent,sample().datetime);
        assert(elements['dev-time-synced'].innerHTML.includes('RTC启动时间（未校准）'));
        assert(!elements['dev-time-synced'].innerHTML.includes('badge-success'));
        assert(elements['dev-time-synced'].title.includes('来源：RTC'));
        assert(elements['dev-time-synced'].title.includes('距启动采用'));
        assert(elements['dev-time-synced'].title.includes('不是RTC校准年龄或精度证明'));
        assert(!elements['dev-time-synced'].title.includes('距最近同步'));
    },
    rtc_cannot_forge_network_sync(){
        unknown({...sample(),timeSource:'rtc'});
        unknown({...sample(),timeSource:'rtc',timeQuality:'synced_unverified',timeSourceAuthenticated:false});
        for(const patch of [{synced:true},{synced:'false'},{timeValid:false},{timeSource:'sntp'},
            {timeSourceAuthenticated:true},{timeSourceAuthenticated:'false'},
            {timeSyncAgeMs:-1},{timeSyncAgeMs:'0'},{timeSyncAgeMs:86400001},
            {timeSyncSequence:0},{timeSyncSequence:'1'},{timeSyncSequence:4294967295},{timeSyncSequence:4294967296}])unknown({...rtcSample(),...patch});
        for(const field of ['synced','timeValid','timeSource','timeSourceAuthenticated','timeSyncAgeMs','timeSyncSequence']){
            const value=rtcSample();delete value[field];unknown(value);
        }
    },
    rtc_superseded_or_invalid_clears_boot_tooltip(){
        app._renderDeviceTime(rtcSample());app._renderDeviceTime(sample());
        assert(elements['dev-time-synced'].innerHTML.includes('badge-success'));
        assert(!elements['dev-time-synced'].title.includes('距启动采用'));
        for(const quality of ['clock_changed','invalid','stale','busy','unknown']){
            app._renderDeviceTime(rtcSample());app._renderDeviceTime({...rtcSample(),timeQuality:quality});
            assert(!elements['dev-time-synced'].title.includes('距启动采用'));
        }
    },
    incomplete_or_contradictory_record(){
        for(const field of ['synced','timeValid','timeSource','timeSourceAuthenticated','timeSyncAgeMs','timeSyncSequence']){
            const value=sample();delete value[field];unknown(value);
        }
        for(const patch of [{synced:false},{synced:'true'},{timeValid:false},{timeValid:1},
            {timeSource:'http'},{timeSource:'sntp'},{timeSource:'mqtt'},{timeSource:'toString'},
            {timeSource:'__proto__'},{timeSource:'<img>'},{timeSourceAuthenticated:false},{timeSourceAuthenticated:'true'},
            {timeQuality:'toString'},{timeQuality:'__proto__'},{timeQuality:'future'},
            {timeQuality:'synced_unverified'}])unknown({...sample(),...patch});
    },
    malformed_age_or_sequence(){
        for(const age of [-1,1.5,'0',null,NaN,Infinity,86400001,Number.MAX_SAFE_INTEGER+1])unknown({...sample(),timeSyncAgeMs:age});
        for(const sequence of [0,-1,1.5,'1',null,NaN,Infinity,4294967295,4294967296])unknown({...sample(),timeSyncSequence:sequence});
    },
    valid_boundaries_and_tooltip(){
        app._renderDeviceTime(sample());assert(elements['dev-time-synced'].title.includes('来源：HTTPS'));
        assert(elements['dev-time-synced'].title.includes('1秒'));
        for(const age of [0,86400000])for(const sequence of [1,4294967294]){
            app._renderDeviceTime({...sample(),timeSyncAgeMs:age,timeSyncSequence:sequence});
            assert(elements['dev-time-synced'].innerHTML.includes('badge-success'));
        }
    },
    unverified_sources(){
        for(const source of ['sntp','http','https','mqtt']){
            app._renderDeviceTime({...sample(),timeQuality:'synced_unverified',timeSource:source,timeSourceAuthenticated:false});
            assert(elements['dev-time-synced'].innerHTML.includes('已同步'));
            assert(elements['dev-time-synced'].innerHTML.includes('badge-warning'));
            assert(elements['dev-time-synced'].title.includes(source.toUpperCase()));
        }
    },
    invalidation_clears_tooltip(){
        for(const quality of ['clock_changed','invalid','stale','busy','unknown']){
            app._renderDeviceTime(sample());app._renderDeviceTime({...sample(),timeQuality:quality,synced:false});
            assert(!elements['dev-time-synced'].innerHTML.includes('badge-success'));
            assert(!elements['dev-time-synced'].title.includes('距最近同步'));
        }
    },
    timezone_label(){app._renderDeviceTime(sample());assert.equal(elements['dev-time-datetime'].title,'当前时区：CST-8');assert.equal(elements['dev-time-datetime'].textContent,sample().datetime);},
    unavailable_clears_previous(){app._renderDeviceTime(sample());app._renderDeviceTime({...sample(),timeDisplayAvailable:false,activeTimezoneAvailable:false});assert.equal(elements['dev-time-datetime'].textContent,'--');assert.equal(elements['dev-time-datetime'].title,'当前时区未确认');},
    legacy_has_unknown_timezone(){app._renderDeviceTime({datetime:'old'});assert.equal(elements['dev-time-datetime'].textContent,'old');assert.equal(elements['dev-time-datetime'].title,'当前时区未确认');assert(elements['dev-time-synced'].innerHTML.includes('来源未知'));},
    invalid_has_no_stale_calendar(){app._renderDeviceTime({...sample(),datetime:null,timeDisplayAvailable:false,timeQuality:'invalid',synced:false});assert.equal(elements['dev-time-datetime'].textContent,'--');assert(elements['dev-time-synced'].innerHTML.includes('系统时间无效'));},
    offline_does_not_erase_fresh_source(){app._renderDeviceTime(sample(),false);assert(elements['dev-time-synced'].innerHTML.includes('已同步'));assert.equal(elements['dev-time-datetime'].textContent,sample().datetime);}
};cases[process.argv[1]]();
'''


class SystemTimeWebTests(unittest.TestCase):
    def check(self, case):
        node=shutil.which('node')
        if not node:
            raise RuntimeError('Node required')
        result=subprocess.run([node,'-e',SCRIPT,case],cwd=ROOT,capture_output=True,
                              text=True,encoding='utf-8',errors='replace',timeout=15)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)


for _case in ('timezone_label','unavailable_clears_previous','legacy_has_unknown_timezone',
              'invalid_has_no_stale_calendar','offline_does_not_erase_fresh_source',
              'incomplete_or_contradictory_record','malformed_age_or_sequence',
              'valid_boundaries_and_tooltip','unverified_sources','invalidation_clears_tooltip',
              'rtc_calendar_is_available_without_sync_claim','rtc_cannot_forge_network_sync',
              'rtc_superseded_or_invalid_clears_boot_tooltip'):
    setattr(SystemTimeWebTests,'test_'+_case,lambda self,case=_case:self.check(case))

if __name__ == '__main__':
    unittest.main()
