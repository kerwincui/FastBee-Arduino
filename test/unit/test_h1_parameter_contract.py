"""Actual H1 parsers and descriptor-fed Web consumers. Host-only; no device I/O."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class H1ParameterContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise RuntimeError('C++17 compiler required')
        json_root = os.environ.get('FASTBEE_TEST_ARDUINOJSON_INCLUDE')
        if not json_root:
            headers = list((ROOT / '.pio/libdeps').glob('*/ArduinoJson/src/ArduinoJson.h'))
            if not headers:
                raise RuntimeError('Installed ArduinoJson headers required')
            json_root = str(headers[0].parent)
        cls.temp = tempfile.TemporaryDirectory(prefix='fastbee-h1-parameters-')
        cls.addClassCleanup(cls.temp.cleanup)
        cls.binary = Path(cls.temp.name) / ('parameters.exe' if os.name == 'nt' else 'parameters')
        built = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
            '-I' + str(ROOT / 'test/mocks'), '-I' + str(ROOT / 'include'), '-I' + json_root,
            str(ROOT / 'src/core/Pca9685ConfigSupport.cpp'), str(ROOT / 'src/core/H1ParameterJson.cpp'),
            str(ROOT / 'test/unit/cpp/h1_parameter_contract_harness.cpp'), '-o', str(cls.binary)],
            capture_output=True, text=True, timeout=90)
        if built.returncode:
            raise AssertionError(built.stdout + built.stderr)
        emitted = subprocess.run([str(cls.binary), 'emit'], capture_output=True, text=True, timeout=10)
        if emitted.returncode:
            raise AssertionError(emitted.stdout + emitted.stderr)
        cls.schema = emitted.stdout
        json.loads(cls.schema)

    def check(self, case):
        ran = subprocess.run([str(self.binary), case], capture_output=True, text=True, timeout=10)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)

    def test_web_consumes_actual_cpp_descriptors(self):
        node = shutil.which('node')
        self.assertIsNotNone(node, 'Node.js required for production form regression')
        ran = subprocess.run([node, str(ROOT / 'test/helpers/h1-parameter-web-harness.js')],
            input=self.schema, capture_output=True, text=True, encoding='utf-8', timeout=20)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
        self.assertIn('H1 actual descriptor Web checks passed', ran.stdout)


for _case in ('typed_fields', 'relationships', 'literal_commands', 'io_compatibility'):
    setattr(H1ParameterContractTests, 'test_' + _case, lambda self, case=_case: self.check(case))

if __name__ == '__main__':
    unittest.main()
