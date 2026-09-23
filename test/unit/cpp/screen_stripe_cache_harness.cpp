#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "screen/StripeChangeCache.h"

using namespace FastBee::Screen;
using String = std::string;
static int allocationAttempt = 0, failAllocation = 0, liveAllocations = 0;
void* allocatePreferred(size_t bytes) {
    if (++allocationAttempt == failAllocation) return nullptr;
    void* result = std::malloc(bytes);
    if (result) ++liveAllocations;
    return result;
}
void heap_caps_free(void* p) { if (p) { --liveAllocations; std::free(p); } }
struct DisplayConfigParams { char backend[8] = "rgb"; uint16_t bufferLines = 16; };
struct DisplayCapabilities { uint16_t width = 480, height = 480; };
struct IDisplayBackend {
    virtual ~IDisplayBackend() = default;
    virtual bool ready() const { return true; }
    virtual DisplayCapabilities capabilities() const { return {}; }
    virtual bool pushPixels(uint16_t, uint16_t, uint16_t, uint16_t, const uint16_t*, size_t) = 0;
};
struct Palette {
    uint16_t background=1, backgroundAlt=2, muted=3, cardBorder=4, accent=5,
             success=6, text=7, danger=8;
};
Palette paletteFor(const char*) { return {}; }
struct ScreenPanelPageSnapshot { char themeId[16] = {}, backgroundPresetId[16] = {}; };
struct ScreenPanelSnapshot {
    uint8_t activePageIndex = 0, pageCount = 1;
    ScreenPanelPageSnapshot pages[1];
    char themeId[16] = {}, backgroundPresetId[16] = {};
};
enum class ScreenCardKind { CONTROL, SYSTEM };
enum class ScreenValueQuality { UNKNOWN, GOOD };
struct ScreenPanelCardSnapshot {
    ScreenCardKind kind = ScreenCardKind::CONTROL;
    ScreenValueQuality quality = ScreenValueQuality::UNKNOWN;
    char systemActionId[17] = {}, title[65] = {};
    char controlRef[32] = {}, sourceRef[32] = {}, value[16] = {}, unit[8] = {},
         weatherCondition[16] = {}, weatherCity[16] = {}, error[32] = {};
    bool pending = false, hasWeatherTemperature = false;
    float weatherTemperature = 0;
};
const char* qualityText(ScreenValueQuality) { return "unknown"; }
struct PixelRect { int16_t x=0, y=0, w=240, h=180; };
static std::vector<uint16_t> desired(480*480, 1);
struct StripeCanvas {
    uint16_t* pixels;
    uint16_t width, row, height;
    std::vector<std::string> labels;
    std::string weatherIcon;
    uint16_t buttonColor = 0;
    StripeCanvas(uint16_t* p, uint16_t w, uint16_t, uint16_t y, uint16_t h)
        : pixels(p), width(w), row(y), height(h) {}
    void clear(uint16_t v) { std::fill_n(pixels, width*height, v); }
    void gradient(uint16_t a, uint16_t) { clear(a); }
    void paint() { std::copy_n(desired.data()+row*width, width*height, pixels); }
    void roundedRect(int,int,int,int,int,uint16_t color,uint16_t) { buttonColor=color; }
    void circle(int,int,int,uint16_t,bool) {}
    void text(int,int,const char* s,uint16_t,int,int) { labels.emplace_back(s); }
};
struct StripeRenderer {
    IDisplayBackend* _display = nullptr;
    uint16_t* _buffer = nullptr;
    StripeChangeCache _stripeCache;
    StripeChangeCache::Entry* _stripeEntries = nullptr;
    uint16_t _width=0, _height=0, _lines=0;
    size_t _bufferBytes=0;
    void drawBackgroundDecoration(StripeCanvas&, const char*, const Palette&) {}
    void drawPage(StripeCanvas& c, const ScreenPanelSnapshot&, const ScreenPanelPageSnapshot&, const Palette&) { c.paint(); }
    void drawEmpty(StripeCanvas& c, const Palette&) { c.paint(); }
    void drawConfirmation(StripeCanvas& c, const ScreenPanelSnapshot&, const char*, const Palette&) { c.paint(); }
    void drawWeatherIcon(StripeCanvas& c,const char* condition,int,int,uint16_t,const Palette&) const { c.weatherIcon=condition; }
    static bool booleanValue(const char* s) { return !std::strcmp(s,"true"); }
#include "production_stripe_renderer.inc"
};
struct Display : IDisplayBackend {
    std::vector<uint16_t> frame = std::vector<uint16_t>(480*480, 0);
    std::vector<unsigned> rows;
    int failRow = -1;
    bool pushPixels(uint16_t x,uint16_t y,uint16_t w,uint16_t h,const uint16_t* p,size_t count) override {
        assert(x==0 && w==480 && y+h<=480 && count==size_t(w)*h);
        rows.push_back(y);
        if (failRow==y) { frame[y*w]=0xffff; return false; }
        std::copy_n(p,count,frame.data()+y*w);
        return true;
    }
};
int main() {
    Display d; StripeRenderer renderer; DisplayConfigParams cfg;
    ScreenPanelSnapshot snapshot; String error;
    assert(renderer.begin(&d,cfg,error));
    assert(renderer._bufferBytes == 480*16*2+30*sizeof(StripeChangeCache::Entry));
    assert(renderer.render(snapshot,"",error) && d.rows.size()==30 && d.frame==desired);
    d.rows.clear();
    assert(renderer.render(snapshot,"",error) && d.rows.empty());
    desired[18*480+20]=12;
    assert(renderer.render(snapshot,"",error) && d.rows==std::vector<unsigned>{16} && d.frame==desired);
    // A failed flush must be retried, including when pixels revert to the last successful frame.
    d.rows.clear(); d.failRow=16; desired[18*480+20]=13;
    assert(!renderer.render(snapshot,"",error) && error=="SCREEN_RENDER_FLUSH_FAILED");
    desired[18*480+20]=12; d.failRow=-1; d.rows.clear();
    assert(renderer.render(snapshot,"",error) && d.rows==std::vector<unsigned>{16} && d.frame==desired);
    // Reinitialize the same object: all metadata invalidated and allocations released.
    d.rows.clear(); assert(renderer.begin(&d,cfg,error) && liveAllocations==2);
    assert(renderer.render(snapshot,"",error) && d.rows.size()==30);
    renderer.end(); assert(liveAllocations==0);
    // Final short stripe and single-line configuration stay inside cache bounds.
    for (uint16_t lines : {uint16_t(31),uint16_t(1)}) {
        cfg.bufferLines=lines; d.rows.clear();
        assert(renderer.begin(&d,cfg,error));
        assert(renderer.render(snapshot,"",error) && d.rows.size()==(480+lines-1U)/lines);
        d.rows.clear(); assert(renderer.render(snapshot,"",error) && d.rows.empty());
        desired.back()++; assert(renderer.render(snapshot,"",error) && d.rows.size()==1 && d.frame==desired);
        renderer.end(); assert(liveAllocations==0);
    }
    cfg.bufferLines=16;
    // Optional metadata allocation failure preserves rendering, not a black screen.
    failAllocation=allocationAttempt+2; d.rows.clear();
    assert(renderer.begin(&d,cfg,error) && !renderer._stripeEntries);
    assert(renderer.render(snapshot,"",error) && renderer.render(snapshot,"",error) && d.rows.size()==60);
    renderer.end(); assert(liveAllocations==0); failAllocation=0;
    std::strcpy(cfg.backend,"spi"); d.rows.clear();
    assert(renderer.begin(&d,cfg,error) && !renderer._stripeEntries);
    assert(renderer.render(snapshot,"",error) && renderer.render(snapshot,"",error) && d.rows.size()==60);
    renderer.end(); assert(liveAllocations==0);
    // Production card methods: defaults are usable placeholders, never fabricated data.
    uint16_t pixels[480] = {}; StripeCanvas canvas(pixels,480,480,0,1);
    ScreenPanelCardSnapshot card; Palette palette; PixelRect rect;
    renderer.drawAction(canvas,card,rect,palette);
    assert(canvas.labels.back()==u8"待配置" && canvas.buttonColor==palette.cardBorder);
    renderer.drawSwitch(canvas,card,rect,palette);
    assert(canvas.labels.back()==u8"待配置" && canvas.buttonColor==palette.cardBorder);
    std::strcpy(card.controlRef,"execution.once");
    renderer.drawAction(canvas,card,rect,palette);
    assert(canvas.labels.back()==u8"执行一次" && canvas.buttonColor==palette.accent);
    card.pending=true; renderer.drawAction(canvas,card,rect,palette);
    assert(canvas.labels.back()==u8"执行中"); card.pending=false;
    canvas.labels.clear(); card.hasWeatherTemperature=true; card.weatherTemperature=23.5;
    renderer.drawWeather(canvas,card,rect,palette);
    assert(canvas.weatherIcon=="cloudy" && canvas.labels.front()==u8"待配置");
    for (const auto& label : canvas.labels) assert(label.find("23.5")==std::string::npos);
    std::strcpy(card.sourceRef,"weather.main"); canvas.labels.clear();
    renderer.drawWeather(canvas,card,rect,palette);
    assert(canvas.labels.front()=="23.5 C");
    std::puts("Stripe renderer scenarios passed");
}
