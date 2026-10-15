"""Screen panel binding validation must accept configuration-known weather sources.

Regression: after the UI refactor, validateAndBindReferences only accepted
weather references backed by a runtime-fetched value. At cold start no fetch
has completed yet, so a layout with a weather card was rejected wholesale and
the panel silently fell back to the draft home page. The fix accepts sources
declared by the active configuration:

- RemoteResourceModel.h gains the pure predicate knowsWeatherSource(config, ref)
  (behaviourally tested here against a compiled harness).
- RemoteResourceManager exposes knowsConfiguredSource(ref) delegating to that
  predicate over the loaded configuration.
- ScreenPanelManager::validateAndBindReferences consults knowsConfiguredSource
  as a fallback after the runtime getValue check (token-pinned below, inside
  the FASTBEE_ENABLE_REMOTE_RESOURCES guard).
"""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class ScreenPanelWeatherBindingTests(unittest.TestCase):
    def test_validate_source_falls_back_to_configuration_known_weather(self):
        from test.unit.test_screen_snapshot_cache import definition
        manager = (ROOT / "src/core/ScreenPanelManager.cpp").read_text(encoding="utf-8")
        body = definition(manager, "bool ScreenPanelManager::validateAndBindReferences")
        get_value = "if (FastBee::RemoteResourceManager::instance().getValue("
        fallback = "if (FastBee::RemoteResourceManager::instance().knowsConfiguredSource("
        self.assertIn(get_value, body)
        self.assertIn(fallback, body)
        # The configuration fallback must come after the runtime-value check so
        # live data keeps precedence, and both stay inside the remote-resources
        # guard that supplies the manager at all.
        self.assertGreater(body.index(fallback), body.index(get_value))
        guard_start = body.index("#if FASTBEE_ENABLE_REMOTE_RESOURCES")
        self.assertGreater(body.index(fallback), guard_start)

    def test_manager_exposes_configuration_source_predicate(self):
        from test.unit.test_screen_snapshot_cache import definition
        header = (ROOT / "include/protocols/RemoteResourceManager.h").read_text(encoding="utf-8")
        self.assertIn("knowsConfiguredSource", header)
        impl = (ROOT / "src/protocols/RemoteResourceManager.cpp").read_text(encoding="utf-8")
        body = definition(impl, "bool RemoteResourceManager::knowsConfiguredSource")
        self.assertIn("knowsWeatherSource(", body)
        self.assertIn("config_", body)

    def test_knows_weather_source_matches_only_enabled_configured_sources(self):
        from test.unit.test_screen_snapshot_cache import definition
        source = (ROOT / "include/protocols/RemoteResourceModel.h").read_text(encoding="utf-8")
        predicate = definition(source, "inline bool knowsWeatherSource")
        main = r'''
#include <Arduino.h>
#include <ArduinoJson.h>
#include <cstdio>
INLINES
int main() {
    JsonDocument doc;
    const char* json = R"json({"weatherSources":[{"id":"home_weather","enabled":true},
        {"id":"off_weather","enabled":false}]})json";
    if (deserializeJson(doc, json)) return 2;
    auto cfg = doc.as<JsonVariantConst>();
    struct Case { const char* ref; bool expected; } cases[] = {
        {"weather.home_weather", true},
        {"weather.home_weather.temperature", true},
        {"weather.home_weather.humidity", true},
        {"weather.home_weather.pressure", false},
        {"weather.off_weather", false},
        {"weather.off_weather.temperature", false},
        {"weather.missing", false},
        {"system.uptime", false},
        {"", false},
    };
    for (const auto& c : cases) {
        const bool got = knowsWeatherSource(cfg, String(c.ref));
        if (got != c.expected) { std::printf("FAIL %s got %d\n", c.ref, got); return 1; }
    }
    JsonDocument empty;
    if (knowsWeatherSource(empty.as<JsonVariantConst>(), String("weather.home_weather"))) return 1;
    if (!knowsWeatherSource(cfg, String("weather.home_weather.temperature"))) return 1;
    std::printf("PASS knowsWeatherSource cases\n");
    return 0;
}
'''
        headers = next((ROOT / ".pio/libdeps").glob("*/ArduinoJson/src/ArduinoJson.h"))
        compiler = shutil.which("g++") or shutil.which("clang++")
        fallback = Path("D:/msys64/ucrt64/bin/g++.exe")
        if not compiler and fallback.exists():
            compiler = str(fallback)
        env = dict(os.environ)
        env["PATH"] = str(Path(compiler).parent) + os.pathsep + env.get("PATH", "")
        with tempfile.TemporaryDirectory(prefix="fastbee-knows-weather-") as folder:
            temp = Path(folder)
            (temp / "main.cpp").write_text(main.replace("INLINES", predicate), encoding="utf-8")
            binary = temp / ("knows.exe" if os.name == "nt" else "knows")
            result = subprocess.run(
                [compiler, "-std=c++17", "-DUNIT_TEST",
                 "-DARDUINOJSON_ENABLE_ARDUINO_STRING=0",
                 "-DARDUINOJSON_ENABLE_STD_STRING=1",
                 f"-I{temp}", f"-I{ROOT / 'test/mocks'}", f"-I{headers.parent}",
                 f"-I{ROOT / 'include'}", str(temp / "main.cpp"), "-o", str(binary)],
                capture_output=True, text=True, encoding="utf-8", errors="replace",
                timeout=60, env=env)
            self.assertEqual(0, result.returncode, result.stdout + result.stderr)
            run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=15, env=env)
            self.assertEqual(0, run.returncode, run.stdout + run.stderr)
            self.assertIn("PASS knowsWeatherSource cases", run.stdout)


if __name__ == "__main__":
    unittest.main()
