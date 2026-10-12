"""Real fixed-capacity passive instance observer registry, no device access."""
import os
import re
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]

class FeatureInstanceRegistryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise unittest.SkipTest('C++17 compiler required')
        cls.folder = tempfile.TemporaryDirectory(prefix='fastbee-feature-instances-')
        cls.addClassCleanup(cls.folder.cleanup)
        cls.binary = Path(cls.folder.name) / ('instances.exe' if os.name == 'nt' else 'instances')
        command = [compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pthread',
                   '-I' + str(ROOT / 'include'),
                   str(ROOT / 'test/unit/cpp/feature_instance_registry_harness.cpp'), '-o', str(cls.binary)]
        result = subprocess.run(command, capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def run_case(self, case):
        result = subprocess.run([str(self.binary), case], capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_observed_unknown_busy_cleanup_and_invalid_snapshots(self): self.run_case('states')
    def test_invalid_descriptors_and_bounded_keys(self): self.run_case('validation')
    def test_concurrent_idempotency_and_capacity_fail_closed(self): self.run_case('concurrency')
    def test_missing_owners_have_explicit_unknown_reasons(self): self.run_case('coverage')

    def test_every_registered_feature_is_in_the_reviewed_coverage_set(self):
        source = (ROOT / 'src/core/RuntimeFeatureManager.cpp').read_text(encoding='utf-8')
        expected = set('webServer mdns lcd neopixel sevenSegment ds18b20 modbus ruleScript '
                       'lcdGb2312 irRemote rfid ioExpander i2cSensors rfLearn ds1302 lcd1602 '
                       'commandScript mqtts deviceControl screenPanel ota otaFs logViewer '
                       'fileLogging userAdmin ble ethernet cellular'.split())
        self.assertEqual(set(re.findall(r'registerFeature\((?:FeatureMeta)?\s*\{\s*"([^"]+)"', source)), expected)

if __name__ == '__main__':
    unittest.main()
