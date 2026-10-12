"""Exercise production GT911 register polling, release and orientation."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest
from test.unit.test_panel_runtime_regressions import block

ROOT = Path(__file__).resolve().parents[2]


class Gt911BackendTests(unittest.TestCase):
    def test_production_register_protocol_and_rotation(self):
        compiler = shutil.which("g++") or shutil.which("clang++")
        self.assertIsNotNone(compiler, "C++17 compiler required")
        config = (ROOT / "include/core/PeripheralConfig.h").read_text(encoding="utf-8")
        backend = (ROOT / "include/screen/ScreenBackend.h").read_text(encoding="utf-8")
        source = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <memory>
#include <map>
#include <cstdio>
#include <cstdlib>
#include "core/RuntimeResourceSnapshot.h"
#undef assert
#define assert(condition) do { if (!(condition)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); std::exit(86); } } while (false)
class String : public std::string {
public: using std::string::string; using std::string::operator=;
 bool isEmpty() const {return empty();}
};
#include "config.inc"
enum{INPUT=0,OUTPUT=1,LOW=0,HIGH=1};
void pinMode(uint8_t,int){}
void digitalWrite(uint8_t,int){}
uint32_t now=0;
uint32_t millis(){return now;}
void delay(uint32_t ms){now+=ms;}
struct Logger {template<class...Args> void infof(const char*,Args...){}} LOGGER;
void initConfig();
struct TwoWire {
 uint8_t registers[65536]{};
 uint8_t address=0,present=0x14;
 uint16_t reg=0,readAt=0,failRead=0;
 uint16_t failWrite=0;
 unsigned failWriteCount=0;
 bool failAck=false;
 bool failVerify=false;
 bool coordinateMode=false;
 uint32_t configReadyAt=0;
 size_t remaining=0;
 unsigned clears=0;
 std::vector<uint8_t> sent;
 std::vector<uint16_t> writes;
 void beginTransmission(uint8_t addr){address=addr;sent.clear();}
 size_t write(uint8_t byte){sent.push_back(byte);return 1;}
 size_t write(const uint8_t* data,size_t n){sent.insert(sent.end(),data,data+n);return n;}
 int endTransmission(bool=true){
   if(address!=present)return 2;
   assert(sent.size()>=2);reg=(sent[0]<<8)|sent[1];
   if(sent.size()>2){
     writes.push_back(reg);
     if(failAck)return 4;
     if(reg==failWrite && failWriteCount>0){--failWriteCount;return 4;}
     assert(sent.size()==3);
     assert(reg==0x8040 || reg==0x814e || reg==0x804c || reg==0x80ff || reg==0x8100);
     if(reg==0x8040){assert(sent[2]==0);coordinateMode=true;}
     if(reg==0x814e){assert(sent[2]==0);++clears;}
     registers[reg]=sent[2];
   }
   return 0;
 }
 size_t requestFrom(uint8_t addr,uint8_t count){
   if(addr!=present || reg==failRead || (failVerify && reg==0x804c && !writes.empty())){remaining=0;return 0;}
   if(reg==0x8047 && configReadyAt && now>=configReadyAt){configReadyAt=0;initConfig();}
   readAt=reg;remaining=count;return count;
 }
 size_t available(){return remaining;}
 int read(){assert(remaining);--remaining;return registers[readAt++];}
} wire;
namespace FastBee {
class I2CBusManager {
public:
 std::map<const void*,uint8_t> owners;
 static I2CBusManager& instance(){static I2CBusManager m;return m;}
 bool acquire(const void* p,int,int,uint32_t,String&,ResourceConsumerRole role){
   assert(role==ResourceConsumerRole::Touch);
   owners.emplace(p,0);return true;
 }
 bool claimAddress(const void* p,uint8_t a,String&){owners.at(p)=a;return true;}
 bool ownsAddress(const void* p,uint8_t a){return owners.count(p) && owners.at(p)==a;}
 bool release(const void* p,uint32_t){owners.erase(p);return true;}
 void deferRelease(const void* p){owners.erase(p);}
 TwoWire* wire(const void* p){return owners.count(p)?&::wire:nullptr;}
};
class I2CBusLock {public: explicit I2CBusLock(uint32_t){}bool locked(){return true;}};
}
#include "Gt911TouchBackend.cpp"
void initConfig(){
 // Nonzero vendor fields prove initialization preserves every other byte.
 for(unsigned i=0;i<184;++i)wire.registers[0x8047+i]=i*7+3;
 wire.registers[0x8048]=0xe0;wire.registers[0x8049]=1;
 wire.registers[0x804a]=0xe0;wire.registers[0x804b]=1;
 wire.registers[0x804c]=0xa5;
 uint8_t sum=0;for(unsigned i=0;i<184;++i)sum+=wire.registers[0x8047+i];
 wire.registers[0x80ff]=0-sum;wire.registers[0x8100]=0;
 wire.writes.clear();wire.failRead=wire.failWrite=0;wire.failWriteCount=0;
 wire.coordinateMode=false;
 wire.registers[0x814e]=0x81; // Latched startup data must not become a tap.
}
void report(uint8_t status,uint16_t x=0,uint16_t y=0){
 wire.registers[0x814e]=wire.coordinateMode?status:0;
 wire.registers[0x814f]=3;
 wire.registers[0x8150]=x;wire.registers[0x8151]=x>>8;
 wire.registers[0x8152]=y;wire.registers[0x8153]=y>>8;
 now+=20;
}
int main(){
 using namespace FastBee::Screen;
 memcpy(wire.registers+0x8140,"911",4);
 initConfig();
 uint8_t original[186];memcpy(original,wire.registers+0x8047,sizeof(original));
 DisplayConfigParams c{};strcpy(c.touchBackend,"gt911");
 c.touchSda=15;c.touchScl=7;c.touchRst=c.touchInt=-1;
 c.width=c.height=480;c.i2cFrequency=100000;
 for(int rotation=0;rotation<4;++rotation){
   initConfig();
   c.rotation=rotation;String error;auto t=createTouchBackend(c,error);
   assert(t && t->begin(c,error) && t->ready());
   assert(wire.coordinateMode && wire.registers[0x814e]==0);
   assert(wire.registers[0x804c]==0xa1); // preserve reserved high nibble
   assert(wire.writes.size()>=3 && wire.writes[0]==0x804c && wire.writes[1]==0x80ff && wire.writes[2]==0x8100);
   uint8_t sum=0;for(unsigned i=0;i<185;++i)sum+=wire.registers[0x8047+i];
   assert(sum==0 && wire.registers[0x8100]==1);
   for(unsigned i=0;i<184;++i)if(i!=5)assert(original[i]==wire.registers[0x8047+i]);
   TouchPoint p;
   assert(!t->poll(p)); // Startup residue must not escape as a first tap.
   unsigned before=wire.clears;
   report(1,70,110);assert(!t->poll(p) && wire.clears==before);
   // Feed physical pixels produced by each display rotation; touch must be
   // the inverse transform and return the same logical target (70,110).
   const uint16_t xs[]={70,369,409,110},ys[]={110,70,369,409};
   report(0x81,xs[rotation],ys[rotation]);
   assert(t->poll(p) && p.pressed && p.x==70 && p.y==110 && p.contactId==3);
   report(0);assert(!t->poll(p)); // no new packet is not a release
   report(0x80);assert(t->poll(p) && !p.pressed && p.x==70 && p.y==110);
   report(0x80);assert(!t->poll(p)); // release exactly once
   report(0x86);assert(!t->poll(p));
   report(0x81,200,300);wire.failRead=0x814f;before=wire.clears;
   assert(!t->poll(p) && wire.clears==before);
   wire.failRead=0;assert(t->poll(p) && p.pressed);
   report(0x80);wire.failAck=true;assert(!t->poll(p));
   wire.failAck=false;assert(t->poll(p) && !p.pressed);
   wire.failRead=0x814e;assert(!t->poll(p));wire.failRead=0;
   now+=30000;assert(!t->poll(p)); // bounded diagnostics do not fabricate input
   // Sustained I2C failure relinquishes readiness, never a synthetic tap.
   report(0x81,200,300);assert(t->poll(p) && p.pressed);
   wire.failRead=0x814e;assert(!t->poll(p) && t->ready());
   now+=1999;assert(!t->poll(p) && t->ready());
   ++now;assert(!t->poll(p) && !t->ready());
   wire.failRead=0;assert(!t->poll(p));
   t->end();assert(FastBee::I2CBusManager::instance().owners.empty() && !t->ready());
 }
 wire.present=0x5d;String error;auto t=createTouchBackend(c,error);
 assert(t->begin(c,error));t->end();
 // Power-on ID can be readable before the vendor config has loaded.
 initConfig();memset(wire.registers+0x8047,0,186);
 wire.configReadyAt=now+70;
 assert(t->begin(c,error) && t->ready() && error.empty());
 assert(wire.registers[0x804c]==0xa1 && wire.registers[0x8048]==0xe0);
 assert(wire.coordinateMode && wire.registers[0x814e]==0);
 t->end();
 // An ACK on configuration alone cannot establish coordinate reporting.
 for(uint16_t fail: {0x8040,0x814e}){
   initConfig();wire.failWrite=fail;wire.failWriteCount=1;
   assert(!t->begin(c,error) && !t->ready());
   assert(error==(fail==0x8040?"SCREEN_TOUCH_GT911_MODE_WRITE_FAILED":"SCREEN_TOUCH_GT911_STATUS_CLEAR_FAILED"));
   assert(FastBee::I2CBusManager::instance().owners.empty());
   wire.failWrite=0;
   assert(t->begin(c,error) && error.empty() && t->ready());
   TouchPoint p;
   assert(!t->poll(p));
   report(0x81,90,120);assert(t->poll(p)&&p.pressed);
   report(0x81,90,200);assert(t->poll(p)&&p.pressed); // first-boot drag
   report(0x80);assert(t->poll(p)&&!p.pressed);
   t->end();
 }
 // An incomplete config read must not stage any writes.
 for(unsigned offset=0;offset<186;offset+=32){
   initConfig();wire.failRead=0x8047+offset;
   assert(!t->begin(c,error) && !t->ready());
   assert(error=="SCREEN_TOUCH_GT911_CONFIG_READ_FAILED" && wire.writes.empty());
   assert(FastBee::I2CBusManager::instance().owners.empty());
 }
 initConfig();wire.registers[0x8051]^=1;
 assert(!t->begin(c,error) && error=="SCREEN_TOUCH_GT911_CONFIG_CHECKSUM_INVALID" && wire.writes.empty());
 // A zero-filled configuration sums to zero but is not a usable panel map.
 for(unsigned mutation=0;mutation<4;++mutation){
   initConfig();
   if(mutation<2){
     memset(wire.registers+0x8047,0,186);
     if(mutation==1){wire.registers[0x804c]=1;wire.registers[0x80ff]=0xff;}
   }else{
     wire.registers[0x804c]=mutation==2?0:6;
     uint8_t sum=0;for(unsigned i=0;i<184;++i)sum+=wire.registers[0x8047+i];
     wire.registers[0x80ff]=0-sum;
   }
   assert(!t->begin(c,error) && error=="SCREEN_TOUCH_GT911_CONFIG_INVALID");
   assert(wire.writes.empty() && !t->ready() && FastBee::I2CBusManager::instance().owners.empty());
 }
 // Each failed write refuses activation and restores the original config.
 for(uint16_t fail: {0x804c,0x80ff,0x8100}){
   initConfig();wire.failWrite=fail;wire.failWriteCount=1;
   assert(!t->begin(c,error) && !t->ready());
   assert(error=="SCREEN_TOUCH_GT911_CONFIG_WRITE_FAILED");
   assert(FastBee::I2CBusManager::instance().owners.empty());
   assert(memcmp(original,wire.registers+0x8047,185)==0);
 }
 initConfig();wire.failWrite=0x80ff;wire.failWriteCount=10;
 assert(!t->begin(c,error) && error=="SCREEN_TOUCH_GT911_CONFIG_RESTORE_FAILED");
 assert(!t->ready() && FastBee::I2CBusManager::instance().owners.empty());
 assert(wire.writes.back()!=0x8100); // never apply incomplete restoration
 initConfig();wire.failVerify=true;
 assert(!t->begin(c,error) && error=="SCREEN_TOUCH_GT911_CONFIG_VERIFY_FAILED");
 assert(!t->ready() && FastBee::I2CBusManager::instance().owners.empty());
 wire.failVerify=false;
 initConfig();
 wire.present=0x20;assert(!t->begin(c,error) && error=="SCREEN_TOUCH_GT911_NOT_FOUND");
 assert(FastBee::I2CBusManager::instance().owners.empty());
}
'''
        with tempfile.TemporaryDirectory(prefix="fastbee-gt911-") as directory:
            base = Path(directory)
            (base / "config.inc").write_text("struct DisplayConfigParams " + block(config, "struct DisplayConfigParams") + ";", encoding="utf-8")
            for name in ["Arduino.h", "core/FeatureFlags.h", "core/I2CBusManager.h", "Wire.h", "systems/LoggerSystem.h"]:
                target = base / name
                target.parent.mkdir(exist_ok=True, parents=True)
                target.write_text("#pragma once\n", encoding="utf-8")
            (base / "screen").mkdir()
            (base / "screen/ScreenBackend.h").write_text(backend.replace('#include "core/PeripheralConfig.h"', ''), encoding="utf-8")
            cpp = base / "test.cpp"
            cpp.write_text(source, encoding="utf-8")
            exe = base / ("test.exe" if os.name == "nt" else "test")
            result = subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-DFASTBEE_ENABLE_SCREEN_UI=1", "-DARDUINO_ARCH_ESP32=1", f"-I{base}", f"-I{ROOT / 'include'}", f"-I{ROOT / 'src/screen'}", str(cpp), "-o", str(exe)],
                                    capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=40)
            self.assertEqual(result.returncode, 0,
                             f"compile exit={result.returncode} compiler={compiler}\n{result.stdout or ''}{result.stderr or ''}")
            result = subprocess.run([str(exe)], capture_output=True, text=True,
                                    encoding="utf-8", errors="replace", timeout=10)
            self.assertEqual(result.returncode, 0,
                             f"runtime exit={result.returncode}\n{result.stdout or ''}{result.stderr or ''}")


if __name__ == "__main__":
    unittest.main()
