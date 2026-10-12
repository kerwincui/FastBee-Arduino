"""Exercise the actual OTA self-check translation unit using only SDK seams."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class OtaSelfCheckTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise AssertionError('C++17 host compiler required')
        temporary = tempfile.TemporaryDirectory(prefix='fastbee-ota-self-check-')
        cls.addClassCleanup(temporary.cleanup)
        cls.binary = Path(temporary.name) / ('self-check.exe' if os.name == 'nt' else 'self-check')
        cls.environment = dict(os.environ)
        cls.environment['PATH'] = str(Path(compiler).parent) + os.pathsep + cls.environment.get('PATH', '')
        built = subprocess.run([
            compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
            '-I' + str(ROOT / 'test/unit/cpp/ota_self_check_mocks'),
            '-I' + str(ROOT / 'include'),
            str(ROOT / 'src/systems/OtaSelfCheck.cpp'),
            str(ROOT / 'test/unit/cpp/ota_self_check_harness.cpp'),
            '-o', str(cls.binary),
        ], capture_output=True, text=True, encoding='utf-8', errors='replace',
            timeout=60, env=cls.environment)
        if built.returncode:
            raise AssertionError(f'compile exit={built.returncode}\n{built.stdout or ""}{built.stderr or ""}')

    def scenario(self, name):
        ran = subprocess.run([str(self.binary), name], capture_output=True,
                             text=True, encoding='utf-8', errors='replace',
                             timeout=10, env=self.environment)
        self.assertEqual(ran.returncode, 0,
                         f'{name}: exit={ran.returncode}\n{ran.stdout or ""}{ran.stderr or ""}')


SCENARIOS = (
    'new_confirm', 'pending_confirm', 'no_partition', 'state_error',
    'valid_image', 'invalid_image', 'aborted_image', 'undefined_image',
    'confirmation_failure_limit', 'confirmation_retry_success',
    'window_wrap', 'retry_wrap', 'repeat_begin',
    'rollback_new', 'rollback_pending', 'rollback_failure',
    'rollback_state_error', 'rollback_no_partition',
    'rollback_fresh_state', 'rollback_before_begin',
    'rollback_after_confirmed', 'rollback_after_confirmation_exhausted',
)
for scenario in SCENARIOS:
    def test(self, name=scenario):
        self.scenario(name)
    setattr(OtaSelfCheckTests, 'test_' + scenario, test)


if __name__ == '__main__':
    unittest.main()
