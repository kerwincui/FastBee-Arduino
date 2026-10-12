"""Production HTTP time method with real JSON; network/system clock mocked only."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test_sensor_publication import definition

ROOT = Path(__file__).resolve().parents[2]


class HttpTimeValidationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise unittest.SkipTest('C++17 compiler required')
        json_root = os.environ.get('FASTBEE_TEST_ARDUINOJSON_INCLUDE')
        if not json_root:
            installed = list((ROOT / '.pio/libdeps').glob('*/ArduinoJson/src/ArduinoJson.h'))
            if not installed:
                raise AssertionError('Installed ArduinoJson headers required')
            json_root = str(installed[0].parent)
        cls.temporary = tempfile.TemporaryDirectory(prefix='fastbee-http-time-')
        cls.addClassCleanup(cls.temporary.cleanup)
        folder = Path(cls.temporary.name)
        source = (ROOT / 'src/utils/TimeUtils.cpp').read_text(encoding='utf-8')
        method = ('std::atomic_flag httpTimeSyncBusy = ATOMIC_FLAG_INIT;\n' +
                  definition(source, 'class HttpTimeSyncGuard') + ';\n' +
                  definition(source, 'bool TimeUtils::syncNTPFromHTTPWithTimestamp('))
        (folder / 'time_http_under_test.inc').write_text(method, encoding='utf-8')
        cls.binary = folder / 'http-time.exe'
        command = [compiler, '-std=c++17', '-pthread', '-Wall', '-Wextra', '-Werror',
                   '-Wno-deprecated-declarations', '-I' + str(folder),
                   '-I' + str(ROOT / 'test/mocks'), '-I' + json_root,
                   '-I' + str(ROOT / 'include'),
                   str(ROOT / 'test/unit/cpp/http_time_validation_harness.cpp'),
                   str(ROOT / 'src/utils/SystemTimeQuality.cpp'),
                   str(ROOT / 'src/utils/SystemTimeZone.cpp'), '-o', str(cls.binary)]
        result = subprocess.run(command, capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def run_case(self, scenario):
        result = subprocess.run([str(self.binary), scenario], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


for scenario in ('valid', 'nested', 'strings', 'xml', 'wrap', 'body_delay',
                 'set_failed', 'begin_failed', 'http_error', 'redirect', 'expired',
                 'invalid_samples', 'partial_pair', 'xml_junk', 'zero_timeout',
                 'oversized_timeout', 'tls_bootstrap', 'tls_failed', 'no_bundle',
                 'bounds32', 'bounds64', 'strict_parser', 'concurrent', 'source_changed',
                 'tls_rtc_bootstrap', 'tls_rtc_failed', 'tls_unknown_clock', 'tls_known_source'):
    setattr(HttpTimeValidationTests, 'test_' + scenario,
            lambda self, name=scenario: self.run_case(name))

if __name__ == '__main__':
    unittest.main()
