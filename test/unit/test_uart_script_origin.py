"""Prepared production script execution/delay provenance with counted I/O fakes."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test.unit.test_periph_exec_config_persistence import cpp_function as method

ROOT = Path(__file__).resolve().parents[2]


class UartScriptOriginTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise RuntimeError('C++17 compiler required')
        headers = list((ROOT / '.pio/libdeps').glob('*/ArduinoJson/src/ArduinoJson.h'))
        if not headers:
            raise RuntimeError('Installed ArduinoJson headers required by FeatureFlags')
        source = (ROOT/'src/core/ScriptEngine.cpp').read_text(encoding='utf-8')
        production = '\n'.join(method(source, signature) for signature in (
            'bool ScriptEngine::execute(', 'bool ScriptEngine::scriptDelay('))
        executor = (ROOT/'src/core/PeriphExecExecutor.cpp').read_text(encoding='utf-8')
        production += '\n' + method(executor, 'bool PeriphExecExecutor::executeScriptAction(')
        # Use the real native dispatcher: absent context must be rejected here,
        # before the counted PM submission seam can accept or touch an output.
        dispatch = (ROOT/'include/core/NativeOutputDispatch.h').read_text(encoding='utf-8')
        production = 'namespace FastBee {\n' + '\n'.join(method(dispatch, signature)
            for signature in ('inline bool isOrdinaryDigitalTarget(',
                'inline bool isOrdinaryScalarTarget(', 'inline bool ordinaryScalarMaximum(',
                'inline bool ordinaryScalarHighValue(',
                'inline void rememberOrdinaryControlResult(',
                'inline bool rejectOrdinaryDigitalAction(',
                'inline bool dispatchOrdinaryDigitalAction(',
                'inline bool dispatchOrdinaryScalarAction(')) + '\n}\n' + production
        template = (ROOT/'test/unit/cpp/uart_script_origin_harness.cpp').read_text(encoding='utf-8')
        cls.temp = tempfile.TemporaryDirectory(prefix='fastbee-uart-script-')
        cls.addClassCleanup(cls.temp.cleanup)
        folder = Path(cls.temp.name)
        cpp = folder/'script.cpp'
        cpp.write_text(template.replace('// @production@', production), encoding='utf-8')
        cls.binaries = {}
        for segment, mqtt in ((0, 1), (1, 1), (0, 0)):
            binary = folder/('script-'+str(segment)+'-mqtt-'+str(mqtt)+'.exe')
            result = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                '-DFASTBEE_ENABLE_COMMAND_SCRIPT=1', '-DFASTBEE_ENABLE_MQTT='+str(mqtt),
                '-DFASTBEE_ENABLE_PERIPH_EXEC=1', '-DFASTBEE_ENABLE_LCD=1', '-DCHIP_HAS_DAC=1',
                '-DFASTBEE_ENABLE_SEVEN_SEGMENT='+str(segment),
                '-I'+str(headers[0].parent), '-I'+str(ROOT/'test/mocks'), '-I'+str(ROOT/'include'),
                str(cpp), '-o', str(binary)], capture_output=True, text=True, timeout=60)
            if result.returncode:
                raise AssertionError(result.stdout+result.stderr)
            cls.binaries[(segment, mqtt)] = binary

    def check(self, case):
        for (segment, mqtt), binary in self.binaries.items():
            if case == 'bind_disabled' and mqtt:
                continue
            if not mqtt and case not in ('bind_disabled', 'bind_no_mqtt') and not case.startswith(('periph_', 'native_')):
                continue
            if case == 'segment_template' and not segment:
                continue
            with self.subTest(segment=segment, mqtt=mqtt):
                run = subprocess.run([str(binary), case], capture_output=True, text=True, timeout=10)
                self.assertEqual(run.returncode, 0, run.stdout+run.stderr)

    def test_production_executor_passes_origin_and_failure_to_outer_rule(self):
        source = (ROOT/'src/core/PeriphExecExecutor.cpp').read_text(encoding='utf-8')
        self.assertIn('executeScriptAction(action, uartOrigin, sourceUnconfirmed, control, controlResult)',
                      method(source, 'bool PeriphExecExecutor::executeActionItem('))
        self.assertIn('ScriptEngine::execute(cmds, publisher, uartOrigin, sourceUnconfirmed, control, controlResult)',
                      method(source, 'bool PeriphExecExecutor::executeScriptAction('))
        run = method(source, 'std::vector<ActionExecResult> PeriphExecExecutor::executeAllActions(')
        group = method(run, 'if (groupCount > 1)')
        self.assertIn('FastBee::UartEventOrigin{}, &sourceUnconfirmed)', group)
        self.assertIn('AsyncExecError::UART_SOURCE_UNCONFIRMED', group)
        self.assertIn('if (sourceUnconfirmed || operationDeadlineReached(deadlineAt))', group)
        self.assertIn('*completedAllOut = false', group)
        self.assertIn('break;', group)


for _case in ('stale', 'malformed', 'busy', 'empty_stale', 'delay', 'last_delay',
              'between', 'last_write', 'claim', 'pwm_release_retired', 'pwm_attach_failure', 'pwm_write_failure', 'lookup', 'stop_then_write',
              'lcd_template', 'segment_template', 'mqtt', 'mqtt_missing', 'mqtt_negative',
              'mqtt_expansion', 'log_expansion', 'valid', 'unbound', 'lcd_clear',
              'bind_valid', 'bind_rebuild_delay', 'bind_unknown', 'bind_absent_start',
              'bind_absent_publish', 'bind_offline', 'bind_rejected', 'bind_failed_after_gpio', 'bind_no_mqtt',
              'bind_rebuild_after_first', 'bind_source_after_acquire', 'bind_disabled',
              'native_high_no_context', 'native_low_no_context', 'native_blink_no_context',
              'native_stop_no_context', 'native_high_no_context_after_prefix'):
    setattr(UartScriptOriginTests, 'test_'+_case, lambda self, case=_case: self.check(case))

for _command in ('high', 'low', 'pwm', 'native_high', 'native_low'):
    for _outcome in ('success', 'rejected', 'after_prefix'):
        _case = 'periph_' + _command + '_' + _outcome
        setattr(UartScriptOriginTests, 'test_' + _case,
                lambda self, case=_case: self.check(case))

if __name__ == '__main__':
    unittest.main()
