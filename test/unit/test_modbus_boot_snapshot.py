"""Prepared full production Modbus action body with counted transport fakes."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test.unit.test_periph_exec_config_persistence import cpp_function as method

ROOT = Path(__file__).resolve().parents[2]


class ModbusBootSnapshotTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        headers = list((ROOT/'.pio/libdeps').glob('*/ArduinoJson/src/ArduinoJson.h'))
        if not compiler or not headers:
            raise RuntimeError('C++17 compiler and installed ArduinoJson required')
        cls.source = (ROOT/'src/core/PeriphExecExecutor.cpp').read_text(encoding='utf-8')
        production = method(cls.source, 'void appendActionResult(') + '\n' + method(cls.source, 'bool PeriphExecExecutor::executeModbusPollAction(')
        production = '#if FASTBEE_ENABLE_MODBUS\n' + method(cls.source, 'bool appendModbusPollReadings(') + '\n#endif\n' + production
        dispatch = (ROOT/'include/core/NativeOutputDispatch.h').read_text(encoding='utf-8')
        production = 'namespace FastBee {\n' + method(dispatch, 'inline void rememberOrdinaryControlResult(') + '\n}\n' + production
        template = (ROOT/'test/unit/cpp/modbus_boot_snapshot_harness.cpp').read_text(encoding='utf-8')
        cls.temp = tempfile.TemporaryDirectory(prefix='fastbee-boot-modbus-')
        cls.addClassCleanup(cls.temp.cleanup)
        folder = Path(cls.temp.name)
        cpp = folder/'action.cpp'
        cpp.write_text(template.replace('// @production@', production), encoding='utf-8')
        cls.binaries = {}
        for enabled in (0, 1):
            binary = folder/('action-'+str(enabled)+'.exe')
            result = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                '-Wno-deprecated-declarations', '-DFASTBEE_ENABLE_MODBUS='+str(enabled),
                '-DARDUINOJSON_ENABLE_ARDUINO_STRING=0', '-DARDUINOJSON_ENABLE_STD_STRING=1',
                '-I'+str(ROOT/'test/mocks'), '-I'+str(ROOT/'include'), '-I'+str(headers[0].parent),
                str(cpp), '-o', str(binary)], capture_output=True, text=True, timeout=90)
            if result.returncode:
                raise AssertionError((result.stdout or '')+(result.stderr or ''))
            cls.binaries[enabled] = binary

    def check(self, case, enabled=1):
        result = subprocess.run([str(self.binaries[enabled]), case], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, (result.stdout or '')+(result.stderr or ''))

    def test_disabled_build_is_inert(self):
        self.check('mixed', enabled=0)

    def test_actual_boot_and_rule_callers_choose_distinct_intents(self):
        boot = method(self.source, 'size_t PeriphExecExecutor::reportRuleCurrentState(')
        normal = method(self.source, 'std::vector<ActionExecResult> PeriphExecExecutor::executeAllActions(')
        self.assertIn('executeModbusPollAction(action, rule, &pollResults, FastBee::ModbusActionMode::ReadOnlySnapshot)', boot)
        self.assertIn('executeModbusPollAction(action, rule, &actionResults, FastBee::ModbusActionMode::ExecuteRule,', normal)
        self.assertIn('traceOut ? traceOut->uartOrigin : FastBee::UartEventOrigin{}, &sourceUnconfirmed,', normal)
        # Read tasks reach FC 01..04 readers; rawTransfer changes the result format,
        # not permission to send arbitrary write frames.
        handler = (ROOT/'src/protocols/ModbusHandler.cpp').read_text(encoding='utf-8')
        for signature in ('OneShotResult ModbusHandler::readRegistersOnce(', 'OneShotError ModbusHandler::_readRangeInto('):
            self.assertIn('functionCode < 0x01 || functionCode > 0x04', method(handler, signature))


for _case in ('frame_pwm_failure','frame_pid_failure','frame_pwm_overflow','frame_negative','frame_pid_parameter',
              'frame_device_missing','frame_device_negative','frame_action_nul','frame_pid_nul','frame_prefix_bad_device',
              'mixed', 'normal', 'raw', 'normal_raw', 'ctrl_only', 'empty', 'legacy',
              'malformed', 'incomplete', 'poll_only', 'malformed_ctrl', 'invalid_mode',
              'no_results', 'normal_then_snapshot', 'source_initial', 'source_malformed',
              'source_lease', 'source_poll', 'source_raw_latched', 'source_delay',
              'source_write', 'source_child', 'source_valid', 'source_unbound',
              'motor_admission', 'motor_worker', 'mapping_frozen', 'mapping_unterminated',
              'mapping_unmapped', 'motor_timeout', 'motor_prefix_failure', 'motor_success_evidence'):
    setattr(ModbusBootSnapshotTests, 'test_'+_case, lambda self, case=_case: self.check(case))

if __name__ == '__main__':
    unittest.main()
