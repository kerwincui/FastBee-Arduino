"""Exercise production file verification with real ArduinoJson allocation."""
from pathlib import Path
import hashlib
import json
import os
import shutil
import subprocess
import tempfile
import unittest
from test_config_storage_failed_commit import method

ROOT = Path(__file__).resolve().parents[2]


class ConfigStorageLoadOwnershipTests(unittest.TestCase):
    def test_large_json_peak_failure_preservation_and_custom_allocator(self):
        compiler = shutil.which('g++')
        headers = ROOT / '.pio/libdeps/esp32-F4R0/ArduinoJson/src'
        self.assertTrue(compiler and (headers / 'ArduinoJson.h').exists())
        source = ROOT / 'src/systems/ConfigStorage.cpp'
        before = hashlib.sha256(source.read_bytes()).hexdigest()
        production = method(source.read_text(encoding='utf-8'), 'bool verifyJsonFile(')
        oracle = (ROOT / 'test/unit/cpp/config_storage_load_before.inc').read_text(encoding='utf-8')
        template = (ROOT / 'test/unit/cpp/config_storage_load_ownership_harness.cpp.in').read_text(encoding='utf-8')
        with tempfile.TemporaryDirectory(prefix='fastbee-config-load-') as temporary:
            folder = Path(temporary)
            cpp, binary = folder / 'load.cpp', folder / ('load.exe' if os.name == 'nt' else 'load')
            cpp.write_text(template.replace('@PRODUCTION@', production).replace('@ORACLE@', oracle), encoding='utf-8')
            environment = dict(os.environ)
            environment['PATH'] = str(Path(compiler).parent) + os.pathsep + environment.get('PATH', '')
            build = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                '-DARDUINOJSON_ENABLE_ARDUINO_STRING=0', '-DARDUINOJSON_ENABLE_STD_STRING=1',
                '-I' + str(headers), str(cpp), '-Wl,--wrap=malloc,--wrap=realloc,--wrap=free', '-o', str(binary)],
                capture_output=True, text=True, env=environment, timeout=60)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(binary)], capture_output=True, text=True, env=environment, timeout=20)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            result = json.loads(run.stdout)
            self.assertEqual(result['cases'], 7)
            self.assertLess(result['ownershipPeakBytes'], result['copyPeakBytes'])
            print(run.stdout.strip())
        self.assertEqual(before, hashlib.sha256(source.read_bytes()).hexdigest())


if __name__ == '__main__':
    unittest.main()
