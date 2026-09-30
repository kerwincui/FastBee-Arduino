"""Compile the actual route body, without hardware/network/serial access."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SOURCE = Path(os.environ.get('FASTBEE_TEST_MOTOR_SOURCE', ROOT))


class MotorStateRouteTests(unittest.TestCase):
    def test_production_route(self):
        route = (SOURCE / 'src/network/handlers/ModbusRouteHandler.cpp').read_text(encoding='utf8')
        start = route.index('void ModbusRouteHandler::handleModbusMotorControl(')
        production = route[start:route.index('\n}', start) + 2]
        header = (SOURCE / 'include/core/MotorControlService.h').read_text(encoding='utf8')
        start = header.index('struct MotorControlState {')
        types = header[start:header.index('\n};', start) + 3]
        harness = (ROOT / 'test/helpers/motor_state_route_harness.cpp').read_text(encoding='utf8').replace('STATE_TYPE', types)
        compiler = shutil.which('g++') or 'D:/msys64/ucrt64/bin/g++.exe'
        json_header = next((ROOT / '.pio/libdeps').glob('*/ArduinoJson/src/ArduinoJson.h'))
        env = dict(os.environ)
        env['PATH'] = str(Path(compiler).parent) + os.pathsep + env.get('PATH', '')
        with tempfile.TemporaryDirectory(prefix='fastbee-motor-state-route-') as directory:
            directory = Path(directory)
            (directory / 'motor_route_production.inc').write_text(production, encoding='utf8')
            (directory / 'harness.cpp').write_text(harness, encoding='utf8')
            binary = directory / 'route.exe'
            build = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                '-DARDUINOJSON_ENABLE_ARDUINO_STRING=0', '-DARDUINOJSON_ENABLE_STD_STRING=1',
                '-I' + str(directory), '-I' + str(ROOT / 'test/mocks'), '-I' + str(json_header.parent),
                str(directory / 'harness.cpp'), '-o', str(binary)], capture_output=True, text=True,
                encoding='utf8', env=env, timeout=60)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(binary)], capture_output=True, text=True, env=env, timeout=10)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
