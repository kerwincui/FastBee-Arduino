"""Bounded production image metadata reader with real IDF format and JSON code."""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def definition(source, signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 1
    cursor = opening + 1
    while depth:
        if source[cursor] == '{':
            depth += 1
        elif source[cursor] == '}':
            depth -= 1
        cursor += 1
    return source[start:cursor]


class RunningImageSizeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise RuntimeError('C++17 compiler required for running-image metadata tests')
        sdk_header = os.environ.get('FASTBEE_APP_FORMAT_HEADER')
        if not sdk_header:
            roots = [ROOT / '.pio', Path.home() / '.platformio/packages']
            sdk_header = next((str(p) for root in roots for p in root.glob(
                '**/bootloader_support/include/esp_app_format.h')), None)
        if not sdk_header:
            raise RuntimeError('Installed Espressif esp_app_format.h required; no replica format allowed')
        json_dir = os.environ.get('FASTBEE_ARDUINOJSON_INCLUDE')
        if not json_dir:
            header = next((ROOT / '.pio/libdeps').glob('*/ArduinoJson/src/ArduinoJson.h'), None)
            json_dir = str(header.parent) if header else None
        if not json_dir:
            raise RuntimeError('Actual ArduinoJson dependency required')
        cls.inputs = [ROOT / p for p in (
            'include/systems/RunningImageSize.h', 'src/systems/RunningImageSize.cpp',
            'include/network/RunningImageSizeJson.h', 'include/network/FixedJsonField.h',
            'src/network/FixedJsonField.cpp', 'src/network/handlers/SystemRouteHandler.cpp',
            'src/network/handlers/BatchRouteHandler.cpp', 'test/unit/test_running_image_size.py',
            'test/unit/cpp/running_image_size_harness.cpp.in')]
        cls.inputs.append(Path(sdk_header))
        cls.hashes = [hashlib.sha256(p.read_bytes()).hexdigest() for p in cls.inputs]
        cls.temp = tempfile.TemporaryDirectory(prefix='fastbee-running-image-')
        cls.addClassCleanup(cls.temp.cleanup)
        folder = Path(cls.temp.name)
        (folder / 'network/handlers').mkdir(parents=True)
        shutil.copyfile(sdk_header, folder / 'esp_app_format.h')
        mocks = {
            'esp_assert.h': '#pragma once\n#define ESP_STATIC_ASSERT(c,m) static_assert(c,m)\n',
            'esp_err.h': '#pragma once\nusing esp_err_t=int;\nconstexpr int ESP_OK=0, ESP_FAIL=-1;\n',
            'sdkconfig.h': '#pragma once\n',
            'esp_partition.h': '''#pragma once
#include <cstdint>
#include <cstddef>
#include "esp_err.h"
constexpr uint8_t ESP_PARTITION_TYPE_APP=0, ESP_PARTITION_TYPE_DATA=1;
struct esp_partition_t {uint8_t type; uint32_t address; uint32_t size;};
esp_err_t esp_partition_read(const esp_partition_t*, size_t, void*, size_t);
''',
            'esp_ota_ops.h': '''#pragma once
#include "esp_partition.h"
const esp_partition_t* esp_ota_get_running_partition();
''',
            'network/handlers/HandlerUtils.h': '''#pragma once
#include <ArduinoJson.h>
#include <map>
#include <string>
struct AsyncWebServerResponse {
    int status; std::string body; std::map<std::string,std::string> headers;
    void addHeader(const char* key,const char* value) {headers[key]=value;}
};
struct AsyncClient {bool closed=false; void close(bool) {closed=true;}};
struct AsyncWebServerRequest {
    AsyncWebServerResponse response{}; bool sent=false, failResponseAlloc=false; AsyncClient connection;
    AsyncClient* client() {return &connection;}
    AsyncWebServerResponse* beginResponse(int code,const char*,const char* body) {
        if (failResponseAlloc) return nullptr;
        response.status=code; response.body=body; return &response;
    }
    void send(AsyncWebServerResponse* r) {sent=true; response=*r;}
};
namespace HandlerUtils {
inline void sendWithClose(AsyncWebServerRequest* request,AsyncWebServerResponse* response) {
    response->addHeader("Connection","close"); request->send(response);
}
inline void sendJsonStream(AsyncWebServerRequest* request,JsonDocument& doc) {
    std::string body; serializeJson(doc,body);
    sendWithClose(request,request->beginResponse(200,"application/json",body.c_str()));
}
}
'''}
        for name, text in mocks.items():
            (folder / name).write_text(text, encoding='utf-8')
        system = cls.inputs[5].read_text(encoding='utf-8')
        batch = cls.inputs[6].read_text(encoding='utf-8')
        template = cls.inputs[8].read_text(encoding='utf-8')
        template = template.replace('@COMPACT_SEND@', definition(system, 'void sendCompactSystemInfo('))
        # Compile the actual endpoint branch, keeping the remaining batch handler
        # outside this focused test so unrelated endpoints need no SDK doubles.
        start = batch.index('    if (url == "/api/system/info") {')
        branch = definition(batch[start:], '    if (url == "/api/system/info") {')
        template = template.replace('@BATCH_BRANCH@', branch)
        cpp = folder / 'running_image.cpp'
        cpp.write_text(template, encoding='utf-8')
        cls.binaries = {}
        for chip, signed in ((0, 0), (5, 0), (9, 0), (13, 0), (9, 1), (9, 2)):
            executable = folder / ('reader-%s-%s%s' % (chip, signed, '.exe' if os.name == 'nt' else ''))
            command = [compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pthread',
                       '-DCONFIG_IDF_FIRMWARE_CHIP_ID=%s' % chip,
                       '-DCONFIG_SECURE_BOOT=%s' % int(signed == 1),
                       '-DCONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT=%s' % int(signed == 2),
                       '-I' + str(folder), '-I' + str(ROOT / 'include'), '-I' + json_dir,
                       str(ROOT / 'src/systems/RunningImageSize.cpp'),
                       str(ROOT / 'src/network/FixedJsonField.cpp'), str(cpp), '-o', str(executable)]
            result = subprocess.run(command, capture_output=True, text=True, timeout=60)
            if result.returncode:
                raise RuntimeError(result.stdout + result.stderr)
            cls.binaries[chip, signed] = executable

    @classmethod
    def tearDownClass(cls):
        assert [hashlib.sha256(p.read_bytes()).hexdigest() for p in cls.inputs] == cls.hashes

    def test_unsigned_real_production_reader_four_chip_ids_bounds_failures_and_concurrency(self):
        for chip in (0, 5, 9, 13):
            with self.subTest(chip=chip):
                result = subprocess.run([str(self.binaries[chip, 0])], capture_output=True, text=True, timeout=15)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn('BOUNDED_METADATA_AND_API_PASS', result.stdout)

    def test_signed_boot_and_signed_app_configurations_explicitly_reject_unknown_size(self):
        for signed in (1, 2):
            result = subprocess.run([str(self.binaries[9, signed])], capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('SIGNED_SIZE_UNKNOWN_PASS', result.stdout)

    def test_actual_pinned_rgb_binary_length_matches_independent_build_report(self):
        binary = os.environ.get('FASTBEE_RUNNING_IMAGE_BINARY')
        expected = os.environ.get('FASTBEE_RUNNING_IMAGE_EXPECTED_BYTES')
        if not binary or not expected:
            self.skipTest('Pinned deployment binary supplied only by artifact acceptance runner')
        result = subprocess.run([str(self.binaries[9, 0]), binary, expected],
                                capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('ACTUAL_BINARY_METADATA_PASS', result.stdout)

    def test_full_compact_and_batch_only_emit_flash_after_successful_shared_reader(self):
        system = self.inputs[5].read_text(encoding='utf-8')
        batch = self.inputs[6].read_text(encoding='utf-8')
        self.assertNotIn('ESP.getSketchSize()', system + batch)
        full = definition(system, 'void SystemRouteHandler::handleSystemInfo(')
        self.assertLess(full.index('readRunningImageSizeForResponse(request, imageSize)'),
                        full.index('auto doc = FastBee::makeJsonDocument()'))
        self.assertIn('uint32_t sketchSize = imageSize.size;', full)
        for source in (system, batch):
            compact = definition(source, 'void fillCompactSystemInfo(')
            self.assertIn('uint32_t sketchSize)', compact)
            self.assertNotIn('readRunningImageSize', compact)


if __name__ == '__main__':
    unittest.main()
