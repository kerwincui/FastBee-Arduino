"""Two resident real NetworkManager methods versus frozen originals and mechanical draft.

Real ArduinoJson, field helper, config/status types and RSSI conversion are used.
SDK WiFi, clock, NVS and file persistence are explicit non-device seams.
"""
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
ROOT = Path(os.environ["FASTBEE_TEST_ROOT"]) if os.environ.get("FASTBEE_TEST_ROOT") else next(
    parent for parent in HERE.parents if (parent / "platformio.ini").is_file())
SUPPORT = Path(os.environ.get("FASTBEE_TEST_SUPPORT_ROOT", ROOT))
sys.path.insert(0, str(SUPPORT / "test/unit"))
from test_periph_exec_fixed_json import definition, local_includes

SIGNATURES = ("bool FBNetworkManager::saveNetworkConfig(", "String FBNetworkManager::scanNetworks(")
COUNTS = (40, 6)


def scalar_draft(source, expected):
    result, count = re.subn(r'(\w+)\["([^"\n]+)"\]\s*=\s*([^;\n]+);',
        lambda match: f'FastBee::setFixedJsonField({match[1]}, "{match[2]}", {match[3]});', source)
    if count != expected:
        raise AssertionError(f"Reviewed NetworkManager assignment count changed: {count} != {expected}")
    return result


def renamed(functions, suffix):
    return "\n\n".join(body.replace("FBNetworkManager::", "Network" + suffix + "::", 1)
                      for body in functions)


class NetworkManagerFixedJsonTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("g++") or "D:/msys64/ucrt64/bin/g++.exe"
        if not Path(compiler).is_file():
            raise RuntimeError("C++17 compiler required")
        library = Path(os.environ["FASTBEE_TEST_ARDUINOJSON_INCLUDE"]) if os.environ.get(
            "FASTBEE_TEST_ARDUINOJSON_INCLUDE") else SUPPORT / ".pio/libdeps/native/ArduinoJson/src"
        if not (library / "ArduinoJson.h").is_file():
            raise RuntimeError("Registered real ArduinoJson headers required")
        source_path = ROOT / "src/network/NetworkManager.cpp"
        types_path = ROOT / "include/network/WiFiManager.h"
        utils_path = ROOT / "src/utils/NetworkUtils.cpp"
        oracle_path = HERE / "cpp/network_manager_fixed_json_oracle.inc"
        harness_path = HERE / "cpp/network_manager_fixed_json_harness.cpp.in"
        source = source_path.read_text(encoding="utf-8")
        oracle = oracle_path.read_text(encoding="utf-8")
        old = [definition(oracle, signature) for signature in SIGNATURES]
        proposed = [scalar_draft(body, count) for body, count in zip(old, COUNTS)]
        current = [definition(source, signature) for signature in SIGNATURES]
        for actual, baseline, draft in zip(current, old, proposed):
            if actual not in (baseline, draft):
                raise AssertionError("NetworkManager changed beyond the reviewed 46 fixed writes")
        types = types_path.read_text(encoding="utf-8")
        type_signatures = (
            "enum class NetworkType", "enum class NetworkMode", "enum class IPConfigType",
            "enum class NetworkStatus", "enum class IPConflictMode", "enum class IPFailoverStrategy",
            "struct WiFiNetwork", "struct EthernetConfig", "struct CellularConfig",
            "struct WiFiConfig", "struct NetworkStatusInfo")
        program = harness_path.read_text(encoding="utf-8")
        replacements = {
            "@NETWORK_TYPES@": "\n\n".join(definition(types, signature) + ";" for signature in type_signatures),
            "@RSSI_FUNCTION@": definition(utils_path.read_text(encoding="utf-8"),
                                          "uint8_t NetworkUtils::rssiToPercentage("),
            "@NETWORK_WRITERS@": "\n\n".join(renamed(functions, suffix) for functions, suffix in
                ((old, "Oracle"), (current, "Current"), (proposed, "Draft")))
        }
        for marker, text in replacements.items():
            if program.count(marker) != 1:
                raise AssertionError("Expected one marker: " + marker)
            program = program.replace(marker, text)
        helper = ROOT / "src/network/FixedJsonField.cpp"
        prelude = SUPPORT / "test/unit/cpp/fixed_json_document_prelude.h"
        mock = SUPPORT / "test/mocks/Arduino.h"
        roots = (HERE / "cpp", SUPPORT / "test/unit/cpp", ROOT / "include",
                 SUPPORT / "include", SUPPORT / "test/mocks", library)
        seeds = (Path(__file__), source_path, types_path, utils_path, oracle_path, harness_path,
                 helper, prelude, mock, ROOT / "include/network/FixedJsonField.h",
                 ROOT / "include/core/SystemConstants.h", SUPPORT / "test/unit/cpp/document_json_probe.h",
                 SUPPORT / "test/unit/test_periph_exec_fixed_json.py")
        cls.inputs = sorted(local_includes(seeds, roots) |
                            set(library.rglob("*.h")) | set(library.rglob("*.hpp")))
        cls.hashes = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in cls.inputs}
        cls.directory = tempfile.TemporaryDirectory(prefix="fastbee-network-manager-json-")
        cls.addClassCleanup(cls.directory.cleanup)
        temporary = Path(cls.directory.name)
        (temporary / "Arduino.h").write_text(mock.read_text(encoding="utf-8"), encoding="utf-8")
        cpp = temporary / "network-manager-fixed-json.cpp"
        cpp.write_text(program, encoding="utf-8")
        cls.binary = temporary / ("network-manager-fixed-json.exe" if os.name == "nt" else "network-manager-fixed-json")
        cls.environment = dict(os.environ)
        cls.environment["PATH"] = str(Path(compiler).parent) + os.pathsep + cls.environment.get("PATH", "")
        result = subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
            "-Wno-error=deprecated-declarations", "-Wno-error=format", "-DUNIT_TEST",
            "-DARDUINOJSON_ENABLE_ARDUINO_STRING=1", "-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0",
            "-DARDUINOJSON_ENABLE_STD_STRING=1", "-I" + str(temporary),
            *["-I" + str(p) for p in roots], "-include", str(prelude), str(cpp), str(helper),
            "-o", str(cls.binary)], capture_output=True, text=True, encoding="utf-8", errors="replace",
            timeout=120, env=cls.environment)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    @classmethod
    def tearDownClass(cls):
        if cls.hashes != {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in cls.inputs}:
            raise AssertionError("NetworkManager JSON inputs changed during validation")

    def check(self, mode):
        ran = subprocess.run([str(self.binary), mode], capture_output=True, text=True,
                             encoding="utf-8", errors="replace", timeout=180, env=self.environment)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
        self.assertIn("PASS " + mode, ran.stdout)


for _mode in ("save-values", "save-oom", "scan-values", "scan-oom"):
    setattr(NetworkManagerFixedJsonTests, "test_" + _mode.replace("-", "_"),
            lambda self, mode=_mode: self.check(mode))

if __name__ == "__main__":
    unittest.main()

