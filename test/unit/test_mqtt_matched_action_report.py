"""Compile the actual matched-action response formatter, with immutable report headers."""
from pathlib import Path
import hashlib
import shutil
import subprocess
import tempfile
import unittest
from test.unit.test_periph_exec_config_persistence import cpp_function

ROOT = Path(__file__).resolve().parents[2]


class MqttMatchedActionReportTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        headers = sorted((ROOT / '.pio/libdeps').glob('*/ArduinoJson/src/ArduinoJson.h'))
        if not compiler or not headers:
            raise RuntimeError('C++17 compiler and installed ArduinoJson required')
        cls.inputs = [ROOT / p for p in (
            'src/core/PeriphExecManager.cpp', 'include/core/ActionReportPayload.h',
            'include/core/ActionReportIdentity.h', 'include/core/PeripheralExecution.h',
            'include/core/SensorSampleMetadata.h', 'test/mocks/Arduino.h',
            'test/unit/cpp/mqtt_matched_action_report_harness.cpp.in')]
        cls.hashes = [hashlib.sha256(p.read_bytes()).hexdigest() for p in cls.inputs]
        body = cpp_function(cls.inputs[0].read_text(encoding='utf-8'),
                            'String PeriphExecManager::processDataCommandMatch(')
        parts = {
            'loop': cpp_function(body, 'for (const auto& ar : results) {'),
            'empty': cpp_function(body, 'if (results.empty()) {'),
        }
        code = cls.inputs[-1].read_text(encoding='utf-8')
        for key, value in parts.items():
            marker = '// @' + key + '@'
            if code.count(marker) != 1:
                raise AssertionError('Ambiguous fixture marker ' + key)
            code = code.replace(marker, value)
        cls.temp = tempfile.TemporaryDirectory(prefix='fastbee-mqtt-matched-report-')
        cls.addClassCleanup(cls.temp.cleanup)
        path = Path(cls.temp.name) / 'matched.cpp'
        path.write_text(code, encoding='utf-8')
        cls.binary = Path(cls.temp.name) / 'matched.exe'
        run = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
            '-DARDUINOJSON_ENABLE_ARDUINO_STRING=0', '-DARDUINOJSON_ENABLE_STD_STRING=1',
            '-I' + str(ROOT / 'test/mocks'), '-I' + str(ROOT / 'include'),
            '-I' + str(headers[0].parent), str(path), '-o', str(cls.binary)],
            capture_output=True, text=True, timeout=90)
        if run.returncode:
            raise AssertionError(run.stdout + run.stderr)

    @classmethod
    def tearDownClass(cls):
        if cls.hashes != [hashlib.sha256(p.read_bytes()).hexdigest() for p in cls.inputs]:
            raise AssertionError('Matched-action report inputs changed')

    def check(self, scenario):
        run = subprocess.run([str(self.binary), scenario], capture_output=True, text=True, timeout=10)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)


for _scenario in (
    'success',
    'empty_success',
    'failed_empty',
    'failed_stale',
    'captured_modbus',
    'colon_value',
    'peripheral_id',
    'success_default_reason',
    'failure_default_reason',
    'truncated_field',
    'lost_field',
    'legacy',
    'invalid_identity',
    'embedded_value',
    'sample',
    'sample_stale',
    'sample_invalid_unit',
    'invalid_after_prefix',
    'mixed_results',
    'no_actions',
    'allocation',
    'no_actions_allocation',
):
    setattr(MqttMatchedActionReportTests, 'test_' + _scenario,
            lambda self, scenario=_scenario: self.check(scenario))


if __name__ == '__main__':
    unittest.main()
