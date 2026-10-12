"""Run the actual injected MQTT relay callback with counted Modbus faults."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test.unit.test_periph_exec_config_persistence import cpp_function

ROOT = Path(__file__).resolve().parents[2]


class ModbusMqttRelayDirectTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise RuntimeError('C++17 compiler required')
        headers = list(Path(os.environ.get('PLATFORMIO_LIBDEPS_DIR',str(ROOT/'.pio/libdeps'))).glob('*/ArduinoJson/src/ArduinoJson.h'))
        if not headers:
            raise RuntimeError('Installed ArduinoJson required')
        production = cpp_function((ROOT/'src/core/FastBeeFramework.cpp').read_text(encoding='utf-8'),
            '[pm](const String& sensorId, const String& value, JsonArray& reportArr,')
        template = (ROOT/'test/unit/cpp/modbus_mqtt_relay_direct_harness.cpp').read_text(encoding='utf-8')
        cls.temp = tempfile.TemporaryDirectory(prefix='fastbee-mqtt-relay-')
        cls.addClassCleanup(cls.temp.cleanup)
        folder = Path(cls.temp.name)
        cpp = folder/'relay.cpp'
        cpp.write_text(template.replace('// @callback@', 'auto callback = '+production+';'), encoding='utf-8')
        cls.binary = folder/'relay.exe'
        build = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
            '-DARDUINOJSON_ENABLE_ARDUINO_STRING=0', '-DARDUINOJSON_ENABLE_STD_STRING=1',
            '-I'+str(ROOT/'test/mocks'), '-I'+str(ROOT/'include'), '-I'+str(headers[0].parent),
            str(cpp), str(ROOT/'src/network/LiteralStringMatch.cpp'), '-o', str(cls.binary)], capture_output=True, text=True, timeout=90)
        if build.returncode:
            raise AssertionError(build.stdout+build.stderr)

    def check(self, scenario):
        result = subprocess.run([str(self.binary), scenario], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)

    def test_callback_true_is_handled_and_skips_fallback(self):
        source = (ROOT/'src/core/PeriphExecManager.cpp').read_text(encoding='utf-8')
        branch = cpp_function(source, 'if (_modbusDirectControlCb) {')
        self.assertIn('reportArr, control)', branch)
        self.assertIn('finishOrdinaryControl(control)', branch)
        self.assertIn('if (handled) continue;', branch)


for _scenario in ('coil_pass', 'register_pass', 'coil_fail_first', 'register_fail_middle',
                  'coil_fail_last', 'batch_mask_pass', 'batch_command_pass', 'batch_fail',
                  'single_pass', 'single_fail', 'single_nc', 'invalid_bool', 'invalid_bool_single',
                  'invalid_bool_nc', 'empty_bool', 'embedded_bool', 'off_pass', 'off_nc',
                  'disabled', 'zero_channels', 'overflow',
                  'invalid_channel', 'invalid_batch_mask', 'fallback_id', 'unmatched', 'unavailable',
                  'source_rejected', 'single_source_rejected', 'mapping_busy', 'motor_base', 'motor_oper_forward', 'motor_oper_reverse', 'motor_oper_stop',
                  'motor_speed', 'motor_spd', 'motor_pulse', 'motor_pls', 'motor_bad_enum', 'motor_bad_number',
                  'motor_bad_text', 'motor_negative', 'motor_embedded', 'motor_disabled', 'motor_rejected'):
    setattr(ModbusMqttRelayDirectTests, 'test_'+_scenario,
            lambda self, scenario=_scenario: self.check(scenario))


if __name__ == '__main__':
    unittest.main()
