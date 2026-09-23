"""Focused contracts for asynchronous, secret-free weather test previews."""

from __future__ import annotations

import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
ROUTE = (ROOT / "src/network/handlers/RemoteResourceRouteHandler.cpp").read_text(encoding="utf-8")
MANAGER = (ROOT / "src/protocols/RemoteResourceManager.cpp").read_text(encoding="utf-8")
MODEL = (ROOT / "include/protocols/RemoteResourceModel.h").read_text(encoding="utf-8")
WEB = "\n".join((ROOT / "web-src/modules/runtime" / name).read_text(encoding="utf-8")
                for name in ("screen-panel.js", "screen-panel-model.js"))


def braced_block(source: str, marker: str) -> str:
    start = source.find(marker)
    if start < 0:
        raise AssertionError(f"missing marker {marker!r}")
    opening = source.find("{", start + len(marker))
    depth = 0
    for index in range(opening, len(source)):
        depth += (source[index] == "{") - (source[index] == "}")
        if depth == 0:
            return source[opening + 1:index]
    raise AssertionError(f"unterminated block after {marker!r}")


class RemoteWeatherPreviewContractTests(unittest.TestCase):
    def test_routes_require_auth_and_developer_mode_before_queue_or_status(self):
        for marker, operation in (
            ("void RemoteResourceRouteHandler::weatherTestStatus(", "getWeatherTestStatus("),
            ("void RemoteResourceRouteHandler::requestWeatherTest(", "requestRead("),
        ):
            with self.subTest(marker=marker):
                block = braced_block(ROUTE, marker)
                auth = block.find("ctx_->requireAuth(r)")
                developer = block.find("ctx_->requireDeveloperMode(r)")
                action = block.find(operation)
                self.assertGreaterEqual(auth, 0)
                self.assertGreaterEqual(developer, 0)
                self.assertGreaterEqual(action, 0)
                self.assertLess(auth, developer)
                self.assertLess(developer, action)
                self.assertIn("RemoteResourceModel::identifier", block)

        post = braced_block(ROUTE, "void RemoteResourceRouteHandler::requestWeatherTest(")
        self.assertIn("r->contentLength()>64", post)
        self.assertNotIn("fetchWeather", post)
        self.assertNotIn("HTTPClient", post)

    def test_network_io_stays_in_the_existing_worker_and_is_rate_limited(self):
        request = braced_block(MANAGER, "bool RemoteResourceManager::requestRead(")
        self.assertIn("now-s.updatedMs<300000U", request)
        self.assertIn("now-s.lastManualRequestMs<300000U", request)
        self.assertIn("s.requested=true", request)
        self.assertNotIn("fetchWeather", request)

        tick = braced_block(MANAGER, "void RemoteResourceManager::tick(")
        worker = braced_block(MANAGER, "void RemoteResourceManager::weatherTask(")
        collect = braced_block(MANAGER, "void RemoteResourceManager::collectWeather(")
        self.assertIn("xTaskCreate(weatherTask", tick)
        self.assertIn("self->collectWeather()", worker)
        self.assertIn("fetchWeather(", collect)

    def test_preview_has_an_explicit_allowlist_and_web_never_posts_credentials(self):
        preview = braced_block(MODEL, "inline void copyWeatherPreview(")
        for allowed in ("temperature", "unit", "city", "description", "condition", "humidity"):
            self.assertIn(f'"{allowed}"', preview)
        for forbidden in ("apiKey", "authHeader", "raw", "token", "url"):
            self.assertNotIn(f'"{forbidden}"', preview)

        self.assertNotIn('"forecast"', preview)
        self.assertIn("apiPost('/api/screen-panel/weather/refresh', { id: model.weather.id })", WEB)
        self.assertIn("apiGet('/api/screen-panel/weather/refresh', { id: model.weather.id })", WEB)
        self.assertNotIn("/api/remote-resources/weather-test", WEB)

    def test_fixed_tls_endpoint_and_retired_capabilities_cannot_run(self):
        fetch = braced_block(MANAGER, "bool fetchWeather(")
        self.assertIn('https://api.seniverse.com/v3/weather/now.json', fetch)
        self.assertIn('time(nullptr)<1577836800', fetch)
        self.assertIn('setCACertBundle', fetch)
        self.assertNotIn('source["url"]', fetch)
        self.assertNotIn('authHeader', fetch)
        self.assertNotIn('forecast', fetch)
        mqtt = braced_block(MANAGER, "void RemoteResourceManager::handleMqtt(")
        self.assertNotIn('publishExact', mqtt)
        self.assertNotIn('client->subscribeExact', mqtt)
        self.assertIn('unsubscribeExact', mqtt)
        self.assertIn('sendError(r,410,', ROUTE)
        self.assertIn('error.startsWith("Weather revision conflict") ? 409', ROUTE)

    def test_production_cpp_weather_model_cases(self):
        compiler = shutil.which("g++") or shutil.which("clang++")
        fallback = pathlib.Path("D:/msys64/ucrt64/bin/g++.exe")
        if not compiler and fallback.exists():
            compiler = str(fallback)
        if not compiler:
            self.skipTest("A C++17 compiler is required")

        json_headers = list((ROOT / ".pio/libdeps").glob("*/ArduinoJson/src/ArduinoJson.h"))
        unity = ROOT / ".pio/libdeps/native/Unity/src"
        if not json_headers or not (unity / "unity.c").exists():
            self.skipTest("Existing ArduinoJson and Unity headers are required")

        with tempfile.TemporaryDirectory(prefix="fastbee-weather-preview-") as temporary:
            binary = pathlib.Path(temporary) / "remote-weather-model.exe"
            environment = dict(os.environ)
            environment["PATH"] = str(pathlib.Path(compiler).parent) + os.pathsep + environment.get("PATH", "")
            result = subprocess.run([
                compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                "-DFASTBEE_REMOTE_MODEL_STANDALONE",
                "-DARDUINOJSON_ENABLE_STD_STRING=1",
                "-DARDUINOJSON_ENABLE_ARDUINO_STRING=0",
                f"-I{ROOT / 'include'}", f"-I{json_headers[0].parent}", f"-I{unity}",
                str(ROOT / "test/test_remote_resources.cpp"), str(unity / "unity.c"),
                "-o", str(binary),
            ], capture_output=True, text=True, encoding="utf-8", errors="replace",
                timeout=60, env=environment)
            self.assertEqual(0, result.returncode, result.stdout + result.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True,
                                    encoding="utf-8", errors="replace", timeout=15,
                                    env=environment)
            self.assertEqual(0, result.returncode, result.stdout + result.stderr)
            self.assertIn("19 Tests 0 Failures 0 Ignored", result.stdout)


if __name__ == "__main__":
    unittest.main()
