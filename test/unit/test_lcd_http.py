"""Compile real LCD HTTP helpers/routes and actual singleton admission branches."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test.unit.test_periph_exec_config_persistence import cpp_function

ROOT = Path(__file__).resolve().parents[2]


class LcdHttpTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        headers = list((ROOT / '.pio/libdeps').glob('*/ArduinoJson/src/ArduinoJson.h'))
        if not compiler or not headers:
            raise RuntimeError('C++17 compiler and installed ArduinoJson required')
        actual = (ROOT / 'src/network/handlers/LcdRouteHandler.cpp').read_text(encoding='utf-8')
        helpers = '\n'.join(cpp_function(actual, sig) for sig in ('bool readLcdInteger(', 'void submitLcdHttp('))
        routes = '\n'.join(cpp_function(actual, 'void LcdRouteHandler::' + name + '(')
                           for name in ('handleDisplayText', 'handleClear', 'handleShowInfo', 'handleSetFont'))
        admission = (ROOT / 'src/core/PeripheralManagerValidation.cpp').read_text(encoding='utf-8')
        admission = cpp_function(admission, 'if (checkRunningGraph && (config.type == PeripheralType::SDIO')
        imported = (ROOT / 'src/core/PeripheralManagerPersistence.cpp').read_text(encoding='utf-8')
        start = imported.index('            const bool singleton =')
        end = imported.index('            if (config.isExpanderChannel()', start)
        imported = imported[start:end]
        code = (ROOT / 'test/unit/cpp/lcd_http_harness.cpp.in').read_text(encoding='utf-8')
        for marker, value in (('http_helpers', helpers), ('routes', routes),
                              ('singleton_validation', admission), ('singleton_import', imported)):
            code = code.replace('// @' + marker + '@', value)
        cls.temp = tempfile.TemporaryDirectory(prefix='fastbee-lcd-http-')
        cls.addClassCleanup(cls.temp.cleanup)
        source = Path(cls.temp.name) / 'lcd_http.cpp'
        source.write_text(code, encoding='utf-8')
        cls.binary = Path(cls.temp.name) / 'lcd_http.exe'
        run = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
            '-DARDUINOJSON_ENABLE_ARDUINO_STRING=0', '-DARDUINOJSON_ENABLE_STD_STRING=1',
            '-I' + str(ROOT / 'test/mocks'), '-I' + str(ROOT / 'include'), '-I' + str(headers[0].parent),
            str(source), '-o', str(cls.binary)], capture_output=True, text=True, timeout=60)
        if run.returncode:
            raise AssertionError(run.stdout + run.stderr)

    def check(self, case):
        run = subprocess.run([str(self.binary), case], capture_output=True, text=True, timeout=10)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)


for _case in ('auth', 'fields', 'parameters', 'context', 'failure', 'singleton', 'commands'):
    setattr(LcdHttpTests, 'test_' + _case, lambda self, case=_case: self.check(case))
