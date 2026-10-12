"""Prepared production SD API lifecycle guard; passive manager/RTOS fakes only."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[2]


class SdAccessTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler=shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise RuntimeError('C++17 compiler required')
        cls.folder=tempfile.TemporaryDirectory(prefix='fastbee-sd-access-')
        cls.addClassCleanup(cls.folder.cleanup)
        cls.binary=Path(cls.folder.name)/'access.exe'
        result=subprocess.run([compiler,'-std=c++17','-Wall','-Wextra','-Werror',
            *['-I'+str(ROOT/path) for path in ('test/unit/cpp/sd_action_mocks',
              'test/unit/cpp/rtc_action_mocks','test/mocks','test/unit/cpp/mocks','include')],
            str(ROOT/'test/unit/cpp/sd_access_harness.cpp'),'-o',str(cls.binary)],
            capture_output=True,text=True,timeout=60)
        if result.returncode:
            raise AssertionError(result.stdout+result.stderr)

    def check(self,case):
        result=subprocess.run([str(self.binary),case],capture_output=True,text=True,timeout=10)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)


for _case in ('valid','busy','missing','wrong_type','changed','disabled','suspended',
              'runtime_missing','snapshot_busy','owner','pins','frequency','unmounted',
              'lost_bus','lost_owner','lost_pins'):
    setattr(SdAccessTests,'test_'+_case,lambda self,case=_case:self.check(case))

if __name__=='__main__':
    unittest.main()
