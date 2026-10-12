"""Execute real delayed-command methods and receipt wrapper, with a fake bus."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test_periph_exec_config_persistence import cpp_function

ROOT = Path(__file__).resolve().parents[2]


class ModbusDelayedCommandReceiptTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise RuntimeError('C++17 compiler required')
        headers = list((ROOT/'.pio/libdeps').glob('*/ArduinoJson/src/ArduinoJson.h'))
        if not headers:
            raise RuntimeError('Installed ArduinoJson required')
        header = (ROOT/'include/protocols/ModbusHandler.h').read_text(encoding='utf-8')
        types = '\n'.join(cpp_function(header, marker) + ';' for marker in
            ('enum OneShotError', 'struct ModbusDelayReceipt', 'struct OneShotResult', 'struct CoilDelayTask'))
        source = (ROOT/'src/protocols/ModbusHandler.cpp').read_text(encoding='utf-8')
        instance = cpp_function(source, 'uint32_t nextModbusDelayInstance()')
        transaction = cpp_function(source, 'OneShotResult ModbusHandler::_executeModbusTransaction(')
        delayed = (ROOT/'src/protocols/ModbusDelay.cpp').read_text(encoding='utf-8')
        delayed = delayed[delayed.index('namespace {'):delayed.rindex('#endif')]
        legacy = '\n'.join(cpp_function(source, signature) for signature in (
            'bool ModbusHandler::addCoilDelayTask(uint8_t slaveAddr, uint16_t coilAddr, unsigned long',
            'bool ModbusHandler::addCoilDelayTask(uint8_t slaveAddr, uint16_t coilAddr, \n'))
        template = (ROOT/'test/unit/cpp/modbus_delay_receipt_harness.cpp').read_text(encoding='utf-8')
        cls.temp = tempfile.TemporaryDirectory(prefix='fastbee-modbus-delay-')
        cls.addClassCleanup(cls.temp.cleanup)
        folder = Path(cls.temp.name)
        program = folder/'delay.cpp'
        program.write_text(template.replace('// @types@', types).replace('// @instance@', instance).replace('// @methods@',
            transaction + '\n' + legacy + '\n' + delayed), encoding='utf-8')
        cls.binary = folder/'delay.exe'
        build = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
            '-DARDUINOJSON_ENABLE_ARDUINO_STRING=0', '-DARDUINOJSON_ENABLE_STD_STRING=1',
            '-I'+str(ROOT/'test/mocks'), '-I'+str(headers[0].parent),
            str(program), '-o', str(cls.binary)], capture_output=True, text=True, timeout=90)
        if build.returncode:
            raise AssertionError(build.stdout+build.stderr)

    def check(self, scenario):
        result = subprocess.run([str(self.binary), scenario], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)

    def test_real_transport_tracks_after_motor_admission_before_write(self):
        source = (ROOT/'src/protocols/ModbusHandler.cpp').read_text(encoding='utf-8')
        actual = cpp_function(source, 'OneShotError ModbusHandler::_executeModbusTransactionRaw(')
        self.assertLess(actual.index('_motorTravel.before'), actual.index('trackCoilDelayWriteLocked'))
        self.assertLess(actual.index('trackCoilDelayWriteLocked'), actual.index('modbusSerial->write'))
        for marker in ('bool ModbusHandler::begin(const ModbusConfig&', 'void ModbusHandler::end()',
                       'bool ModbusHandler::loadConfigFromFile('):
            method = cpp_function(source, marker)
            self.assertLess(method.index('MotorBusGuard lock'), method.index('invalidateCoilDelayTasksLocked'))

    def test_route_cannot_bypass_shared_output_and_owned_delay_admission(self):
        source = (ROOT/'src/network/handlers/ModbusRouteHandler.cpp').read_text(encoding='utf-8')
        actual = cpp_function(source, 'void ModbusRouteHandler::handleModbusCoilDelay(')
        # Runtime behavior is exercised by test_modbus_http_batch_delay. Keep a
        # boundary assertion here; duplicated source-layout counts do not prove
        # receipt binding, partial acceptance or exact scheduling behavior.
        self.assertIn('ModbusHttp::Output', actual)
        self.assertNotIn('modbus->writeRegisterOnce(', actual)
        self.assertNotIn('modbus->writeCoilOnce(', actual)
        self.assertNotIn('modbus->addCoilDelayTask(', actual)


for _scenario in ('success', 'register', 'no_receipt', 'wrong_instance', 'wrong_configuration',
                  'wrong_address', 'wrong_function', 'late_arm', 'replay', 'retire_same_target',
                  'range_overlap', 'range_disjoint', 'other_space', 'proprietary', 'broadcast',
                  'batch_alias', 'configuration', 'cross_handler', 'clock64', 'clock_reverse',
                  'busy', 'one_per_tick', 'queue_full', 'timeout', 'crc', 'exception',
                  'short_echo', 'wrong_echo', 'travel_blocked', 'failure_retained', 'exhaustion'):
    setattr(ModbusDelayedCommandReceiptTests, 'test_'+_scenario,
            lambda self, scenario=_scenario: self.check(scenario))

if __name__ == '__main__':
    unittest.main()
