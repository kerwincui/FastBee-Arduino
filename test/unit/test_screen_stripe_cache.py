"""Run the production renderer lifecycle/flush and default-card methods on host."""
import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest

from test.unit.test_screen_panel_contract import braced_block

ROOT = pathlib.Path(__file__).resolve().parents[2]


class ScreenStripeCacheTests(unittest.TestCase):
    def test_production_render_retry_lifecycle_and_placeholder_cards(self):
        compiler = shutil.which("g++") or shutil.which("clang++")
        fallback = pathlib.Path("D:/msys64/ucrt64/bin/g++.exe")
        if not compiler and fallback.exists():
            compiler = str(fallback)
        if not compiler:
            self.skipTest("A C++17 compiler is required")
        runtime = (ROOT / "src/screen/ScreenRuntime.cpp").read_text(encoding="utf-8")
        renderer = runtime[runtime.index("class StripeRenderer"):]
        signatures = [
            "void invalidate()",
            "void setPressed(const char* cardId, uint16_t x = 0, uint16_t y = 0)",
            "bool controlPressed(const ScreenPanelCardSnapshot& card, const PixelRect& bounds) const",
            "bool begin(IDisplayBackend* display, const DisplayConfigParams& config,\n               String& error)",
            "void end()",
            "bool render(const ScreenPanelSnapshot& snapshot,\n                const char* confirmationCardId, String& error)",
            "void drawSwitch(StripeCanvas& canvas, const ScreenPanelCardSnapshot& card,\n                    const PixelRect& rect, const Palette& palette) const",
            "void drawAction(StripeCanvas& canvas, const ScreenPanelCardSnapshot& card,\n                    const PixelRect& rect, const Palette& palette) const",
            "void drawWeather(StripeCanvas& canvas, const ScreenPanelCardSnapshot& card,\n                     const PixelRect& rect, const Palette& palette) const",
        ]
        methods = "\n".join(signature + " {" + braced_block(renderer, signature) + "}"
                            for signature in signatures)
        with tempfile.TemporaryDirectory(prefix="fastbee-stripe-cache-") as directory:
            temporary = pathlib.Path(directory)
            (temporary / "production_stripe_renderer.inc").write_text(methods, encoding="utf-8")
            rgb_start = runtime.index("constexpr uint16_t rgb565(")
            rgb_signature = runtime[rgb_start:runtime.index("{", rgb_start)].strip()
            start = runtime.index("uint16_t blend565(")
            blend_signature = runtime[start:runtime.index("{", start)].strip()
            (temporary / "production_blend.inc").write_text(
                rgb_signature + " {" + braced_block(runtime, rgb_signature) + "}\n" +
                blend_signature + " {" + braced_block(runtime, blend_signature) + "}", encoding="utf-8")
            executable = temporary / ("stripe.exe" if os.name == "nt" else "stripe")
            env = dict(os.environ)
            env["PATH"] = str(pathlib.Path(compiler).parent) + os.pathsep + env.get("PATH", "")
            build = subprocess.run([
                compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                f"-I{temporary}", f"-I{ROOT / 'include'}",
                str(ROOT / "test/unit/cpp/screen_stripe_cache_harness.cpp"), "-o", str(executable)
            ], capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=60, env=env)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(executable)], capture_output=True, text=True,
                                 encoding="utf-8", errors="replace", timeout=15, env=env)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn("Stripe renderer scenarios passed", run.stdout)


if __name__ == "__main__":
    unittest.main()
