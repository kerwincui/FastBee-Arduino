"""Actual Framework MQTT PWM/PID callback, including all failure suffixes."""
import os
from pathlib import Path
import hashlib
import shutil
import subprocess
import tempfile
import unittest
from test.unit.test_periph_exec_config_persistence import cpp_function

ROOT = Path(__file__).resolve().parents[2]


class ModbusMqttScalarDirectTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        headers = sorted(Path(os.environ.get('PLATFORMIO_LIBDEPS_DIR',str(ROOT/'.pio/libdeps'))).glob('*/ArduinoJson/src/ArduinoJson.h'))
        if not compiler or not headers:
            raise RuntimeError('C++17 compiler and installed ArduinoJson required')
        cls.inputs = [ROOT / p for p in (
            'src/core/FastBeeFramework.cpp', 'include/protocols/ModbusHandler.h',
            'include/core/ModbusOutputFrame.h', 'include/core/ModbusOrdinaryControl.h',
            'include/core/NativeOutputControl.h', 'include/core/OrdinaryControlPolicy.h',
            'include/core/SystemConstants.h', 'include/core/UartEventOrigin.h',
            'include/protocols/ModbusMotorReference.h', 'include/protocols/MotorProtocol.h',
            'test/mocks/Arduino.h', 'include/network/LiteralStringMatch.h',
            'src/network/LiteralStringMatch.cpp', 'test/unit/cpp/modbus_mqtt_scalar_direct_harness.cpp.in')]
        cls.hashes = [hashlib.sha256(p.read_bytes()).hexdigest() for p in cls.inputs]
        source, header = [p.read_text(encoding='utf-8') for p in cls.inputs[:2]]
        callback = cpp_function(source,
            '[pm](const String& sensorId, const String& value, JsonArray& reportArr,')
        code = cls.inputs[-1].read_text(encoding='utf-8')
        parts = {
            'types': '\n'.join(cpp_function(header, key) + ';' for key in (
                'enum OneShotError', 'struct ModbusDelayReceipt', 'struct OneShotResult')),
            'callback': 'auto callback = ' + callback + ';',
        }
        for key, value in parts.items():
            marker = '// @' + key + '@'
            if code.count(marker) != 1:
                raise AssertionError('Ambiguous fixture marker ' + key)
            code = code.replace(marker, value)
        cls.temp = tempfile.TemporaryDirectory(prefix='fastbee-mqtt-scalar-')
        cls.addClassCleanup(cls.temp.cleanup)
        path = Path(cls.temp.name) / 'scalar.cpp'
        path.write_text(code, encoding='utf-8')
        cls.binary = Path(cls.temp.name) / 'scalar.exe'
        run = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
            '-DARDUINOJSON_ENABLE_ARDUINO_STRING=0', '-DARDUINOJSON_ENABLE_STD_STRING=1',
            '-I' + str(ROOT / 'test/mocks'), '-I' + str(ROOT / 'include'),
            '-I' + str(headers[0].parent), str(path), str(ROOT/'src/network/LiteralStringMatch.cpp'), '-o', str(cls.binary)],
            capture_output=True, text=True, timeout=90)
        if run.returncode:
            raise AssertionError(run.stdout + run.stderr)

    @classmethod
    def tearDownClass(cls):
        if cls.hashes != [hashlib.sha256(p.read_bytes()).hexdigest() for p in cls.inputs]:
            raise AssertionError('MQTT scalar callback inputs changed')

    def check(self, scenario):
        run = subprocess.run([str(self.binary), scenario], capture_output=True, text=True, timeout=10)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)


for _scenario in (
    'pwm_single',
    'pwm_max',
    'pwm_zero',
    'pwm_leading_zero',
    'pwm_last_address',
    'pwm_width_overflow',
    'pwm_invalid_width',
    'pwm_zero_width',
    'pwm_bad_channel',
    'pwm_bad_action',
    'pwm_disabled',
    'pwm_zero_channels',
    'pwm_address_overflow',
    'pid_base',
    'pid_all',
    'pid_sv',
    'pid_p',
    'pid_i',
    'pid_d',
    'pid_pv',
    'pid_out',
    'pid_bad_action',
    'pid_bad_index',
    'pid_mismatched_index',
    'pid_unmapped',
    'number_negative',
    'number_plus',
    'number_space',
    'number_trailing',
    'number_tab',
    'number_empty',
    'number_text',
    'number_fraction',
    'number_overflow',
    'number_wrap',
    'number_embedded',
    'all_pass',
    'all_failed_first',
    'all_failed_middle',
    'all_failed_last',
    'all_denied_middle',
    'all_unmapped_middle',
    'all_stale_config',
    'all_stale_instance',
    'all_stale_slave',
    'all_fallback_id',
    'all_unterminated_id',
    'all_single_channel',
    'single_denied',
    'single_failed',
    'single_inconsistent',
    'source_rule',
    'mapping_busy',
    'unmatched',
    'unavailable',
):
    setattr(ModbusMqttScalarDirectTests, 'test_' + _scenario,
            lambda self, scenario=_scenario: self.check(scenario))


if __name__ == '__main__':
    unittest.main()
