"""Execute the production local-time helper with success/failure conversion."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test_sensor_publication import definition
ROOT = Path(__file__).resolve().parents[2]

class RtcSystemTimeTests(unittest.TestCase):
    def test_utc_marker_never_labels_local_calendar_or_failed_conversion(self):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler: self.skipTest('C++17 compiler required')
        function = definition((ROOT/'src/utils/TimeUtils.cpp').read_text(encoding='utf-8'),
                              'String TimeUtils::formatTime(')
        harness = r'''
#include <Arduino.h>
#include <ctime>
#include <cassert>
static int utcCalls=0,localCalls=0,nowCalls=0;
static bool fail=false,formatFail=false;
static time_t received=0;
static tm* utcCalendar(const time_t* t,tm* out) {
    ++utcCalls; received=*t;
    out->tm_year=126;out->tm_mon=8;out->tm_mday=30;out->tm_hour=23;
    out->tm_min=30;out->tm_sec=45;
    return fail?nullptr:out;
}
static tm* localCalendar(const time_t* t,tm* out) {
    ++localCalls; received=*t;
    out->tm_year=126;out->tm_mon=9;out->tm_mday=1;out->tm_hour=7;
    out->tm_min=30;out->tm_sec=45;out->tm_wday=4;
    return fail?nullptr:out;
}
static size_t formatCalendar(char* b,size_t n,const char* f,const tm* t) {
    if(formatFail) { b[0]='x';return 0; }
    return std::strftime(b,n,f,t);
}
class TimeUtils { public:
    enum TimeFormat { ISO8601,RFC822,HUMAN_READABLE,TIME_ONLY,DATE_ONLY };
    static time_t getTimestamp(){++nowCalls;return 123456;}
    static String formatTime(time_t,TimeFormat=ISO8601);
};
#define gmtime_r utcCalendar
#define localtime_r localCalendar
#define strftime formatCalendar
''' + function + r'''
int main() {
    assert(TimeUtils::formatTime(99)=="2026-09-30T23:30:45Z");
    assert(utcCalls==1 && localCalls==0 && received==99 && nowCalls==0);
    assert(TimeUtils::formatTime(0)=="2026-09-30T23:30:45Z");
    assert(utcCalls==2 && received==123456 && nowCalls==1);
    assert(TimeUtils::formatTime(99,TimeUtils::HUMAN_READABLE)=="2026-10-01 07:30:45");
    assert(TimeUtils::formatTime(99,TimeUtils::TIME_ONLY)=="07:30:45");
    assert(TimeUtils::formatTime(99,TimeUtils::DATE_ONLY)=="2026-10-01");
    assert(TimeUtils::formatTime(99,TimeUtils::RFC822).indexOf("01 Oct 2026 07:30:45")>=0);
    assert(TimeUtils::formatTime(99,static_cast<TimeUtils::TimeFormat>(99))=="2026-10-01 07:30:45");
    assert(utcCalls==2 && localCalls==5);
    fail=true;
    assert(TimeUtils::formatTime(99).isEmpty());
    assert(TimeUtils::formatTime(99,TimeUtils::HUMAN_READABLE).isEmpty());
    fail=false;formatFail=true;
    assert(TimeUtils::formatTime(99).isEmpty());
    assert(TimeUtils::formatTime(99,TimeUtils::DATE_ONLY).isEmpty());
}
'''
        with tempfile.TemporaryDirectory(prefix='rtc-format-time-') as folder:
            source=Path(folder)/'format.cpp'; source.write_text(harness,encoding='utf-8')
            binary=Path(folder)/'format.exe'
            result=subprocess.run([compiler,'-std=c++17','-Wall','-Wextra','-Werror',
                '-I'+str(ROOT/'test/mocks'),str(source),'-o',str(binary)],
                capture_output=True,text=True,timeout=60)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            result=subprocess.run([str(binary)],capture_output=True,text=True,timeout=10)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)

    def test_failed_conversion_returns_no_plausible_calendar(self):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:self.skipTest('C++17 compiler required')
        function = definition((ROOT/'src/utils/TimeUtils.cpp').read_text(encoding='utf-8'),
                              'struct tm TimeUtils::getTimeStruct(')
        harness = r'''
#include <time.h>
#include <cassert>
static bool failConversion=false;
static int nowCalls=0;
static time_t received=0;
static tm* rtcLocaltime(const time_t* input,tm* output) {
    received=*input;
    output->tm_year=124;output->tm_mon=1;output->tm_mday=29;
    return failConversion?nullptr:output;
}
class TimeUtils { public:
    static time_t getTimestamp(){++nowCalls;return 123456;}
    static tm getTimeStruct(time_t timestamp=0);
};
#define localtime_r rtcLocaltime
''' + function + r'''
int main(){
    auto value=TimeUtils::getTimeStruct();
    assert(value.tm_year==124 && received==123456 && nowCalls==1);
    value=TimeUtils::getTimeStruct(3456);
    assert(value.tm_mday==29 && received==3456 && nowCalls==1);
    failConversion=true;value=TimeUtils::getTimeStruct();
    assert(value.tm_year==0 && value.tm_mon==0 && value.tm_mday==0);
    assert(value.tm_hour==0 && value.tm_min==0 && value.tm_sec==0 && value.tm_wday==0);
}
'''
        with tempfile.TemporaryDirectory(prefix='rtc-system-time-') as folder:
            source=Path(folder)/'time.cpp';source.write_text(harness,encoding='utf-8')
            binary=Path(folder)/'time.exe'
            result=subprocess.run([compiler,'-std=c++17','-Wall','-Wextra','-Werror',str(source),'-o',str(binary)],capture_output=True,text=True,timeout=60)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            result=subprocess.run([str(binary)],capture_output=True,text=True,timeout=10)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)

if __name__=='__main__':unittest.main()
