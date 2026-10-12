"""Run production timer scan/admission/commit with explicit clock/queue seams."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test_sensor_publication import definition

ROOT = Path(__file__).resolve().parents[2]


class DailyCalendarWatermarkTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise RuntimeError('C++17 compiler required')
        cls.temp = tempfile.TemporaryDirectory(prefix='fastbee-daily-watermark-')
        cls.addClassCleanup(cls.temp.cleanup)
        folder = Path(cls.temp.name)
        source = (ROOT/'src/core/PeriphExecManager.cpp').read_text(encoding='utf-8')
        (folder/'daily_timer_under_test.inc').write_text(
            definition(source, 'void PeriphExecManager::checkTimerTriggers('), encoding='utf-8')
        cls.binary = folder/'daily.exe'
        command = [compiler, '-std=c++17', '-pthread', '-Wall', '-Wextra', '-Werror',
                   '-I'+str(folder), '-I'+str(ROOT/'test/mocks'), '-I'+str(ROOT/'include'),
                   str(ROOT/'test/unit/cpp/daily_calendar_watermark_harness.cpp'), '-o', str(cls.binary)]
        result = subprocess.run(command, capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise AssertionError(result.stdout+result.stderr)

    def check(self, case):
        result = subprocess.run([str(self.binary), case], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)


for _case in ('same_day', 'rollback', 'advance_day', 'forward_skip', 'future_correction',
              'rejected_retry', 'rejected_window', 'late_jump', 'invalid_calendar',
              'clock_unavailable', 'interval', 'duplicate', 'revision_change',
              'busy_tick', 'guards_release'):
    setattr(DailyCalendarWatermarkTests, 'test_'+_case, lambda self, case=_case: self.check(case))

if __name__ == '__main__':
    unittest.main()
