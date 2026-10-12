"""Actual HTTP control boundary; mock sessions/PM, never touch devices."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test.unit.test_periph_exec_config_persistence import cpp_function

ROOT = Path(__file__).resolve().parents[2]


class HttpOutputControlTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        headers = list((ROOT / '.pio/libdeps').glob('*/ArduinoJson/src/ArduinoJson.h'))
        if not compiler or not headers:
            raise RuntimeError('C++17 compiler and installed ArduinoJson required')
        cls.temp = tempfile.TemporaryDirectory(prefix='fastbee-http-output-')
        cls.addClassCleanup(cls.temp.cleanup)
        directory = Path(cls.temp.name)
        actual = (ROOT / 'src/network/HttpOutputControl.cpp').read_text(encoding='utf-8')
        for include in ('network/HttpOutputControl.h', 'network/WebHandlerContext.h',
                        'core/PeripheralManager.h', 'core/NativeOutputDispatch.h', 'core/MotionOutputDispatch.h'):
            actual = actual.replace('#include "' + include + '"', '')
        template = (ROOT / 'test/unit/cpp/http_output_control_harness.cpp.in').read_text(encoding='utf-8')
        dispatch = (ROOT / 'include/core/NativeOutputDispatch.h').read_text(encoding='utf-8')
        scalar = '\n'.join(cpp_function(dispatch, signature) for signature in (
            'inline bool isOrdinaryScalarTarget(', 'inline bool ordinaryScalarMaximum(',
            'inline bool ordinaryScalarHighValue(',
            'inline bool parseOrdinaryScalarValue('))
        motion = (ROOT / 'include/core/MotionOutputDispatch.h').read_text(encoding='utf-8')
        scalar += '\n' + cpp_function(motion, 'inline bool parseServoRawDuty(')
        actual = 'namespace FastBee {\n' + scalar + '\n}\n' + actual
        source = directory / 'boundary.cpp'
        source.write_text(template.replace('// @actual_http_boundary@', actual), encoding='utf-8')
        cls.binary = directory / 'boundary.exe'
        result = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
            '-Wno-deprecated-declarations', '-DARDUINOJSON_ENABLE_ARDUINO_STRING=0',
            '-DARDUINOJSON_ENABLE_STD_STRING=1', '-I' + str(ROOT / 'test/mocks'),
            '-I' + str(ROOT / 'include'), '-I' + str(headers[0].parent),
            str(source), '-o', str(cls.binary)], capture_output=True, text=True, encoding='utf-8', timeout=60)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def check(self, case):
        result = subprocess.run([str(self.binary), case], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


for _case in ('identity', 'fields', 'state', 'commands', 'hold', 'confirmation',
              'rf_identity', 'rf_parameters', 'rf_fields', 'rf_context', 'rf_failure',
              'scalar_pwm', 'scalar_dac', 'scalar_invalid', 'scalar_commands', 'scalar_context', 'scalar_unknown',
              'scalar_passive', 'scalar_toggle', 'scalar_blink', 'scalar_waveform_rejected',
              'generation', 'epoch', 'conflict', 'unknown', 'not_attempted', 'rule',
              'pca_identity', 'pca_fields', 'pca_review', 'modbus_identity', 'modbus_fields', 'modbus_review',
              'servo_raw', 'servo_angle', 'servo_parameters', 'servo_context',
              'servo_review', 'servo_fields', 'servo_unknown', 'motor_identity', 'motor_fields', 'motor_review', 'attempt_unknown'):
    setattr(HttpOutputControlTests, 'test_' + _case, lambda self, case=_case: self.check(case))
