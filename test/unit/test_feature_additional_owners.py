"""Actual DS18B20, TM1637 and NeoPixel owner transitions with SDK/RTOS doubles.

This suite has no device access. Keep serial with other complete Host/build runs.
"""
import hashlib
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from test_sensor_publication import definition

ROOT = Path(__file__).resolve().parents[2]


class AdditionalFeatureOwnerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise unittest.SkipTest('C++17 compiler required')
        paths = ('src/peripherals/SensorDriver.cpp', 'include/peripherals/SensorDriver.h',
                 'src/peripherals/SevenSegmentDriver.cpp', 'include/peripherals/SevenSegmentDriver.h',
                 'src/core/PeripheralManager.cpp', 'include/core/FeatureInstanceRegistry.h',
                 'include/core/AsyncExecTypes.h', 'include/core/SensorSampleMetadata.h',
                 'test/unit/cpp/feature_additional_owners_harness.cpp.in',
                 'test/unit/test_feature_additional_owners.py', 'test/mocks/Arduino.h',
                 'include/peripherals/NeoPixelTransport.h',
                 'include/core/RuntimeResourceSnapshot.h',
                 'include/core/ResourceConsumerIdentity.h', 'include/core/DisplayOutputTypes.h',
                 'include/core/Ds18b20Address.h')
        cls.inputs = [ROOT / p for p in paths]
        cls.hashes = [hashlib.sha256(p.read_bytes()).hexdigest() for p in cls.inputs]
        sensor = (ROOT / paths[0]).read_text(encoding='utf-8')
        header = (ROOT / paths[1]).read_text(encoding='utf-8')
        types = '\n'.join(definition(header, signature) + ';' for signature in (
            'struct SensorReadResult {', 'struct SensorValueResult {', 'struct ADCSensorCalibration {',
            'struct CurrentLoopCalibration {', 'struct NTCSensorCalibration {', 'enum class SensorDriverType',
            'class SensorDriver {'))
        # Expose state only for malformed-snapshot injection; methods stay intact.
        types = types.replace('private:', 'public:')
        types += '\n' + definition((ROOT / paths[3]).read_text(encoding='utf-8'),
                                  'class SevenSegmentDriver {').replace('private:', 'public:') + ';'
        production = '\n'.join(definition(sensor, signature) for signature in (
            'static SemaphoreHandle_t sensor_bus_mutex(', 'struct SensorBusLock',
            'static uint64_t sensorSampleNowMs(', 'static float deliverSensorSample(',
            'static inline bool retry_pending(', 'static uint32_t failure_backoff_ms(',
            'SensorDriver::SensorDriver()', 'SensorDriver& SensorDriver::getInstance()',
            'FastBee::FeatureInstanceSnapshot SensorDriver::observeDs18b20Instances(',
            'SensorDriver::~SensorDriver()', 'float SensorDriver::readDS18B20(',
            'float SensorDriver::readDS18B20Step(',
            'void SensorDriver::release(', 'void SensorDriver::releaseAll('))
        # definition returns the closing brace without a declaration semicolon.
        sensor_lock = definition(sensor, 'struct SensorBusLock')
        production = production.replace(sensor_lock, sensor_lock + ';', 1)
        seven = (ROOT / paths[2]).read_text(encoding='utf-8')
        seven = seven[seven.index('// TM1637 指令'):seven.rindex('#endif')]
        peripheral = (ROOT / paths[4]).read_text(encoding='utf-8')
        neo = peripheral[peripheral.index('static Adafruit_NeoPixel* g_neopixelInstance'):]
        neo = neo[:neo.index('\n#endif')]
        neo += '\n' + definition(peripheral, 'bool fastbeeNeoPixelProjectPinRetained(')
        guard = definition((ROOT / paths[6]).read_text(encoding='utf-8'), 'class MutexGuard') + ';'
        template = (ROOT / paths[8]).read_text(encoding='utf-8')
        code = template.replace('// @guard@', guard).replace('// @types@', types)
        code = code.replace('// @sensor@', production).replace('// @seven@', seven).replace('// @neo@', neo)
        cls.temp = tempfile.TemporaryDirectory(prefix='fastbee-additional-owners-')
        cls.addClassCleanup(cls.temp.cleanup)
        folder = Path(cls.temp.name)
        cpp = folder / 'owners.cpp'
        cpp.write_text(code, encoding='utf-8')
        cls.binary = folder / 'owners.exe'
        built = subprocess.run([compiler, '-std=c++17', '-pthread', '-Wall', '-Wextra', '-Werror',
            f'-I{ROOT / "test/mocks"}', f'-I{ROOT / "include"}', str(cpp), '-o', str(cls.binary)],
            capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=90)
        if built.returncode:
            raise AssertionError(built.stdout + built.stderr)

    @classmethod
    def tearDownClass(cls):
        if cls.hashes != [hashlib.sha256(p.read_bytes()).hexdigest() for p in cls.inputs]:
            raise AssertionError('Additional owner inputs changed during tests')

    def check(self, mode):
        result = subprocess.run([str(self.binary), mode], capture_output=True, text=True,
                                encoding='utf-8', errors='replace', timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_cold_queries_do_not_construct_owners(self): self.check('cold')
    def test_ds18_failed_read_keeps_only_allocated_evidence(self): self.check('ds18')
    def test_ds18_partial_owner_is_unknown_then_actual_release_clears(self): self.check('ds18_invalid')
    def test_ds18_busy_query_and_actual_release_serialize(self): self.check('ds18_busy')
    def test_ds18_business_read_recovers_failed_mutex_allocation(self): self.check('ds18_retry')
    def test_seven_actual_bitbang_lifecycle_does_not_claim_ack_success(self): self.check('seven')
    def test_seven_write_and_release_share_one_owner_lock(self): self.check('seven_busy')
    def test_seven_missing_mutex_retries_in_business_getter(self): self.check('seven_retry')
    def test_seven_concurrent_first_getters_publish_only_one_mutex(self): self.check('seven_first')
    def test_neopixel_actual_allocation_and_delete_publish_lifecycle(self): self.check('neo')
    def test_neopixel_gate_and_oom_failure_keep_stopped(self): self.check('neo_failure')
    def test_neopixel_query_during_allocation_and_delete_is_passive(self): self.check('neo_busy')
    def test_neopixel_begin_pixels_and_transport_failure_do_not_claim_success(self): self.check('neo_sdk_failure')
    def test_neopixel_retirement_failure_preserves_original_object_and_pin(self): self.check('neo_retire_failure')
    def test_neopixel_business_oom_retries_but_failed_teardown_retains_pin(self): self.check('neo_retryable')


if __name__ == '__main__':
    unittest.main()
