import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class ScreenPanelMigrationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++') or 'D:/msys64/ucrt64/bin/g++.exe'
        cls.temporary = tempfile.TemporaryDirectory(prefix='fastbee-screen-migration-')
        cls.binary = str(Path(cls.temporary.name) / 'migration.exe')
        cls.env = dict(os.environ)
        cls.env['PATH'] = str(Path(compiler).parent) + os.pathsep + cls.env.get('PATH', '')
        json_header = next((ROOT / '.pio/libdeps').glob('*/ArduinoJson/src/ArduinoJson.h'))
        result = subprocess.run([compiler, '-std=c++17', '-DUNIT_TEST', '-include', 'cmath',
            '-DFASTBEE_ENABLE_POINT_HISTORY=0', '-DFASTBEE_ENABLE_REMOTE_RESOURCES=0',
            '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
            '-DARDUINOJSON_ENABLE_STD_STRING=1', '-DARDUINOJSON_ENABLE_ARDUINO_STRING=0',
            *['-I' + str(p) for p in (ROOT / 'test/mocks', ROOT / 'test/helpers', ROOT / 'include', json_header.parent)],
            str(ROOT / 'test/helpers/screen_migration_harness.cpp'), '-o', cls.binary],
            capture_output=True, text=True, encoding='utf-8', errors='replace', env=cls.env, timeout=120)
        if result.returncode:
            cls.temporary.cleanup()
            raise AssertionError(result.stdout + result.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def test_actual_boot_migration_backup_and_recovery(self):
        for scenario in ('legacy', 'custom', 'already', 'bad-backup', 'recover', 'interrupted', 'sample', 'settings', 'system-template', 'system-custom'):
            with self.subTest(scenario=scenario):
                result = subprocess.run([self.binary, scenario,
                    str(ROOT / 'docs/waveshare-esp32s3-touch-lcd4/screen_panel.json')],
                    capture_output=True, text=True, encoding='utf-8', errors='replace', env=self.env, timeout=15)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
