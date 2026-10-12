"""Production timezone ownership; fake SDK/environment never changes host TZ or time."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class SystemTimezoneTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise RuntimeError('C++17 compiler required')
        cls.folder = tempfile.TemporaryDirectory(prefix='fastbee-timezone-')
        cls.addClassCleanup(cls.folder.cleanup)
        cls.binary = Path(cls.folder.name) / 'timezone.exe'
        result = subprocess.run([compiler, '-std=c++17', '-pthread', '-Wall', '-Wextra', '-Werror',
            '-I' + str(ROOT / 'include'), str(ROOT / 'src/utils/SystemTimeZone.cpp'),
            str(ROOT / 'test/unit/cpp/system_timezone_harness.cpp'), '-o', str(cls.binary)],
            capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def check(self, case):
        result = subprocess.run([str(self.binary), case], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_all_application_tz_writers_use_one_platform_boundary(self):
        import re
        writers = re.compile(r'\b(?:configTzTime|configTime|tzset)\s*\(|\bsetenv\s*\(\s*"TZ"')
        for path in (ROOT / 'src').rglob('*.cpp'):
            if path.name == 'SystemTimePlatform.cpp':
                continue
            # Do not treat explanatory comments as function calls.
            source = re.sub(r'//[^\n]*|/\*.*?\*/', '', path.read_text(encoding='utf-8'), flags=re.S)
            self.assertIsNone(writers.search(source), str(path))


for _case in ('preserve', 'set', 'default', 'invalid', 'busy', 'write_failure', 'sdk_drift', 'buffer',
              'local_snapshot', 'local_invalid', 'local_busy', 'local_failed'):
    setattr(SystemTimezoneTests, 'test_' + _case, lambda self, case=_case: self.check(case))

if __name__ == '__main__':
    unittest.main()
