"""Exercise the real catalog descriptors/handler and bounded response with ArduinoJson."""
import os
import hashlib
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from test.unit.test_generic_peripheral_route_contract import definition
from test_peripheral_resident_shared import checked_catalog, frozen_graph
from test_peripheral_route_fixed_json import definition as source_definition
from test_sensor_publication import definition as braced_definition

ROOT = Path(__file__).resolve().parents[2]


class PeripheralCatalogResponseTests(unittest.TestCase):
    def test_full_catalog_equivalence_memory_and_response_lifetime(self):
        compiler = shutil.which("g++") or "D:/msys64/ucrt64/bin/g++.exe"
        headers = list((ROOT / ".pio/libdeps").glob("*/ArduinoJson/src/ArduinoJson.h"))
        self.assertTrue(headers)
        source = "\n".join(definition(name) for name in (
            "addSensorDriverDescriptors", "addIoExpanderDriverDescriptors",
            "addScreenDisplayDescriptor", "addPeripheralTypeDescriptorDetails",
            "PeripheralRouteHandler::handleGetPeripheralTypes"))
        catalog_source = (ROOT / 'src/network/handlers/PeripheralCatalogRoutes.cpp').read_text(encoding='utf-8')
        source = source_definition(catalog_source, 'catalogLiteralFragment') + '\n' + definition('appendCatalogCopiedValues') + '\n' + source
        source = checked_catalog(catalog_source) + '\n' + source
        streamed = braced_definition(catalog_source, 'class SensorDriverCatalogFragment') + ';\n'
        streamed += source_definition(catalog_source, 'addSensorCatalogFragment') + '\n'
        # Exercise the actual private serializer body independently of its wire
        # representation, preserving scalar types and linked-string ownership.
        streamed += source_definition(catalog_source, 'writeDescriptor').replace(
            'void writeDescriptor(', 'void streamedSensorDescriptor(', 1) + '\n'
        # The copied-value helper must precede the stream factory.
        anchor = definition('addPeripheralTypeDescriptorDetails')
        source = source.replace(anchor, streamed + anchor, 1)
        legacy = source_definition(frozen_graph()[1], 'PeripheralRouteHandler::handleGetPeripheralTypes')
        legacy = legacy.replace('void PeripheralRouteHandler::handleGetPeripheralTypes(AsyncWebServerRequest* request)',
            'void legacyCatalogHandler(PeripheralRouteHandler* handler, AsyncWebServerRequest* request)')
        legacy = legacy.replace('ctx->requireAuth', 'handler->ctx->requireAuth')
        source += '\n' + legacy
        before_path = ROOT / 'test/unit/cpp/peripheral_catalog_response_before_r2.inc'
        before = before_path.read_bytes()
        self.assertEqual(hashlib.sha256(before).hexdigest(), '036561c1f3d2fd1b8f0416a961a92914fe1efad28ca677eee7afb24965c6556c')
        before_method = source_definition(before.decode('utf-8'), 'PeripheralRouteHandler::handleGetPeripheralTypes')
        before_method = before_method.replace(
            'void PeripheralRouteHandler::handleGetPeripheralTypes(AsyncWebServerRequest* request)',
            'void beforeR2CatalogHandler(PeripheralRouteHandler* handler, AsyncWebServerRequest* request)')
        source += '\n' + before_method.replace('ctx->requireAuth', 'handler->ctx->requireAuth')
        mock = (ROOT / "test/mocks/Arduino.h").read_text(encoding="utf-8")
        mock = mock.replace("class String {", "inline int catalogReserveFailure = -1;\n"
                            "inline bool catalogConcatFailure = false;\n"
                            "inline size_t catalogReservedBytes = 0, catalogStringPeak = 0, catalogJsonBytes = 0, catalogCombinedPeak = 0;\n"
                            "inline void catalogTrackMemory() { catalogStringPeak = std::max(catalogStringPeak, catalogReservedBytes); "
                            "catalogCombinedPeak = std::max(catalogCombinedPeak, catalogReservedBytes + catalogJsonBytes); }\nclass String {")
        mock = mock.replace('    String() = default;', '''
    size_t catalogReservation = 0;
    String() = default;
    String(const String& other) : _value(other._value) {}
    String(String&& other) noexcept : catalogReservation(other.catalogReservation), _value(std::move(other._value)) {
        other.catalogReservation = 0;
    }
    String& operator=(const String& other) {
        catalogReservedBytes -= catalogReservation; catalogReservation = 0;
        _value = other._value; return *this;
    }
    String& operator=(String&& other) noexcept {
        catalogReservedBytes -= catalogReservation;
        catalogReservation = other.catalogReservation; other.catalogReservation = 0;
        _value = std::move(other._value); return *this;
    }
    ~String() { catalogReservedBytes -= catalogReservation; }
''')
        reserve = "bool reserve(size_t size) { _value.reserve(size); return true; }"
        concat = "bool concat(const char* value, unsigned int length) { if (!value && length) return false; if (length) _value.append(value, length); return true; }"
        # Fail explicitly if the host String API changes; a silent replacement
        # miss would turn these OOM/short-write checks into ordinary requests.
        self.assertEqual(mock.count(reserve), 1)
        self.assertEqual(mock.count(concat), 1)
        mock = mock.replace(reserve, """
    bool reserve(size_t size) {
        if (catalogReserveFailure == 0) return false;
        if (catalogReserveFailure > 0) --catalogReserveFailure;
        // ESP String rounds capacities to 16 bytes; each heap block also has
        // allocator bookkeeping. Keep these distinct from host std::string.
        const size_t reserved = ((size + 16) / 16) * 16 + 8;
        if (reserved > catalogReservation) {
            catalogReservedBytes += reserved - catalogReservation;
            catalogReservation = reserved;
            catalogTrackMemory();
        }
        _value.reserve(size); return true;
    }
""")
        mock = mock.replace(concat, """
    bool concat(const char* bytes, unsigned int count) {
        if (catalogConcatFailure) return false;
        _value.append(bytes, count); return true;
    }
""")
        with tempfile.TemporaryDirectory(prefix="fastbee-catalog-") as directory:
            temporary = Path(directory)
            (temporary / "Arduino.h").write_text(mock, encoding="utf-8")
            (temporary / "ESPAsyncWebServer.h").write_text("""#pragma once
#include <Arduino.h>
class AsyncAbstractResponse {
public:
 virtual ~AsyncAbstractResponse()=default;
 virtual bool _sourceValid() const { return false; }
 virtual size_t _fillBuffer(uint8_t*,size_t)=0;
 int _code=0; String _contentType;
 size_t _contentLength=0,_sentLength=0; bool _sendContentLength=false;
};
""", encoding="utf-8")
            (temporary / "catalog_under_test.inc").write_text(source, encoding="utf-8")
            environment = dict(os.environ)
            environment["PATH"] = str(Path(compiler).parent) + os.pathsep + environment.get("PATH", "")
            for screen in (0, 1):
                executable = temporary / f"catalog-{screen}.exe"
                command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    "-DARDUINOJSON_ENABLE_ARDUINO_STRING=0", "-DARDUINOJSON_ENABLE_STD_STRING=1",
                    "-DFASTBEE_ENABLE_IO_EXPANDER=1",
                    f"-DFASTBEE_ENABLE_SCREEN_UI={screen}", f"-I{temporary}",
                    f"-I{ROOT / 'test/unit/cpp'}",
                    f"-I{ROOT / 'include'}", f"-I{headers[0].parent}",
                    str(ROOT / "test/unit/cpp/peripheral_catalog_response_harness.cpp"),
                    str(ROOT / "src/core/H1ParameterJson.cpp"),
                    str(ROOT / "src/network/FixedJsonField.cpp"),
                    str(ROOT / "src/utils/SharedJsonSerializer.cpp"),
                    "-o", str(executable)]
                if screen: command.insert(1, "-DCONFIG_IDF_TARGET_ESP32S3=1")
                built = subprocess.run(command, capture_output=True, text=True, env=environment, timeout=90)
                self.assertEqual(0, built.returncode, built.stdout + built.stderr)
                run = subprocess.run([str(executable)], capture_output=True, text=True, env=environment, timeout=20)
                self.assertEqual(0, run.returncode, run.stdout + run.stderr)
                print(run.stdout.strip())


if __name__ == "__main__": unittest.main()
