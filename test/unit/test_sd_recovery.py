"""Prepared real SD manager/SPI lifecycle recovery with deterministic media faults."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[2]


class SdRecoveryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler=shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise RuntimeError('C++17 compiler required')
        json_root=os.environ.get('FASTBEE_TEST_ARDUINOJSON_INCLUDE')
        if not json_root:
            installed=list((ROOT/'.pio/libdeps').glob('*/ArduinoJson/src/ArduinoJson.h'))
            if not installed:
                raise RuntimeError('Installed ArduinoJson required')
            json_root=str(installed[0].parent)
        cls.folder=tempfile.TemporaryDirectory(prefix='fastbee-sd-recovery-')
        cls.addClassCleanup(cls.folder.cleanup)
        cls.binary=Path(cls.folder.name)/'recovery.exe'
        result=subprocess.run([compiler,'-std=c++17','-pthread','-Wall','-Wextra','-Werror',
            *['-I'+str(ROOT/path) for path in ('test/unit/cpp/sd_bus_mocks','test/unit/cpp/spi_bus_mocks','test/mocks','include')],
            '-I'+json_root,*[str(ROOT/path) for path in ('test/unit/cpp/sd_recovery_harness.cpp',
            'src/peripherals/SDCardManager.cpp','src/core/SpiBusInitializer.cpp',
            'src/network/FixedJsonField.cpp')],'-o',str(cls.binary)],
            capture_output=True,text=True,timeout=90)
        if result.returncode:
            raise AssertionError(result.stdout+result.stderr)

    def check(self,case):
        result=subprocess.run([str(self.binary),case],capture_output=True,text=True,timeout=15)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)


for _case in ('cold','unmounted_reads','unmounted_writes','owner','busy','write_failure',
              'read_history','mount_failure','shared_bus','bus_lost','lost_peer',
              'delete_unconfirmed','delete_failure','json_history'):
    setattr(SdRecoveryTests,'test_'+_case,lambda self,case=_case:self.check(case))

if __name__=='__main__':
    unittest.main()
