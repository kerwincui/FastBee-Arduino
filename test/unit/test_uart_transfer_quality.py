"""Prepared actual UART switch bodies and API admission, with transport fakes.

Extract source fragments at test time; do not copy the implementation into tests.
Full route serialization and firmware translation units remain final-gate work.
"""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def method(source, signature):
    start = source.index(signature)
    return source[start:source.index('\n}', start) + 2]


def uart_method(source, signature):
    body = method(source, signature)
    start = body.index('        case PeripheralType::UART: {')
    end = body.index('\n        }', start) + len('\n        }')
    # Retain the real validation/locking prefix and the accounting epilogue;
    # unrelated peripheral switch arms are outside this focused harness.
    return (body[:body.index('    switch (config->type)')] +
            '    switch (config->type) {\n' + body[start:end] +
            '\n        default: break;\n    }\n' + body[body.index('    // 更新运行时状态'):])


class UartTransferQualityTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise unittest.SkipTest('C++17 compiler required')
        cls.inputs = [ROOT / p for p in ('src/core/PeripheralManagerIo.cpp',
            'src/network/handlers/PeripheralRouteHandler.cpp', 'include/peripherals/BoundedStreamRead.h',
            'test/unit/cpp/uart_transfer_quality_harness.cpp.in', 'test/mocks/Arduino.h',
            'src/network/handlers/PeripheralReadRoutes.cpp', 'include/core/UartReceiveFault.h',
            'include/core/UartTextDiagnostics.h')]
        cls.hashes = [hashlib.sha256(p.read_bytes()).hexdigest() for p in cls.inputs]
        source = cls.inputs[0].read_text(encoding='utf-8')
        manager = '\n'.join((method(source, 'uint32_t saturatingAddUint32('),
            method(source, 'uint16_t boundedUartAvailable('),
            uart_method(source, 'bool PeripheralManager::writeData('),
            uart_method(source, 'bool PeripheralManager::readData(')))
        route = '\n'.join(cls.inputs[index].read_text(encoding='utf-8') for index in (1, 5))
        read = method(route, 'void PeripheralRouteHandler::handleReadPeripheral(')
        start = read.index('        uint8_t buffer[UART_API_MAX_BYTES];')
        read = read[start:read.index('        if (readError)', start)]
        write = method(route, 'void PeripheralRouteHandler::handleWritePeripheral(')
        start = write.index('        const auto writeCommunication =')
        write = write[start:write.index('\n        };', start) + len('\n        };')]
        template = cls.inputs[3].read_text(encoding='utf-8')
        code = template.replace('// @manager@', manager).replace('// @read@', read).replace('// @write@', write)
        cls.folder = tempfile.TemporaryDirectory(prefix='fastbee-uart-quality-')
        cls.addClassCleanup(cls.folder.cleanup)
        cpp = Path(cls.folder.name) / 'harness.cpp'
        cpp.write_text(code, encoding='utf-8')
        cls.binary = Path(cls.folder.name) / ('harness.exe' if os.name == 'nt' else 'harness')
        result = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
            '-I'+str(ROOT/'test/mocks'), '-I'+str(ROOT/'include'), str(cpp), '-o', str(cls.binary)],
            capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=90)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    @classmethod
    def tearDownClass(cls):
        if cls.hashes != [hashlib.sha256(p.read_bytes()).hexdigest() for p in cls.inputs]:
            raise AssertionError('UART inputs changed during tests')

    def run_case(self, name):
        result = subprocess.run([str(self.binary), name], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


for _name in ('write_full', 'write_partial', 'write_zero', 'write_saturate', 'write_overclaim',
              'read_empty', 'read_bytes', 'read_partial', 'read_fault_before', 'read_fault_inside',
              'route_fault_inside', 'io_busy', 'io_suspended', 'io_lost',
              'io_no_state', 'route_busy', 'route_changed', 'route_type', 'route_disabled',
              'route_lost', 'route_suspended', 'route_read_error', 'route_empty',
              'route_snapshot_lost', 'route_write_partial'):
    setattr(UartTransferQualityTests, 'test_' + _name, lambda self, name=_name: self.run_case(name))


if __name__ == '__main__':
    unittest.main()
