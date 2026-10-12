"""Real UART scheduler, framer, fault registry and cross-unit callback state.
No SDK driver, UART, device or production output is opened by these tests.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class UartReceiveFaultTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise RuntimeError('C++17 compiler required')
        json_root = os.environ.get('FASTBEE_TEST_ARDUINOJSON_INCLUDE')
        if not json_root:
            headers = list((ROOT/'.pio/libdeps').glob('*/ArduinoJson/src/ArduinoJson.h'))
            if not headers:
                raise RuntimeError('Installed ArduinoJson headers required')
            json_root = str(headers[0].parent)
        source = (ROOT/'src/core/PeriphExecScheduler.cpp').read_text(encoding='utf-8')
        start = source.index('void PeriphExecScheduler::checkSerialEvents(')
        scheduler = source[start:source.index('\n}', start)+2]
        template = (ROOT/'test/unit/cpp/uart_receive_fault_harness.cpp.in').read_text(encoding='utf-8')
        code = template.replace('// @scheduler@', scheduler)
        cls.temp = tempfile.TemporaryDirectory(prefix='fastbee-uart-rx-fault-')
        cls.addClassCleanup(cls.temp.cleanup)
        cpp = Path(cls.temp.name)/'scheduler.cpp'
        cpp.write_text(code, encoding='utf-8')
        other = Path(cls.temp.name)/'peer.cpp'
        other.write_text('''#include "core/UartReceiveFault.h"
bool peerHealthy(uint8_t p,uint32_t g) { return FastBee::UartReceiveFault::healthy(p,g); }
void peerObserve(uint8_t p,uint32_t g) { FastBee::UartReceiveFault::observe(p,g,FastBee::UartReceiveFault::Error::Frame); }
''', encoding='utf-8')
        cls.binary = Path(cls.temp.name)/'uart-rx-fault.exe'
        built = subprocess.run([compiler, '-std=c++17', '-pthread', '-Wall', '-Wextra', '-Werror',
            '-I'+str(ROOT/'test/mocks'), '-I'+str(ROOT/'include'), '-I'+json_root,
            str(cpp), str(other), '-o', str(cls.binary)],
            capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=90)
        if built.returncode:
            raise AssertionError(built.stdout+built.stderr)

    def check(self, case):
        ran = subprocess.run([str(self.binary), case], capture_output=True, text=True, timeout=20)
        self.assertEqual(ran.returncode, 0, ran.stdout+ran.stderr)


for _case in ('normal', 'after_read', 'during_serialization', 'between_frames',
              'idle_fault', 'partial_restart', 'registry_concurrent'):
    setattr(UartReceiveFaultTests, 'test_'+_case, lambda self, case=_case: self.check(case))

if __name__ == '__main__':
    unittest.main()
