"""Exercise production sensor commit/claim methods with real timed mutexes.

Only unrelated platform, transport and history backends are substituted. PM
snapshot/commit and observation cache methods are compiled verbatim from source.
"""
import os
import re
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def definition(source, signature):
    start = source.index(signature)
    # Braces in character/string literals and comments are not C++ scopes.
    masked = re.sub(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*[\s\S]*?\*/',
                    lambda match: " " * len(match.group()), source)
    opening = masked.index("{", start)
    depth = 0
    for index in range(opening, len(source)):
        if masked[index] == "{":
            depth += 1
        elif masked[index] == "}":
            depth -= 1
            if not depth:
                return source[start:index + 1]
    raise AssertionError(signature)


class SensorPublicationTests(unittest.TestCase):
    def test_production_commit_claim_invalidation_and_callback_lock_order(self):
        compiler = shutil.which("g++") or shutil.which("clang++")
        if not compiler:
            self.skipTest("A C++17 compiler is required")
        json_headers = list((ROOT / ".pio/libdeps").glob("*/ArduinoJson/src/ArduinoJson.h"))
        if not json_headers:
            self.skipTest("Install ArduinoJson first")
        manager = (ROOT / "src/core/PeriphExecManager.cpp").read_text(encoding="utf-8")
        header = (ROOT / "include/core/PeriphExecManager.h").read_text(encoding="utf-8")
        pm = (ROOT / "src/core/PeripheralManager.cpp").read_text(encoding="utf-8")
        guards = (ROOT / "include/core/AsyncExecTypes.h").read_text(encoding="utf-8")
        snippets = [definition(pm, signature) for signature in (
            "bool PeripheralManager::getPeripheralSnapshot(",
            "bool PeripheralManager::commitSensorRead(",
            "void PeripheralManager::invalidateSensorConfigurationLocked(",
        )]
        snippets += [definition(manager, signature) for signature in (
            "bool PeriphExecManager::updateSensorReadCache(",
            "bool PeriphExecManager::upsertObservation(",
            "void PeriphExecManager::publishObservation(",
            "bool PeriphExecManager::upsertSensorObservationBatch(",
            "void PeriphExecManager::invalidateSensorReadCache(",
            "void PeriphExecManager::invalidateAllSensorReadCache(",
            "std::map<String, PeriphExecManager::SensorReadCache> PeriphExecManager::getSensorReadCacheCopy(",
            "bool PeriphExecManager::lookupSensorCache(",
            "bool PeriphExecManager::getObservation(",
            "std::vector<ObservationSnapshot> PeriphExecManager::getObservationSnapshots(",
            "std::vector<ObservationSnapshot> PeriphExecManager::getObservationSnapshots(\n    const std::vector<const char*>&",
            "void PeriphExecManager::evictStaleSensorCache(",
        )]
        with tempfile.TemporaryDirectory(prefix="fastbee-sensor-publication-") as directory:
            temporary = Path(directory)
            (temporary / "publication_methods.inc").write_text("\n\n".join(snippets), encoding="utf-8")
            (temporary / "publication_types.inc").write_text(
                "\n".join(definition(header, item) + ";" for item in (
                    "enum class ObservationQuality", "struct ObservationUpdate", "struct ObservationSnapshot")),
                encoding="utf-8")
            (temporary / "publication_cache.inc").write_text(
                definition(header, "struct SensorReadCache") + ";", encoding="utf-8")
            (temporary / "publication_guards.inc").write_text(
                "\n".join(definition(guards, "class " + item) + ";"
                          for item in ("MutexGuard", "RecursiveMutexGuard")), encoding="utf-8")
            executable = temporary / ("publication.exe" if os.name == "nt" else "publication")
            command = [compiler, "-std=c++17", "-O2", "-fstack-usage", "-pthread", "-Wall", "-Wextra", "-Werror",
                       "-DARDUINOJSON_ENABLE_ARDUINO_STRING=0", "-DARDUINOJSON_ENABLE_STD_STRING=1",
                       f"-I{temporary}", f"-I{ROOT / 'test/mocks'}", f"-I{ROOT / 'include'}",
                       f"-I{json_headers[0].parent}",
                       str(ROOT / "test/unit/cpp/sensor_publication_harness.cpp"), "-o", str(executable)]
            result = subprocess.run(command, capture_output=True, text=True,
                                    encoding="utf-8", errors="replace", timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            print(result.stdout.strip())
            for usage in temporary.glob("*.su"):
                for line in usage.read_text().splitlines():
                    if "bool PeriphExecManager::upsertSensorObservationBatch(" in line and "lambda" not in line:
                        print("host stack (-O2, mocked RTOS): " + line)

    def test_execution_and_http_carry_the_snapshot_version(self):
        executor = (ROOT / "src/core/PeriphExecExecutor.cpp").read_text(encoding="utf-8")
        route = (ROOT / "src/network/handlers/PeripheralRouteHandler.cpp").read_text(encoding="utf-8")
        action = definition(executor, "bool PeriphExecExecutor::executeSensorReadAction(")
        read = definition(route, "void PeripheralRouteHandler::handleReadPeripheral(")
        self.assertIn("boundConfig, &sensorConfigVersion", action)
        self.assertIn("&sensorSampleTimestamp, sensorConfigVersion, collectionStartedAt", action)
        execution = definition(executor, "std::vector<ActionExecResult> PeriphExecExecutor::executeAllActions(")
        self.assertIn("const uint32_t collectionStartedAt = static_cast<uint32_t>(millis())", execution)
        self.assertIn("executeSensorReadAction(action, ar, rule.recordHistory, &collectionStartedAt)", execution)
        self.assertIn('sensorSampleTimestamp, "", sensorConfigVersion)', action)
        self.assertIn("config, &sensorConfigVersion", read)
        self.assertIn("&sampleTimestamp, sensorConfigVersion", read)
        commit = definition(read, "const bool committed = pm.commitSensorRead(")
        self.assertIn('data["value"] = value', commit)
        self.assertNotIn("send", commit)
        self.assertNotIn("readChannel", commit)
        lcd = (ROOT / "src/peripherals/LCDManager.cpp").read_text(encoding="utf-8")
        self.assertIn("SensorConfigEpoch::matches(entry.configVersion)", lcd)


if __name__ == "__main__":
    unittest.main()
