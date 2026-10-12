"""Actual ProtocolManager raw MQTT and delegated callbacks with counted boundaries."""
from pathlib import Path
import hashlib
import shutil
import subprocess
import tempfile
import unittest
from test.unit.test_periph_exec_config_persistence import cpp_function

ROOT = Path(__file__).resolve().parents[2]


class ProtocolModbusOutputTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        headers = sorted((ROOT / '.pio/libdeps').glob('*/ArduinoJson/src/ArduinoJson.h'))
        if not compiler or not headers:
            raise RuntimeError('C++17 compiler and installed ArduinoJson required')
        cls.inputs = [ROOT / p for p in (
            'src/protocols/ProtocolManager.cpp', 'src/core/PeriphExecManager.cpp',
            'src/protocols/ModbusHandler.cpp', 'include/protocols/ModbusHandler.h',
            'include/core/ModbusOutputFrame.h', 'include/core/ModbusOrdinaryControl.h',
            'include/core/NativeOutputControl.h', 'include/core/OrdinaryControlPolicy.h',
            'include/core/PeripheralManager.h', 'include/core/SensorConfigEpoch.h',
            'include/core/SystemConstants.h', 'include/protocols/ProtocolManager.h',
            'test/unit/cpp/protocol_modbus_output_harness.cpp.in')]
        cls.hashes = [hashlib.sha256(p.read_bytes()).hexdigest() for p in cls.inputs]
        protocol, manager, transport, header = [p.read_text(encoding='utf-8') for p in cls.inputs[:4]]
        register = cpp_function(protocol, 'void ProtocolManager::registerModbusSubDevices(')
        preprocess_start = manager.index('    String transparentHexResult;')
        preprocess = manager[preprocess_start:manager.index('    // 阶段1:', preprocess_start)]
        guarded = cpp_function(manager, 'if (itemId == "modbus_raw_send") {\n                continue;')
        callbacks = cpp_function(register, 'if (modbusHandler) {')
        code = cls.inputs[-1].read_text(encoding='utf-8')
        parts = {
            'types': '\n'.join(cpp_function(header, key) + ';' for key in (
                'enum OneShotError', 'struct ModbusDelayReceipt', 'struct OneShotResult')),
            'crc': cpp_function(transport, 'uint16_t ModbusHandler::calculateCRC('),
            'helpers': '\n'.join(cpp_function(protocol, key) for key in (
                'static OneShotResult protocolModbusDenied(', 'static OneShotResult submitProtocolModbusFrame(')),
            'raw': cpp_function(protocol, 'String ProtocolManager::executeModbusRawSend('),
            'protocol_type': cpp_function((ROOT / 'include/protocols/ProtocolManager.h').read_text(
                encoding='utf-8'), 'enum class ProtocolType') + ';',
            'send_data': cpp_function(protocol, 'bool ProtocolManager::sendData('),
            'callbacks': callbacks, 'preprocess': preprocess, 'handled_guard': guarded,
        }
        for key, value in parts.items():
            marker = '// @' + key + '@'
            if code.count(marker) != 1:
                raise AssertionError('Ambiguous fixture marker ' + key)
            code = code.replace(marker, value)
        cls.temp = tempfile.TemporaryDirectory(prefix='fastbee-protocol-modbus-output-')
        cls.addClassCleanup(cls.temp.cleanup)
        source = Path(cls.temp.name) / 'protocol.cpp'
        source.write_text(code, encoding='utf-8')
        cls.binary = Path(cls.temp.name) / 'protocol.exe'
        run = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
            '-DARDUINOJSON_ENABLE_ARDUINO_STRING=0', '-DARDUINOJSON_ENABLE_STD_STRING=1',
            '-I' + str(ROOT / 'test/mocks'), '-I' + str(ROOT / 'include'),
            '-I' + str(headers[0].parent), str(source), '-o', str(cls.binary)],
            capture_output=True, text=True, timeout=90)
        if run.returncode:
            raise AssertionError(run.stdout + run.stderr)

    @classmethod
    def tearDownClass(cls):
        if cls.hashes != [hashlib.sha256(p.read_bytes()).hexdigest() for p in cls.inputs]:
            raise AssertionError('Protocol Modbus inputs changed')

    def check(self, mode):
        run = subprocess.run([str(self.binary), mode], capture_output=True, text=True, timeout=10)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)


for _case in ('read', 'read_full', 'read_exception', 'read_failure', 'write_fc05', 'write_fc06',
              'write_fc15', 'write_fc16', 'write_vendor', 'write_vendor255', 'write_vendor_unmapped', 'write_exception', 'write_mapping',
              'write_timeout', 'write_denied', 'write_context', 'write_epoch', 'bad_hex',
              'bad_crc', 'crc_envelope_precedes_all_transmit_paths',
              'bad_frame', 'unknown_vendor', 'broadcast', 'unavailable', 'wrong_mode',
              'response_length', 'callback_coil', 'callback_register', 'callback_failure',
              'callback_mapping', 'callback_no_context', 'callback_unavailable', 'handled'):
    setattr(ProtocolModbusOutputTests, 'test_' + _case, lambda self, case=_case: self.check(case))

for _case in ('master', 'narrowing', 'malformed', 'unavailable', 'mqtt', 'mqtt_failure',
              'mqtt_unavailable', 'slave', 'slave_failure', 'unknown'):
    setattr(ProtocolModbusOutputTests, 'test_send_data_' + _case,
            lambda self, case='send_data_' + _case: self.check(case))
