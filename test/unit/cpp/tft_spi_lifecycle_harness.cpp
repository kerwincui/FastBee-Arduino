#include "screen/ScreenDevice.h"
#include "core/SpiBusInitializer.h"
#include <freertos/semphr.h>
#include <cassert>
#include <atomic>
#include <thread>
#include <esp_heap_caps.h>
using namespace FastBee;
using namespace FastBee::Screen;
namespace FastBee { namespace Screen {
std::unique_ptr<IDisplayBackend> createRgbDisplayBackend(const DisplayConfigParams&, String&) { return nullptr; }
}}

static DisplayConfigParams config(const char* driver = "st7789") {
    PeripheralConfig peripheral;
    initializeDisplayDefaults(peripheral);
    auto result = peripheral.params.display;
    std::strcpy(result.backend, "spi_tft");
    std::strcpy(result.driver, driver);
    result.width=240; result.height=320;
    result.sclk=18; result.mosi=23; result.cs=5; result.dc=16;
    result.rst=17; result.backlight=4; result.brightness=70;
    return result;
}
static void clearTraffic() { spiIoEvents.clear(); SPI.sent.clear(); }
static void allocationAdmission() {
    auto& bus = SpiBusInitializer::instance(); String error; auto cfg = config();
    auto display = createDisplayBackend(cfg,error);
    FastBee::SpiResourceSnapshot snapshot;
    bus.snapshotResources(snapshot);
    assert(snapshot.count == 0 && tftSpiHeapAttempts == 0);
    const int begins = SPI.begins, gpio = spiTestPinModes + spiTestPinWrites;
    tftSpiHeapFail = true;
    assert(!display->begin(cfg,error) && error.startsWith("SPI_CLIENT_STORAGE_UNAVAILABLE"));
    assert(!display->ready() && !bus.isAcquired(display.get()));
    assert(SPI.begins == begins && spiTestPinModes + spiTestPinWrites == gpio);
    assert(tftSpiHeapAttempts == 1 && !tftSpiHeapAllocations && !tftSpiHeapStorage);
    tftSpiHeapFail = false;
    assert(display->begin(cfg,error) && tftSpiHeapAllocations == 1);
    void* leaseStorage = tftSpiHeapStorage;
    const unsigned attempts = tftSpiHeapAttempts;
    tftSpiHeapFail = true;
    assert(!bus.end(error) && error.startsWith("SPI_IN_USE"));
    assert(tftSpiHeapStorage == leaseStorage && tftSpiHeapFrees == 0);
    assert(display->stop(error) && tftSpiHeapStorage == leaseStorage);
    assert(bus.end(error) && !tftSpiHeapStorage && tftSpiHeapFrees == 1);
    const int retiredBegins = SPI.begins, retiredGpio = spiTestPinModes + spiTestPinWrites;
    assert(!display->begin(cfg,error) && error.startsWith("SPI_CLIENT_STORAGE_UNAVAILABLE"));
    assert(tftSpiHeapAttempts == attempts + 1 && !tftSpiHeapStorage);
    assert(SPI.begins == retiredBegins && spiTestPinModes + spiTestPinWrites == retiredGpio);
    tftSpiHeapFail = false;
}

static void failureAndSharing() {
    auto& bus=SpiBusInitializer::instance(); String error; auto cfg=config();
    auto display=createDisplayBackend(cfg,error); int peer=0;
    cfg.width=0; const int gpio=spiTestPinModes;
    assert(!display->begin(cfg,error) && error=="SCREEN_DISPLAY_DIMENSIONS_UNSUPPORTED");
    assert(spiTestPinModes==gpio && !bus.isAcquired(display.get()));
    cfg=config(); spiMutexFailTake=true;
    assert(!display->begin(cfg,error) && error.startsWith("SPI_BUSY"));
    assert(spiTestPinModes==gpio); spiMutexFailTake=false;
    SPI.failAfterAllocation=true;
    assert(!display->begin(cfg,error) && !SPI.active && !bus.isAcquired(display.get()));
    assert(spiTestPinModes==gpio); SPI.failAfterAllocation=false;
    // Input-capable peer first: a write-only TFT must share without changing it.
    assert(bus.acquire(&peer,18,19,23,6,error)); const int ends=SPI.ends;
    cfg.cs=6; assert(!display->begin(cfg,error) && error.startsWith("SPI_CS_CONFLICT"));
    assert(spiTestPinModes==gpio && bus.isAcquired(&peer)); cfg.cs=5;
    tftAttachOk=false;
    assert(!display->begin(cfg,error) && error=="SCREEN_BACKLIGHT_LEDC_INIT_FAILED");
    assert(!bus.isAcquired(display.get()) && bus.isAcquired(&peer));
    assert(SPI.ends==ends && spiTestPinMode[5]==INPUT); tftAttachOk=true;
    tftWriteOk=false;
    assert(!display->begin(cfg,error) && error=="SCREEN_BACKLIGHT_WRITE_FAILED");
    assert(!bus.isAcquired(display.get()) && !display->ready()); tftWriteOk=true;
    assert(display->begin(cfg,error) && display->ready());
    assert(bus.isAcquired(display.get()) && bus.isAcquired(&peer));
    const int starts=SPI.begins, pins=spiTestPinModes, writes=spiTestPinWrites;
    assert(!display->begin(cfg,error) && error.startsWith("SCREEN_DISPLAY_ACTIVE"));
    assert(SPI.begins==starts && spiTestPinModes==pins && spiTestPinWrites==writes);
    assert(!bus.end(error) && error.startsWith("SPI_IN_USE"));
    uint16_t pixels[2]={0x1234,0xabcd};
    clearTraffic(); assert(display->pushPixels(0,0,2,1,pixels,2));
    assert(spiIoEvents.front()=="begin" && spiIoEvents[1]=="low");
    assert(spiIoEvents[spiIoEvents.size()-2]=="high" && spiIoEvents.back()=="end");
    assert(SPI.sent[SPI.sent.size()-4]==0x12 && SPI.sent.back()==0xcd);
    const int transactions=SPI.transactions;
    assert(!display->pushPixels(239,0,2,1,pixels,2));
    assert(!display->pushPixels(0,0,2,1,pixels,1));
    assert(SPI.transactions==transactions);
    spiMutexFailTake=true; const int busyPins=spiTestPinModes, busyWrites=spiTestPinWrites;
    assert(!display->stop(error) && error.startsWith("SPI_BUSY"));
    assert(!display->pushPixels(0,0,2,1,pixels,2));
    assert(!display->fill(0) && !display->setBrightness(80) && !display->setSleep(true));
    assert(spiTestPinModes==busyPins && spiTestPinWrites==busyWrites && SPI.transactions==transactions);
    spiMutexFailTake=false; assert(display->ready() && bus.isAcquired(display.get()));
    tftDetachOk=false; assert(!display->stop(error));
    assert(error.startsWith("SCREEN_BACKLIGHT_DETACH_FAILED") && bus.isAcquired(display.get()));
    FastBee::BacklightResourceSnapshot retained;
    display->snapshotBacklightResource(retained);
    assert(retained.available && retained.retained && retained.cleanupFailed && retained.pin==4);
    assert(spiTestPinMode[5]==OUTPUT); tftDetachOk=true;
    assert(display->stop(error) && !display->ready() && !bus.isAcquired(display.get()));
    display->snapshotBacklightResource(retained);
    assert(retained.available && !retained.retained && !retained.cleanupFailed);
    assert(bus.isAcquired(&peer) && SPI.active && SPI.ends==ends && spiTestPinMode[5]==INPUT);
    assert(display->stop(error));
    assert(bus.release(&peer,error) && bus.end(error));
    // Write-only TFT first: adding MISO later requires a stopped bus, not a
    // silently successful input client with no real MISO attachment.
    assert(display->begin(cfg,error));
    const int firstBegins=SPI.begins;
    assert(!bus.acquire(&peer,18,19,23,6,error));
    assert(error.startsWith("SPI_PINS_CONFLICT") && SPI.begins==firstBegins);
    assert(bus.isAcquired(display.get()) && !bus.isAcquired(&peer));
    assert(display->stop(error) && bus.end(error));
}
static void lostHandleAndControllerFailure() {
    auto& bus=SpiBusInitializer::instance(); String error; auto cfg=config();
    auto display=createDisplayBackend(cfg,error);
    bool dropped=false; tftOnDelay=[&] { if(!dropped){SPI.end(); dropped=true;} };
    assert(!display->begin(cfg,error) && error=="SCREEN_CONTROLLER_INIT_FAILED");
    tftOnDelay={}; assert(!bus.isAcquired(display.get()) && !display->ready());
    assert(display->begin(cfg,error)); SPI.end(); const int gpio=spiTestPinModes+spiTestPinWrites;
    const int transactions=SPI.transactions; uint16_t pixel=0;
    assert(!display->pushPixels(0,0,1,1,&pixel,1));
    assert(!display->fill(0) && !display->setBrightness(50) && !display->setSleep(true));
    assert(SPI.transactions==transactions && spiTestPinModes+spiTestPinWrites==gpio);
    assert(display->stop(error) && !bus.isAcquired(display.get()) && bus.end(error));
    // A failed initialization whose PWM cannot detach retains the exact owner.
    tftWriteOk=false; tftDetachOk=false;
    assert(!display->begin(cfg,error) && bus.isAcquired(display.get()));
    assert(!display->stop(error) && bus.isAcquired(display.get()));
    tftWriteOk=tftDetachOk=true;
    assert(display->stop(error) && !bus.isAcquired(display.get()) && bus.end(error));
}
static void inFlightStopAndRepeatedCycles() {
    auto& bus=SpiBusInitializer::instance(); String error; auto cfg=config("ili9341");
    auto display=createDisplayBackend(cfg,error); assert(display->begin(cfg,error));
    std::atomic<bool> finished{false}; std::thread stopper; bool started=false;
    SPI.onTransfer=[&] {
        if(started) return;
        started=true;
        const int takes=spiMutexTakes;
        stopper=std::thread([&] { String e; assert(display->stop(e)); finished=true; });
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
        while(spiMutexTakes<=takes && std::chrono::steady_clock::now()<deadline) std::this_thread::yield();
        assert(spiMutexTakes>takes && !finished && bus.isAcquired(display.get()));
        assert(spiTestPinMode[5]==OUTPUT);
    };
    uint16_t pixel=0xffff; assert(display->pushPixels(0,0,1,1,&pixel,1));
    stopper.join(); SPI.onTransfer={};
    assert(finished && !bus.isAcquired(display.get()) && !display->ready());
    assert(SPI.transactions==SPI.transactionEnds && bus.end(error));
    cfg.width=2; cfg.height=2;
    for(int cycle=0;cycle<1000;++cycle) {
        clearTraffic(); assert(display->begin(cfg,error));
        assert(display->fill(0x1234)); assert(display->setSleep(true)); assert(display->setSleep(false));
        assert(display->stop(error) && !bus.isAcquired(display.get()));
    }
    assert(SPI.transactions==SPI.transactionEnds && bus.end(error));
    assert(spiMutexCreates-spiMutexDeletes==1);
}
int main() {
    allocationAdmission();
    failureAndSharing(); lostHandleAndControllerFailure(); inFlightStopAndRepeatedCycles();
    assert(tftSpiHeapAllocations == tftSpiHeapFrees && !tftSpiHeapStorage);
}
