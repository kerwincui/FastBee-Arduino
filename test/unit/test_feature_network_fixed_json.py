"""Actual feature/network serializer differentials; no hardware backends."""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = next(path for path in Path(__file__).resolve().parents
            if (path / "platformio.ini").is_file())
HERE = Path(__file__).resolve().parent
CPP = HERE if (HERE / "feature_status_serializer_harness.cpp").is_file() else ROOT / "test/unit/cpp"
sys.path.insert(0, str(ROOT / "test/unit"))
from test_sensor_publication import definition

class SerializerFixtureBase(unittest.TestCase):
    @classmethod
    def prepare(cls, fixture_names, source_names):
        cls.compiler = shutil.which("g++") or shutil.which("clang++")
        if not cls.compiler:
            raise RuntimeError("C++17 compiler required for actual serializer coverage")
        json_root = os.environ.get("FASTBEE_TEST_ARDUINOJSON_INCLUDE")
        if not json_root:
            installed = sorted((ROOT / ".pio/libdeps").glob("*/ArduinoJson/src/ArduinoJson.h"))
            if not installed:
                raise RuntimeError("Installed ArduinoJson required")
            json_root = str(installed[0].parent)
        cls.library = Path(json_root)
        cls.inputs = [Path(__file__), ROOT / "test/unit/test_sensor_publication.py",
                      ROOT / "test/mocks/Arduino.h",
                      ROOT / "include/network/FixedJsonField.h",
                      ROOT / "src/network/FixedJsonField.cpp",
                      CPP / "serializer_trace_allocator.h"]
        cls.inputs += [ROOT / name for name in source_names]
        cls.inputs += [CPP / name for name in fixture_names]
        cls.inputs += sorted(cls.library.rglob("*.h")) + sorted(cls.library.rglob("*.hpp"))
        cls.before = {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in cls.inputs}
        cls.folder = tempfile.TemporaryDirectory(prefix="fastbee-feature-network-json-")
        cls.addClassCleanup(cls.folder.cleanup)
        cls.addClassCleanup(cls.verify_inputs)
        cls.temp = Path(cls.folder.name)
        (cls.temp / 'fixture_arduino_prefix.h').write_text(
            '#include <Arduino.h>\n'
            '#if defined(FASTBEE_TEST_ARDUINO_HEX_MACRO)\n#define HEX 16\n#endif\n'
            '#undef malloc\n#undef free\n', encoding='utf-8')

    @classmethod
    def verify_inputs(cls):
        after = {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in cls.inputs}
        if cls.before != after:
            raise AssertionError("Serializer inputs changed during tests")

    @classmethod
    def compile(cls, fixture, name, flags, extra_sources=()):
        executable = cls.temp / (name + ".exe")
        command = [cls.compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                   "-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0", "-DARDUINOJSON_ENABLE_STD_STRING=1",
                   *flags, "-I" + str(cls.temp), "-I" + str(CPP),
                   "-I" + str(ROOT / "test/mocks"), "-I" + str(ROOT / "include"),
                   "-include", str(cls.temp / "fixture_arduino_prefix.h"),
                   "-I" + str(cls.library), str(CPP / fixture),
                   str(ROOT / "src/network/FixedJsonField.cpp"),
                   *(str(ROOT / source) for source in extra_sources), "-o", str(executable)]
        result = subprocess.run(command, capture_output=True, text=True,
                                encoding="utf-8", errors="replace", timeout=90)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)
        return executable

    def check(self, binary, mode):
        result = subprocess.run([str(binary), mode], capture_output=True, text=True,
                                encoding="utf-8", errors="replace", timeout=90)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

class FeatureStatusSerializerTests(SerializerFixtureBase):
    @classmethod
    def setUpClass(cls):
        cls.prepare(("feature_status_serializer_harness.cpp", "feature_status_oracle.inc"),
                    ("src/core/RuntimeFeatureManager.cpp", "include/core/RuntimeFeatureManager.h",
                     "include/core/FeatureInstanceRegistry.h"))
        source = (ROOT / "src/core/RuntimeFeatureManager.cpp").read_text(encoding="utf-8")
        header = (ROOT / "include/core/RuntimeFeatureManager.h").read_text(encoding="utf-8")
        oracle_header = header.replace("FASTBEE_RUNTIME_FEATURE_MANAGER_H", "FASTBEE_ORACLE_FEATURE_MANAGER_H")
        oracle_header = oracle_header.replace("RuntimeFeatureManager", "OracleFeatureManager")
        (cls.temp / "oracle_feature_manager.h").write_text(oracle_header, encoding="utf-8")
        signatures = (
            "const RuntimeFeatureManager::FeatureState* RuntimeFeatureManager::findFeature(",
            "bool RuntimeFeatureManager::shouldEnable(",
            "const char* RuntimeFeatureManager::admissionState(",
            "bool RuntimeFeatureManager::getFeatureStatusJson(",
            "void RuntimeFeatureManager::getStatusSummaryJson(")
        (cls.temp / "feature_status_actual.inc").write_text(
            "\n\n".join(definition(source, signature) for signature in signatures), encoding="utf-8")
        bundles = (definition(source, "struct FeatureBundle {") + ";\n" +
                   definition(source, "constexpr FeatureBundle kFeatureBundles[]") + ";\n")
        (cls.temp / "feature_bundle_types.inc").write_text(bundles, encoding="utf-8")
        cls.host = cls.compile("feature_status_serializer_harness.cpp", "feature-host",
                               ["-DARDUINOJSON_ENABLE_ARDUINO_STRING=0"])
        cls.sdk = cls.compile("feature_status_serializer_harness.cpp", "feature-sdk",
                              ["-DARDUINOJSON_ENABLE_ARDUINO_STRING=0", "-DESP_PLATFORM=1"])

    def test_feature_bytes_ownership_observer_and_oom_host(self):
        self.check(self.host, "feature")

    def test_feature_bytes_ownership_observer_and_oom_sdk(self):
        self.check(self.sdk, "feature")

    def test_summary_accounting_and_oom_host(self):
        self.check(self.host, "summary")

    def test_summary_getter_order_and_oom_sdk(self):
        self.check(self.sdk, "summary")

class FeatureStatusCatalogTests(FeatureStatusSerializerTests):
    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        additional = (
            "include/network/FeatureStatusCatalog.h", "src/network/FeatureStatusCatalog.cpp",
            "include/network/FeatureStatusResponse.h", "test/unit/cpp/feature_status_catalog_harness.cpp")
        for name in additional:
            path = ROOT / name
            cls.inputs.append(path)
            cls.before[str(path)] = hashlib.sha256(path.read_bytes()).hexdigest()
        (cls.temp / "utils").mkdir()
        (cls.temp / "utils/PsramJsonDocument.h").write_text(
            '#pragma once\n#include <ArduinoJson.h>\n'
            'extern ArduinoJson::Allocator* catalogAllocator;\n'
            'namespace FastBee { inline JsonDocument makeJsonDocument(size_t = 8192) '
            '{ return catalogAllocator ? JsonDocument(catalogAllocator) : JsonDocument(); } }\n', encoding="utf-8")
        (cls.temp / "ESPAsyncWebServer.h").write_text(
            '#pragma once\n#include <cstddef>\n#include <cstdint>\n#include <string>\n'
            'class AsyncAbstractResponse { public: virtual ~AsyncAbstractResponse()=default; '
            'virtual bool _sourceValid() const=0; virtual size_t _fillBuffer(uint8_t*,size_t)=0; '
            'int _code=0; std::string _contentType; size_t _contentLength=0; '
            'bool _sendContentLength=false; };\n', encoding="utf-8")
        cls.catalog = cls.compile("feature_status_catalog_harness.cpp", "feature-catalog",
            ["-DARDUINOJSON_ENABLE_ARDUINO_STRING=0", "-DESP_PLATFORM=1", "-DFASTBEE_TEST_ARDUINO_HEX_MACRO=1"],
            ("src/network/FeatureStatusCatalog.cpp",))

    def test_snapshot_matches_existing_schema_and_never_rereads_live_state(self):
        self.check(self.catalog, "snapshot")

    def test_small_slices_disconnect_and_preflight_reject_oversize_or_oom(self):
        self.check(self.catalog, "failure")

class NetworkSerializerTests(SerializerFixtureBase):
    @classmethod
    def setUpClass(cls):
        cls.prepare(("network_serializer_harness.cpp", "network_serializer_oracle.inc"),
                    ("src/utils/JsonSerializationHelper.cpp", "include/utils/JsonSerializationHelper.h",
                     "include/network/WiFiManager.h", "include/core/SystemConstants.h"))
        source = (ROOT / "src/utils/JsonSerializationHelper.cpp").read_text(encoding="utf-8")
        header = (ROOT / "include/utils/JsonSerializationHelper.h").read_text(encoding="utf-8")
        wifi = (ROOT / "include/network/WiFiManager.h").read_text(encoding="utf-8")
        type_signatures = (
            "enum class NetworkType", "enum class NetworkMode", "enum class IPConfigType",
            "enum class NetworkStatus", "enum class IPConflictMode", "enum class IPFailoverStrategy",
            "struct WiFiNetwork", "struct EthernetConfig", "struct CellularConfig",
            "struct WiFiConfig", "struct NetworkStatusInfo")
        actual_class = definition(header, "class JsonSerializationHelper")
        declarations = "\n\n".join(definition(wifi, signature) + ";" for signature in type_signatures)
        declarations += "\n" + actual_class + ";\n"
        declarations += actual_class.replace("JsonSerializationHelper", "OracleNetworkSerializer") + ";\n"
        (cls.temp / "network_serializer_types.inc").write_text(declarations, encoding="utf-8")
        signatures = (
            "bool JsonSerializationHelper::wifiConfigToJson(",
            "void JsonSerializationHelper::networkStatusToJson(",
            "String JsonSerializationHelper::fullNetworkInfoToJson(",
            "String JsonSerializationHelper::networkModeToString(",
            "String JsonSerializationHelper::ipConfigTypeToString(",
            "String JsonSerializationHelper::networkStatusToString(")
        (cls.temp / "network_serializer_actual.inc").write_text(
            "\n\n".join(definition(source, signature) for signature in signatures), encoding="utf-8")
        # Keep the original deprecated createNestedArray oracle as a warning.
        cls.binary = cls.compile("network_serializer_harness.cpp", "network",
                                 ["-DARDUINOJSON_ENABLE_ARDUINO_STRING=1",
                                  "-Wno-error=deprecated-declarations"])

    def test_configuration_fields_backup_arrays_ownership_and_oom(self):
        self.check(self.binary, "config")

    def test_network_status_enum_strings_boundaries_and_oom(self):
        self.check(self.binary, "status")

    def test_full_network_json_getter_order_returned_bytes_and_oom(self):
        self.check(self.binary, "full")

if __name__ == "__main__":
    unittest.main()
