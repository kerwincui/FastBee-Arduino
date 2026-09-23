"""Execute production feature/logging orchestration with controllable backends.

Persistence, network and logger failures are injected; the route transaction,
response-state helper and NORMAL memory recovery branch come from real source.
No PlatformIO build or shared build output is used.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from test_sensor_publication import definition

ROOT = Path(__file__).resolve().parents[2]
ROUTE = ROOT / "src/network/handlers/FeatureRouteHandler.cpp"
HEALTH = ROOT / "src/systems/HealthMonitor.cpp"


class FeatureLoggingRuntimeTests(unittest.TestCase):
    def run_harness(self, screen_ui):
        compiler = shutil.which("g++") or shutil.which("clang++")
        if not compiler:
            self.skipTest("A C++17 compiler is required")
        json_headers = list((ROOT / ".pio/libdeps").glob("*/ArduinoJson/src/ArduinoJson.h"))
        if not json_headers:
            self.skipTest("Install ArduinoJson first")
        route = ROUTE.read_text(encoding="utf-8")
        health = HEALTH.read_text(encoding="utf-8")
        recovery = definition(health, "void HealthMonitor::applyDegradation(")
        recovery = definition(recovery, "if (newLevel == MemoryGuardLevel::NORMAL)")
        snippets = [definition(route, signature) for signature in (
            "void appendFileLoggingApplyState(",
            "FeatureRouteHandler::PersistResult FeatureRouteHandler::persistFeatureUpdate(",
            "void FeatureRouteHandler::handleConfig(",
            "void FeatureRouteHandler::handleApply(",
            "void FeatureRouteHandler::handleUpdate(",
        )]
        snippets.append("void HealthMonitor::recover(MemoryGuardLevel newLevel) {\n"
                        "auto& logger = LoggerSystem::getInstance();\n" + recovery + "\n}")
        with tempfile.TemporaryDirectory(prefix="fastbee-feature-logging-") as directory:
            temporary = Path(directory)
            (temporary / "feature_logging_methods.inc").write_text(
                "\n\n".join(snippets), encoding="utf-8")
            executable = temporary / ("features.exe" if os.name == "nt" else "features")
            command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                       "-DARDUINOJSON_ENABLE_ARDUINO_STRING=0",
                       "-DARDUINOJSON_ENABLE_STD_STRING=1",
                       f"-DFASTBEE_ENABLE_SCREEN_UI={screen_ui}", "-DFASTBEE_ENABLE_MQTT=0",
                       f"-I{temporary}", f"-I{json_headers[0].parent}",
                       str(ROOT / "test/unit/cpp/feature_logging_runtime_harness.cpp"),
                       "-o", str(executable)]
            result = subprocess.run(command, capture_output=True, text=True,
                                    encoding="utf-8", errors="replace", timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_logging_transaction_and_recovery_with_screen_ui(self):
        self.run_harness(1)

    def test_logging_transaction_and_recovery_without_screen_ui(self):
        self.run_harness(0)

    def test_post_routes_keep_their_separate_entry_points(self):
        route = ROUTE.read_text(encoding="utf-8")
        setup = definition(route, "void FeatureRouteHandler::setupRoutes(")
        for path, method in (("config", "handleConfig"), ("apply", "handleApply")):
            with self.subTest(path=path):
                start = setup.index(f'"/api/features/{path}", HTTP_POST')
                callback = setup[start:setup.index("nullptr", start)]
                self.assertIn(f"{method}(request)", callback)


if __name__ == "__main__":
    unittest.main()
