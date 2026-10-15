"""FW-P0-01 OneShotResult bounds matrix, compiled against production Modbus methods."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SOURCE = Path(os.environ.get('FASTBEE_TEST_MODBUS_SOURCE', ROOT))
METHOD_SIGNATURES = (
    'uint16_t ModbusHandler::calculateCRC(',
    'bool ModbusHandler::validateFrame(',
    'void ModbusHandler::_syncMotorTravel(',
    'OneShotError ModbusHandler::_executeModbusTransactionRaw(',
    'OneShotError ModbusHandler::_sendOneShotRequestRaw(',
    'OneShotResult ModbusHandler::readRegistersOnce(',
    'OneShotError ModbusHandler::_readRangeInto(',
)


class ModbusOneShotBoundsTests(unittest.TestCase):
    def test_production_oneshot_bounds_matrix(self):
        source = (SOURCE / 'src/protocols/ModbusHandler.cpp').read_text(encoding='utf8')
        methods = []
        for signature in METHOD_SIGNATURES:
            start = source.index(signature)
            methods.append(source[start:source.index('\n}', start) + 2])
        compiler = shutil.which('g++') or 'D:/msys64/ucrt64/bin/g++.exe'
        env = dict(os.environ)
        env['PATH'] = str(Path(compiler).parent) + os.pathsep + env.get('PATH', '')
        with tempfile.TemporaryDirectory(prefix='fastbee-modbus-oneshot-bounds-') as temp:
            temp = Path(temp)
            (temp / 'modbus_oneshot_bounds_production.inc').write_text('\n'.join(methods), encoding='utf8')
            binary = temp / ('bounds.exe' if os.name == 'nt' else 'bounds')
            built = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                '-I' + str(temp), '-I' + str(SOURCE / 'include'),
                str(ROOT / 'test/unit/cpp/modbus_oneshot_bounds_harness.cpp'), '-o', str(binary)],
                capture_output=True, text=True, encoding='utf8', env=env, timeout=120)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            ran = subprocess.run([str(binary)], capture_output=True, text=True, env=env, timeout=10)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
            self.assertIn('FW-P0-01 OneShotResult bounds matrix passed', ran.stdout)

    def test_production_source_keeps_bounds_gates(self):
        source = (SOURCE / 'src/protocols/ModbusHandler.cpp').read_text(encoding='utf8')
        start = source.index('OneShotResult ModbusHandler::readRegistersOnce(')
        body = source[start:source.index('\n}', start)]
        self.assertIn('? Protocols::MODBUS_MAX_REGISTERS_PER_READ', body)
        self.assertIn(': Protocols::MODBUS_ONESHOT_BUFFER_SIZE', body)
        self.assertIn('rawResponseHex && quantity > Protocols::MODBUS_ONESHOT_BUFFER_SIZE', body)
        self.assertIn('result.count = 0;', body)
        self.assertIn('respLen != static_cast<size_t>(5U + quantity * 2U)', body)
        self.assertIn('byteCount != quantity * 2', body)
        range_start = source.index('OneShotError ModbusHandler::_readRangeInto(')
        range_body = source[range_start:source.index('\n}', range_start)]
        self.assertIn('quantity > outCap', range_body)
        self.assertIn('quantity > Protocols::MODBUS_MAX_REGISTERS_PER_READ', range_body)


if __name__ == '__main__':
    unittest.main()
