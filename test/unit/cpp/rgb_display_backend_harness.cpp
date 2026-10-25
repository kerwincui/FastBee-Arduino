#include <algorithm>
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include "core/ResourceConsumerIdentity.h"
// Return deterministic diagnostics instead of a Windows CRT assertion dialog.
// Keep every assertion active, including builds that happen to define NDEBUG.
#undef assert
#define assert(expression) do { if (!(expression)) { \
    std::fprintf(stderr, "RGB CHECK line %d: %s\n", __LINE__, #expression); \
    std::exit(86); } } while(false)
#ifndef FASTBEE_RGB_SCAN_TELEMETRY
#define FASTBEE_RGB_SCAN_TELEMETRY 0
#endif
#ifndef FASTBEE_RGB_BUDGET_ONLY
#define FASTBEE_RGB_BUDGET_ONLY 0
#endif
using String = std::string;
// Arduino String's integer constructor is used only for diagnostics.
#define String(value) std::to_string(value)
#include "production_display_config.inc"
enum { OUTPUT=1, INPUT=0, HIGH=1, LOW=0, MALLOC_CAP_SPIRAM=1, MALLOC_CAP_8BIT=2, MALLOC_CAP_INTERNAL=4,
       ESP_OK=0, ESP_FAIL=-1, ESP_ERR_NO_MEM=-2, LCD_CLK_SRC_DEFAULT=0 };
using esp_err_t = int;
#define GPIO_IS_VALID_OUTPUT_GPIO(pin) ((pin) >= 0 && (pin) <= 48)
static unsigned pinModes=0;
void pinMode(int, int) { ++pinModes; }
void digitalWrite(int, int) {}
uint32_t elapsedMs=0;
uint32_t millis() { return elapsedMs; }
void delay(int milliseconds) { elapsedMs+=milliseconds; }
void delayMicroseconds(int) {}
bool ledcAttachChannel(int, int, int, int) { return true; }
int lastBacklightDuty = -1;
bool failBacklightWrite = false;
bool ledcWrite(int, int duty) { if(failBacklightWrite)return false;lastBacklightDuty=duty;return true; }
bool failBacklightDetach=false;
unsigned backlightDetaches=0;
bool ledcDetach(int) { ++backlightDetaches;return !failBacklightDetach; }
size_t psramBytes = 8*1024*1024;
constexpr size_t normalInternalBytes = 96*1024;
constexpr size_t normalInternalLargest = 64*1024;
size_t internalBytes = normalInternalBytes, internalLargest = normalInternalLargest;
size_t heap_caps_get_free_size(int caps) { return caps & MALLOC_CAP_SPIRAM ? psramBytes : internalBytes; }
size_t heap_caps_get_largest_free_block(int caps) { return caps & MALLOC_CAP_SPIRAM ? psramBytes : internalLargest; }
void esp_rom_printf(const char*, ...) {}
int64_t espTimerUs=0;
int64_t esp_timer_get_time() { return espTimerUs; }
#define DRAM_ATTR
#define IRAM_ATTR
using portMUX_TYPE = int;
#define portMUX_INITIALIZER_UNLOCKED 0
void portENTER_CRITICAL(portMUX_TYPE*) {}
void portEXIT_CRITICAL(portMUX_TYPE*) {}
void portENTER_CRITICAL_ISR(portMUX_TYPE*) {}
void portEXIT_CRITICAL_ISR(portMUX_TYPE*) {}
struct TestSerial {
    unsigned calls=0;
    std::string last;
    int printf(const char* format, ...) {
        char buffer[512];
        va_list args;
        va_start(args,format);
        const int length=std::vsnprintf(buffer,sizeof(buffer),format,args);
        va_end(args);
        ++calls;last=buffer;
        return length;
    }
} Serial;
struct TestLogger { void infof(const char*, ...) {} } LOGGER;
struct TwoWire {
    uint8_t registers[256]{};
    uint8_t current=0;
    std::vector<uint8_t> writeBytes;
    std::vector<std::pair<uint8_t,uint8_t>> writes;
    std::vector<uint32_t> writeTimes;
    unsigned writeAttempts=0,failWriteAttempt=0;
    unsigned reads[256]{};
    bool writeOnlyControl=true;
    bool fail=false;
    bool pendingRead=false;
    void beginTransmission(uint8_t) { writeBytes.clear(); }
    size_t write(uint8_t byte) { writeBytes.push_back(byte); return 1; }
    int endTransmission(bool = true) {
        if (fail) return 1;
        current=writeBytes[0];
        if (writeBytes.size()>1) {
            if (++writeAttempts==failWriteAttempt) return 1;
            registers[current]=writeBytes[1]; writes.emplace_back(current,writeBytes[1]);
            writeTimes.push_back(elapsedMs);
        }
        return 0;
    }
    int requestFrom(uint8_t, uint8_t) { pendingRead=!fail; return fail ? 0 : 1; }
    int available() const { return pendingRead ? 1 : 0; }
    int read() {
        if(!pendingRead) return -1;
        pendingRead=false;
        ++reads[current];
        return writeOnlyControl && (current==2 || current==3) ? 0 : registers[current];
    }
} wire;
namespace FastBee {
class I2CBusManager {
public:
    const void* owner=nullptr;
    bool releaseFails=false, deferred=false;
    uint8_t claimedAddress=0, externalAddress=0;
    static I2CBusManager& instance() { static I2CBusManager instance; return instance; }
    bool acquire(const void* value,int8_t,int8_t,uint32_t,String&,ResourceConsumerRole role) {
        assert(role==ResourceConsumerRole::Display); owner=value; return true;
    }
    bool claimAddress(const void* value,uint8_t address,String& error) {
        if(value!=owner || address==externalAddress) { error="I2C_ADDRESS_CONFLICT"; return false; }
        if(address<8 || address>0x77) { error="I2C_ADDRESS_INVALID"; return false; }
        claimedAddress=address; return true;
    }
    bool ownsAddress(const void* value,uint8_t address) const {
        return value==owner && address==claimedAddress && address!=externalAddress;
    }
    bool release(const void*) { if (releaseFails) return false; owner=nullptr; claimedAddress=0; return true; }
    void deferRelease(const void*) { deferred=true; owner=nullptr; claimedAddress=0; }
    TwoWire* wire(const void* value) { return owner==value ? &::wire : nullptr; }
};
class I2CBusLock { public: explicit I2CBusLock(uint32_t=1000) {} bool locked() const { return true; } };
}
struct esp_lcd_rgb_panel_config_t {
    int clk_src=0, data_width=0,bits_per_pixel=0,num_fbs=0,dma_burst_size=0;
    int hsync_gpio_num=0,vsync_gpio_num=0,de_gpio_num=0,pclk_gpio_num=0,disp_gpio_num=0;
    int data_gpio_nums[16]{};
    size_t bounce_buffer_size_px=0;
    struct { bool fb_in_psram=false, bb_invalidate_cache=false; } flags;
    struct {
        uint32_t pclk_hz=0,h_res=0,v_res=0,hsync_front_porch=0,hsync_pulse_width=0,hsync_back_porch=0;
        uint32_t vsync_front_porch=0,vsync_pulse_width=0,vsync_back_porch=0;
        struct { bool hsync_idle_low=false,vsync_idle_low=false,pclk_active_neg=false; } flags;
    } timings;
};
struct FakePanel {
    uint16_t frame[480*480]{};
    bool swapXY=false, mirrorX=false, mirrorY=false;
};
using esp_lcd_panel_handle_t = FakePanel*;
struct esp_lcd_rgb_panel_event_data_t {};
using RgbEventCallback = bool (*)(esp_lcd_panel_handle_t,
                                  const esp_lcd_rgb_panel_event_data_t*, void*);
struct esp_lcd_rgb_panel_event_callbacks_t {
    RgbEventCallback on_vsync=nullptr;
    RgbEventCallback on_frame_buf_complete=nullptr;
};
#include "screen/RgbRecovery.h"
void* recoveryAttached = nullptr;
bool failRecoveryAttach = false;
bool fastbee_rgb_recovery_attach(void* value, uint32_t) {
    if (failRecoveryAttach) return false;
    assert(!recoveryAttached); recoveryAttached = value; return true;
}
void fastbee_rgb_recovery_detach(void* value) {
    if (recoveryAttached == value) recoveryAttached = nullptr;
}
esp_lcd_rgb_panel_config_t lastConfig;
FakePanel* activePanel=nullptr;
int livePanels=0;
bool failInit=false;
bool failPanelDelete=false;
bool failRotation=false, failDraw=false;
bool failBounce=false, consumeExtraMemory=false;
size_t allocatedBounce=0;
size_t largestBeforeAllocation=0;
bool failGetFrame=false;
int drawCalls=0,newPanelCalls=0,callbackRegistrations=0;
esp_lcd_rgb_panel_event_callbacks_t registeredCallbacks{};
void* registeredCallbackContext=nullptr;
int esp_lcd_new_rgb_panel(const esp_lcd_rgb_panel_config_t* config, esp_lcd_panel_handle_t* panel) {
    ++newPanelCalls;
    lastConfig=*config;
    if(config->bounce_buffer_size_px && failBounce) return ESP_ERR_NO_MEM;
    allocatedBounce=config->bounce_buffer_size_px*4;
    if(allocatedBounce && consumeExtraMemory) allocatedBounce+=10240;
    if(allocatedBounce>internalBytes || allocatedBounce>internalLargest) {
        allocatedBounce=0;
        return ESP_ERR_NO_MEM;
    }
    largestBeforeAllocation=internalLargest;
    internalBytes-=allocatedBounce;
    internalLargest-=allocatedBounce;
    *panel=new FakePanel; activePanel=*panel; ++livePanels; return 0;
}
int esp_lcd_panel_reset(FakePanel*) { return 0; }
int esp_lcd_panel_init(FakePanel*) { return failInit ? -1 : 0; }
int esp_lcd_panel_swap_xy(FakePanel* panel, bool swap) {
    if(failRotation) return -1;
    panel->swapXY=swap;return 0;
}
int esp_lcd_panel_mirror(FakePanel* panel, bool x, bool y) {
    if(failRotation) return -1;
    panel->mirrorX=x;panel->mirrorY=y;return 0;
}
int esp_lcd_panel_del(FakePanel* panel) {
    assert(recoveryAttached != panel);
    if(failPanelDelete)return ESP_FAIL;
    internalBytes+=allocatedBounce;
    if(allocatedBounce) internalLargest=largestBeforeAllocation;
    allocatedBounce=0;
    delete panel; activePanel=nullptr; --livePanels; return 0;
}
int esp_lcd_rgb_panel_register_event_callbacks(
        FakePanel*, const esp_lcd_rgb_panel_event_callbacks_t* callbacks, void* context) {
    ++callbackRegistrations;
    registeredCallbacks=*callbacks;
    registeredCallbackContext=context;
    return ESP_OK;
}
int esp_lcd_rgb_panel_get_frame_buffer(FakePanel* panel,int count,void** frame) {
    assert(count==1);
    if(failGetFrame)return -1;
    *frame=panel->frame;return 0;
}
int esp_lcd_panel_draw_bitmap(FakePanel* panel,int x,int y,int x2,int y2,const void* data) {
    assert(x>=0 && y>=0 && x2<=480 && y2<=480);
    ++drawCalls;
    if(failDraw) return -1;
    if(data==panel->frame) return 0;
    const auto* pixels=static_cast<const uint16_t*>(data);
    // IDF's documented software transform swaps axes before mirroring the
    // physical axes. The backend owns submission shape, not this transform.
    for(int row=y;row<y2;++row) for(int col=x;col<x2;++col) {
        int px=panel->swapXY ? row : col,py=panel->swapXY ? col : row;
        if(panel->mirrorX)px=479-px;
        if(panel->mirrorY)py=479-py;
        panel->frame[py*480+px]=*pixels++;
    }
    return 0;
}
#define private public
#include "RgbDisplayBackend.cpp"
#undef private
#undef String

class DefaultServiceDisplay final : public FastBee::Screen::IDisplayBackend {
public:
    bool begin(const DisplayConfigParams&, String&) override { return true; }
    void end() override {}
    bool ready() const override { return true; }
    FastBee::Screen::DisplayCapabilities capabilities() const override { return {}; }
    bool pushPixels(uint16_t,uint16_t,uint16_t,uint16_t,const uint16_t*,size_t) override { return true; }
    bool fill(uint16_t) override { return true; }
    bool setBrightness(uint8_t) override { return true; }
    bool setSleep(bool) override { return true; }
    bool setBuzzer(bool) override { return true; }
};

DisplayConfigParams configuration() {
    DisplayConfigParams c{};
    std::strcpy(c.backend,"rgb"); std::strcpy(c.driver,"st7701");
    std::strcpy(c.rgbInitProfile,"st7701_type1"); std::strcpy(c.controlBackend,"ch32v003");
    c.width=c.height=480; c.colorDepth=16; c.rgbFrequency=12000000;
    c.hsyncPulseWidth=c.vsyncPulseWidth=8;
    c.cs=42;c.sclk=2;c.mosi=1;c.rst=-1;c.backlight=-1;
    c.controlSda=15;c.controlScl=7;c.i2cFrequency=100000;c.controlAddress=0x24;
    c.controlResetMask=0x0a;c.controlEnableMask=0x20;c.brightness=50;c.backlightActiveHigh=true;
    c.rgbDe=40;c.rgbHsync=38;c.rgbVsync=39;c.rgbPclk=41;
    for(int i=0;i<16;++i)c.rgbDataPins[i]=i+3;
    return c;
}
#if FASTBEE_RGB_BUDGET_ONLY
int main() {
    using namespace FastBee::Screen;
    static_assert(FASTBEE_RGB_REQUIRE_BOUNCE==1,"budget harness requires bounce");
    static_assert(FASTBEE_RGB_BOUNCE_LINES==16 || FASTBEE_RGB_BOUNCE_LINES==32,
                  "budget harness covers 16 and 32 lines");
    constexpr size_t initialBytes=95900;
    constexpr size_t initialLargest=77812;
    internalBytes=initialBytes;
    internalLargest=initialLargest;
    const int callsBefore=newPanelCalls;
    auto config=configuration();
    String error;
    auto display=createRgbDisplayBackend(config,error);
#if FASTBEE_RGB_BOUNCE_LINES == 16
    assert(display && display->begin(config,error));
    assert(error.empty());
    assert(lastConfig.bounce_buffer_size_px==480U*16U);
    assert(allocatedBounce==30720);
    assert(internalBytes==initialBytes-30720);
    assert(internalLargest==initialLargest-30720);
    display->end();
    assert(livePanels==0 && !FastBee::I2CBusManager::instance().owner);
    assert(internalBytes==initialBytes && internalLargest==initialLargest);
    assert(allocatedBounce==0 && newPanelCalls==callsBefore+1);
    std::cout << "RGB budget: required16 accepted\n";
#else
    assert(display && !display->begin(config,error));
    assert(error=="SCREEN_RGB_BOUNCE_MEMORY_REQUIRED");
    assert(livePanels==0 && !FastBee::I2CBusManager::instance().owner);
    assert(internalBytes==initialBytes && internalLargest==initialLargest);
    assert(allocatedBounce==0 && newPanelCalls==callsBefore);
    std::cout << "RGB budget: required32 rejected\n";
#endif
}
#else
int main() {
    using namespace FastBee::Screen;
    auto& manager=FastBee::I2CBusManager::instance();
    // Admission and cleanup must not alter another client's expander address.
    for(const char* controller : {"ch32v003","tca9554"}) {
        auto config=configuration();std::strcpy(config.controlBackend,controller);
        config.controlBuzzerMask=std::strcmp(controller,"ch32v003")==0 ? 0x40 : 0;
        const auto before=wire.writes.size();
        const auto panelCalls=newPanelCalls;
        manager.externalAddress=config.controlAddress;
        String error; auto display=createRgbDisplayBackend(config,error);
        assert(!display->begin(config,error));
        assert(error=="I2C_ADDRESS_CONFLICT" && !display->ready());
        assert(wire.writes.size()==before && newPanelCalls==panelCalls);
        assert(!manager.owner && manager.externalAddress==config.controlAddress);
        manager.externalAddress=0;
    }
    // A bus lease alone is insufficient after ownership loss, including end().
    {
        auto config=configuration();config.controlBuzzerMask=0x40;
        String error;auto display=createRgbDisplayBackend(config,error);
        assert(display->begin(config,error));
        const auto before=wire.writes.size();
        manager.claimedAddress=0;
        assert(!display->setBrightness(10) && !display->setBuzzer(true));
        display->end();
        assert(wire.writes.size()==before && !manager.owner && livePanels==0);
    }
    // Existing display backends need not implement optional housekeeping.
    {
        auto config=configuration();config.backlight=45;config.backlightChannel=3;
        String error;auto display=createRgbDisplayBackend(config,error);
        assert(display->begin(config,error));
        const auto before=pinModes;const int newBefore=newPanelCalls;
        auto* originalPanel=activePanel;
        failBacklightDetach=true;
        assert(!display->stop(error) && error=="SCREEN_BACKLIGHT_DETACH_FAILED: display retained");
        assert(!display->ready() && activePanel==originalPanel && livePanels==1 && manager.owner);
        FastBee::BacklightResourceSnapshot retained;
        display->snapshotBacklightResource(retained);
        assert(retained.available && retained.retained && retained.cleanupFailed && retained.pin==45);
        const auto detachBefore=backlightDetaches;
        for(unsigned i=0;i<1000;++i)display->snapshotBacklightResource(retained);
        assert(backlightDetaches==detachBefore && pinModes==before);
        assert(pinModes==before);
        auto replacement=config;replacement.backlight=46;
        assert(!display->begin(replacement,error));
        assert(newPanelCalls==newBefore && static_cast<RgbDisplayBackend*>(display.get())->_config.backlight==45);
        failBacklightDetach=false;failPanelDelete=true;
        assert(!display->stop(error) && error=="SCREEN_RGB_PANEL_DELETE_FAILED");
        assert(activePanel==originalPanel && livePanels==1 && manager.owner && pinModes==before);
        const auto detached=backlightDetaches;failPanelDelete=false;
        assert(display->stop(error));
        assert(backlightDetaches==detached && !manager.owner && livePanels==0);
        display->snapshotBacklightResource(retained);
        assert(retained.available && !retained.retained && !retained.cleanupFailed && retained.pin==-1);
        assert(display->begin(replacement,error));assert(display->stop(error));
    }
    DefaultServiceDisplay defaultService;
    defaultService.service(1234);
    // Four rotations: asymmetric points establish orientation, bounds and colors.
    for(uint8_t rotation=0;rotation<4;++rotation) {
        auto config=configuration();config.rotation=rotation;
        String error; auto display=createRgbDisplayBackend(config,error);
        assert(display && display->begin(config,error));
        const uint16_t pixels[]={1,2,3,4,5,6};
        drawCalls=0;
        assert(display->pushPixels(7,11,3,2,pixels,6));
        assert(drawCalls==1);
        for(int row=0;row<2;++row)for(int col=0;col<3;++col) {
            int x=7+col,y=11+row,px=x,py=y;
            if(rotation==1){px=479-y;py=x;}
            if(rotation==2){px=479-x;py=479-y;}
            if(rotation==3){px=y;py=479-x;}
            assert(activePanel->frame[py*480+px]==pixels[row*3+col]);
        }
        assert(lastConfig.timings.pclk_hz==config.rgbFrequency && lastConfig.flags.fb_in_psram);
        #if defined(CONFIG_LCD_RGB_ISR_IRAM_SAFE) && CONFIG_LCD_RGB_ISR_IRAM_SAFE && \
            defined(CONFIG_SPIRAM_XIP_FROM_PSRAM) && CONFIG_SPIRAM_XIP_FROM_PSRAM && \
            defined(CONFIG_SPIRAM_FETCH_INSTRUCTIONS) && CONFIG_SPIRAM_FETCH_INSTRUCTIONS && \
            defined(CONFIG_SPIRAM_RODATA) && CONFIG_SPIRAM_RODATA
        assert(lastConfig.bounce_buffer_size_px==480U*FASTBEE_RGB_BOUNCE_LINES);
        #else
        assert(lastConfig.bounce_buffer_size_px==0 && allocatedBounce==0);
        #endif
        assert(!lastConfig.flags.bb_invalidate_cache);
#if FASTBEE_RGB_SCAN_TELEMETRY == 1
        assert(registeredCallbacks.on_vsync && registeredCallbacks.on_frame_buf_complete);
        if(rotation==0) {
            auto* backend=static_cast<RgbDisplayBackend*>(display.get());
            const unsigned reportsBefore=Serial.calls;
            esp_lcd_rgb_panel_event_data_t event{};
            espTimerUs=1000;
            assert(!registeredCallbacks.on_vsync(activePanel,&event,registeredCallbackContext));
            espTimerUs=18000;
            assert(!registeredCallbacks.on_vsync(activePanel,&event,registeredCallbackContext));
            assert(!registeredCallbacks.on_frame_buf_complete(activePanel,&event,registeredCallbackContext));
            display->service(backend->_scanReportedAt+29999U);
            assert(Serial.calls==reportsBefore);
            display->service(backend->_scanReportedAt+30000U);
            assert(Serial.calls==reportsBefore+1);
            assert(Serial.last.find("vsync=2 frames=1")!=std::string::npos);
        }
#else
        assert(callbackRegistrations==0);
#endif
        assert(!display->pushPixels(479,479,3,2,pixels,6));
        assert(drawCalls==1);
        // A renderer-sized stripe stays one submission at every rotation.
        std::vector<uint16_t> stripe(480*16,0x5aa5);
        assert(display->pushPixels(0,32,480,16,stripe.data(),stripe.size()));
        assert(drawCalls==2);
        failDraw=true;
        assert(!display->pushPixels(7,11,3,2,pixels,6));failDraw=false;
        const int beforeFill=drawCalls;
        assert(display->fill(0x1234) && drawCalls==beforeFill+1);
        assert(std::all_of(std::begin(activePanel->frame),std::end(activePanel->frame),
            [](uint16_t color){return color==0x1234;}));
        display->end();assert(livePanels==0 && !manager.owner);
#if FASTBEE_RGB_SCAN_TELEMETRY == 1
        assert(!registeredCallbacks.on_vsync && !registeredCallbacks.on_frame_buf_complete);
#endif
    }
    // Preserve unrelated control bits, and restore brightness after sleep.
    {
        auto config=configuration(); String error;
        wire.registers[2]=0x80; wire.registers[3]=0xc1; wire.registers[4]=0xc1;
        wire.writes.clear();
        auto display=createRgbDisplayBackend(config,error);assert(display->begin(config,error));
        assert(wire.registers[2]==0x2a && wire.registers[3]==0xeb);
        assert(wire.writes[0]==std::make_pair(uint8_t(3),uint8_t(0xe1)));
        assert(wire.writes[1]==std::make_pair(uint8_t(2),uint8_t(0x2a)));
        assert(wire.writes[2]==std::make_pair(uint8_t(3),uint8_t(0xeb)));
        assert(!wire.reads[2] && !wire.reads[3]);
        assert(!(wire.registers[2]&0x04)); // TP_INT is an input
        // A failed write must not poison the source of the next masked write.
        auto* backend=static_cast<RgbDisplayBackend*>(display.get());
        wire.fail=true;assert(!backend->updateRegister(3,0x20,0));
        assert(backend->_ch32Output==0xeb);
        wire.fail=false;assert(backend->updateRegister(3,0x02,0));
        assert(wire.registers[3]==0xe9 && backend->_ch32Output==0xe9);
        assert(backend->updateRegister(3,0x02,0x02) && wire.registers[3]==0xeb);
        assert(wire.registers[5]==140);
        assert(display->setSleep(true) && wire.registers[5]==0);
        assert(display->setSleep(false) && wire.registers[5]==140);
        display->end();assert(wire.registers[3]==0xeb && !manager.owner);
    }
    // CH32 wake emits one final ON command, not another OFF immediately before it.
    {
        auto config=configuration(); String error; config.backlightActiveHigh=false;
        auto display=createRgbDisplayBackend(config,error);assert(display->begin(config,error));
        auto* backend=static_cast<RgbDisplayBackend*>(display.get());
        const size_t sequenceStart=wire.writes.size();
        assert(display->setSleep(true));
        assert(display->setBrightness(49));
        wire.fail=true;
        assert(!display->setSleep(false));
        assert(backend->_sleeping && backend->_brightness==49);
        wire.fail=false;
        assert(display->setSleep(false) && !backend->_sleeping);
        std::vector<std::pair<uint8_t,uint8_t>> pwmWrites;
        for(size_t i=sequenceStart;i<wire.writes.size();++i)
            if(wire.writes[i].first==5)pwmWrites.push_back(wire.writes[i]);
        assert(pwmWrites.size()==2);
        assert(pwmWrites[0]==std::make_pair(uint8_t(5),uint8_t(255)));
        assert(pwmWrites[1]==std::make_pair(uint8_t(5),uint8_t(117)));
        display->end();
    }
    // Mixed CH32 control with GPIO backlight keeps GPIO writes and failures.
    {
        auto config=configuration(); String error;
        config.backlight=45; config.backlightActiveHigh=false;
        auto display=createRgbDisplayBackend(config,error);assert(display->begin(config,error));
        auto* backend=static_cast<RgbDisplayBackend*>(display.get());
        assert(display->setSleep(true) && lastBacklightDuty==255);
        failBacklightWrite=true;
        assert(!display->setBrightness(49));
        assert(backend->_sleeping && backend->_brightness==50);
        failBacklightWrite=false;
        assert(display->setBrightness(49) && lastBacklightDuty==255);
        assert(display->setSleep(false) && lastBacklightDuty==117);
        display->end();
    }
    // CH32 and GPIO PWM obey configured polarity. 0 remains visible; only
    // explicit sleep/end extinguishes it, and wake restores the latest value.
    for(bool activeHigh : {false,true}) for(bool gpio : {false,true}) {
        auto config=configuration();String error;config.backlightActiveHigh=activeHigh;
        if(gpio)config.backlight=45;
        auto display=createRgbDisplayBackend(config,error);assert(display->begin(config,error));
        auto value=[&](){return gpio?lastBacklightDuty:int(wire.registers[5]);};
        auto expected=[&](int bright){return activeHigh?bright:255-bright;};
        assert(display->setBrightness(0)&&value()==expected(26));
        int previous=value();
        for(int level=1;level<=100;++level) {
            assert(display->setBrightness(level));
            assert(activeHigh?value()>=previous:value()<=previous);previous=value();
        }
        assert(value()==expected(255));
        assert(display->setBrightness(30)&&value()==expected(94));
        wire.fail=true;failBacklightWrite=true;
        assert(!display->setSleep(true));
        assert(!display->setBrightness(80));
        wire.fail=false;failBacklightWrite=false;
        assert(display->setSleep(false)&&value()==expected(94));
        assert(display->setSleep(true)&&value()==expected(0));
        assert(display->setBrightness(70)&&value()==expected(0));
        assert(display->setSleep(false)&&value()==expected(186));
        display->end();assert(value()==expected(0));
    }
    // Explicit inactive outputs clear an inherited alarm without dropping
    // power or changing undeclared outputs, including after shutdown.
    {
        auto config=configuration(); config.controlInactiveMask=0x40; String error;
        wire.registers[2]=0x80; wire.registers[3]=0xff; wire.registers[4]=0xff;
        auto display=createRgbDisplayBackend(config,error);assert(display->begin(config,error));
        assert(wire.registers[2]==0x6a && wire.registers[3]==0xbf);
        display->end();assert(wire.registers[3]==0xbf && !manager.owner);
        config.controlInactiveMask=0x20;
        assert(!display->begin(config,error) && error=="SCREEN_RGB_CONTROL_MASK_CONFLICT");
        assert(wire.registers[3]==0xbf && !manager.owner);
    }
    // The optional active-high buzzer is initialized low, uses the shared
    // write-only CH32 shadow, and is cleared again on every shutdown path.
    {
        auto config=configuration();config.controlBuzzerMask=0x40;String error;
        wire.registers[2]=0x80;wire.registers[3]=0xff;wire.registers[4]=0xff;
        auto display=createRgbDisplayBackend(config,error);assert(display->begin(config,error));
        assert(display->capabilities().buzzer);
        assert((wire.registers[2]&0x40)==0x40 && (wire.registers[3]&0x40)==0);
        assert(display->setBuzzer(true) && (wire.registers[3]&0x40));
        auto* backend=static_cast<RgbDisplayBackend*>(display.get());
        const uint8_t asserted=backend->_ch32Output;
        wire.fail=true;assert(!display->setBuzzer(false));wire.fail=false;
        assert(backend->_ch32Output==asserted);
        assert(display->setBuzzer(false) && !(wire.registers[3]&0x40));
        assert(display->setBuzzer(true));
        wire.failWriteAttempt=wire.writeAttempts+1;
        display->end();assert(!(wire.registers[3]&0x40) && !manager.owner);
        wire.failWriteAttempt=0;
        // An MCU-only restart with a stale external ON and no framebuffer
        // memory must still initialize OFF before failing admission.
        wire.registers[3]|=0x40;psramBytes=0;
        assert(!display->begin(config,error) && error=="SCREEN_RGB_PSRAM_REQUIRED");
        assert(!(wire.registers[3]&0x40) && !manager.owner);
        psramBytes=8*1024*1024;
        strcpy(config.controlBackend,"tca9554");
        assert(!display->begin(config,error) &&
               error=="SCREEN_RGB_BUZZER_CONTROL_UNSUPPORTED");
        strcpy(config.controlBackend,"ch32v003");
        config.controlInactiveMask=0x40;
        assert(!display->begin(config,error) &&
               error=="SCREEN_RGB_CONTROL_MASK_CONFLICT");
    }
    // Optional expander INT is low before reset release, stays driven through
    // GT911 sync, then becomes input. Test both direction polarities.
    for(bool ch32 : {true,false}) {
        auto config=configuration();String error;
        strcpy(config.touchBackend,"gt911");config.touchInt=config.touchRst=-1;
        config.controlInactiveMask=0x40;config.controlTouchIntMask=0x04;
        if(!ch32)strcpy(config.controlBackend,"tca9554");
        wire.writeOnlyControl=ch32;wire.registers[4]=0xff;
        wire.registers[1]=wire.registers[3]=0xff;
        wire.writes.clear();wire.writeTimes.clear();elapsedMs=0;
        auto display=createRgbDisplayBackend(config,error);assert(display->begin(config,error));
        const uint8_t outputReg=ch32?3:1,directionReg=ch32?2:3;
        assert(wire.writes.size()>=4);
        assert(wire.writes[0].first==outputReg && (wire.writes[0].second&0x0e)==0);
        assert(wire.writes[1].first==directionReg);
        assert((wire.writes[1].second&0x04)==(ch32?0x04:0));
        assert(wire.writes[2].first==outputReg && (wire.writes[2].second&0x0e)==0x0a);
        assert(wire.writeTimes[2]-wire.writeTimes[1]>=100);
        assert(wire.writes[3].first==directionReg);
        assert((wire.writes[3].second&0x04)==(ch32?0:0x04));
        assert(wire.writeTimes[3]-wire.writeTimes[2]>=50);
        for(const auto& write:wire.writes)if(write.first==outputReg){
            assert((write.second&0x20)==0x20 && !(write.second&0x44));
        }
        assert((wire.registers[outputReg]&0x2a)==0x2a);
        display->end();assert(!manager.owner && livePanels==0);
    }
    wire.writeOnlyControl=true;
    // A direction-release failure must not claim a ready display or update
    // the shadow to input. The next explicit initialization can retry safely.
    {
        auto config=configuration();String error;
        strcpy(config.touchBackend,"gt911");config.touchInt=config.touchRst=-1;
        config.controlInactiveMask=0x40;config.controlTouchIntMask=0x04;
        wire.writeAttempts=0;wire.failWriteAttempt=4;
        auto display=createRgbDisplayBackend(config,error);
        assert(!display->begin(config,error) && error=="SCREEN_RGB_TOUCH_SYNC_FAILED");
        assert(!display->ready() && !manager.owner && livePanels==0);
        assert(static_cast<RgbDisplayBackend*>(display.get())->_ch32Direction==0x6e);
        wire.failWriteAttempt=0;
        assert(display->begin(config,error));display->end();
        config.controlInactiveMask=0x44;
        assert(!display->begin(config,error) && error=="SCREEN_RGB_CONTROL_MASK_CONFLICT");
        config.controlInactiveMask=0x40;config.touchInt=6;
        assert(!display->begin(config,error) && error=="SCREEN_RGB_TOUCH_CONTROL_INVALID");
    }
    // Allocation/init failures return ownership and delete the partial panel.
    {
        auto config=configuration(); String error; failInit=true;
        auto display=createRgbDisplayBackend(config,error);assert(!display->begin(config,error));
        assert(livePanels==0 && !manager.owner);failInit=false;
        failRotation=true;assert(!display->begin(config,error));
        assert(livePanels==0 && !manager.owner);failRotation=false;
        psramBytes=4096;assert(!display->begin(config,error) && !manager.owner);psramBytes=8*1024*1024;
        wire.fail=true;assert(!display->begin(config,error) && !manager.owner);wire.fail=false;
    }
    // TCA direction is active-low, output bits are changed independently.
    {
        auto config=configuration();std::strcpy(config.controlBackend,"tca9554");
        wire.writeOnlyControl=false;
        config.controlBacklightMask=0x04;config.backlightActiveHigh=true;
        wire.registers[1]=0x81;wire.registers[3]=0xff;
        String error;auto display=createRgbDisplayBackend(config,error);assert(display->begin(config,error));
        assert(wire.registers[3]==0xd1 && wire.registers[1]==0xaf);
        display->end();assert(wire.registers[1]==0xab);
        assert(wire.reads[3]>0);wire.writeOnlyControl=true;
    }
    // Deferred lease cleanup is used when a concurrent bus transaction owns lock.
    {
        auto config=configuration();String error;
        auto display=createRgbDisplayBackend(config,error);assert(display->begin(config,error));
        manager.releaseFails=true;display.reset();assert(manager.deferred && !manager.owner && livePanels==0);
        manager.releaseFails=false;
    }
    // Tight total heap and fragmented heap both keep the direct DMA mode.
    for(bool fragmented : {false,true}) {
        internalBytes=fragmented ? 32768 : 25000;
        internalLargest=fragmented ? 7000 : 12288;
        auto config=configuration(); String error;
        auto display=createRgbDisplayBackend(config,error);assert(display->begin(config,error));
        assert(lastConfig.bounce_buffer_size_px==0 && config.rgbFrequency==lastConfig.timings.pclk_hz);
        display->end();assert(livePanels==0);
    }
    internalBytes=normalInternalBytes;internalLargest=normalInternalLargest;
    // Both allocator failure and post-allocation reserve loss fall back safely.
    for(bool allocatorFailure : {false,true}) {
        constexpr size_t requestedBounceBytes=480U*FASTBEE_RGB_BOUNCE_LINES*4U;
        internalBytes=48U*1024U+requestedBounceBytes+1536U;
        internalLargest=20U*1024U+requestedBounceBytes;
        const size_t initialBytes=internalBytes,initialLargest=internalLargest;
        const int panelCallsBefore=newPanelCalls;
        failBounce=allocatorFailure;consumeExtraMemory=!allocatorFailure;
        auto config=configuration(); String error;
        auto display=createRgbDisplayBackend(config,error);assert(display->begin(config,error));
        assert(lastConfig.bounce_buffer_size_px==0 && livePanels==1);
#if defined(CONFIG_LCD_RGB_ISR_IRAM_SAFE) && CONFIG_LCD_RGB_ISR_IRAM_SAFE && \
    defined(CONFIG_SPIRAM_XIP_FROM_PSRAM) && CONFIG_SPIRAM_XIP_FROM_PSRAM && \
    defined(CONFIG_SPIRAM_FETCH_INSTRUCTIONS) && CONFIG_SPIRAM_FETCH_INSTRUCTIONS && \
    defined(CONFIG_SPIRAM_RODATA) && CONFIG_SPIRAM_RODATA
        assert(newPanelCalls==panelCallsBefore+2);
#else
        assert(newPanelCalls==panelCallsBefore+1);
#endif
        failGetFrame=true;assert(!display->fill(0));failGetFrame=false;
        display->end();
        assert(livePanels==0 && internalBytes==initialBytes && internalLargest==initialLargest);
        assert(allocatedBounce==0);
    }
    failBounce=false;consumeExtraMemory=false;
    internalBytes=normalInternalBytes;internalLargest=normalInternalLargest;
    // Recovery attachment failure must unwind the panel and its bus lease.
    {
        failRecoveryAttach=true;
        auto config=configuration(); String error;
        auto display=createRgbDisplayBackend(config,error);
        assert(!display->begin(config,error));
        assert(livePanels==0 && !manager.owner && !recoveryAttached);
        failRecoveryAttach=false;
    }
    assert(internalBytes==normalInternalBytes && internalLargest==normalInternalLargest);
#if FASTBEE_RGB_SCAN_TELEMETRY == 0
    assert(callbackRegistrations==0);
#endif
    std::cout << "RGB backend: lifecycle and memory scenarios passed\n";
}
#endif
