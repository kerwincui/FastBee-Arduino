"""Production rule revision and system-action stage checks; no hardware access."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test.unit.test_periph_exec_config_persistence import cpp_function

ROOT = Path(__file__).resolve().parents[2]


class RuleExecutionRevisionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        headers = list((ROOT/'.pio/libdeps').glob('*/ArduinoJson/src/ArduinoJson.h'))
        if not compiler or not headers:
            raise RuntimeError('C++17 and ArduinoJson are required')
        production = '\n'.join(cpp_function((ROOT/file).read_text(encoding='utf-8'), signature)
            for file, signature in (
                ('src/core/PeriphExecRuleQueries.cpp', 'bool PeriphExecManager::isRuleExecutionCurrent('),
                ('src/core/PeriphExecManager.cpp', 'void PeriphExecManager::refreshRuleRuntimeIndexesLocked('),
                ('src/core/PeriphExecExecutor.cpp', 'bool PeriphExecExecutor::executeSystemAction(')))
        cls.temp = tempfile.TemporaryDirectory(prefix='fastbee-rule-revision-')
        cls.addClassCleanup(cls.temp.cleanup)
        cpp = Path(cls.temp.name)/'revision.cpp'
        cpp.write_text((ROOT/'test/unit/cpp/rule_execution_revision_harness.cpp.in').read_text(
            encoding='utf-8').replace('// @production@', production), encoding='utf-8')
        cls.binary = cpp.with_suffix('.exe')
        built = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
            '-DFASTBEE_ENABLE_OTA=1', '-DARDUINOJSON_ENABLE_ARDUINO_STRING=1',
            '-I'+str(ROOT/'test/mocks'), '-I'+str(ROOT/'include'), '-I'+str(headers[0].parent),
            str(cpp), '-o', str(cls.binary)], capture_output=True, text=True, timeout=90)
        if built.returncode:
            raise AssertionError(built.stdout+built.stderr)

    def check(self, mode):
        result = subprocess.run([str(self.binary), mode], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)


for _mode in ('revision', 'exhausted', 'normal', 'initial', 'save', 'get-network', 'mirror',
              'event', 'restart-event', 'ntp-retire', 'ota', 'unbound'):
    setattr(RuleExecutionRevisionTests, 'test_'+_mode, lambda self, mode=_mode: self.check(mode))
