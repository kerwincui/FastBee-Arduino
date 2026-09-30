import subprocess
import os
import shutil
import tempfile
import unittest
from pathlib import Path


class MotorControlBehaviorTests(unittest.TestCase):
    def test_read_only_state_sync_lifecycle(self):
        result = subprocess.run(['node', 'scripts/test-motor-state-sync.cjs'],
                                cwd=Path(__file__).resolve().parents[2], capture_output=True,
                                text=True, encoding='utf-8', timeout=20)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_production_service_with_mock_bus(self):
        root = Path(__file__).resolve().parents[2]
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            compiler = str(Path('D:/msys64/ucrt64/bin/g++.exe'))
        json_header = next((root / '.pio/libdeps').glob('*/ArduinoJson/src/ArduinoJson.h'))
        with tempfile.TemporaryDirectory(prefix='fastbee-motor-service-') as temp:
            temporary = Path(temp)
            source = (root / 'src/core/MotorControlService.cpp').read_text(encoding='utf-8')
            (temporary / 'motor_service_production.inc').write_text('\n'.join(
                line for line in source.splitlines() if not line.lstrip().startswith('#include')), encoding='utf-8')
            executable = str(temporary / 'motor-service.exe')
            env = dict(os.environ)
            env['PATH'] = str(Path(compiler).parent) + os.pathsep + env.get('PATH', '')
            build = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                    '-DARDUINO_ARCH_ESP32=1', '-DFASTBEE_ENABLE_MODBUS=1', '-DBOARD_HAS_PSRAM=1',
                                    '-DARDUINOJSON_ENABLE_STD_STRING=1', '-DARDUINOJSON_ENABLE_ARDUINO_STRING=0',
                                    *['-I' + str(p) for p in (temporary, root / 'test/mocks', root / 'include', json_header.parent)],
                                    str(root / 'test/helpers/motor_service_harness.cpp'), '-o', executable],
                                   capture_output=True, text=True, encoding='utf-8', env=env, timeout=120)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            for mode in ('psram', 'internal', 'fallback', 'failure'):
                with self.subTest(allocation=mode):
                    run = subprocess.run([executable, mode], capture_output=True, text=True, env=env, timeout=15)
                    self.assertEqual(run.returncode, 0, run.stdout + run.stderr)

    def test_production_queue_policy(self):
        root = Path(__file__).resolve().parents[2]
        compiler = shutil.which('g++') or shutil.which('clang++')
        fallback = Path('D:/msys64/ucrt64/bin/g++.exe')
        if not compiler and fallback.exists():
            compiler = str(fallback)
        self.assertTrue(compiler, 'C++ compiler required for motor queue acceptance')
        with tempfile.TemporaryDirectory(prefix='fastbee-motor-') as temp:
            executable = str(Path(temp) / 'motor-queue.exe')
            env = dict(os.environ)
            env['PATH'] = str(Path(compiler).parent) + os.pathsep + env.get('PATH', '')
            build = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                    '-I' + str(root / 'include'),
                                    str(root / 'test/helpers/motor_command_queue_harness.cpp'),
                                    '-o', executable], capture_output=True, text=True, env=env, timeout=30)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([executable], capture_output=True, text=True, env=env, timeout=10)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)


    def test_production_dashboard_lifecycle_and_requests(self):
        result = subprocess.run(
            ['node', 'scripts/test-motor-control-lifecycle.js'],
            cwd=Path(__file__).resolve().parents[2], capture_output=True, text=True,
            encoding='utf-8', timeout=20)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_production_protocol_motor_contract(self):
        result = subprocess.run(
            ['node', 'scripts/test-protocol-motor-contract.cjs'],
            cwd=Path(__file__).resolve().parents[2], capture_output=True, text=True,
            encoding='utf-8', timeout=20)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
