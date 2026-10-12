"""Actual compiled serializer and production sinks; synchronous byte-trace equivalence."""
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SUPPORT = Path(os.environ.get("FASTBEE_TEST_SUPPORT_ROOT", ROOT))


def production_class(source, marker):
    if source.count(marker) != 1:
        raise AssertionError("Writer definition not unique: " + marker)
    start = source.index(marker)
    masked = re.sub(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
                    lambda match: " " * len(match.group()), source)
    opening = masked.index("{", start)
    depth = 0
    for at in range(opening, len(masked)):
        if masked[at] == "{":
            depth += 1
        elif masked[at] == "}":
            depth -= 1
            if depth == 0:
                if source[at + 1] != ";":
                    raise AssertionError("Expected writer class terminator")
                return source[start:at + 2]
    raise AssertionError("Unterminated " + marker)


class SharedJsonSerializerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("g++") or "D:/msys64/ucrt64/bin/g++.exe"
        if not Path(compiler).is_file():
            raise RuntimeError("C++17 compiler required")
        library = SUPPORT / ".pio/libdeps/native/ArduinoJson/src"
        if not (library / "ArduinoJson.h").is_file():
            raise RuntimeError("Registered native ArduinoJson required")
        relative = (
            "include/utils/SharedJsonSerializer.h", "src/utils/SharedJsonSerializer.cpp",
            "include/network/JsonDocumentResponse.h", "include/network/JsonFragmentsResponse.h",
            "include/core/CheckedReportTemplate.h", "include/utils/CheckedText.h",
            "src/core/PeriphExecConfigStore.cpp", "test/unit/test_shared_json_serializer.py",
            "test/unit/cpp/shared_json_serializer_harness.cpp.in",
            "test/unit/cpp/shared_json_serializer_arduino.h")
        cls.inputs = [ROOT / name for name in relative]
        cls.inputs += sorted(library.rglob("*.h")) + sorted(library.rglob("*.hpp"))
        cls.hashes = {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in cls.inputs}
        cls.temporary = tempfile.TemporaryDirectory(prefix="fastbee-shared-json-")
        cls.addClassCleanup(cls.temporary.cleanup)
        temp = Path(cls.temporary.name)
        (temp / "utils").mkdir()
        shutil.copyfile(ROOT / "test/unit/cpp/shared_json_serializer_arduino.h", temp / "Arduino.h")
        (temp / "ESPAsyncWebServer.h").write_text("""#pragma once
#include <Arduino.h>
class AsyncAbstractResponse {
public:
 virtual ~AsyncAbstractResponse()=default;
 virtual bool _sourceValid() const=0;
 virtual size_t _fillBuffer(uint8_t*,size_t)=0;
 int _code=0; String _contentType;
 size_t _contentLength=0,_sentLength=0; bool _sendContentLength=false;
};
""", encoding="utf-8")
        (temp / "utils/PsramJsonDocument.h").write_text("""#pragma once
#include <cstddef>
constexpr int MALLOC_CAP_SPIRAM=1,MALLOC_CAP_8BIT=2;
void* heap_caps_malloc(size_t,int); void heap_caps_free(void*);
namespace FastBee { bool psramAvailableForJson(size_t=8192); }
""", encoding="utf-8")
        response = (ROOT / "include/network/JsonDocumentResponse.h").read_text(encoding="utf-8")
        fragments = (ROOT / "include/network/JsonFragmentsResponse.h").read_text(encoding="utf-8")
        report = (ROOT / "include/core/CheckedReportTemplate.h").read_text(encoding="utf-8")
        store = (ROOT / "src/core/PeriphExecConfigStore.cpp").read_text(encoding="utf-8")
        definitions = "\n".join((production_class(response, "class RangeWriter {"),
            production_class(fragments, "struct AppendWriter {"),
            production_class(store, "class ConfigStringAppender {")))
        # The independent oracle is the current complete business path with
        # only the one changed serializer call restored to the pre-refactor API.
        for name, text in (("response", response), ("fragments", fragments), ("template", report)):
            if text.count("serializeJsonShared(") != 1:
                raise AssertionError(name + " must have exactly one shared serializer call")
        legacy_response = response.replace("JsonDocumentResponse", "LegacyJsonDocumentResponse")
        legacy_fragments = fragments.replace("JsonFragmentsResponse", "LegacyJsonFragmentsResponse")
        (temp / "legacy_responses.inc").write_text(
            (legacy_response + "\n" + legacy_fragments).replace("serializeJsonShared(", "serializeJson("),
            encoding="utf-8")
        legacy_report = report.replace("CheckedTemplateWriter", "LegacyCheckedTemplateWriter")
        legacy_report = legacy_report.replace("checkedTemplateValue", "legacyCheckedTemplateValue")
        legacy_report = legacy_report.replace("applyCheckedReportTemplate", "legacyApplyCheckedReportTemplate")
        (temp / "legacy_template.inc").write_text(
            legacy_report.replace("serializeJsonShared(", "serializeJson("), encoding="utf-8")
        fixture = (ROOT / "test/unit/cpp/shared_json_serializer_harness.cpp.in").read_text(encoding="utf-8")
        if fixture.count("@PRODUCTION_SINKS@") != 1:
            raise AssertionError("Expected production writer marker")
        (temp / "harness.cpp").write_text(fixture.replace("@PRODUCTION_SINKS@", definitions), encoding="utf-8")
        cls.environment = dict(os.environ)
        cls.environment["PATH"] = str(Path(compiler).parent) + os.pathsep + cls.environment.get("PATH", "")
        cls.binaries = []
        for special_numbers in (0, 1):
            binary = temp / ("serializer-" + str(special_numbers) + ".exe")
            # Preserve the unchanged legacy containsKey oracle: AJ 7 deprecates
            # it, but the warning is unrelated to serialization equivalence.
            command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                "-Wno-error=deprecated-declarations", "-DESP32",
                "-DARDUINOJSON_ENABLE_ARDUINO_STRING=0", "-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0",
                "-DARDUINOJSON_ENABLE_STD_STRING=1", "-DARDUINOJSON_ENABLE_NAN=" + str(special_numbers),
                "-DARDUINOJSON_ENABLE_INFINITY=" + str(special_numbers),
                "-I" + str(temp), "-I" + str(ROOT / "include"), "-I" + str(library),
                str(temp / "harness.cpp"), str(ROOT / "src/utils/SharedJsonSerializer.cpp"), "-o", str(binary)]
            built = subprocess.run(command, capture_output=True, text=True, encoding="utf-8",
                                   errors="replace", timeout=120, env=cls.environment)
            if built.returncode:
                raise AssertionError(built.stdout + built.stderr)
            cls.binaries.append(binary)

    @classmethod
    def tearDownClass(cls):
        after = {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in cls.inputs}
        if cls.hashes != after:
            raise AssertionError("Shared serializer verification inputs changed")

    def check(self, mode):
        for binary in self.binaries:
            with self.subTest(binary=binary.name):
                result = subprocess.run([str(binary), mode], capture_output=True, text=True,
                    encoding="utf-8", errors="replace", timeout=90, env=self.environment)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


for _mode in ("values", "write-failures", "range", "ownership", "fragments", "templates", "partial-documents", "exceptions"):
    setattr(SharedJsonSerializerTests, "test_" + _mode.replace("-", "_"),
            lambda self, mode=_mode: self.check(mode))

if __name__ == "__main__":
    unittest.main()
