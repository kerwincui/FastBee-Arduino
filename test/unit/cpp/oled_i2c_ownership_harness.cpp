#include <Arduino.h>
#include "core/I2CBusManager.h"
#include "core/DisplayOutputTypes.h"
#include "core/SensorConfigEpoch.h"
#include <atomic>
#include <thread>
#define FASTBEE_ENABLE_LCD_GB2312 0
enum class PeripheralType { LCD };
struct PeripheralConfig {
    String id="oled"; PeripheralType type=PeripheralType::LCD;
    uint8_t pinCount=2,pins[2]={21,22};
    struct { struct { uint8_t interface=2,width=128,height=64,contrast=91; } lcd; } params;
};
enum class TextAlign : uint8_t { LEFT=1,CENTER=2,RIGHT=3 };
struct SensorDisplayEntry {
    String id,label,unit; float value=0; uint8_t decimals=1;
    unsigned long timestamp=0; bool valid=true; uint32_t configVersion=0;
};
constexpr uint8_t u8g2_font_5x7_tf[]={0};
constexpr int WL_CONNECTED=1;
struct Wifi { int status() const { return 0; } struct IP { String toString() { return "offline"; } }; IP localIP() { return {}; } } WiFi;
char* dtostrf(double value,signed char,uint8_t decimals,char* out) {
    std::snprintf(out,16,"%.*f",static_cast<int>(decimals),value); return out;
}
static bool displayBusy=false;
namespace FastBee {
struct RuntimeFeatureManager {
    static RuntimeFeatureManager& instance() { static RuntimeFeatureManager value;return value; }
    bool shouldEnable(const char*) { return true; }
};
struct FeatureBudgetRegistry {
    static FeatureBudgetRegistry& instance() { static FeatureBudgetRegistry value;return value; }
    bool isDenied(const char*) { return false; }
};
}

#include <cstdio>
#include <cstdlib>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"CHECK line %d: %s\n",__LINE__,#x); std::exit(86); } } while(false)
#define LOG_WARNINGF(...) ((void)0)
#define LOG_WARNING(...) ((void)0)
#define LOG_INFO(...) ((void)0)
#define LOG_INFOF(...) ((void)0)
struct u8x8_t { uint8_t address=0x78; };
uint8_t u8x8_GetI2CAddress(u8x8_t* u) { return u->address; }
enum { U8X8_MSG_BYTE_INIT, U8X8_MSG_BYTE_SET_DC, U8X8_MSG_BYTE_START_TRANSFER,
       U8X8_MSG_BYTE_SEND, U8X8_MSG_BYTE_END_TRANSFER };
CALLBACKS
#define LCD_GUARD_OR_RETURN(value) if(displayBusy) return value
#define LCD_GUARD_OR_RETURN_VOID() if(displayBusy) return
struct RecursiveMutexGuard {
    RecursiveMutexGuard(void*,unsigned) {}
    bool isLocked() const { return !displayBusy; }
};
struct Display {
    bool failOnce=false, nackOnce=false, unfinished=false, timeoutFirst=false;
    unsigned clears=0,draws=0;
    void clearBuffer() { ++clears; }
    void drawUTF8(uint8_t,uint8_t,const char*) { ++draws; }
    unsigned getUTF8Width(const char* text) { return std::strlen(text)*5; }
    void drawLine(uint8_t,uint8_t,uint8_t,uint8_t) {}
    void setFont(const uint8_t*) {}
    unsigned calls=0;
    void transfer() {
        ++calls;
        if(timeoutFirst) { timeoutFirst=false; g_lcdForegroundStarted=millis()-251UL; }
        u8x8_t u; uint8_t data[2]={0x40,0xff};
        // Like U8g2's void API, keep going after an earlier callback fails.
        for(unsigned i=0;i<2;++i) {
            managedU8g2I2cByte(&u,U8X8_MSG_BYTE_START_TRANSFER,0,nullptr);
            Wire.written=failOnce?0:1; failOnce=false;
            managedU8g2I2cByte(&u,U8X8_MSG_BYTE_SEND,2,data);
            Wire.written=1;
            Wire.txStatus=nackOnce?2:0; nackOnce=false;
            if(!unfinished) managedU8g2I2cByte(&u,U8X8_MSG_BYTE_END_TRANSFER,0,nullptr);
            Wire.txStatus=0;
        }
    }
    void begin() { transfer(); }
    void initDisplay() { transfer(); }
    void sendBuffer() { transfer(); }
    void setPowerSave(bool) { transfer(); }
    void setContrast(uint8_t) { transfer(); }
};
enum class DisplayInterface { I2C_MODE, SPI_MODE };
struct LCDManager {
    int _sdaPin=-1, _sclPin=-1;
    uint8_t _i2cAddress=0x3c;
    bool _busAcquired=false;
    bool _initialized=true, _contentChanged=true, _forceRefresh=false, _foregroundFailed=false;
    String _activeId="oled";
    uint8_t _configuredWidth=128,_configuredHeight=64,_configuredContrast=91;
    uint8_t _width=128,_height=64,_currentFont=1,_currentSensorPage=0;
    std::vector<SensorDisplayEntry> _sensorEntries;
    bool updateDue=true;
    bool matchesConfig(const PeripheralConfig&) const;
    bool identityAdmission(const PeripheralConfig&);
    bool quiescent(const PeripheralConfig&) const;
    FastBee::DisplayWriteResult write(const PeripheralConfig&,const FastBee::LcdOutputRequest&);
    void clear();
    bool print(const String&,uint8_t=0,uint8_t=0,TextAlign=TextAlign::LEFT);
    bool printLine(const String&,uint8_t);
    bool printLines(const String[],uint8_t);
    bool showCustomText(const String&);
    bool showSensorPage(int8_t);
    bool showSystemInfo();
    bool drawLine(uint8_t,uint8_t,uint8_t,uint8_t);
    bool setFont(uint8_t);
    uint8_t getFontHeight() const;
    uint8_t getMaxLines() const;
    uint8_t getSensorPageCount() const;
    unsigned long _lastUpdate=123, _initTime=0, _lastReassert=0, _lastFullRecovery=0;
    uint8_t _contrast=91;
    void* _displayMutex=nullptr;
    Display display;
    Display* _display=&display;
    unsigned long _lastBusWarning=0;
    DisplayInterface _interface=DisplayInterface::I2C_MODE;
    bool probeI2cDisplay(bool logWarning);
    bool shouldUpdate() const { return updateDue; }
    bool refresh();
    bool setContrast(uint8_t contrast);
    bool setDisplayOn(bool on);
    void performMaintenance(const PeripheralConfig&);
    bool initializeCommands() {
        struct { struct { struct { uint8_t contrast=55; } lcd; } params; } config;
        INITIALIZATION
        return true;
    }
    bool admit() { const int sda=21,scl=22; ADMISSION return true; }
    void deinitialize() {
        abortManagedLcdTransfer();
        auto& bus=FastBee::I2CBusManager::instance();
        if(_busAcquired && !bus.release(this,100)) bus.deferRelease(this);
        _busAcquired=false;
        _initialized=false;
        if(g_lcdI2cOwner==this) g_lcdI2cOwner=nullptr;
    }
};
PROBE
HIGH_LEVEL_METHODS
FOREGROUND_METHODS
IDENTITY_ADMISSION
class BusHolder {
    std::atomic<bool> held{false},stop{false}; std::thread worker;
public:
    BusHolder():worker([&] {
        CHECK(FastBee::I2CBusManager::instance().lock(100)); held=true;
        while(!stop.load()) std::this_thread::yield();
        FastBee::I2CBusManager::instance().unlock();
    }) { while(!held.load()) std::this_thread::yield(); }
    ~BusHolder() { stop=true; worker.join(); }
};
int main(int argc,char** argv) {
    CHECK(argc==2);
    const std::string mode=argv[1];
    auto& bus=FastBee::I2CBusManager::instance();
    String error; int other=0; LCDManager lcd; u8x8_t u;
    auto call=[&](uint8_t msg,uint8_t n=0,void* data=nullptr) {
        return managedU8g2I2cByte(&u,msg,n,data);
    };
    if(mode=="collision") {
        CHECK(bus.acquire(&other,21,22,100000,error) && bus.claimAddress(&other,0x3c,error));
        CHECK(!lcd.admit());
        CHECK(Wire.transmissions==0 && bus.clientCount()==1 && bus.ownsAddress(&other,0x3c));
        CHECK(!lcd._busAcquired && !g_lcdI2cOwner);
        CHECK(bus.release(&other)); return 0;
    }
    CHECK(lcd.admit());
    if(mode.rfind("foreground_",0)==0) {
        using Command=FastBee::LcdOutputCommand;
        PeripheralConfig config;
        FastBee::LcdOutputRequest output;
        output.command=Command::CustomText; output.text="# title\nbody";
        const unsigned before=Wire.transmissions;
        if(mode=="foreground_identity") {
            CHECK(lcd.identityAdmission(config));
            config.id="wrong"; CHECK(!lcd.identityAdmission(config)); CHECK(!lcd.write(config,output).acknowledged);
            config.id="oled"; config.pins[1]=23; CHECK(!lcd.write(config,output).acknowledged);
            config.pins[1]=22; ++config.params.lcd.width; CHECK(!lcd.write(config,output).acknowledged);
            CHECK(Wire.transmissions==before);
        } else if(mode=="foreground_busy") {
            displayBusy=true; const auto result=lcd.write(config,output);
            CHECK(!result.acknowledged && !result.attempted && Wire.transmissions==before);
            displayBusy=false;
            { BusHolder holder; const auto busy=lcd.write(config,output);
              CHECK(!busy.acknowledged && !busy.attempted && Wire.transmissions==before); }
        } else if(mode=="foreground_font") {
            output.command=Command::Font; output.text=""; output.value=2;
            const auto result=lcd.write(config,output);
            CHECK(result.acknowledged && !result.attempted && Wire.transmissions==before);
        } else if(mode=="foreground_throttle") {
            lcd.updateDue=false;
            CHECK(lcd.refresh() && Wire.transmissions==before);
            const auto result=lcd.write(config,output);
            CHECK(result.acknowledged && result.attempted && Wire.transmissions==before+3);
            CHECK(!lcd._forceRefresh && !g_lcdForegroundActive);
        } else if(mode=="foreground_commands") {
            lcd._sensorEntries.push_back({});
            for(const auto command:{Command::CustomText,Command::Clear,Command::TextAt,
                    Command::TextLine,Command::SensorPage,Command::SystemInfo}) {
                output.command=command; output.value=0;
                CHECK(lcd.write(config,output).acknowledged);
            }
            output.command=Command::TextLine; output.value=255;
            CHECK(!lcd.write(config,output).attempted);
        } else {
            lcd.display.failOnce=mode=="foreground_short";
            lcd.display.nackOnce=mode=="foreground_nack";
            lcd.display.timeoutFirst=mode=="foreground_deadline";
            const auto result=lcd.write(config,output);
            CHECK(!result.acknowledged && result.attempted && lcd._foregroundFailed);
            CHECK(lcd.display.clears==2);
            // Probe plus only the first frame chunk; no later chunk is retried.
            CHECK(Wire.transmissions==before+(mode=="foreground_deadline"?1:2));
            const unsigned failed=Wire.transmissions;
            lcd._initTime=millis()-10000UL; lcd._lastReassert=millis()-5000UL;
            lcd.performMaintenance(config);
            CHECK(Wire.transmissions==failed && !g_lcdForegroundActive && !lcd._forceRefresh);
            // Explicit new foreground command permits new transmission.
            CHECK(lcd.write(config,output).acknowledged && !lcd._foregroundFailed);
        }
    } else if(mode.find("_error")!=std::string::npos || mode=="refresh_unfinished") {
        lcd.display.failOnce=true;
        const unsigned long before=lcd._lastUpdate;
        if(mode=="refresh_error" || mode=="refresh_unfinished") {
            lcd.display.unfinished=mode=="refresh_unfinished";
            CHECK(!lcd.refresh());
            CHECK(lcd._contentChanged && lcd._lastUpdate==before);
            lcd.display.unfinished=false;
            CHECK(lcd.refresh() && !lcd._contentChanged);
        } else if(mode=="contrast_error") {
            CHECK(!lcd.setContrast(77) && lcd._contrast==91);
            CHECK(lcd.setContrast(77) && lcd._contrast==77);
        } else if(mode=="power_error") {
            CHECK(!lcd.setDisplayOn(false)); CHECK(lcd.setDisplayOn(false));
        } else if(mode=="initialization_error") {
            CHECK(!lcd.initializeCommands());
            CHECK(!lcd._initialized && !lcd._busAcquired && !bus.clientCount());
            CHECK(lcd.admit()); CHECK(lcd.initializeCommands());
        } else {
            const unsigned long now=millis();
            lcd._initTime=now-(mode=="recovery_error"?40000UL:10000UL);
            lcd._lastReassert=now-5000UL;
            lcd._lastFullRecovery=now-300001UL;
            lcd.performMaintenance(PeripheralConfig{});
            CHECK(lcd._lastUpdate==before && lcd._contentChanged);
            CHECK(lcd.refresh() && !lcd._contentChanged);
        }
    } else if(mode=="reserved") {
        CHECK(bus.ownsAddress(&lcd,0x3c));
        CHECK(bus.acquire(&other,21,22,100000,error));
        CHECK(!bus.claimAddress(&other,0x3c,error));
        CHECK(bus.release(&other));
    } else if(mode=="lost_lease" || mode=="probe_lost") {
        bus.deferRelease(&lcd);
        const unsigned before=Wire.transmissions;
        CHECK(!call(U8X8_MSG_BYTE_INIT));
        CHECK(!call(U8X8_MSG_BYTE_START_TRANSFER));
        CHECK(!lcd.probeI2cDisplay(false) && Wire.transmissions==before);
    } else if(mode=="wrong_address") {
        u.address=0x7a;
        const unsigned before=Wire.transmissions;
        CHECK(!call(U8X8_MSG_BYTE_INIT));
        CHECK(!call(U8X8_MSG_BYTE_START_TRANSFER) && Wire.transmissions==before);
    } else if(mode=="end_without_start") {
        CHECK(!call(U8X8_MSG_BYTE_END_TRANSFER));
    } else {
        CHECK(call(U8X8_MSG_BYTE_INIT));
        CHECK(call(U8X8_MSG_BYTE_START_TRANSFER));
        uint8_t data[2]={0x40,0xff};
        if(mode=="short") {
            Wire.written=0; CHECK(!call(U8X8_MSG_BYTE_SEND,2,data));
            Wire.written=1; CHECK(!call(U8X8_MSG_BYTE_SEND,2,data));
            CHECK(!call(U8X8_MSG_BYTE_END_TRANSFER));
        } else if(mode=="null_payload") {
            CHECK(!call(U8X8_MSG_BYTE_SEND,2,nullptr));
            CHECK(!call(U8X8_MSG_BYTE_END_TRANSFER));
        } else if(mode=="lost_during_transfer") {
            bus.deferRelease(&lcd);
            CHECK(!call(U8X8_MSG_BYTE_SEND,2,data));
            CHECK(!call(U8X8_MSG_BYTE_END_TRANSFER));
        } else {
            CHECK(call(U8X8_MSG_BYTE_SEND,2,data));
            Wire.txStatus=mode=="nack"?2:0;
            CHECK(call(U8X8_MSG_BYTE_END_TRANSFER)==(mode=="normal"));
        }
    }
    CHECK(!g_lcdI2cTransferLocked);
    lcd.deinitialize();
    CHECK(bus.clientCount()==0);
    // Ending a failed transfer cannot poison the next independent client.
    Wire.txStatus=0; Wire.written=1;
    CHECK(bus.acquire(&other,21,22,100000,error));
    CHECK(bus.claimAddress(&other,0x3c,error));
    CHECK(bus.release(&other));
}
