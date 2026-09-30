"""Compile the real controls serializer and route; no device/network access."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from test_sensor_publication import definition

ROOT = Path(__file__).resolve().parents[2]


class DeviceControlActionsTests(unittest.TestCase):
    def test_production_route_and_json_with_and_without_ota(self):
        compiler = shutil.which('g++') or shutil.which('clang++')
        self.assertIsNotNone(compiler, 'C++17 compiler required')
        headers = list((ROOT / '.pio/libdeps').glob('*/ArduinoJson/src/ArduinoJson.h'))
        self.assertTrue(headers, 'Project ArduinoJson dependency required')
        route = (ROOT / 'src/network/handlers/PeriphExecRouteHandler.cpp').read_text(encoding='utf-8')
        types = (ROOT / 'include/core/PeripheralExecution.h').read_text(encoding='utf-8')
        with tempfile.TemporaryDirectory(prefix='fastbee-controls-') as directory:
            temp = Path(directory)
            # Adapt only String allocation APIs absent from the generic host mock.
            arduino = (ROOT / 'test/mocks/Arduino.h').read_text(encoding='utf-8')
            arduino = arduino.replace('void reserve(size_t size) { _value.reserve(size); }',
                'bool reserve(size_t size) { _value.reserve(size); return true; }\n'
                'bool concat(const char* s, unsigned int n) { _value.append(s,n); return true; }\n'
                'bool concat(const char* s) { _value.append(s); return true; }\n'
                'bool concat(char c) { _value.push_back(c); return true; }')
            (temp / 'Arduino.h').write_text(arduino, encoding='utf-8')
            (temp / 'WString.h').write_text('#include <Arduino.h>\n', encoding='utf-8')
            (temp / 'control_types.inc').write_text(definition(types, 'enum class ExecActionType') + ';\n' +
                definition(route, 'enum class ControlGroup') + ';', encoding='utf-8')
            (temp / 'control_serializer.inc').write_text(
                definition(route, 'ControlGroup controlGroupForAction(') + '\n' +
                definition(route, 'class ControlJsonWriter') + ';\n' +
                definition(route, 'bool buildControlItemJson('), encoding='utf-8')
            (temp / 'control_route.inc').write_text(
                definition(route, 'void PeriphExecRouteHandler::handleGetControls('), encoding='utf-8')
            for ota in (0, 1):
                with self.subTest(ota=ota):
                    exe = temp / ('controls.exe' if os.name == 'nt' else 'controls')
                    result = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-DARDUINOJSON_ENABLE_ARDUINO_STRING=1', '-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0',
                        '-DARDUINOJSON_ENABLE_ARDUINO_PRINT=0', '-DARDUINOJSON_ENABLE_PROGMEM=0',
                        f'-DFASTBEE_ENABLE_OTA={ota}', f'-I{temp}', f'-I{headers[0].parent}',
                        str(ROOT / 'test/unit/cpp/device_control_actions_harness.cpp'), '-o', str(exe)],
                        capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=60)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=15)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
