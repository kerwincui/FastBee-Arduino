"""Actual legacy SensorDriver methods with deterministic bus/clock seams."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test.unit.test_periph_exec_config_persistence import cpp_function

ROOT = Path(__file__).resolve().parents[2]


class LegacySensorQualityTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise RuntimeError('C++17 compiler required')
        source = (ROOT/'src/peripherals/SensorDriver.cpp').read_text(encoding='utf-8')
        header = (ROOT/'include/peripherals/SensorDriver.h').read_text(encoding='utf-8')
        types = '\n'.join(cpp_function(header, signature)+';' for signature in (
            'struct SensorReadResult {', 'struct SensorValueResult {', 'struct ADCSensorCalibration {',
            'struct CurrentLoopCalibration {', 'struct NTCSensorCalibration {', 'enum class SensorDriverType',
            'class SensorDriver {')).replace('private:', 'public:')
        production = '\n'.join(cpp_function(source, signature) for signature in (
            'static uint64_t sensorSampleNowMs(', 'static bool readLegacyAdcSamples(',
            'struct LegacyAdcPinGuard', 'static float deliverSensorSample(',
            'static bool adc_calibration_equal(', 'static bool current_loop_calibration_equal(',
            'static bool finite_positive(', 'static inline bool retry_pending(',
            'static uint32_t failure_backoff_ms(', 'static inline float dht_cached_field(',
            'SensorDriver::SensorDriver()', 'SensorDriver& SensorDriver::getInstance()',
            'SensorDriver::~SensorDriver()', 'float SensorDriver::readDHT(',
            'float SensorDriver::readDS18B20(', 'float SensorDriver::readDS18B20Step(', 'float SensorDriver::readUltrasonic(',
            'float SensorDriver::readCurrent(', 'float SensorDriver::readCurrentLoop(',
            'float SensorDriver::readVoltage(', 'float SensorDriver::readNTC(',
            'void SensorDriver::release(', 'void SensorDriver::releaseAll('))
        guard = cpp_function(source, 'struct LegacyAdcPinGuard')
        production = production.replace(guard, guard + ';', 1)
        template = (ROOT/'test/unit/cpp/legacy_sensor_quality_harness.cpp.in').read_text(encoding='utf-8')
        cls.temp = tempfile.TemporaryDirectory(prefix='fastbee-legacy-quality-')
        cls.addClassCleanup(cls.temp.cleanup)
        folder = Path(cls.temp.name)
        cpp = folder/'legacy.cpp'
        cpp.write_text(template.replace('// @types@',types).replace('// @production@',production),encoding='utf-8')
        cls.binary=folder/'legacy.exe'
        built=subprocess.run([compiler,'-std=c++17','-Wall','-Wextra','-Werror',
            '-I'+str(ROOT/'test/mocks'),'-I'+str(ROOT/'include'),str(cpp),'-o',str(cls.binary)],
            capture_output=True,text=True,timeout=90)
        if built.returncode:
            raise AssertionError(built.stdout+built.stderr)

    def check(self,mode):
        run=subprocess.run([str(self.binary),mode],capture_output=True,text=True,timeout=10)
        self.assertEqual(run.returncode,0,run.stdout+run.stderr)


for _mode in ('dht_cache','dht_stale','dht_type','dht_wrap','dht_invalid_field','ds_index',
              'ds_stale','ds_zero','ds_wrap','ds_invalid','ds_pending','ds_release','ds_rollback',
              'ds_removed','ds_address','ds_slow','ds_disabled','ds_recovery',
              'ds_public_wait','ds_public_release','ds_public_reinit','ds_public_slow','ds_public_multiple',
              'ultrasonic_stale','ultrasonic_pin','voltage_calibration','ntc_calibration',
              'current_cache','current_loop','busy','invalid_calibration','sample_zero','sample_clock_backwards',
              'adc_failure','adc_pin_conflict'):
    setattr(LegacySensorQualityTests,'test_'+_mode,lambda self,mode=_mode:self.check(mode))


if __name__=='__main__':
    unittest.main()
