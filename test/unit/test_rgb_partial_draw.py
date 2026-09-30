"""Check the real vendored RGB copy/rotation and cache-flush window.

This verifies pixel layout and submission metadata, not DMA timing or PSRAM
bandwidth. The 480x480/16-line case matches the screen worker's RGB submissions.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

try:
    from test.unit.test_sensor_publication import definition
except ModuleNotFoundError:
    from test_sensor_publication import definition

ROOT = Path(__file__).resolve().parents[2]
VENDOR = ROOT / "vendor/esp-idf-rgb-735507283d"


class RgbPartialDrawTests(unittest.TestCase):
    def test_real_driver_rotated_stripes_pixel_stride_and_flush_ranges(self):
        compiler = shutil.which("g++") or shutil.which("clang++")
        if not compiler:
            self.skipTest("A C++17 compiler is required")
        driver = (VENDOR / "esp_lcd_panel_rgb.c").read_text(encoding="utf-8")
        # Include the opening brace to skip the forward declaration.
        method = definition(driver, "static esp_err_t rgb_panel_draw_bitmap(esp_lcd_panel_t *panel, "
                            "int x_start, int y_start, int x_end, int y_end, const void *color_data)\n{")
        source = r'''
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>
#include "rgb_lcd_rotation_sw.h"
using esp_err_t = int;
constexpr int ESP_OK = 0, ESP_ERR_NOT_SUPPORTED = -1;
constexpr int ESP_CACHE_MSYNC_FLAG_DIR_C2M = 1, ESP_CACHE_MSYNC_FLAG_UNALIGNED = 2;
#define ESP_RETURN_ON_FALSE(test, error, ...) do { if (!(test)) return error; } while (0)
#define ESP_LOGV(...) ((void)0)
#define MAX(a,b) std::max(a,b)
#define MIN(a,b) std::min(a,b)
#define __containerof(ptr,type,member) reinterpret_cast<type*>(ptr)
struct esp_lcd_panel_t {};
using esp_lcd_rgb_panel_draw_buf_complete_cb_t = void(*)(esp_lcd_panel_t*, void*, void*);
struct esp_rgb_panel_t {
    esp_lcd_panel_t base;
    int num_fbs = 1, fb_bits_per_pixel = 16, x_gap = 0, y_gap = 0;
    esp_lcd_rgb_panel_draw_buf_complete_cb_t on_color_trans_done = nullptr;
    void* user_ctx = nullptr;
    size_t fb_size = 480U * 480U * 2U, bb_size = 0;
    struct { int h_res = 480, v_res = 480; } timings;
    unsigned rotate_mask = 0, cur_fb_index = 0;
    uint8_t* fbs[1]{};
    int dma_fb_links[1]{42};
    struct { bool fb_behind_cache = true, stream_mode = true; } flags;
};
static void* flushed = nullptr;
static size_t flushSize = 0;
static int flushCalls = 0, concatCalls = 0;
static int esp_cache_msync(void* address, size_t size, unsigned flags) {
    assert(flags == (ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED));
    flushed = address; flushSize = size; ++flushCalls; return ESP_OK;
}
static int gdma_link_concat(int a, int ai, int b, int bi) {
    assert(a == 42 && b == a && ai == -1 && bi == 0);
    ++concatCalls; return ESP_OK;
}
PRODUCTION_METHOD
static uint16_t pattern(int x, int y) { return static_cast<uint16_t>(1 + x + y * 479); }
static void expectedPosition(unsigned rotation, int x, int y, int& px, int& py) {
    px = x; py = y;
    if (rotation == 1) { px = 479 - y; py = x; }
    if (rotation == 2) { px = 479 - x; py = 479 - y; }
    if (rotation == 3) { px = y; py = 479 - x; }
}
static void verifyGuard(const std::vector<uint16_t>& guarded) {
    for (int i = 0; i < 64; ++i) {
        assert(guarded[i] == 0xA55A);
        assert(guarded[64 + 480 * 480 + i] == 0xA55A);
    }
}
int main() {
    for (unsigned rotation = 0; rotation < 4; ++rotation) {
        std::vector<uint16_t> guarded(480 * 480 + 128, 0xA55A);
        auto* frame = guarded.data() + 64;
        std::fill_n(frame, 480 * 480, 0);
        esp_rgb_panel_t panel;
        panel.fbs[0] = reinterpret_cast<uint8_t*>(frame);
        const unsigned masks[] = {0, ROTATE_MASK_SWAP_XY | ROTATE_MASK_MIRROR_X,
            ROTATE_MASK_MIRROR_X | ROTATE_MASK_MIRROR_Y,
            ROTATE_MASK_SWAP_XY | ROTATE_MASK_MIRROR_Y};
        panel.rotate_mask = masks[rotation];
        flushCalls = concatCalls = 0;
        for (int y = 0; y < 480; y += 16) {
            std::vector<uint16_t> stripe(480 * 16);
            for (int row = 0; row < 16; ++row)
                for (int x = 0; x < 480; ++x) stripe[row * 480 + x] = pattern(x, y + row);
            assert(rgb_panel_draw_bitmap(&panel.base, 0, y, 480, y + 16, stripe.data()) == ESP_OK);
            if (rotation == 0 || rotation == 2) {
                const int firstRow = rotation == 0 ? y : 480 - y - 16;
                assert(flushed == frame + firstRow * 480 && flushSize == 16U * 480U * 2U);
            } else {
                // Rotated horizontal stripes touch every physical output row.
                assert(flushed == frame && flushSize == 480U * 480U * 2U);
            }
        }
        assert(flushCalls == 30 && concatCalls == 30 && panel.cur_fb_index == 0);
        for (int y = 0; y < 480; ++y) for (int x = 0; x < 480; ++x) {
            int px, py; expectedPosition(rotation, x, y, px, py);
            assert(frame[py * 480 + px] == pattern(x, y));
        }
        verifyGuard(guarded);

        // A nonzero X offset and a narrow source have their own packed stride.
        std::fill_n(frame, 480 * 480, 0);
        constexpr int left = 37, top = 73, width = 29, height = 11;
        std::vector<uint16_t> tile(width * height);
        for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x)
            tile[y * width + x] = static_cast<uint16_t>(1 + y * width + x);
        assert(rgb_panel_draw_bitmap(&panel.base, left, top, left + width, top + height, tile.data()) == ESP_OK);
        std::vector<uint16_t> expected(480 * 480, 0);
        for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
            int px, py; expectedPosition(rotation, x + left, y + top, px, py);
            expected[py * 480 + px] = tile[y * width + x];
        }
        assert(std::equal(expected.begin(), expected.end(), frame));
        const int firstRow = rotation == 0 ? top : rotation == 1 ? left :
                             rotation == 2 ? 480 - top - height : 480 - left - width;
        const int rows = rotation % 2 == 0 ? height : width;
        assert(flushed == frame + firstRow * 480 && flushSize == static_cast<size_t>(rows * 480 * 2));
        verifyGuard(guarded);
    }
}
'''.replace("PRODUCTION_METHOD", method)
        with tempfile.TemporaryDirectory(prefix="fastbee-rgb-partial-") as directory:
            temporary = Path(directory)
            (temporary / "esp_bit_defs.h").write_text("#pragma once\n#define BIT(n) (1U << (n))\n", encoding="utf-8")
            cpp = temporary / "driver.cpp"
            cpp.write_text(source, encoding="utf-8")
            executable = temporary / ("driver.exe" if os.name == "nt" else "driver")
            result = subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                                     "-Wno-sign-compare", f"-I{temporary}", f"-I{VENDOR}",
                                     str(cpp), "-o", str(executable)],
                                    capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
