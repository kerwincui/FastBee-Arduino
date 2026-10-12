"""Prepared UART executor regressions; no real serial I/O. Run at final gates."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class UartActionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise RuntimeError('C++17 compiler required')
        cls.directory = tempfile.TemporaryDirectory(prefix='fastbee-uart-action-')
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = Path(cls.directory.name) / 'uart-action.exe'
        json_root = os.environ.get('FASTBEE_TEST_ARDUINOJSON_INCLUDE')
        if not json_root:
            headers = list((ROOT / '.pio/libdeps').glob('*/ArduinoJson/src/ArduinoJson.h'))
            if not headers:
                raise RuntimeError('Installed ArduinoJson headers required')
            json_root = str(headers[0].parent)
        command = [compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-DFASTBEE_ENABLE_PERIPH_EXEC=1',
                   '-DARDUINOJSON_ENABLE_ARDUINO_STRING=1', '-I' + json_root]
        command += ['-I' + str(ROOT / path) for path in (
            'test/unit/cpp/uart_action_mocks', 'test/unit/cpp/rtc_action_mocks',
            'test/mocks', 'test/unit/cpp/mocks', 'include')]
        command += [str(ROOT / 'test/unit/cpp/uart_action_harness.cpp'),
                    str(ROOT / 'src/core/PeriphExecUart.cpp'), '-o', str(cls.binary)]
        built = subprocess.run(command, capture_output=True, text=True, timeout=90)
        if built.returncode:
            raise AssertionError(built.stdout + built.stderr)

    def check(self, case):
        ran = subprocess.run([str(self.binary), case], capture_output=True, text=True, timeout=10)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)


for _case in ('text', 'generic', 'literal_command', 'json_literal', 'json_injection', 'target_redirect',
              'target_template', 'invalid_command', 'numeric_payload', 'nul_payload', 'two_fields',
              'unknown_field', 'malformed', 'empty_json', 'empty', 'unresolved', 'repeated',
              'nonrecursive', 'direct', 'max_bytes', 'oversize', 'expanded_oversize', 'utf8_bytes',
              'changed', 'missing', 'wrong_type', 'disabled', 'no_state', 'suspended', 'lost_lease',
              'busy', 'partial', 'wrong_action', 'queued_normal', 'queued_gpio', 'queued_changed',
              'queued_retyped', 'queued_missing', 'queued_busy', 'queued_changed_after_resolve'):
    setattr(UartActionTests, 'test_' + _case, lambda self, case=_case: self.check(case))

if __name__ == '__main__':
    unittest.main()
