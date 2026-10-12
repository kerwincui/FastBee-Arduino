"""Production output-report capture across configuration/I/O boundaries."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test.unit.test_periph_exec_config_persistence import cpp_function

ROOT = Path(__file__).resolve().parents[2]


class OutputReportBindingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise RuntimeError('C++17 compiler required')
        cls.source = (ROOT/'src/core/PeriphExecExecutor.cpp').read_text(encoding='utf-8')
        methods = '\n'.join(cpp_function(cls.source, signature) for signature in (
            'String normalizeScalarReportValue(', 'void appendActionResult(',
            'void prepareOutputReport(', 'void finishOutputReport('))
        template = (ROOT/'test/unit/cpp/output_report_binding_harness.cpp.in').read_text(encoding='utf-8')
        cls.temp = tempfile.TemporaryDirectory(prefix='fastbee-output-report-')
        cls.addClassCleanup(cls.temp.cleanup)
        folder = Path(cls.temp.name)
        source = folder/'binding.cpp'
        source.write_text(template.replace('// @production@', methods), encoding='utf-8')
        cls.binary = folder/'binding.exe'
        built = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
            '-I'+str(ROOT/'test/mocks'), '-I'+str(ROOT/'include'), str(source), '-o', str(cls.binary)],
            capture_output=True, text=True, timeout=90)
        if built.returncode:
            raise AssertionError(built.stdout+built.stderr)

    def check(self, mode):
        run = subprocess.run([str(self.binary), mode], capture_output=True, text=True, timeout=10)
        self.assertEqual(run.returncode, 0, run.stdout+run.stderr)

    def test_serializer_never_resolves_current_configuration(self):
        body = cpp_function(self.source, 'void PeriphExecExecutor::reportActionResults(')
        for forbidden in ('getPeripheralSnapshot', 'buildSensorId', 'acquireModbusHandler',
                          'looksLikeHexPayload', 'substring(', '.toInt('):
            self.assertNotIn(forbidden, body)
        for caller in ('std::vector<ActionExecResult> PeriphExecExecutor::executeAllActions(',
                       'size_t PeriphExecExecutor::reportRuleCurrentState('):
            body = cpp_function(self.source, caller)
            self.assertIn('prepareOutputReport(', body)
            self.assertIn('finishOutputReport(', body)


for _mode in ('gpio', 'mapped', 'register', 'single_channel', 'unmapped', 'owner_changed',
              'owner_busy', 'snapshot_changed', 'snapshot_missing', 'malformed_channel',
              'unterminated_id', 'already_invalid', 'split_boundaries', 'failed_write'):
    setattr(OutputReportBindingTests, 'test_'+_mode, lambda self, mode=_mode: self.check(mode))

if __name__ == '__main__':
    unittest.main()
