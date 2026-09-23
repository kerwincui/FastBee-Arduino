"""Exercise real feature loading, API writes and status around restart-only flags."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from test_sensor_publication import definition

ROOT = Path(__file__).resolve().parents[2]


class FeatureBootSnapshotTests(unittest.TestCase):
    def test_saved_intent_reload_defaults_and_restart_semantics(self):
        compiler = shutil.which("g++") or shutil.which("clang++")
        if not compiler:
            self.skipTest("A C++17 compiler is required")
        json_headers = list((ROOT / ".pio/libdeps").glob("*/ArduinoJson/src/ArduinoJson.h"))
        if not json_headers:
            self.skipTest("Install ArduinoJson first")
        source = (ROOT / "src/core/RuntimeFeatureManager.cpp").read_text(encoding="utf-8")
        signatures = (
            "void RuntimeFeatureManager::registerFeature(",
            "RuntimeFeatureManager::FeatureState* RuntimeFeatureManager::findFeature(",
            "const RuntimeFeatureManager::FeatureState* RuntimeFeatureManager::findFeature(",
            "void RuntimeFeatureManager::latchBootFeatures()",
            "bool RuntimeFeatureManager::loadConfig()",
            "bool RuntimeFeatureManager::shouldEnable(",
            "bool RuntimeFeatureManager::isUserEnabled(",
            "bool RuntimeFeatureManager::setUserEnabled(",
            "bool RuntimeFeatureManager::resetToDefaults()",
            "bool RuntimeFeatureManager::updateFromJson(",
            "RuntimeFeatureManager::UserConfigSnapshot RuntimeFeatureManager::captureUserConfig()",
            "bool RuntimeFeatureManager::restoreUserConfig(",
            "RuntimeFeatureManager::ConfigUpdateResult RuntimeFeatureManager::updateAndSave(",
            "void RuntimeFeatureManager::buildConfigDoc(",
            "bool RuntimeFeatureManager::saveCurrentConfig()",
            "bool RuntimeFeatureManager::getFeatureStatusJson(",
            "void RuntimeFeatureManager::setRuntimeDisabled(",
            "void RuntimeFeatureManager::clearRuntimeDisabled(",
        )
        with tempfile.TemporaryDirectory(prefix="fastbee-feature-boot-") as directory:
            temporary = Path(directory)
            (temporary / "feature_boot_methods.inc").write_text(
                "\n\n".join(definition(source, signature) for signature in signatures), encoding="utf-8")
            executable = temporary / ("features.exe" if os.name == "nt" else "features")
            command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                       "-DARDUINOJSON_ENABLE_ARDUINO_STRING=0", "-DARDUINOJSON_ENABLE_STD_STRING=1",
                       f"-I{temporary}", f"-I{ROOT / 'test/mocks'}", f"-I{ROOT / 'include'}",
                       f"-I{json_headers[0].parent}",
                       str(ROOT / "test/unit/cpp/feature_boot_snapshot_harness.cpp"), "-o", str(executable)]
            result = subprocess.run(command, capture_output=True, text=True,
                                    encoding="utf-8", errors="replace", timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
