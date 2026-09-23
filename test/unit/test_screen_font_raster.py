"""Render production StripeCanvas with the actual U8g2 C renderer and fonts."""
import os
import re
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class ScreenFontRasterTests(unittest.TestCase):
    def test_actual_glyphs_survive_stripe_clipping_and_scaling(self):
        candidates = [Path(os.environ.get("FASTBEE_U8G2_CLIB", "missing")),
                      ROOT / ".pio/libdeps/esp32s3-F16R8/U8g2/src/clib",
                      ROOT.parent / "fb-panel-check-20260917/.pio/libdeps/esp32s3-F16R8/U8g2/src/clib"]
        library = next((p for p in candidates if (p / "u8g2.h").exists()), None)
        compiler = shutil.which("g++")
        if library is None or compiler is None:
            self.skipTest("Requires g++ and installed U8g2 (or FASTBEE_U8G2_CLIB)")
        runtime = (ROOT / "src/screen/ScreenRuntime.cpp").read_text(encoding="utf-8")
        production = (runtime[runtime.index("constexpr uint16_t rgb565"):runtime.index("struct Palette")]
                      + runtime[runtime.index("uint16_t decodeUtf8"):runtime.index("class StripeRenderer")])
        # Expose only visibility in the extracted production canvas for coverage
        # assertions; glyph lookup and rasterization remain production code.
        production = production.replace("private:\n    static void fontLine", "public:\n    static void fontLine")
        labels = runtime + (ROOT / "docs/waveshare-esp32s3-touch-lcd4/screen_panel.json").read_text(encoding="utf-8")
        codepoints = sorted({ord(char) for text in re.findall(r'"([^"\n]*)"', labels)
                             for char in text if '\u4e00' <= char <= '\u9fff'})
        harness = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <u8g2.h>
// These hardware entry points are not used by the canvas; fail if reached.
extern "C" void u8x8_cad_vsendf(u8x8_t*, const char*, va_list) { std::abort(); }
extern "C" void u8x8_Setup(u8x8_t*, u8x8_msg_cb, u8x8_msg_cb, u8x8_msg_cb, u8x8_msg_cb) { std::abort(); }
extern "C" uint8_t u8x8_d_null_cb(u8x8_t*, uint8_t, uint8_t, void*) { std::abort(); }
extern "C" uint8_t u8x8_cad_empty(u8x8_t*, uint8_t, uint8_t, void*) { std::abort(); }
extern "C" uint8_t u8x8_DrawTile(u8x8_t*, uint8_t, uint8_t, uint8_t, uint8_t*) { std::abort(); }
PRODUCTION
std::vector<uint16_t> render(int lines, const char* text, int x, int y, int scale) {
    std::vector<uint16_t> result(480*480, 0);
    for(int row=0; row<480; row+=lines) {
        const int height=std::min(lines,480-row);
        std::vector<uint16_t> stripe(480*height+2, 0);
        stripe.front()=stripe.back()=0xBEEF;
        {
            StripeCanvas canvas(stripe.data()+1,480,480,row,height);
            canvas.text(x,y,text,0xFFFF,480-x,scale);
        }
        assert(stripe.front()==0xBEEF && stripe.back()==0xBEEF);
        std::copy(stripe.begin()+1,stripe.end()-1,result.begin()+row*480);
    }
    return result;
}
int main() {
    static_assert(sizeof(u8g2_uint_t)==2,"480px needs 16-bit coordinates");
    std::vector<uint16_t> check(480*480,0);
    {
        StripeCanvas canvas(check.data(),480,480,0,480);
        for(uint16_t codepoint : {CODEPOINTS}) assert(canvas.selectFont(codepoint)!=nullptr);
        canvas.roundedRect(10,10,100,60,6,0x1234,0xFFFF);
        assert(check[40*480+10]==0xFFFF && check[40*480+109]==0xFFFF);
        for(int x=11;x<109;++x) assert(check[40*480+x]==0x1234);
        assert(check[10*480+10]==0); // clipped outer corner
    }
    for(const char* label : {"12:34 22.5 C", u8"温度湿度"}) {
        for(int y : {5,15,255,307,465}) {
            for(int scale : {1,2}) {
                auto full=render(480,label,280,y,scale);
                assert(std::count(full.begin(),full.end(),0xFFFF)>20);
                for(int lines : {1,7,16,31}) assert(full==render(lines,label,280,y,scale));
            }
        }
    }
}
'''.replace("PRODUCTION", production).replace("CODEPOINTS", ','.join(str(cp) for cp in codepoints))
        with tempfile.TemporaryDirectory(prefix="fastbee-font-") as temporary:
            tmp = Path(temporary)
            cpp = tmp / "font.cpp"
            exe = tmp / ("font.exe" if os.name == "nt" else "font")
            cpp.write_text(harness, encoding="utf-8")
            names = ["u8g2_font.c", "u8g2_fonts.c", "u8g2_setup.c", "u8g2_hvline.c", "u8g2_intersection.c",
                     "u8g2_ll_hvline.c", "u8g2_kerning.c", "u8x8_8x8.c"]
            command = [compiler, "-std=c++17", "-DU8G2_USE_LARGE_FONTS", "-ffunction-sections", "-fdata-sections",
                       "-Wl,--gc-sections", f"-I{library}", str(cpp),
                       *[str(library / name) for name in names], "-o", str(exe)]
            compiled = subprocess.run(command, capture_output=True, text=True, timeout=120)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            # Verify this regression actually detects the blank-text defect.
            cpp.write_text(harness.replace("u8g2_SetMaxClipWindow(&_font);", ""), encoding="utf-8")
            compiled = subprocess.run(command, capture_output=True, text=True, timeout=120)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            broken = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
            self.assertNotEqual(broken.returncode, 0, "Missing clip initialization must make this test fail")


if __name__ == "__main__":
    unittest.main()
