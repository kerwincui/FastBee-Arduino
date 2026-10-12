"""Real quality state machine; platform fake cannot read or write a host/device clock."""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class SystemTimeQualityTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise RuntimeError('C++17 compiler required')
        cls.inputs = [ROOT / path for path in ('include/utils/SystemTimeQuality.h',
            'include/utils/SystemTimePlatform.h', 'src/utils/SystemTimeQuality.cpp',
            'test/unit/cpp/system_time_quality_harness.cpp')]
        cls.hashes = [hashlib.sha256(path.read_bytes()).hexdigest() for path in cls.inputs]
        cls.directory = tempfile.TemporaryDirectory(prefix='fastbee-time-quality-')
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = Path(cls.directory.name) / ('quality.exe' if os.name == 'nt' else 'quality')
        built = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
            '-I' + str(ROOT / 'include'), str(cls.inputs[-1]), str(cls.inputs[-2]), '-o', str(cls.binary)],
            capture_output=True, text=True, timeout=60)
        if built.returncode:
            raise AssertionError(built.stdout + built.stderr)

    @classmethod
    def tearDownClass(cls):
        if cls.hashes != [hashlib.sha256(path.read_bytes()).hexdigest() for path in cls.inputs]:
            raise AssertionError('Time quality inputs changed during tests')

    def check(self, case):
        ran = subprocess.run([str(self.binary), case], capture_output=True, text=True, timeout=10)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)


for _case in ('unknown', 'apply', 'revision', 'range', 'write_failure', 'clock_jump',
              'long_uptime', 'sntp', 'sntp_race', 'binding_window', 'binding_revision',
              'binding_expiry', 'binding_clock', 'binding_unknown', 'clock_return',
              'invalid_return', 'monotonic_return', 'monotonic_before_sync',
              'stale_rollback', 'failed_resync_latched', 'drift_tolerance',
              'sntp_nested', 'apply_capture_race', 'snapshot_wall_race',
              'snapshot_mono_race', 'failed_write_keeps_unknown', 'monotonic_conversion',
              'monotonic_zero', 'monotonic_invalid_before_write', 'monotonic_invalid_after_write',
              'monotonic_rollback_after_write', 'monotonic_invalid_sntp', 'monotonic_invalid_snapshot'):
    setattr(SystemTimeQualityTests, 'test_' + _case, lambda self, case=_case: self.check(case))

if __name__ == '__main__':
    unittest.main()
