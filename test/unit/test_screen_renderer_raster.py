"""Compare the real renderer/U8g2 pixels across stripes and unculled code.

Only hardware allocation/display and wall time are substituted. Snapshot,
geometry, cache, palette, glyph, primitive and widget code come from production.
"""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


class ScreenRendererRasterTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        candidates = [Path(os.environ.get("FASTBEE_U8G2_CLIB", "missing")),
                      ROOT / ".pio/libdeps/esp32s3-F16R8/U8g2/src/clib",
                      ROOT.parent / "fb-panel-check-20260917/.pio/libdeps/esp32s3-F16R8/U8g2/src/clib"]
        library = next((p for p in candidates if (p / "u8g2.h").exists()), None)
        compiler = shutil.which("g++")
        fallback = Path("D:/msys64/ucrt64/bin/g++.exe")
        if not compiler and fallback.exists():
            compiler = str(fallback)
        if library is None or compiler is None:
            raise unittest.SkipTest("Requires g++ and installed U8g2 (or FASTBEE_U8G2_CLIB)")

        runtime = (ROOT / "src/screen/ScreenRuntime.cpp").read_text(encoding="utf-8")
        production = runtime[runtime.index("constexpr uint16_t rgb565"):runtime.index("uint32_t hashBytes")]
        # Test visibility only: uint8_t config.bufferLines cannot represent a
        # 480-row reference. Its scratch storage is enlarged after real begin().
        production = production.replace("private:", "public:")
        guards = [
            r"if \(!intersectsRows\(std::min<int32_t>\(y0, y1\),\s*"
            r"std::max<int32_t>\(y0, y1\) \+ 1\)\) return;",
            r"if \(!intersectsRows\(static_cast<int32_t>\(centerY\) - radius,\s*"
            r"static_cast<int32_t>\(centerY\) \+ radius \+ 1\)\) return;",
            r"if \(rect.h >= 96 && !canvas.intersectsRows\(rect.y,\s*"
            r"static_cast<int32_t>\(rect.y\) \+ rect.h\)\) return;",
        ]
        reference = production
        for guard in guards:
            reference, count = re.subn(guard, "", reference)
            if count != 1:
                raise AssertionError("Expected exactly one production culling guard: " + guard)
        # Sensitivity check: dropping overflow from small legacy cards must be
        # caught, even when the same text remains visible in the full frame.
        mutant = production.replace("rect.h >= 96 && !canvas.intersectsRows",
                                    "rect.h >= 8 && !canvas.intersectsRows")
        if mutant == production:
            raise AssertionError("Card culling sensitivity mutation was not applied")
        manager = (ROOT / "include/core/ScreenPanelManager.h").read_text(encoding="utf-8")
        snapshots = manager[manager.index("enum class ScreenCardKind"):manager.index("class ScreenPanelManager")]
        config = (ROOT / "include/core/PeripheralConfig.h").read_text(encoding="utf-8")
        config = config[config.index("struct DisplayConfigParams"):config.index("// 外设配置结构体")]
        backend = (ROOT / "include/screen/ScreenBackend.h").read_text(encoding="utf-8")
        backend = backend[backend.index("struct DisplayCapabilities"):backend.index("class ITouchBackend")]
        sources = "\n".join(
            "#undef FASTBEE_SCREEN_WORKER_ISOLATION\n"
            "#define FASTBEE_SCREEN_WORKER_ISOLATION " + ("1" if name == "diagnostic" else "0") + "\n"
            "namespace " + name + " {\n"
            "time_t time(time_t* p) { return fixedTime(p); }\n"
            "tm* localtime_r(const time_t* p, tm* out) { return fixedLocaltime(p, out); }\n"
            + code + "\n}\n"
            for name, code in (("optimized", production), ("unculled", reference),
                               ("overculled", mutant), ("diagnostic", production)))
        harness = (HARNESS.replace("@CONFIG@", config).replace("@SNAPSHOTS@", snapshots)
                   .replace("@BACKEND@", backend).replace("@PRODUCTION@", sources))
        cls.temporary = tempfile.TemporaryDirectory(prefix="fastbee-renderer-raster-")
        cls.addClassCleanup(cls.temporary.cleanup)
        directory = Path(cls.temporary.name)
        cpp = directory / "raster.cpp"
        cls.executable = directory / ("raster.exe" if os.name == "nt" else "raster")
        cpp.write_text(harness, encoding="utf-8")
        cls.environment = dict(os.environ)
        cls.environment["PATH"] = str(Path(compiler).parent) + os.pathsep + cls.environment.get("PATH", "")
        names = ["u8g2_font.c", "u8g2_fonts.c", "u8g2_setup.c", "u8g2_hvline.c", "u8g2_intersection.c",
                 "u8g2_ll_hvline.c", "u8g2_kerning.c", "u8x8_8x8.c"]
        build = subprocess.run([
            compiler, "-std=c++17", "-O2", "-DU8G2_USE_LARGE_FONTS",
            "-DFASTBEE_SCREEN_WORKER_ISOLATION=0", "-ffunction-sections",
            "-fdata-sections", "-Wl,--gc-sections", f"-I{library}",
            f"-I{ROOT / 'include'}", f"-I{ROOT / 'test/mocks'}", str(cpp),
            *[str(library / name) for name in names], "-o", str(cls.executable)
        ], capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=120,
            env=cls.environment)
        if build.returncode:
            raise AssertionError(build.stdout + build.stderr)

    def run_group(self, group):
        run = subprocess.run([str(self.executable), group], capture_output=True, text=True,
                             encoding="utf-8", errors="replace", timeout=60, env=self.environment)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertIn(group + " raster comparisons passed", run.stdout)

    def test_every_visual_small_cards_and_final_row(self):
        self.run_group("visuals")

    def test_weather_icons_and_confirmation_overlay(self):
        self.run_group("weather")

    def test_rotation_logical_sizes_and_partial_offscreen_cards(self):
        self.run_group("geometry")

    def test_primitive_clipping_preserves_bresenham_and_circle_pixels(self):
        self.run_group("primitives")

    def test_detects_culling_visible_small_card_overflow(self):
        self.run_group("sensitivity")

    def test_diagnostic_phase_labels_are_painted_and_reversible(self):
        self.run_group("labels")

    def test_canonical_high_contrast_and_grid_change_real_pixels(self):
        self.run_group("themes")

    def test_multi_source_bars_render_values_quality_and_update_times(self):
        self.run_group("bars")

    def test_refined_home_and_motor_composition(self):
        self.run_group("preview")


HARNESS = r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <string>
#include <vector>
#include <Arduino.h>
#undef malloc
#undef free
#include <u8g2.h>
#include "screen/ScreenGeometry.h"
#include "screen/ScreenInteraction.h"
#include "screen/StripeChangeCache.h"
#include "screen/ScreenWorkerIsolation.h"
using namespace FastBee::Screen;
using std::isfinite;

// The actual U8g2 glyph engine never needs these hardware callbacks.
extern "C" void u8x8_cad_vsendf(u8x8_t*, const char*, va_list) { std::abort(); }
extern "C" void u8x8_Setup(u8x8_t*, u8x8_msg_cb, u8x8_msg_cb, u8x8_msg_cb, u8x8_msg_cb) { std::abort(); }
extern "C" uint8_t u8x8_d_null_cb(u8x8_t*, uint8_t, uint8_t, void*) { std::abort(); }
extern "C" uint8_t u8x8_cad_empty(u8x8_t*, uint8_t, uint8_t, void*) { std::abort(); }
extern "C" uint8_t u8x8_DrawTile(u8x8_t*, uint8_t, uint8_t, uint8_t, uint8_t*) { std::abort(); }

@CONFIG@
@SNAPSHOTS@
@BACKEND@

static bool clockSynchronized = true;
time_t fixedTime(time_t* result) {
    const time_t value = clockSynchronized ? 1790000000 : 0;
    if (result) *result = value;
    return value;
}
tm* fixedLocaltime(const time_t*, tm* result) {
    *result = {};
    result->tm_year = 126;
    result->tm_mon = 8;
    result->tm_mday = 20;
    result->tm_hour = 12;
    result->tm_min = 34;
    result->tm_sec = 56;
    return result;
}

struct Allocation { unsigned char* bytes; size_t count; };
static std::map<void*, Allocation> allocations;
void checkAllocations() {
    for (const auto& item : allocations) {
        for (size_t i = 0; i < 32; ++i) {
            assert(item.second.bytes[i] == 0xCD);
            assert(item.second.bytes[32 + item.second.count + i] == 0xCD);
        }
    }
}
void* allocatePreferred(size_t size) {
    auto* bytes = static_cast<unsigned char*>(std::malloc(size + 64));
    assert(bytes);
    std::memset(bytes, 0xCD, size + 64);
    void* result = bytes + 32;
    allocations[result] = {bytes, size};
    return result;
}
void heap_caps_free(void* pointer) {
    if (!pointer) return;
    checkAllocations();
    const auto item = allocations.find(pointer);
    assert(item != allocations.end());
    std::free(item->second.bytes);
    allocations.erase(item);
}

@PRODUCTION@

class MemoryDisplay final : public IDisplayBackend {
public:
    static constexpr int Width = 480, Height = 480;
    std::vector<uint16_t> pixels = std::vector<uint16_t>(Width * Height + 2, 0xBEEF);
    std::vector<uint8_t> rows = std::vector<uint8_t>(Height, 0);
    size_t pushes = 0;
    bool begin(const DisplayConfigParams&, String&) override { return true; }
    void end() override {}
    bool ready() const override { return true; }
    DisplayCapabilities capabilities() const override { return {Width, Height}; }
    bool pushPixels(uint16_t x, uint16_t y, uint16_t width, uint16_t height,
                    const uint16_t* source, size_t count) override {
        assert(x == 0 && width == Width && y + height <= Height && height > 0);
        assert(count == size_t(width) * height);
        for (size_t row = y; row < y + height; ++row) ++rows[row];
        std::copy(source, source + count, pixels.begin() + 1 + y * Width);
        ++pushes;
        assert(pixels.front() == 0xBEEF && pixels.back() == 0xBEEF);
        checkAllocations();
        return true;
    }
    bool fill(uint16_t) override { std::abort(); }
    bool setBrightness(uint8_t) override { std::abort(); }
    bool setSleep(bool) override { std::abort(); }
    bool setBuzzer(bool) override { std::abort(); }
};

template<class Renderer>
std::vector<uint16_t> raster(const ScreenPanelSnapshot& snapshot, int lines,
                             const char* confirmation = "") {
    const size_t allocationsBefore = allocations.size();
    MemoryDisplay display;
    {
        DisplayConfigParams config{};
        std::strcpy(config.backend, "rgb");
        config.bufferLines = static_cast<uint8_t>(std::min(lines, 255));
        Renderer renderer;
        String error;
        assert(renderer.begin(&display, config, error));
        if (lines > 255) {
            // Visibility-only test seam: full-frame oracle, same render loop.
            heap_caps_free(renderer._buffer);
            renderer._buffer = static_cast<uint16_t*>(allocatePreferred(480 * lines * sizeof(uint16_t)));
            renderer._lines = static_cast<uint16_t>(lines);
        }
        assert(renderer.render(snapshot, confirmation, error));
        assert(display.pushes == static_cast<size_t>((480 + lines - 1) / lines));
        for (auto count : display.rows) assert(count == 1);
        const auto first = display.pixels;
        const size_t firstPushes = display.pushes;
        assert(renderer.render(snapshot, confirmation, error));
        assert(display.pixels == first && display.pushes == firstPushes);
    }
    assert(allocations.size() == allocationsBefore);
    return {display.pixels.begin() + 1, display.pixels.end() - 1};
}

size_t comparisons = 0;
void equalPixels(const std::vector<uint16_t>& wanted, const std::vector<uint16_t>& actual,
                 const std::string& scenario, int lines) {
    assert(wanted.size() == actual.size());
    for (size_t i = 0; i < wanted.size(); ++i) {
        if (wanted[i] != actual[i]) {
            std::fprintf(stderr, "%s, stripe=%d, x=%zu y=%zu: expected=%04x actual=%04x\n",
                         scenario.c_str(), lines, i % 480, i / 480, wanted[i], actual[i]);
            std::exit(2);
        }
    }
    ++comparisons;
}
void compare(const ScreenPanelSnapshot& snapshot, const std::string& scenario,
             const char* confirmation = "", bool stripedReference = false) {
    const auto wanted = raster<unculled::StripeRenderer>(snapshot, 480, confirmation);
    assert(std::count(wanted.begin(), wanted.end(), wanted.front()) < int(wanted.size()));
    for (int lines : {480, 1, 7, 16, 31}) {
        equalPixels(wanted, raster<optimized::StripeRenderer>(snapshot, lines, confirmation), scenario, lines);
        if (stripedReference)
            equalPixels(wanted, raster<unculled::StripeRenderer>(snapshot, lines, confirmation), scenario + " reference", lines);
    }
}

ScreenPanelSnapshot scene(ScreenVisualType visual, int height = 132, int y = 37) {
    ScreenPanelSnapshot snapshot{};
    snapshot.pageCount = 3;
    snapshot.activePageIndex = 1;
    snapshot.cardCount = 1;
    std::strcpy(snapshot.themeId, "dark");
    std::strcpy(snapshot.backgroundPresetId, "gradient");
    auto& page = snapshot.pages[1];
    std::strcpy(page.id, "main");
    page.cardCount = 1;
    auto& card = snapshot.cards[0];
    std::strcpy(card.id, "target");
    std::strcpy(card.pageId, "main");
    std::strcpy(card.title, u8"温度 / 设备状态");
    std::strcpy(card.sourceRef, "temperature");
    std::strcpy(card.controlRef, "execute-rule");
    std::strcpy(card.targetPageId, "next");
    std::strcpy(card.iconId, "temperature");
    std::strcpy(card.value, "27.5");
    std::strcpy(card.unit, "C");
    card.visualType = visual;
    card.quality = ScreenValueQuality::GOOD;
    card.rect = {37, static_cast<uint16_t>(y), 280, static_cast<uint16_t>(height)};
    card.hasRange = true;
    card.rangeMin = -10;
    card.rangeMax = 60;
    card.pending = true;
    card.interactive = true;
    card.historyCount = 10;
    const double samples[] = {-2, 1, 7, 4, 35, 19, 16, 30, 29, 12};
    for (int i = 0; i < 10; ++i) card.history[i].value = samples[i];
    card.hasWeatherTemperature = true;
    card.weatherTemperature = 24.5f;
    std::strcpy(card.weatherCondition, "thunderstorm");
    std::strcpy(card.weatherCity, u8"天气城市");
    if (visual == ScreenVisualType::SWITCH || visual == ScreenVisualType::ACTION)
        card.kind = ScreenCardKind::CONTROL;
    else if (visual == ScreenVisualType::NAVIGATION) card.kind = ScreenCardKind::PAGE_LINK;
    else if (visual == ScreenVisualType::WEATHER) card.kind = ScreenCardKind::WEATHER;
    else if (visual == ScreenVisualType::CLOCK) card.kind = ScreenCardKind::CLOCK;
    return snapshot;
}

void visuals() {
    for (int visual = 0; visual <= static_cast<int>(ScreenVisualType::SELECT); ++visual) {
        for (int height : {8, 62, 95, 96, 132, 300}) {
            auto snapshot = scene(static_cast<ScreenVisualType>(visual), height);
            const auto name = "visual=" + std::to_string(visual) + ", height=" + std::to_string(height);
            compare(snapshot, name);
            snapshot.cards[0].rect.y = static_cast<uint16_t>(480 - height);
            compare(snapshot, name + " final-row");
        }
    }
    for (auto quality : {ScreenValueQuality::UNKNOWN, ScreenValueQuality::STALE,
                         ScreenValueQuality::ERROR, ScreenValueQuality::OFFLINE}) {
        auto snapshot = scene(ScreenVisualType::STATUS, 96, 95);
        snapshot.cards[0].quality = quality;
        snapshot.cards[0].value[0] = 0;
        compare(snapshot, "status-quality=" + std::to_string(static_cast<int>(quality)));
    }
    clockSynchronized = false;
    compare(scene(ScreenVisualType::CLOCK, 96), "clock-unsynchronized");
    clockSynchronized = true;
    for (int count : {0, 1, 10}) {
        auto snapshot = scene(ScreenVisualType::LINE, 132, 300);
        snapshot.cards[0].historyCount = count;
        for (auto& point : snapshot.cards[0].history) point.value = 7.0;
        compare(snapshot, "line-constant-count=" + std::to_string(count));
    }
}

void weather() {
    for (const char* condition : {"sunny", "clear-night", "cloudy", "overcast", "rain",
                                 "snow", "thunderstorm", "fog", "wind", "cold", "hot", "unknown"}) {
        auto snapshot = scene(ScreenVisualType::WEATHER, 132, 127);
        std::strcpy(snapshot.cards[0].weatherCondition, condition);
        compare(snapshot, condition);
    }
    auto missing = scene(ScreenVisualType::WEATHER, 95, 7);
    missing.cards[0].sourceRef[0] = 0;
    compare(missing, "weather-unconfigured", "", true);
    auto error = scene(ScreenVisualType::WEATHER, 132, 31);
    std::strcpy(error.cards[0].error, u8"天气读取失败");
    error.cards[0].hasWeatherTemperature = false;
    compare(error, "weather-error");
    for (int kind : {0, 1, 2}) {
        auto snapshot = scene(ScreenVisualType::ACTION, 132, 169);
        snapshot.cards[0].kind = kind ? ScreenCardKind::SYSTEM : ScreenCardKind::CONTROL;
        if (kind == 2) std::strcpy(snapshot.cards[0].error, "history_save_failed");
        compare(snapshot, "confirmation=" + std::to_string(kind), "target", true);
    }
}

void geometry() {
    for (uint16_t rotation : {0, 90, 180, 270}) {
        for (bool smaller : {false, true}) {
            auto snapshot = scene(ScreenVisualType::WEATHER, 132, 31);
            auto& page = snapshot.pages[1];
            page.rotation = rotation;
            page.logicalWidth = smaller ? 320 : 480;
            page.logicalHeight = smaller ? 240 : 480;
            std::strcpy(page.backgroundPresetId, rotation == 90 ? "geometry" : "solid");
            std::strcpy(page.themeId, rotation == 180 ? "light" : "accent");
            page.cardCount = snapshot.cardCount = 4;
            snapshot.cards[1] = scene(ScreenVisualType::NAVIGATION, 62).cards[0];
            snapshot.cards[1].rect = {8, 170, 130, 62};
            snapshot.cards[2] = scene(ScreenVisualType::GAUGE, 95).cards[0];
            snapshot.cards[2].rect = {150, 140, 160, 95};
            snapshot.cards[3] = scene(ScreenVisualType::LINE, 8).cards[0];
            snapshot.cards[3].rect = {16, 0, 120, 8};
            compare(snapshot, "rotation=" + std::to_string(rotation) + ", small=" + std::to_string(smaller));
        }
    }
    for (const ScreenPanelRect rect : {ScreenPanelRect{455, 40, 100, 132}, {20, 455, 280, 132},
                                       {465, 472, 100, 96}, {0, 480, 280, 96}, {480, 0, 280, 96}}) {
        auto snapshot = scene(ScreenVisualType::WEATHER);
        snapshot.cards[0].rect = rect;
        compare(snapshot, "partial=" + std::to_string(rect.x) + "," + std::to_string(rect.y));
    }
    auto empty = scene(ScreenVisualType::CLOCK);
    empty.pageCount = empty.cardCount = 0;
    compare(empty, "empty-page", "", true);
}

template<class Canvas>
std::vector<uint16_t> primitiveRaster(int lines) {
    std::vector<uint16_t> result(480 * 480);
    for (int row = 0; row < 480; row += lines) {
        const int height = std::min(lines, 480 - row);
        std::vector<uint16_t> stripe(480 * height + 2, 0xBEEF);
        {
            Canvas canvas(stripe.data() + 1, 480, 480, row, height);
            canvas.clear(0);
            for (int y : {-30, -1, 0, 1, 6, 7, 15, 16, 30, 31, 32, 479, 480, 510}) {
                canvas.line(-20, y, 500, 479 - y, 0x1234);
                canvas.line(500, y, -20, 479 - y, 0x4321);
                canvas.line(11, y, 11, y, 0x1357);
                canvas.line(-7, y, 488, y, 0x2468);
                for (int radius : {1, 2, 7, 18, 39}) {
                    canvas.circle(1, y, radius, 0x07E0, false);
                    canvas.circle(478, y, radius, 0xF800, true);
                    canvas.circle(240, y, radius, 0x001F, radius % 2 == 0);
                }
            }
            canvas.roundedRect(-8, -5, 67, 45, 9, 0x1234, 0xFFFF);
            canvas.roundedRect(440, 469, 89, 60, 6, 0x4321, 0xFFFF);
            canvas.line(0, 479, 479, 479, 0xABCD);
        }
        assert(stripe.front() == 0xBEEF && stripe.back() == 0xBEEF);
        std::copy(stripe.begin() + 1, stripe.end() - 1, result.begin() + row * 480);
    }
    return result;
}

void primitives() {
    const auto wanted = primitiveRaster<unculled::StripeCanvas>(480);
    for (int x = 0; x < 480; ++x) assert(wanted[479 * 480 + x] == 0xABCD);
    for (int lines : {480, 1, 7, 16, 31}) {
        equalPixels(wanted, primitiveRaster<optimized::StripeCanvas>(lines), "primitives", lines);
        equalPixels(wanted, primitiveRaster<unculled::StripeCanvas>(lines), "primitives-reference", lines);
    }
}

void sensitivity() {
    size_t detected = 0;
    for (auto visual : {ScreenVisualType::VALUE, ScreenVisualType::CLOCK, ScreenVisualType::WEATHER}) {
        // The fixed footer scales the 480-unit canvas into 428 physical rows.
        // Ten logical rows remain >=8 pixels and still exercise legacy overflow.
        auto snapshot = scene(visual, 10, 31);
        const auto wanted = raster<unculled::StripeRenderer>(snapshot, 480);
        equalPixels(wanted, raster<overculled::StripeRenderer>(snapshot, 480), "mutant-full-frame", 480);
        if (wanted != raster<overculled::StripeRenderer>(snapshot, 1)) ++detected;
    }
    assert(detected == 3); // The oracle catches overflow discarded by an unsafe guard.
}

void labels() {
    auto snapshot = scene(ScreenVisualType::VALUE, 132, 80);
    std::strcpy(snapshot.pages[1].title, "Original title");
    const auto normal = raster<optimized::StripeRenderer>(snapshot, 7);
    MemoryDisplay display;
    {
        diagnostic::StripeRenderer renderer;
        DisplayConfigParams config{};
        std::strcpy(config.backend, "rgb");
        config.bufferLines = 7;
        String error;
        assert(renderer.begin(&display, config, error));
        assert(renderer.render(snapshot, "", error));
        const auto baseline = display.pixels;
        equalPixels(normal, {baseline.begin() + 1, baseline.end() - 1}, "diagnostic-normal", 7);
        std::vector<std::vector<uint16_t>> labels;
        for (const char* label : {"A: HOLD", "B: SNAPSHOT", "C: RENDER"}) {
            {
                ScreenIsolationTitle<sizeof(snapshot.pages[1].title)> override(snapshot.pages[1].title, label);
                assert(renderer.render(snapshot, "", error));
                assert(display.pixels != baseline);
                // The title appears in the fixed footer as well as the
                // diagnostic band; the page content must remain unchanged.
                for (size_t i = 18 * 480 + 1; i < 428 * 480 + 1; ++i)
                    assert(display.pixels[i] == baseline[i]);
                const auto band = display.pixels;
                for (const auto& previous : labels) assert(previous != band);
                labels.push_back(band);
                // Normal rendering may show the current title in the footer,
                // but must never add the diagnostic band over the content.
                const auto withTitle = raster<optimized::StripeRenderer>(snapshot, 7);
                for (size_t i = 0; i < 428 * 480; ++i) assert(normal[i] == withTitle[i]);
            }
            assert(std::strcmp(snapshot.pages[1].title, "Original title") == 0);
            assert(renderer.render(snapshot, "", error));
            assert(display.pixels == baseline);
        }
    }
    assert(allocations.empty());
}

void themes() {
    const auto contrast = optimized::paletteFor("high-contrast");
    assert(contrast.background == 0 && contrast.text == 0xffff);
    assert(contrast.background != optimized::paletteFor("dark").background);
    auto snapshot = scene(ScreenVisualType::VALUE, 132, 180);
    std::strcpy(snapshot.themeId, "high-contrast");
    std::strcpy(snapshot.backgroundPresetId, "solid");
    const auto solid = raster<optimized::StripeRenderer>(snapshot, 7);
    std::strcpy(snapshot.backgroundPresetId, "grid");
    const auto grid = raster<optimized::StripeRenderer>(snapshot, 7);
    assert(solid != grid);
    assert(grid[56 * 480 + 28] != grid[57 * 480 + 29]);
    compare(snapshot, "canonical-high-contrast-grid");
    std::strcpy(snapshot.backgroundPresetId, "geometry");
    equalPixels(grid, raster<optimized::StripeRenderer>(snapshot, 31), "legacy-grid-alias", 31);
}

void bars() {
    for (uint8_t count = 1; count <= 4; ++count) {
        // Leave >=40 physical pixels per bar after footer viewport scaling.
        auto snapshot = scene(ScreenVisualType::BAR, 50 + 46 * count, 80);
        auto& card = snapshot.cards[0];
        card.seriesCount = count;
        card.precision = 1;
        for (uint8_t i = 0; i < count; ++i) {
            auto& series = card.series[i];
            std::snprintf(series.sourceRef, sizeof(series.sourceRef), "sensor.%u", i);
            std::snprintf(series.label, sizeof(series.label), "Sensor %u", i);
            std::snprintf(series.value, sizeof(series.value), "%u", i * 10);
            std::strcpy(series.unit, "C");
            series.quality = i % 2 ? ScreenValueQuality::STALE : ScreenValueQuality::GOOD;
            series.updatedMs = (i + 1) * 12000;
        }
        compare(snapshot, "multi-bars-" + std::to_string(count));
        const auto original = raster<optimized::StripeRenderer>(snapshot, 7);
        std::strcpy(card.series[count - 1].value, "41.2");
        assert(original != raster<optimized::StripeRenderer>(snapshot, 7));
        const auto changed = raster<optimized::StripeRenderer>(snapshot, 7);
        card.series[count - 1].updatedMs += 15000;
        assert(changed != raster<optimized::StripeRenderer>(snapshot, 7));
        card.series[count - 1].quality = ScreenValueQuality::UNKNOWN;
        const auto unknown = raster<optimized::StripeRenderer>(snapshot, 7);
        card.series[count - 1].value[0] = 0;
        equalPixels(unknown, raster<optimized::StripeRenderer>(snapshot, 7), "unknown-is-not-zero", 7);
        if (count > 1) {
            card.series[count - 1].quality = ScreenValueQuality::GOOD;
            std::strcpy(card.series[count - 1].unit, "V");
            std::strcpy(card.series[count - 1].value, "50");
            const auto mismatch = raster<optimized::StripeRenderer>(snapshot, 7);
            std::strcpy(card.series[count - 1].value, "25");
            equalPixels(mismatch, raster<optimized::StripeRenderer>(snapshot, 7), "unit-mismatch-is-inert", 7);
        }
        card.rect.h = 8;
        compare(snapshot, "small-multi-bars-" + std::to_string(count));
    }
}

void preview() {
    auto snapshot = scene(ScreenVisualType::CLOCK);
    snapshot.pageCount = 1; snapshot.activePageIndex = 0; snapshot.cardCount = 4;
    snapshot.pages[0] = snapshot.pages[1]; snapshot.pages[0].firstCard = 0; snapshot.pages[0].cardCount = 4;
    std::strcpy(snapshot.pages[0].title, u8"首页");
    auto& clock = snapshot.cards[0]; clock.kind=ScreenCardKind::CLOCK; clock.visualType=ScreenVisualType::CLOCK;
    std::strcpy(snapshot.backgroundPresetId,"solid");
    clock.rect={12,8,456,112};std::strcpy(clock.title,u8"日期与时间");clock.iconId[0]=0;
    auto& temp = snapshot.cards[1];temp=snapshot.cards[0];temp.kind=ScreenCardKind::DATA;temp.visualType=ScreenVisualType::VALUE;
    temp.rect={12,130,223,146};std::strcpy(temp.id,"temp");std::strcpy(temp.title,u8"室内温度");std::strcpy(temp.value,"24.6");std::strcpy(temp.unit,u8"℃");std::strcpy(temp.iconId,"temperature");temp.quality=ScreenValueQuality::GOOD;
    auto& humidity=snapshot.cards[2];humidity=temp;humidity.rect={245,130,223,146};std::strcpy(humidity.id,"humidity");std::strcpy(humidity.title,u8"空气湿度");std::strcpy(humidity.value,"58.2");std::strcpy(humidity.unit,"%");std::strcpy(humidity.iconId,"humidity");
    auto& brightness=snapshot.cards[3];brightness={};brightness.kind=ScreenCardKind::SYSTEM;brightness.visualType=ScreenVisualType::SLIDER;
    brightness.rect={12,286,456,88};brightness.numericValue=70;std::strcpy(brightness.id,"brightness");std::strcpy(brightness.title,u8"屏幕亮度");std::strcpy(brightness.systemActionId,"brightness");
    compare(snapshot,"refined-home");
    auto save=[&](const char* name){
        const char* directory=std::getenv("FASTBEE_SCREEN_PREVIEW_DIR");if(!directory)return;
        std::string filename=std::string(directory)+"/"+name+".ppm";FILE* output=std::fopen(filename.c_str(),"wb");assert(output);
        std::fprintf(output,"P6\n480 480\n255\n");
        for(uint16_t pixel:raster<optimized::StripeRenderer>(snapshot,16)){unsigned char rgb[3]={static_cast<unsigned char>(((pixel>>11)&31)*255/31),static_cast<unsigned char>(((pixel>>5)&63)*255/63),static_cast<unsigned char>((pixel&31)*255/31)};assert(std::fwrite(rgb,1,3,output)==3);}std::fclose(output);
    };
    save("screen-home");
    snapshot.cardCount=1;snapshot.pages[0].cardCount=1;std::strcpy(snapshot.pages[0].title,u8"电机控制");
    auto& motor=snapshot.cards[0];motor={};motor.rect={12,8,456,464};motor.kind=ScreenCardKind::CONTROL;motor.visualType=ScreenVisualType::MOTOR_CONTROL;
    std::strcpy(motor.id,"motor");std::strcpy(motor.title,u8"传送电机");motor.motorKnown=true;motor.motorSpeed=500;motor.motorSpeedInput=500;motor.motorPrecision=1;motor.motorPulse=1600;motor.motorPulseInput=1600;
    compare(snapshot,"refined-motor");save("screen-motor");
    snapshot.cardCount=5;snapshot.pages[0].cardCount=5;std::strcpy(snapshot.pages[0].title,u8"系统设置");
    const char* ids[]={"brightness","sleep-time","touch-beep","confirm-idle","sleep"};
    const char* titles[]={u8"屏幕亮度",u8"自动息屏",u8"触摸提示音",u8"操作确认间隔",u8"立即息屏"};
    const ScreenVisualType visuals[]={ScreenVisualType::SLIDER,ScreenVisualType::SELECT,ScreenVisualType::SWITCH,ScreenVisualType::SELECT,ScreenVisualType::ACTION};
    for(int i=0;i<5;++i){auto& card=snapshot.cards[i];card={};card.rect={12,static_cast<uint16_t>(8+i*94),456,88};
        card.kind=ScreenCardKind::SYSTEM;card.visualType=visuals[i];card.interactive=true;card.quality=ScreenValueQuality::GOOD;
        std::strcpy(card.id,ids[i]);std::strcpy(card.systemActionId,ids[i]);std::strcpy(card.title,titles[i]);}
    snapshot.cards[0].numericValue=70;snapshot.cards[1].numericValue=120;snapshot.cards[3].numericValue=60;
    std::strcpy(snapshot.cards[2].value,"true");
    compare(snapshot,"industrial-system");save("screen-system");
}

int main(int argc, char** argv) {
    static_assert(sizeof(u8g2_uint_t) == 2, "480px requires 16-bit U8g2 coordinates");
    assert(argc == 2);
    const std::string group = argv[1];
    if (group == "visuals") visuals();
    else if (group == "weather") weather();
    else if (group == "geometry") geometry();
    else if (group == "primitives") primitives();
    else if (group == "sensitivity") sensitivity();
    else if (group == "labels") labels();
    else if (group == "themes") themes();
    else if (group == "bars") bars();
    else if (group == "preview") preview();
    else return 3;
    assert(allocations.empty());
    std::printf("%s raster comparisons passed (%zu pixel comparisons)\n", group.c_str(), comparisons);
}
'''


if __name__ == "__main__":
    unittest.main()
