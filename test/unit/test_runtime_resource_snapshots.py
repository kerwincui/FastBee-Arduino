"""Passive resource API/JSON and real lazy Web behavior. No live requests."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test.unit.test_screen_panel_contract import braced_block
from test_peripheral_route_fixed_json import definition

ROOT = Path(__file__).resolve().parents[2]


class RuntimeResourceSnapshotTests(unittest.TestCase):
    def compile_run(self, name, route=False, send=False):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise RuntimeError('C++17 compiler required')
        json_header = next((ROOT / '.pio/libdeps').glob('*/ArduinoJson/src/ArduinoJson.h'))
        with tempfile.TemporaryDirectory(prefix='fastbee-runtime-resources-') as folder:
            temp = Path(folder)
            if route:
                source = (ROOT / 'src/network/handlers/PeripheralRouteHandler.cpp').read_text(encoding='utf-8')
                signature = 'void PeripheralRouteHandler::handleGetPeripheralResources(AsyncWebServerRequest* request)'
                body = braced_block(source, 'void PeripheralRouteHandler::handleGetPeripheralResources(')
                helper = definition(source, 'allocatePreflightWorkspace')
                (temp / 'resource_route_under_test.inc').write_text(helper + '\n' + signature + ' {' + body + '}\n', encoding='utf-8')
            if send:
                source = (ROOT / 'include/network/handlers/HandlerUtils.h').read_text(encoding='utf-8')
                signature = 'inline void sendJsonStream(AsyncWebServerRequest* request, JsonDocument& doc)'
                body = braced_block(source, signature)
                (temp / 'resource_send_under_test.inc').write_text(signature + ' {' + body + '}\n', encoding='utf-8')
            executable = temp / ('resources.exe' if os.name == 'nt' else 'resources')
            built = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                '-DARDUINOJSON_ENABLE_ARDUINO_STRING=0', '-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0',
                '-DCHIP_UART_COUNT=2', '-I' + str(temp), '-I' + str(ROOT / 'test/mocks'),
                '-I' + str(ROOT / 'include'), '-I' + str(json_header.parent),
                str(ROOT / 'test/unit/cpp' / name),
                str(ROOT / 'src/network/FixedJsonField.cpp'), '-o', str(executable)],
                capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=90)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            ran = subprocess.run([str(executable)], capture_output=True, text=True, timeout=20)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)

    def test_real_json_busy_retained_retiring_max_capacity_and_copied_ids(self):
        self.compile_run('runtime_resource_json_harness.cpp')

    def test_real_route_separates_configuration_runtime_busy_and_comparison(self):
        self.compile_run('runtime_resource_route_harness.cpp', route=True)

    def test_real_json_allocation_failures_never_send_partial_success(self):
        self.compile_run('runtime_resource_oom_harness.cpp', send=True)

    def test_real_lazy_ui_fresh_read_unknowns_and_stale_responses(self):
        node = shutil.which('node')
        if not node:
            raise RuntimeError('Node required')
        result = subprocess.run([node, str(ROOT / 'test/unit/runtime_resource_web_harness.cjs')],
            cwd=ROOT, capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=20)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
