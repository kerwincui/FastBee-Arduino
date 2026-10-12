"""Execute actual batch/delay HTTP bodies and their real output helper.

PM admission, bus transport, authentication and publication are counted seams;
this verifies HTTP semantics, not the complete PM/bus or physical relay chain.
"""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test_periph_exec_config_persistence import cpp_function
import modbus_document_field_support as document_fields

ROOT = Path(__file__).resolve().parents[2]
SUPPORT = Path(os.environ.get('FASTBEE_TEST_SUPPORT_ROOT', ROOT))


class ModbusHttpBatchDelayTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or 'D:/msys64/ucrt64/bin/g++.exe'
        if not Path(compiler).is_file():
            raise unittest.SkipTest('C++17 compiler required')
        cls.inputs = [ROOT / path for path in (
            'src/network/handlers/ModbusRouteHandler.cpp',
            'src/network/handlers/ModbusHttpOutput.h',
            'include/protocols/ModbusHandler.h',
            'test/unit/cpp/modbus_http_batch_delay_harness.cpp.in',
            'test/unit/test_modbus_http_batch_delay.py',
            'include/core/ModbusOutputFrame.h', 'include/core/ModbusOrdinaryControl.h',
            'include/core/NativeOutputControl.h', 'include/core/OrdinaryControlPolicy.h',
            'include/core/SensorConfigEpoch.h', 'include/core/SystemConstants.h',
            'include/utils/ConsumedStringJsonReader.h')]
        cls.inputs += [ROOT / 'include/utils/SharedJsonDeserializer.h', ROOT / 'src/utils/SharedJsonDeserializer.cpp']
        cls.inputs += document_fields.inputs()
        cls.hashes = [hashlib.sha256(p.read_bytes()).hexdigest() for p in cls.inputs]
        route, helper, header, template = [p.read_text(encoding='utf-8') for p in cls.inputs[:4]]
        actual = '\n'.join(cpp_function(route, 'void ModbusRouteHandler::' + name + '(') for name in (
            'handleModbusCoilBatch', 'handleModbusCoilDelay'))
        types = '\n'.join(cpp_function(header, marker) + ';' for marker in (
            'enum OneShotError', 'struct ModbusDelayReceipt', 'struct OneShotResult'))
        constants = cls.inputs[10].read_text(encoding='utf-8')
        capacity = next(line.strip() for line in constants.splitlines() if 'MODBUS_ONESHOT_BUFFER_SIZE ' in line)
        types = 'namespace Protocols { ' + capacity + '\n}\n' + types
        cls.temp = tempfile.TemporaryDirectory(prefix='fastbee-modbus-http-batch-delay-')
        cls.addClassCleanup(cls.temp.cleanup)
        folder = Path(cls.temp.name)
        program = folder / 'routes.cpp'
        program.write_text(template.replace('// @result_types@', types)
            .replace('// @output_helper@', helper[helper.index('namespace ModbusHttp {'):])
            .replace('// @route_methods@', actual), encoding='utf-8')
        cls.binary = folder / 'routes.exe'
        oracle_source = folder / 'routes-oracle.cpp'
        oracle_source.write_text(document_fields.oracle_program(program.read_text(encoding='utf-8'),
            ('handleModbusCoilBatch', 'handleModbusCoilDelay')), encoding='utf-8')
        cls.oracle_binary = folder / 'routes-oracle.exe'
        cls.env = dict(os.environ)
        cls.env['PATH'] = str(Path(compiler).parent) + os.pathsep + cls.env.get('PATH', '')
        command = [compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
            '-include', str(SUPPORT / 'test/mocks/Arduino.h'),
            '-DARDUINOJSON_ENABLE_ARDUINO_STRING=1', '-DARDUINOJSON_ENABLE_STD_STRING=1',
            '-I' + str(SUPPORT / 'test/mocks'), '-I' + str(ROOT / 'include'),
            '-I' + str(SUPPORT / '.pio/libdeps/native/ArduinoJson/src'),
            str(program), str(ROOT / 'src/utils/SharedJsonDeserializer.cpp'),
            str(ROOT / 'src/network/FixedJsonField.cpp'), '-o', str(cls.binary)]
        for fixture, binary in ((program, cls.binary), (oracle_source, cls.oracle_binary)):
            selected = [str(fixture) if arg == str(program) else str(binary)
                        if arg == str(cls.binary) else arg for arg in command]
            result = subprocess.run(selected, capture_output=True, text=True,
                encoding='utf-8', errors='replace', timeout=90, env=cls.env)
            if result.returncode:
                raise AssertionError(result.stdout + result.stderr)

    @classmethod
    def tearDownClass(cls):
        if cls.hashes != [hashlib.sha256(p.read_bytes()).hexdigest() for p in cls.inputs]:
            raise AssertionError('Modbus HTTP batch/delay inputs changed during tests')

    def check(self, mode):
        result = subprocess.run([str(self.binary), mode], capture_output=True, text=True,
            encoding='utf-8', errors='replace', timeout=15, env=self.env)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        oracle = subprocess.run([str(self.oracle_binary), mode], capture_output=True, text=True,
            encoding='utf-8', errors='replace', timeout=15, env=self.env)
        self.assertEqual(oracle.returncode, 0, oracle.stdout + oracle.stderr)
        self.assertEqual(result.stdout, oracle.stdout, 'Actual batch/delay status/response bytes changed')


for _mode in ('batch_coils_on', 'batch_coils_off', 'batch_coils_padding', 'batch_bitmap_on',
              'batch_bitmap_off', 'batch_bitmap_toggle', 'batch_bitmap_16', 'batch_bitmap_readback', 'batch_command_on', 'batch_command_off',
              'batch_command_toggle', 'batch_register_on', 'batch_register_off', 'batch_register_toggle', 'batch_register_readback',
              'batch_vendor_toggle_unknown', 'batch_vendor_toggle_readback', 'batch_partial_ack',
              'batch_source_rejected', 'batch_mapping_rejected', 'batch_mapping_changes',
              'batch_short_readback', 'batch_failed_readback', 'batch_nc_frozen_report',
              'batch_parameters', 'batch_auth', 'batch_session', 'batch_duplicate', 'batch_spoofed_source',
              'batch_subset_mapping', 'batch_pressure'):
    setattr(ModbusHttpBatchDelayTests, 'test_' + _mode, lambda self, mode=_mode: self.check(mode))

for _mode in ('delay_software_coil_no', 'delay_software_coil_nc', 'delay_software_register_no',
              'delay_software_register_nc', 'delay_saved_software', 'delay_hardware_coil',
              'delay_hardware_coil_255', 'delay_hardware_register_no', 'delay_hardware_register_nc',
              'delay_hardware_register_saved', 'delay_hardware_register_second_failure',
              'delay_hardware_register_mapping_changed',
              'delay_hardware_coil_initial_failure', 'delay_hardware_register_no_receipt',
              'delay_software_no_receipt', 'delay_software_unscheduled', 'delay_hardware_coil_untracked',
              'delay_hardware_register_nc_unscheduled', 'delay_hold_equal', 'delay_hold_short',
              'delay_parameters', 'delay_mode_invalid', 'delay_profile_rejected', 'delay_nc_mismatch',
              'delay_nc_invalid', 'delay_source_rejected', 'delay_mapping_rejected', 'delay_auth',
              'delay_session', 'delay_spoofed_operation'):
    setattr(ModbusHttpBatchDelayTests, 'test_' + _mode, lambda self, mode=_mode: self.check(mode))


if __name__ == '__main__':
    unittest.main()
