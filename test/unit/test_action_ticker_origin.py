"""No-context timer rejection and retained legacy producer suppression.

Actual checked PWM/AO/IOX production, expiry and failure cases live in
ordinary_control_harness; no former backend is mocked as writable here.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test.unit.test_periph_exec_config_persistence import cpp_function

ROOT = Path(__file__).resolve().parents[2]


class ActionTickerOriginTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise RuntimeError('C++17 compiler required')
        cls.source = (ROOT/'src/core/PeripheralManager.cpp').read_text(encoding='utf-8')
        header = (ROOT/'include/core/PeripheralManager.h').read_text(encoding='utf-8')
        context = cpp_function(header, 'struct ActionTickerData {') + ';'
        globals_ = cpp_function(cls.source, 'struct IoBlink {') + ';\n' + cpp_function(cls.source, 'std::map<String, IoBlink>& ioBlinks()')
        methods = '\n'.join(cpp_function(cls.source, signature) for signature in (
            'void PeripheralManager::stopActionTicker(', 'static void actionTickerCallback(',
            'void PeripheralManager::serviceActionTicker(', 'bool PeripheralManager::startActionTicker('))
        harness = (ROOT/'test/unit/cpp/action_ticker_origin_harness.cpp.in').read_text(encoding='utf-8')
        harness = harness.replace('// @context@', context).replace('// @globals@', globals_).replace('// @production@', methods)
        cls.temp = tempfile.TemporaryDirectory(prefix='fastbee-ticker-origin-')
        cls.addClassCleanup(cls.temp.cleanup)
        folder = Path(cls.temp.name)
        source = folder/'ticker.cpp'
        source.write_text(harness, encoding='utf-8')
        cls.binary = folder/'ticker.exe'
        build = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
            '-I'+str(ROOT/'test/unit/cpp/ordinary_control_mocks'),
            '-I'+str(ROOT/'test/mocks'), '-I'+str(ROOT/'include'), str(source), '-o', str(cls.binary)],
            capture_output=True, text=True, timeout=90)
        if build.returncode:
            raise AssertionError(build.stdout+build.stderr)

    def check(self, mode):
        run = subprocess.run([str(self.binary), mode], capture_output=True, text=True, timeout=10)
        self.assertEqual(run.returncode, 0, run.stdout+run.stderr)

    def test_all_ticker_callers_forward_the_origin_and_result(self):
        executor = (ROOT/'src/core/PeriphExecExecutor.cpp').read_text(encoding='utf-8')
        script = (ROOT/'src/core/ScriptEngine.cpp').read_text(encoding='utf-8')
        calls = [line for line in (executor+'\n'+script).splitlines() if 'pm.startActionTicker(' in line]
        self.assertTrue(calls)
        self.assertTrue(all('uartOrigin)' in line and ('return ' in line or 'if (!' in line) for line in calls))
        poll = cpp_function(self.source, 'void PeripheralManager::pollIoExpanders(')
        self.assertIn('serviceOrdinaryIoProducerLocked(id)', poll)
        self.assertNotIn('togglePin(', poll)
        producer = cpp_function(self.source, 'void PeripheralManager::serviceOrdinaryIoProducerLocked(')
        self.assertLess(producer.index('isUartEventOriginCurrent(context.uartOrigin)'),
                        producer.index('if (!current)'))
        self.assertLess(producer.index('if (!current)'), producer.index('writeGpioStateLocked('))
        self.assertIn('&blink->second.permit', producer)


for _mode in ('digital', 'pwm', 'analog', 'passive', 'iox', 'protocol', 'unsupported',
              'unbound', 'stale', 'malformed', 'legacy_pwm', 'legacy_analog', 'legacy_passive'):
    setattr(ActionTickerOriginTests, 'test_'+_mode, lambda self, mode=_mode: self.check(mode))

if __name__ == '__main__':
    unittest.main()
