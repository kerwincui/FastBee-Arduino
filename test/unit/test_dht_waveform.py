"""Exercise the production DHT reader against timed GPIO waveforms."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

from test_sensor_publication import definition

ROOT = Path(__file__).resolve().parents[2]


class DhtWaveformTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise unittest.SkipTest('A C++17 compiler is required')
        source_path = Path(os.environ.get('FASTBEE_SENSOR_SOURCE',
                                         ROOT / 'src/peripherals/SensorDriver.cpp'))
        source = source_path.read_text(encoding='utf-8')
        constants = '\n'.join(re.findall(
            r'static constexpr uint32_t DHT_(?:EDGE|START_EDGE|TRANSACTION)_TIMEOUT_US[^;]+;',
            source))
        cls.temporary = tempfile.TemporaryDirectory(prefix='fastbee-dht-waveform-')
        cls.addClassCleanup(cls.temporary.cleanup)
        directory = Path(cls.temporary.name)
        (directory / 'dht_reader.inc').write_text(
            constants + '\n' + definition(source, 'struct DhtReadDiagnostic') + ';\n' +
            definition(source, 'static bool readDhtBounded('),
            encoding='utf-8')
        cls.executable = directory / ('dht.exe' if os.name == 'nt' else 'dht')
        result = subprocess.run([compiler, '-std=c++17', '-O2', '-Wall', '-Wextra',
                                 '-Werror', '-I' + str(directory),
                                 str(ROOT / 'test/unit/cpp/dht_waveform_harness.cpp'),
                                 '-o', str(cls.executable)],
                                capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def run_case(self, name):
        result = subprocess.run([str(self.executable), name], capture_output=True,
                                text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_dht11_valid_frame_already_in_preamble(self):
        self.run_case('dht11')

    def test_dht22_valid_frame(self):
        self.run_case('dht22')

    def test_dht22_negative_temperature(self):
        self.run_case('negative')

    def test_micros_wrap(self):
        self.run_case('wrap')

    def test_idle_high_disconnected_is_bounded(self):
        self.run_case('disconnected')

    def test_held_low_is_bounded(self):
        self.run_case('stuck-low')

    def test_missing_bit_is_rejected(self):
        self.run_case('truncated')

    def test_bad_checksum_is_rejected(self):
        self.run_case('checksum')

    def test_implausible_pulse_is_rejected(self):
        self.run_case('long-pulse')

    def test_invalid_humidity_is_rejected(self):
        self.run_case('range')

    def test_interrupt_delay_is_bounded(self):
        self.run_case('preempted')

    def test_missing_response_high_is_distinguished(self):
        self.run_case('missing-response-high')

    def test_diagnostic_is_optional_and_decoding_unchanged(self):
        self.run_case('no-diagnostic')


if __name__ == '__main__':
    unittest.main()
