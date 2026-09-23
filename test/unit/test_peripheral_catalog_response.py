"""Exercise the real catalog descriptors/handler and bounded response with ArduinoJson."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from test.unit.test_generic_peripheral_route_contract import definition

ROOT = Path(__file__).resolve().parents[2]


class PeripheralCatalogResponseTests(unittest.TestCase):
    def test_full_catalog_equivalence_memory_and_response_lifetime(self):
        compiler = shutil.which("g++") or "D:/msys64/ucrt64/bin/g++.exe"
        headers = list((ROOT / ".pio/libdeps").glob("*/ArduinoJson/src/ArduinoJson.h"))
        self.assertTrue(headers)
        source = "\n".join(definition(name) for name in (
            "addSensorDriverDescriptors", "addIoExpanderDriverDescriptors",
            "addPeripheralTypeDescriptorDetails",
            "PeripheralRouteHandler::handleGetPeripheralTypes"))
        mock = (ROOT / "test/mocks/Arduino.h").read_text(encoding="utf-8")
        mock = mock.replace("class String {", "inline int catalogReserveFailure = -1;\n"
                            "inline bool catalogConcatFailure = false;\nclass String {")
        mock = mock.replace("void reserve(size_t size) { _value.reserve(size); }", """
    bool reserve(size_t size) {
        if (catalogReserveFailure == 0) return false;
        if (catalogReserveFailure > 0) --catalogReserveFailure;
        _value.reserve(size); return true;
    }
    bool concat(const char* bytes, size_t count) {
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
                    f"-I{ROOT / 'include'}", f"-I{headers[0].parent}",
                    str(ROOT / "test/unit/cpp/peripheral_catalog_response_harness.cpp"),
                    "-o", str(executable)]
                if screen: command.insert(1, "-DCONFIG_IDF_TARGET_ESP32S3=1")
                built = subprocess.run(command, capture_output=True, text=True, env=environment, timeout=90)
                self.assertEqual(0, built.returncode, built.stdout + built.stderr)
                run = subprocess.run([str(executable)], capture_output=True, text=True, env=environment, timeout=20)
                self.assertEqual(0, run.returncode, run.stdout + run.stderr)
                print(run.stdout.strip())


if __name__ == "__main__": unittest.main()
