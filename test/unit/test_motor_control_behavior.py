import subprocess
import os
import shutil
import tempfile
import unittest
from pathlib import Path


class MotorControlBehaviorTests(unittest.TestCase):
    def test_simple_motor_configuration_without_required_setup(self):
        result = subprocess.run(['node', 'scripts/test-motor-simple-config.cjs'],
                                cwd=Path(__file__).resolve().parents[2], capture_output=True,
                                text=True, encoding='utf-8', timeout=20)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


    def test_motor_configuration_save_and_readback(self):
        for script in ('test-motor-edit-save.cjs', 'test-motor-travel-save.cjs', 'test-motor-save-readback.cjs',
                       'test-modbus-config-preservation.cjs', 'test-modbus-offline-recovery.cjs'):
            result = subprocess.run(['node', 'scripts/' + script],
                                    cwd=Path(__file__).resolve().parents[2], capture_output=True,
                                    text=True, encoding='utf-8', timeout=20)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_reference_confirmation_ui(self):
        result = subprocess.run(['node', 'scripts/test-motor-travel-ui.cjs'],
                                cwd=Path(__file__).resolve().parents[2], capture_output=True,
                                text=True, encoding='utf-8', timeout=20)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_read_only_state_sync_lifecycle(self):
        result = subprocess.run(['node', 'scripts/test-motor-state-sync.cjs'],
                                cwd=Path(__file__).resolve().parents[2], capture_output=True,
                                text=True, encoding='utf-8', timeout=20)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_production_service_with_mock_bus(self):
        self._production_service_with_mock_bus(True)

    def test_production_report_without_rule_feature(self):
        self._production_service_with_mock_bus(False)

    def _production_service_with_mock_bus(self, rules):
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
            backend = (root / 'src/core/PeripheralControlModbus.cpp').read_text(encoding='utf-8')
            (temporary / 'modbus_backend_production.inc').write_text('\n'.join(
                line for line in backend.splitlines() if not line.lstrip().startswith('#include')), encoding='utf-8')
            executable = str(temporary / 'motor-service.exe')
            env = dict(os.environ)
            env['PATH'] = str(Path(compiler).parent) + os.pathsep + env.get('PATH', '')
            build = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                    '-DARDUINO_ARCH_ESP32=1', '-DCONFIG_IDF_TARGET_ESP32S3=1',
                                    '-DFASTBEE_ENABLE_MODBUS=1', '-DBOARD_HAS_PSRAM=1',
                                    '-DFASTBEE_ENABLE_PERIPH_EXEC=' + ('1' if rules else '0'),
                                    '-DARDUINOJSON_ENABLE_STD_STRING=1', '-DARDUINOJSON_ENABLE_ARDUINO_STRING=0',
                                    *['-I' + str(p) for p in (temporary, root / 'test/helpers/motor_service_mocks',
                                                            root / 'test/mocks', root / 'include', json_header.parent)],
                                    str(root / 'test/helpers/motor_service_harness.cpp'), '-o', executable],
                                   capture_output=True, text=True, encoding='utf-8', env=env, timeout=120)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            for mode in (('psram', 'internal', 'fallback', 'failure') if rules else ('no-rules',)):
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
