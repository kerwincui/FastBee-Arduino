"""Production mapping parser/codec and actual report/cache/one-shot mapping blocks."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from test.unit.test_screen_panel_contract import braced_block

ROOT = Path(__file__).resolve().parents[2]


class ModbusRegisterMappingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or 'D:/msys64/ucrt64/bin/g++.exe'
        header = next((ROOT / '.pio/libdeps').glob('*/ArduinoJson/src/ArduinoJson.h'))
        cls.temp = tempfile.TemporaryDirectory(prefix='fastbee-modbus-codec-')
        cls.addClassCleanup(cls.temp.cleanup)
        directory = Path(cls.temp.name)
        handler = (ROOT/'src/protocols/ModbusHandler.cpp').read_text(encoding='utf-8')
        manager = (ROOT/'src/protocols/ProtocolManager.cpp').read_text(encoding='utf-8')
        start = handler.index('    // 应用寄存器映射生成 JSON')
        end = handler.index('    // 无映射配置:', start)
        direct_start = manager.index('            // JSON模式 + 有映射：应用映射转换')
        direct_end = manager.index('        } else {\n            // 无映射：', direct_start)
        code = 'String ModbusHandler::collectCachedPollData() const {' + braced_block(handler, 'String ModbusHandler::collectCachedPollData() const') + '}\n'
        code += 'String mappedReport(const PollTask& task, const OneShotResult& result, bool emitCallback, const std::function<void(uint8_t,const String&)>& dataCallback) {\n' + handler[start:end] + '\nreturn "[]"; }\n'
        code += 'void directReport(JsonObject t, const OneShotResult& readResult, JsonArray resultArr) {\n' + manager[direct_start:direct_end] + '\n}\n'
        (directory/'production_modbus_mapping.inc').write_text(code, encoding='utf-8')
        cls.binary = directory/'mapping.exe'
        cls.environment = dict(os.environ)
        cls.environment['PATH'] = str(Path(compiler).parent) + os.pathsep + cls.environment.get('PATH', '')
        built = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-Wno-deprecated-declarations',
            '-DARDUINOJSON_ENABLE_STD_STRING=1', '-DARDUINOJSON_ENABLE_ARDUINO_STRING=0',
            '-I'+str(ROOT/'test/mocks'), '-I'+str(ROOT/'include'), '-I'+str(header.parent), '-I'+str(directory),
            str(ROOT/'test/unit/cpp/modbus_register_mapping_harness.cpp'), '-o', str(cls.binary)],
            capture_output=True, text=True, encoding='utf-8', errors='replace', env=cls.environment, timeout=60)
        if built.returncode:
            raise AssertionError(built.stdout + built.stderr)

    def check(self, mode):
        result = subprocess.run([str(self.binary), mode], capture_output=True, text=True,
                                env=self.environment, timeout=10)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)


for _mode in ('legacy', 'unsigned', 'signed', 'float', 'orders', 'invalid', 'short', 'zero', 'format', 'parser', 'outputs'):
    setattr(ModbusRegisterMappingTests, 'test_' + _mode, lambda self, mode=_mode: self.check(mode))
