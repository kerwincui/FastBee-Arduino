"""Persisted millimeter setup is metadata, never permission to move."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class MotorTravelDraftTests(unittest.TestCase):
    def test_json_validation_and_roundtrip_without_limit_mutation(self):
        compiler = shutil.which('g++') or 'D:/msys64/ucrt64/bin/g++.exe'
        json_header = next((ROOT / '.pio/libdeps').glob('*/ArduinoJson/src/ArduinoJson.h'))
        env = dict(os.environ)
        env['PATH'] = str(Path(compiler).parent) + os.pathsep + env.get('PATH', '')
        with tempfile.TemporaryDirectory(prefix='fastbee-travel-draft-') as directory:
            binary = Path(directory) / 'draft.exe'
            build = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                '-DARDUINOJSON_ENABLE_STD_STRING=1', '-DARDUINOJSON_ENABLE_ARDUINO_STRING=0',
                '-I' + str(ROOT / 'include'), '-I' + str(json_header.parent),
                str(ROOT / 'test/helpers/motor_travel_draft_harness.cpp'), '-o', str(binary)],
                capture_output=True, text=True, encoding='utf8', env=env, timeout=60)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(binary)], capture_output=True, text=True, env=env, timeout=10)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)

    def test_api_summary_save_and_both_load_paths_preserve_drafts(self):
        route = (ROOT / 'src/network/handlers/ProtocolRouteHandler.cpp').read_text(encoding='utf8')
        for marker in ['copyJsonMember(out, in, "motorTravelDraft")',
                       'saveMotorTravelDraft(deviceOut, device.motorTravelDraft)',
                       'devObj["motorTravelDraft"].set(dv["motorTravelDraft"])']:
            self.assertIn(marker, route)
        self.assertLess(route.index('loadMotorTravelDraft(device, draft)'),
                        route.index('if (next.size() >= previous.size())'))
        for name in ['ModbusHandler.cpp', 'ProtocolManager.cpp']:
            self.assertIn('loadMotorTravelDraft(d, dev.motorTravelDraft)',
                          (ROOT / 'src/protocols' / name).read_text(encoding='utf8'))
        self.assertIn('saveMotorTravelDraft(d, dev.motorTravelDraft)',
                      (ROOT / 'src/protocols/ModbusHandler.cpp').read_text(encoding='utf8'))

    def test_source_and_minified_editor_roundtrip_and_explicit_conversion(self):
        run = subprocess.run(['node', 'scripts/test-motor-travel-settings.cjs'], cwd=ROOT,
                             capture_output=True, text=True, encoding='utf8', timeout=30)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)

    def test_production_web_load_save_reload(self):
        run = subprocess.run(['node', 'scripts/test-motor-travel-save.cjs'], cwd=ROOT,
                             capture_output=True, text=True, encoding='utf8', timeout=30)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)


if __name__ == '__main__':
    unittest.main()
