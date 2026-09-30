import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test_sensor_publication import definition

ROOT=Path(__file__).resolve().parents[2]
class RmtDhtTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler=shutil.which('g++') or shutil.which('clang++')
        if not compiler: raise unittest.SkipTest('C++17 compiler required')
        source_path=Path(os.environ.get('FASTBEE_SENSOR_SOURCE',ROOT/'src/peripherals/SensorDriver.cpp'))
        source=source_path.read_text(encoding='utf-8')
        cls.temp=tempfile.TemporaryDirectory(prefix='fastbee-dht-rmt-')
        cls.addClassCleanup(cls.temp.cleanup)
        directory=Path(cls.temp.name)
        include=definition(source,'struct DhtReadDiagnostic')+';\n'+(source_path.parent/'DhtRmtCapture.h').read_text(encoding='utf-8').replace('#pragma once','')+'\n'+definition(source,'static bool readDhtBounded(')
        (directory/'dht_rmt_reader.inc').write_text(include,encoding='utf-8')
        cls.exe=directory/'test.exe'
        result=subprocess.run([compiler,'-std=c++17','-O2','-Wall','-Wextra','-Werror','-I'+str(directory),str(ROOT/'test/unit/cpp/dht_rmt_harness.cpp'),'-o',str(cls.exe)],capture_output=True,text=True,timeout=60)
        if result.returncode: raise AssertionError(result.stdout+result.stderr)

def make_test(name):
    def test(self):
        result=subprocess.run([str(self.exe),name],capture_output=True,text=True,timeout=5)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
    return test
for case in ['valid','dht22','negative','no-prefix','release-prefix','cpu-preempted','wrap','stuck-low','init-fail','config-fail','arm-fail','timeout','no-response','truncated','long-pulse','checksum','range','cleanup-recovery','arm-preempted','arm-preempted-dht22','arm-preempted-repeat','arm-retry-fail','arm-delay-short','arm-expiry-after-check']:
    setattr(RmtDhtTests,'test_'+case.replace('-','_'),make_test(case))
if __name__=='__main__': unittest.main(verbosity=2)
