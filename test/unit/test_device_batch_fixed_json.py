"""Actual device/config and affected batch routes against immutable pre-sharing sources.

Real ArduinoJson and FixedJsonField; SDK, filesystem, auth and network are fakes.
No device access. Coordinator must run this after integrating the reviewed patch.
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

PATTERN = re.compile(r'\b(doc|resp|fileCfg|data|out|respDoc)((?:\["[^"\n]+"\]){1,3})([ \t]*)= ([^\n]*);')


def share_fixed_fields(source, count):
    def change(match):
        if ";" in match[4]:
            raise AssertionError("A reviewed assignment now contains a nested statement")
        keys = re.findall(r'\["([^"\n]+)"\]', match[2])
        return 'FastBee::setFixedJsonField(' + match[1] + ', ' + ', '.join(
            '"' + key + '"' for key in keys) + ', ' + match[4] + ');'
    result, changed = PATTERN.subn(change, source)
    if changed != count:
        raise AssertionError(f"Reviewed assignment count changed: {changed} != {count}")
    return result


DEVICE_METHODS = ("handleSaveDeviceConfigJson", "handleGetDeviceConfig", "handleSetDeveloperMode",
                  "handleSaveDeviceConfig", "handleGetDeviceInfo")
BATCH_URLS = ("/api/network/status", "/api/network/config", "/api/device/config", "/api/device/time")


def class_definitions(device, batch, suffix):
    methods = '\n\n'.join(definition(device, 'void DeviceRouteHandler::' + name + '(')
                          for name in DEVICE_METHODS)
    methods = methods.replace('DeviceRouteHandler::', 'Device' + suffix + '::')
    methods = re.sub(r'JsonDocument (\w+);', r'JsonDocument \1(routeAllocator);', methods)
    declarations = '\n'.join('void ' + name + '(AsyncWebServerRequest*' +
        (', JsonVariant&' if name == 'handleSaveDeviceConfigJson' else '') + ');' for name in DEVICE_METHODS)
    output = 'struct Device' + suffix + ' { Context* ctx;\n' + declarations + '\n};\n' + methods
    branches = '\n\n'.join(definition(batch, 'if (url == "' + url + '")') for url in BATCH_URLS)
    branches = re.sub(r'JsonDocument (\w+);', r'JsonDocument \1(routeAllocator);', branches)
    request = definition(batch, 'void BatchRouteHandler::handleBatchRequest(')
    request = request.replace('BatchRouteHandler::', 'Batch' + suffix + '::')
    output += '\nstruct Batch' + suffix + ' { Context* ctx;\n'
    output += 'bool buildSubResponse(AsyncWebServerRequest*, const String&, JsonObject);\n'
    output += 'void handleBatchRequest(AsyncWebServerRequest*, JsonVariant&);\n};\n'
    output += 'bool Batch' + suffix + '::buildSubResponse(AsyncWebServerRequest* request, const String& url, JsonObject out) {\n'
    output += branches + '\nreturn false;\n}\n' + request + '\n'
    return output


class DeviceBatchFixedJsonTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or 'D:/msys64/ucrt64/bin/g++.exe'
        if not Path(compiler).is_file():
            raise RuntimeError('C++17 compiler required')
        library = Path(os.environ['FASTBEE_TEST_ARDUINOJSON_INCLUDE']) if os.environ.get(
            'FASTBEE_TEST_ARDUINOJSON_INCLUDE') else SUPPORT / '.pio/libdeps/native/ArduinoJson/src'
        if not (library / 'ArduinoJson.h').is_file():
            raise RuntimeError('Registered ArduinoJson headers required')
        sources = [ROOT / ('src/network/handlers/' + name + 'RouteHandler.cpp') for name in ('Device', 'Batch')]
        oracles = [HERE / ('cpp/' + name + '_route_fixed_json_oracle.inc') for name in ('device', 'batch')]
        old = [path.read_text(encoding='utf-8') for path in oracles]
        expected_hashes = ('4162801857e97463e7afb79dc1635486fc3d40a06cc6a69d74dd65aeeeb04229',
                           '99943693fd3c9172ef87a0468cb3594a43c3c9e8cebcc4fe656ff5f7d46d7a84')
        if tuple(hashlib.sha256(text.encode('utf-8')).hexdigest() for text in old) != expected_hashes:
            raise AssertionError('Frozen device/batch pre-sharing oracle changed')
        current = [path.read_text(encoding='utf-8') for path in sources]
        expected = [share_fixed_fields(text, count) for text, count in zip(old, (71, 121))]
        expected[0] = expected[0].replace('#include "network/SystemTimeStatus.h"',
            '#include "network/SystemTimeStatus.h"\n#include "network/FixedJsonField.h"', 1)
        for actual, reviewed in zip(current, expected):
            if actual != reviewed:
                raise AssertionError('Complete production file differs beyond the reviewed fixed-key assignment rewrite')
        wifi_path = ROOT / 'include/network/WiFiManager.h'
        wifi = wifi_path.read_text(encoding='utf-8')
        type_names = ('enum class NetworkType', 'enum class NetworkMode', 'enum class IPConfigType',
                      'enum class NetworkStatus', 'enum class IPConflictMode', 'enum class IPFailoverStrategy',
                      'struct WiFiNetwork', 'struct EthernetConfig', 'struct CellularConfig',
                      'struct WiFiConfig', 'struct NetworkStatusInfo')
        types = '\n\n'.join(definition(wifi, name) + ';' for name in type_names)
        harness = HERE / 'cpp/device_batch_fixed_json_harness.cpp.in'
        helper = ROOT / 'src/network/FixedJsonField.cpp'
        prelude = SUPPORT / 'test/unit/cpp/fixed_json_document_prelude.h'
        roots = (HERE / 'cpp', SUPPORT / 'test/unit/cpp', ROOT / 'include', SUPPORT / 'test/mocks', library)
        seeds = (Path(__file__), *sources, *oracles, wifi_path, harness, helper, prelude,
                 SUPPORT / 'test/unit/test_periph_exec_fixed_json.py')
        cls.inputs = sorted(local_includes(seeds, roots) | set(library.rglob('*.h')) | set(library.rglob('*.hpp')))
        cls.hashes = {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in cls.inputs}
        cls.addClassCleanup(cls.verify_inputs)
        program = harness.read_text(encoding='utf-8')
        for marker in ('@NETWORK_TYPES@', '@ROUTE_DEFINITIONS@'):
            if program.count(marker) != 1:
                raise AssertionError('Harness expansion marker changed: ' + marker)
        program = program.replace('@NETWORK_TYPES@', types).replace('@ROUTE_DEFINITIONS@',
            class_definitions(*old, 'Oracle') + '\n' + class_definitions(*current, 'Current'))
        cls.temporary = tempfile.TemporaryDirectory(prefix='fastbee-device-batch-json-')
        cls.addClassCleanup(cls.temporary.cleanup)
        temporary = Path(cls.temporary.name)
        cpp = temporary / 'routes.cpp'
        cpp.write_text(program, encoding='utf-8')
        cls.binary = temporary / ('routes.exe' if os.name == 'nt' else 'routes')
        cls.environment = dict(os.environ)
        cls.environment['PATH'] = str(Path(compiler).parent) + os.pathsep + cls.environment.get('PATH', '')
        command = [compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-DUNIT_TEST',
                   '-DARDUINOJSON_ENABLE_ARDUINO_STRING=1', '-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0',
                   '-DARDUINOJSON_ENABLE_ARDUINO_PRINT=0', '-DARDUINOJSON_ENABLE_STD_STRING=1',
                   *['-I' + str(path) for path in roots], '-include', str(prelude),
                   str(cpp), str(helper), '-o', str(cls.binary)]
        result = subprocess.run(command, text=True, capture_output=True, encoding='utf-8', errors='replace',
                                timeout=120, env=cls.environment)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    @classmethod
    def verify_inputs(cls):
        after = {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in cls.inputs}
        if after != cls.hashes:
            raise AssertionError('Device/batch differential inputs changed during execution')

    def check(self, mode):
        result = subprocess.run([str(self.binary), mode], capture_output=True, text=True,
                                encoding='utf-8', errors='replace', timeout=90, env=self.environment)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('PASS ' + mode, result.stdout)


for _mode in ('device-save', 'device-get', 'device-info', 'developer-mode',
              'batch-network', 'batch-status', 'batch-device', 'batch-time', 'batch-request',
              'device-oom', 'batch-oom', 'ownership-and-identity'):
    setattr(DeviceBatchFixedJsonTests, 'test_' + _mode.replace('-', '_'),
            lambda self, mode=_mode: self.check(mode))

if __name__ == '__main__':
    unittest.main()

