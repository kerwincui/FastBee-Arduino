"""Real encoder allocation/release/snapshot methods, with no GPIO or devices."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test.unit.test_sensor_publication import definition

ROOT = Path(__file__).resolve().parents[2]


class EncoderResourceSnapshotTests(unittest.TestCase):
    def test_actual_slots_capacity_release_copy_and_busy_without_isr_lock(self):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise RuntimeError('C++17 compiler required')
        header = (ROOT / 'include/core/PeripheralManager.h').read_text(encoding='utf-8')
        source = (ROOT / 'src/core/PeripheralManagerIo.cpp').read_text(encoding='utf-8')
        methods = '\n'.join(definition(source, signature) for signature in (
            'void PeripheralManager::snapshotEncoderResources(',
            'PeripheralManager::EncoderIsrSlot* PeripheralManager::findEncoderSlot(',
            'PeripheralManager::EncoderIsrSlot* PeripheralManager::allocEncoderSlot(',
            'void PeripheralManager::releaseEncoderSlot('))
        methods += '\n' + definition((ROOT / 'src/core/PeripheralManagerEncoder.cpp').read_text(encoding='utf-8'),
                                     'bool PeripheralManager::encoderSlotReady(')
        harness = (ROOT / 'test/unit/cpp/encoder_resource_harness.cpp.in').read_text(encoding='utf-8')
        harness = harness.replace('// @slot@', definition(header, 'struct EncoderIsrSlot') + ';')
        harness = harness.replace('// @production@', methods)
        with tempfile.TemporaryDirectory(prefix='fastbee-encoder-resource-') as folder:
            temp = Path(folder)
            cpp = temp / 'encoder.cpp'
            cpp.write_text(harness, encoding='utf-8')
            binary = temp / ('encoder.exe' if os.name == 'nt' else 'encoder')
            built = subprocess.run([compiler, '-std=c++17', '-pthread', '-Wall', '-Wextra', '-Werror',
                '-DCONFIG_IDF_TARGET_ESP32S3=1',
                '-I' + str(ROOT / 'test/unit/cpp/managed_pin_mocks'),
                '-I' + str(ROOT / 'test/unit/cpp'), '-I' + str(ROOT / 'include'),
                str(cpp), str(ROOT / 'src/core/ManagedPinIo.cpp'), '-o', str(binary)],
                capture_output=True, text=True, timeout=60)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            ran = subprocess.run([str(binary)], capture_output=True, text=True, timeout=15)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)


if __name__ == '__main__':
    unittest.main()
