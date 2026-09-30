"""Offline admission tests. Runtime integration and hardware are separate gates."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class MotorTravelAdmissionTests(unittest.TestCase):
    def test_per_axis_admission_state_machine(self):
        compiler = shutil.which('g++') or 'D:/msys64/ucrt64/bin/g++.exe'
        env = dict(os.environ)
        env['PATH'] = str(Path(compiler).parent) + os.pathsep + env.get('PATH', '')
        with tempfile.TemporaryDirectory(prefix='fastbee-travel-admission-') as temp:
            binary = Path(temp) / 'admission.exe'
            built = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                '-I' + str(ROOT / 'include'), str(ROOT / 'test/helpers/motor_travel_admission_harness.cpp'),
                '-o', str(binary)], capture_output=True, text=True, env=env, timeout=60)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, env=env, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
