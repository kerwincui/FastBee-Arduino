"""Production SNTP wait and event routing at the RTC boot source boundary."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test_sensor_publication import definition

ROOT = Path(__file__).resolve().parents[2]


class RtcTimeConsumerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise RuntimeError('C++17 compiler required')
        cls.temp = tempfile.TemporaryDirectory(prefix='fastbee-rtc-consumers-')
        cls.addClassCleanup(cls.temp.cleanup)
        folder = Path(cls.temp.name)
        wait_source = (ROOT/'src/utils/TimeUtils.cpp').read_text(encoding='utf-8')
        (folder/'rtc_sync_wait_under_test.inc').write_text(
            definition(wait_source, 'bool TimeUtils::syncNTP('), encoding='utf-8')
        manager = (ROOT/'src/core/PeriphExecManager.cpp').read_text(encoding='utf-8')
        scheduler = (ROOT/'src/core/PeriphExecScheduler.cpp').read_text(encoding='utf-8')
        (folder/'rtc_event_under_test.inc').write_text('\n'.join((
            definition(manager, 'void PeriphExecManager::dispatchEventMatchedRules('),
            definition(scheduler, 'void PeriphExecScheduler::triggerEvent('))), encoding='utf-8')
        json_root = os.environ.get('FASTBEE_TEST_ARDUINOJSON_INCLUDE')
        if not json_root:
            installed = list(Path(os.environ.get('PLATFORMIO_LIBDEPS_DIR',str(ROOT/'.pio/libdeps'))).glob('*/ArduinoJson/src/ArduinoJson.h'))
            if not installed:
                raise RuntimeError('Installed ArduinoJson headers required')
            json_root = str(installed[0].parent)
        cls.binaries = {}
        sources = {
            'wait': ('test/unit/cpp/rtc_sync_wait_harness.cpp', 'src/utils/SystemTimeQuality.cpp'),
            'event': ('test/unit/cpp/rtc_ntp_event_harness.cpp', 'src/core/PeripheralExecution.cpp', 'src/network/LiteralStringMatch.cpp'),
        }
        for kind, paths in sources.items():
            binary = folder/(kind+'.exe')
            command = [compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                       '-Wno-deprecated-declarations', '-I'+str(folder),
                       '-I'+str(ROOT/'test/mocks'), '-I'+str(ROOT/'include'), '-I'+json_root]
            command += [str(ROOT/path) for path in paths]
            result = subprocess.run(command+['-o', str(binary)], capture_output=True,
                                    text=True, encoding='utf-8', errors='replace', timeout=60)
            if result.returncode:
                raise AssertionError(result.stdout+result.stderr)
            cls.binaries[kind] = binary

    def check(self, kind, case):
        result = subprocess.run([str(self.binaries[kind]), case,
                                 str(ROOT/'docs/rtc-ntp-writeback-disabled-example-20261004.json')],
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)

    def test_rtc_sequence_change_is_not_sntp_success(self): self.check('wait', 'rtc_only')
    def test_real_observer_after_rtc_seed_completes_wait(self): self.check('wait', 'sntp_after_rtc')


for _case in ('disabled', 'matched', 'wrong_event', 'uninitialized', 'cooldown'):
    setattr(RtcTimeConsumerTests, 'test_ntp_event_'+_case,
            lambda self, case=_case: self.check('event', case))

if __name__ == '__main__':
    unittest.main()
