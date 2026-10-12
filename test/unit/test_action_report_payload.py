"""Prepared real JSON serialization/batching boundary tests, no MQTT/device."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class ActionReportPayloadTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise RuntimeError('C++17 compiler required')
        cls.directory = tempfile.TemporaryDirectory(prefix='fastbee-report-payload-')
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = Path(cls.directory.name) / 'report-payload.exe'
        json_root = os.environ.get('FASTBEE_TEST_ARDUINOJSON_INCLUDE')
        if not json_root:
            headers = list((ROOT / '.pio/libdeps').glob('*/ArduinoJson/src/ArduinoJson.h'))
            if not headers:
                raise RuntimeError('Installed ArduinoJson headers required')
            json_root = str(headers[0].parent)
        built = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
            '-I'+str(ROOT/'test/mocks'), '-I'+str(ROOT/'include'), '-I'+json_root,
            str(ROOT/'test/unit/cpp/action_report_payload_harness.cpp'), '-o', str(cls.binary)],
            capture_output=True, text=True, timeout=90)
        if built.returncode:
            raise AssertionError(built.stdout + built.stderr)

    def check(self, case):
        ran = subprocess.run([str(self.binary), case], capture_output=True, text=True, timeout=10)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)


for _case in ('empty', 'invalid', 'oversize', 'late_oversize', 'exact', 'sd', 'escaped', 'utf8', 'many', 'multi', 'small',
              'captured_identity', 'captured_failures', 'captured_peripheral',
              'sensor_sample', 'sensor_stale', 'sensor_unknown', 'sensor_unit_lost'):
    setattr(ActionReportPayloadTests, 'test_' + _case, lambda self, case=_case: self.check(case))

if __name__ == '__main__':
    unittest.main()
