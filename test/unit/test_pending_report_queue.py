"""Prepared real queue/JSON and deterministic OS-thread contention regressions."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class PendingReportQueueTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise RuntimeError('C++17 compiler required')
        cls.directory = tempfile.TemporaryDirectory(prefix='fastbee-pending-reports-')
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = Path(cls.directory.name) / 'pending-reports.exe'
        json_root = os.environ.get('FASTBEE_TEST_ARDUINOJSON_INCLUDE')
        if not json_root:
            headers = list((ROOT / '.pio/libdeps').glob('*/ArduinoJson/src/ArduinoJson.h'))
            if not headers:
                raise RuntimeError('Installed ArduinoJson headers required')
            json_root = str(headers[0].parent)
        built = subprocess.run([compiler, '-std=c++17', '-pthread', '-Wall', '-Wextra', '-Werror',
            '-include', str(ROOT/'test/mocks/Arduino.h'),
            '-I'+str(ROOT/'test/unit/cpp/pending_report_mocks'), '-I'+str(ROOT/'test/mocks'),
            '-I'+str(ROOT/'include'), '-I'+json_root,
            str(ROOT/'test/unit/cpp/pending_report_queue_harness.cpp'),
            str(ROOT/'src/network/FixedJsonField.cpp'), '-o', str(cls.binary)],
            capture_output=True, text=True, timeout=90)
        if built.returncode:
            raise AssertionError(built.stdout + built.stderr)

    def check(self, case):
        ran = subprocess.run([str(self.binary), case], capture_output=True, text=True, timeout=15)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)


for _case in ('unavailable','invalid','low_memory','busy','empty','offline','failed_retained',
              'throttle','wrap','full_evict','reentrant','completion_busy','callback_exception',
              'age','json','json_unavailable','concurrent_pin','max_bytes','accepted',
              'clock_under_lock','retry_details','retry_detail_wrap',
              'instance_unknown','instance_sequence','instance_reuse','instance_sequence_threads',
              'instance_same','instance_mismatch','instance_offline','instance_rejected',
              'instance_mixed','instance_completion_busy',
              'collection_release','collection_busy','collection_absent','collection_test_only',
              'collection_same','collection_changed','collection_unknown','collection_offline',
              'collection_refused','collection_dropped','collection_batch',
              'admission_changed_full','admission_changed_inflight','admission_changed_busy',
              'admission_changed_history','admission_identity_unknown','admission_disconnected_changed'):
    setattr(PendingReportQueueTests, 'test_' + _case, lambda self, case=_case: self.check(case))

if __name__ == '__main__':
    unittest.main()
