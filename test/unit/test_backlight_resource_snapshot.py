"""Production screen-task publication and passive backlight observer methods."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test.unit.test_sensor_publication import definition

ROOT = Path(__file__).resolve().parents[2]


class BacklightResourceSnapshotTests(unittest.TestCase):
    def test_publication_lifetime_failures_busy_and_no_observer_construction(self):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise RuntimeError('C++17 compiler required')
        runtime = (ROOT / 'src/screen/ScreenRuntime.cpp').read_text(encoding='utf-8')
        methods = '\n'.join(definition(runtime, signature) for signature in (
            'void publishBacklightResources(', 'bool deactivateScreenDeviceResources(',
            'void ScreenRuntime::snapshotBacklightResources('))
        template = (ROOT / 'test/unit/cpp/backlight_resource_harness.cpp.in').read_text(encoding='utf-8')
        with tempfile.TemporaryDirectory(prefix='fastbee-backlight-observation-') as directory:
            folder = Path(directory)
            cpp = folder / 'backlight.cpp'
            cpp.write_text(template.replace('// @production@', methods), encoding='utf-8')
            binary = folder / ('backlight.exe' if os.name == 'nt' else 'backlight')
            built = subprocess.run([compiler, '-std=c++17', '-pthread', '-Wall', '-Wextra', '-Werror',
                '-I' + str(ROOT / 'include'), str(cpp), '-o', str(binary)],
                capture_output=True, text=True, timeout=90)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            ran = subprocess.run([str(binary)], capture_output=True, text=True, timeout=20)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
