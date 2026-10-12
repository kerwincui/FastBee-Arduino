"""Prepared actual random expansion and String wrapper, including allocation failure."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test.unit.test_periph_exec_config_persistence import cpp_function

ROOT = Path(__file__).resolve().parents[2]


class ScriptRandomExpansionTests(unittest.TestCase):
    def test_actual_expansion_with_boundary_and_failure_inputs(self):
        compiler = shutil.which('g++') or shutil.which('clang++')
        self.assertTrue(compiler, 'C++17 compiler required')
        headers = list((ROOT / '.pio/libdeps').glob('*/ArduinoJson/src/ArduinoJson.h'))
        self.assertTrue(headers, 'Installed ArduinoJson headers required by FeatureFlags')
        source = (ROOT / 'src/core/ScriptEngine.cpp').read_text(encoding='utf-8')
        production = cpp_function(source, 'bool ScriptEngine::processRandomExpressions(')
        template = (ROOT / 'test/unit/cpp/script_random_expansion_harness.cpp').read_text(encoding='utf-8')
        with tempfile.TemporaryDirectory(prefix='fastbee-script-expansion-') as tmp:
            folder = Path(tmp)
            cpp, binary = folder / 'expansion.cpp', folder / 'expansion.exe'
            cpp.write_text(template.replace('// @production@', production), encoding='utf-8')
            build = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                    '-DFASTBEE_ENABLE_COMMAND_SCRIPT=1',
                                    '-I' + str(headers[0].parent),
                                    '-I' + str(ROOT / 'test/mocks/script-random'),
                                    '-I' + str(ROOT / 'include'), str(cpp), '-o', str(binary)],
                                   capture_output=True, text=True, timeout=60)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)


if __name__ == '__main__':
    unittest.main()
