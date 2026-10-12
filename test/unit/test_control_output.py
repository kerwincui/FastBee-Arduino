"""Prepared H1 native GPIO ownership tests. No device I/O; run at final gates."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[2]


class ControlOutputTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler=shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise RuntimeError('C++17 compiler required')
        cls.directory=tempfile.TemporaryDirectory(prefix='fastbee-control-output-')
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary=Path(cls.directory.name)/'control.exe'
        result=subprocess.run([compiler,'-std=c++17','-Wall','-Wextra','-Werror','-pthread',
            *['-I'+str(ROOT/path) for path in ('test/unit/cpp/control_output_mocks',
              'test/unit/cpp/mcp23017_device_mocks','test/mocks','include')],
            str(ROOT/'test/unit/cpp/control_output_harness.cpp'),
            str(ROOT/'src/core/PeripheralControlOutput.cpp'),'-o',str(cls.binary)],
            capture_output=True,text=True,timeout=90)
        if result.returncode:
            raise AssertionError(result.stdout+result.stderr)

    def check(self,case):
        result=subprocess.run([str(self.binary),case],capture_output=True,text=True,timeout=10)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)


for _case in ('ordinary_owner','consumer_identity','resource_snapshot','normal','pulse','owner','disabled','uninitialized','suspended','expander',
              'passive','inverted','initial_low','wrong_type','reserved','invalid','input_only',
              'conflict','ticker','busy','init_failure','pulse_failure','release_failure',
              'readback','capacity','thread_busy','drift','unknown_command','reset_adapter','gate_adapter',
              'destructor_quarantine','retire_normal','retire_live_owner','retire_thread_busy',
              'retire_busy','retire_address_reuse','retire_saturation','retire_wrap',
              'retire_feedback','retire_direction','retire_write','retire_drift','retire_disabled','retire_missing',
              'retire_init_failure','retire_all_slots'):
    setattr(ControlOutputTests,'test_'+_case,lambda self,case=_case:self.check(case))

if __name__=='__main__':
    unittest.main()
