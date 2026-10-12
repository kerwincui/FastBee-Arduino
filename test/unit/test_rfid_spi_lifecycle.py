"""Execute the full production RFID driver with bounded SPI ownership."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class RfidSpiLifecycleTests(unittest.TestCase):
    def compile_and_run(self, events):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            self.skipTest('C++17 compiler required')
        with tempfile.TemporaryDirectory(prefix='fastbee-rfid-spi-') as tmp:
            binary = Path(tmp) / ('rfid-tests.exe' if os.name == 'nt' else 'rfid-tests')
            includes = ['test/unit/cpp/rfid_bus_mocks', 'test/unit/cpp/spi_bus_mocks',
                        'test/mocks', 'include']
            sources = ['test/unit/cpp/rfid_spi_lifecycle_harness.cpp',
                       'src/peripherals/drivers/RFIDDriver.cpp', 'src/core/SpiBusInitializer.cpp']
            command = [compiler, '-std=c++17', '-pthread', '-Wall', '-Wextra', '-Werror',
                       '-DFASTBEE_ENABLE_PERIPH_EXEC=' + str(events),
                       *['-I' + str(ROOT / name) for name in includes],
                       *[str(ROOT / name) for name in sources], '-o', str(binary)]
            result = subprocess.run(command, capture_output=True, text=True,
                                    encoding='utf-8', errors='replace', timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_production_lifecycle_and_reentrant_events(self):
        self.compile_and_run(1)

    def test_production_lifecycle_without_rule_engine(self):
        self.compile_and_run(0)

    def test_manager_propagates_stop_failure_before_releasing_pins(self):
        source = (ROOT / 'src/core/PeripheralManager.cpp').read_text(encoding='utf-8')
        teardown = source[source.index('bool PeripheralManager::teardownHardware'): ]
        rfid = teardown.split('config.type == PeripheralType::RFID_MFRC522', 1)[1]
        rfid = rfid.split('config.type == PeripheralType::SENSOR_GENERIC', 1)[0]
        self.assertIn('FastBee::SpiBusOperation operation;', rfid)
        stop = rfid.index('!RFIDDriver::getInstance().stop(&stopError)')
        self.assertLess(stop, rfid.index('return false;', stop))
        self.assertLess(rfid.index('return false;', stop), rfid.index('pinMode(pin, INPUT)'))
        self.assertIn('state->second.lastError = stopError', rfid)

    def test_status_route_uses_one_coherent_nonblocking_snapshot(self):
        source = (ROOT / 'src/network/handlers/PeripheralRouteHandler.cpp').read_text(encoding='utf-8')
        rfid = source.split('RFIDDriver::Status rfidStatus;', 1)[1].split('#else', 1)[0]
        self.assertEqual(rfid.count('.getStatus('), 1)
        self.assertIn('FastBee::setFixedJsonField(data, "statusAvailable", available)', rfid)
        self.assertIn('FastBee::setFixedJsonField(data, "driverError", rfidStatusError)', rfid)
        self.assertNotIn('.getLastUID()', rfid)
        self.assertNotIn('.isInitialized()', rfid)

    def test_web_busy_snapshot_is_not_rendered_as_disabled_or_fresh_data(self):
        node = shutil.which('node')
        if not node:
            self.skipTest('Node required for production renderer')
        script = r'''
const fs = require('fs'), vm = require('vm'), assert = require('assert');
let module, error = '', output = '', requests = 0;
const labels = {};
const input = {value:'USER_INPUT'};
const sandbox = {
    AppState: {registerModule: (name, impl) => {module = impl;}},
    document: {getElementById: () => input},
    apiPost: () => {requests++; throw Error('No device writes in a renderer');}
};
vm.runInNewContext(fs.readFileSync('web-src/modules/runtime/peripheral-io.js','utf8'), sandbox);
module._setPeripheralIoText = (id,value) => {labels[id] = value;};
module._setPeripheralIoError = value => {error = value;};
module._setPeripheralIoOutput = (id,value) => {output = value;};
module._renderPeripheralRfidData({statusAvailable:false, initialized:false, uid:'STALE',
    hex:'STALE', driverError:'SPI_BUSY: RFID status unavailable'});
assert.strictEqual(labels['peripheral-rfid-ready'], '忙碌，状态待刷新');
assert.strictEqual(labels['peripheral-rfid-uid'], '--');
assert.strictEqual(input.value, 'USER_INPUT');
assert.strictEqual(output, '');
assert.ok(error.startsWith('SPI_BUSY'));
module._renderPeripheralRfidData({statusAvailable:true, initialized:true, uid:'A1B2',cardType:'MIFARE'});
assert.strictEqual(labels['peripheral-rfid-ready'], '已初始化');
assert.strictEqual(labels['peripheral-rfid-uid'], 'A1B2');
assert.strictEqual(error, '');
module._renderPeripheralRfidData({initialized:false, driverError:'RFID_SPI_LEASE_LOST'});
assert.strictEqual(labels['peripheral-rfid-ready'], '未初始化');
assert.strictEqual(error, 'RFID_SPI_LEASE_LOST');
assert.strictEqual(requests, 0);
'''
        result = subprocess.run([node, '-e', script], cwd=ROOT, capture_output=True,
                                text=True, encoding='utf-8', errors='replace', timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
