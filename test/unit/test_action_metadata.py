"""Actual metadata/target predicates and JSON, no hardware."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test_sensor_publication import definition

ROOT = Path(__file__).resolve().parents[2]


class ActionMetadataTests(unittest.TestCase):
    def test_production_target_and_metadata_json(self):
        compiler = shutil.which('g++') or shutil.which('clang++')
        self.assertIsNotNone(compiler, 'C++17 compiler required')
        headers = sorted((ROOT / '.pio/libdeps').glob('*/ArduinoJson/src/ArduinoJson.h'))
        self.assertTrue(headers, 'Installed ArduinoJson required; no download')
        with tempfile.TemporaryDirectory(prefix='fastbee-action-metadata-') as directory:
            binary = Path(directory) / 'metadata.exe'
            source = Path(directory) / 'metadata.cpp'
            wrapper = (ROOT / 'src/network/RuleActionContracts.cpp').read_text(encoding='utf-8')
            parser = (ROOT / 'src/core/Pca9685ConfigSupport.cpp').read_text(encoding='utf-8')
            template = (ROOT / 'test/unit/cpp/action_metadata_harness.cpp').read_text(encoding='utf-8')
            source.write_text(template.replace('// @wrapper@', definition(wrapper, 'void appendRuleActionContracts('))
                .replace('// @parser@', definition(parser, 'bool parsePca9685Command(')), encoding='utf-8')
            built = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                '-I' + str(ROOT / 'test/mocks'), '-I' + str(ROOT / 'include'),
                '-I' + str(headers[0].parent), str(source),
                '-o', str(binary)], capture_output=True, text=True, timeout=90)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            ran = subprocess.run([str(binary)], capture_output=True, text=True, timeout=15)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)


if __name__ == '__main__':
    unittest.main()
