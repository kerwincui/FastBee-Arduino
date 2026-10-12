"""Compile actual Modbus HTTP forms, shared helper and wire-frame validators.
PM/HTTP/handler calls are counted doubles; raw UART and core arbitration have
their own production fixtures. This test never opens a device or a serial port.
"""
from pathlib import Path
import hashlib
import shutil
import subprocess
import tempfile
import unittest
from test.unit.test_periph_exec_config_persistence import cpp_function
from test.unit import modbus_document_field_support as document_fields

ROOT = Path(__file__).resolve().parents[2]
ROUTES = ('handleModbusWrite', 'handleModbusRegisterWrite', 'handleModbusRegisterBatchWrite',
          'handleModbusDeviceAddress', 'handleModbusDeviceBaudrate', 'handleModbusCoilControl')


class ModbusHttpOutputTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        headers = sorted((ROOT / '.pio/libdeps').glob('*/ArduinoJson/src/ArduinoJson.h'))
        if not compiler or not headers:
            raise RuntimeError('C++17 compiler and installed ArduinoJson required')
        cls.inputs = [ROOT / p for p in (
            'src/network/handlers/ModbusRouteHandler.cpp',
            'src/network/handlers/ModbusHttpOutput.h', 'src/network/HttpOutputControl.cpp',
            'include/core/ModbusOutputFrame.h', 'include/core/ModbusOrdinaryControl.h',
            'include/core/NativeOutputControl.h', 'include/core/OrdinaryControlPolicy.h',
            'include/protocols/ModbusHandler.h', 'test/mocks/Arduino.h',
            'include/core/SystemConstants.h', 'include/core/ActionReportIdentity.h',
            'include/utils/ConsumedStringJsonReader.h',
            'test/unit/cpp/modbus_http_output_harness.cpp.in', 'test/unit/test_modbus_http_output.py')]
        cls.inputs[-2:-2] = [ROOT / 'include/utils/SharedJsonDeserializer.h', ROOT / 'src/utils/SharedJsonDeserializer.cpp']
        cls.inputs[-2:-2] = document_fields.inputs()
        cls.hashes = [hashlib.sha256(p.read_bytes()).hexdigest() for p in cls.inputs]
        route = cls.inputs[0].read_text(encoding='utf-8')
        helper = cls.inputs[1].read_text(encoding='utf-8')
        serializer = cls.inputs[2].read_text(encoding='utf-8')
        types = (ROOT / 'include/protocols/ModbusHandler.h').read_text(encoding='utf-8')
        replacements = {
            'types': '\n'.join(cpp_function(types, sig) + ';' for sig in (
                'enum OneShotError', 'struct ModbusDelayReceipt', 'struct OneShotResult')),
            'http_helpers': cpp_function(helper, 'namespace ModbusHttp {'),
            'serializer': '\n'.join(cpp_function(serializer, sig) for sig in (
                'const char* sourceName(', 'const char* phaseName(', 'void writeOrdinaryControlJson(',
                'String fieldText(', 'bool unsignedDecimal(', 'bool manualHold(',
                'uint64_t sessionActor(', 'bool beginHttpModbusRelayOutput(')),
            'route_helpers': '\n'.join(cpp_function(route, sig) for sig in (
                'static ProtocolManager::ModbusHandlerLease getModbusMaster(',
                'static int findDeviceIndexBySlaveAddr(', 'bool validRelayChannel(')),
            'routes': '\n'.join(cpp_function(route, 'void ModbusRouteHandler::' + name + '(')
                                for name in ROUTES),
        }
        code = cls.inputs[-2].read_text(encoding='utf-8')
        for key, value in replacements.items():
            marker = '// @' + key + '@'
            if code.count(marker) != 1:
                raise AssertionError('Ambiguous fixture marker ' + key)
            code = code.replace(marker, value)
        cls.temp = tempfile.TemporaryDirectory(prefix='fastbee-modbus-http-')
        cls.addClassCleanup(cls.temp.cleanup)
        source = Path(cls.temp.name) / 'http.cpp'
        source.write_text(code, encoding='utf-8')
        cls.binary = Path(cls.temp.name) / 'http.exe'
        oracle_source = Path(cls.temp.name) / 'http-oracle.cpp'
        oracle_source.write_text(document_fields.oracle_program(code, ROUTES), encoding='utf-8')
        cls.oracle_binary = Path(cls.temp.name) / 'http-oracle.exe'
        command = [compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
            '-include', str(ROOT / 'test/mocks/Arduino.h'),
            '-DARDUINOJSON_ENABLE_ARDUINO_STRING=0', '-DARDUINOJSON_ENABLE_STD_STRING=1',
            '-I' + str(ROOT / 'test/mocks'), '-I' + str(ROOT / 'include'),
            '-I' + str(headers[0].parent), str(source), str(ROOT / 'src/utils/SharedJsonDeserializer.cpp'),
            str(ROOT / 'src/network/FixedJsonField.cpp'), '-o', str(cls.binary)]
        for fixture, binary in ((source, cls.binary), (oracle_source, cls.oracle_binary)):
            selected = [str(fixture) if arg == str(source) else str(binary)
                        if arg == str(cls.binary) else arg for arg in command]
            build = subprocess.run(selected, capture_output=True, text=True, timeout=90)
            if build.returncode:
                raise AssertionError(build.stdout + build.stderr)

    @classmethod
    def tearDownClass(cls):
        if cls.hashes != [hashlib.sha256(p.read_bytes()).hexdigest() for p in cls.inputs]:
            raise AssertionError('Modbus HTTP fixture inputs changed')

    def check(self, mode):
        run = subprocess.run([str(self.binary), mode], capture_output=True, text=True, timeout=10)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        oracle = subprocess.run([str(self.oracle_binary), mode], capture_output=True, text=True, timeout=10)
        self.assertEqual(oracle.returncode, 0, oracle.stdout + oracle.stderr)
        self.assertEqual(run.stdout, oracle.stdout, 'Actual HTTP status/error/response bytes changed')


for _case in ('auth', 'developer', 'session', 'context', 'fields', 'numeric',
              'numeric_boundaries', 'hold', 'single_frames', 'mapping', 'attempted',
              'pressure', 'lease', 'batch_values', 'batch_limits', 'batch_frame',
              'address_read', 'address_write', 'baud_codes', 'baud_override',
              'baud_saved', 'baud_saved_drift', 'baud_submit_drift', 'report',
              'report_drift', 'prefix', 'prefix_cap', 'prefix_mapping', 'invalid_frame'):
    setattr(ModbusHttpOutputTests, 'test_' + _case, lambda self, case=_case: self.check(case))

for _case in ('numeric', 'fields', 'bounds', 'defaults', 'action', 'auth', 'session', 'hold',
              'review_valid', 'review_invalid', 'review_unexpected', 'context', 'mapping',
              'frames', 'nc', 'report_frozen', 'unattempted', 'attempted', 'source_retired',
              'config_retired', 'no_mqtt', 'pressure', 'lease'):
    setattr(ModbusHttpOutputTests, 'test_coil_' + _case,
            lambda self, case='coil_' + _case: self.check(case))
