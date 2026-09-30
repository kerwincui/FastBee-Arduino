"""Generic motor profile validation and persistence, entirely off-device."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class MotorProtocolTests(unittest.TestCase):
    def test_production_wire_frames_match_vendor_and_generic_profiles(self):
        source = (ROOT / 'src/protocols/ModbusHandler.cpp').read_text(encoding='utf8')
        methods = []
        for signature in ('uint16_t ModbusHandler::calculateCRC(', 'OneShotResult ModbusHandler::readRegistersOnce(',
                          'OneShotResult ModbusHandler::writeRegisterOnce(', 'OneShotResult ModbusHandler::writeMultipleRegistersOnce('):
            start = source.index(signature)
            methods.append(source[start:source.index('\n}', start) + 2])
        compiler = shutil.which('g++') or 'D:/msys64/ucrt64/bin/g++.exe'
        env = dict(os.environ)
        env['PATH'] = str(Path(compiler).parent) + os.pathsep + env.get('PATH', '')
        with tempfile.TemporaryDirectory(prefix='fastbee-motor-wire-') as temp:
            temp = Path(temp)
            (temp / 'motor_wire_production.inc').write_text('\n'.join(methods), encoding='utf8')
            binary = temp / 'wire.exe'
            built = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                '-I' + str(temp), '-I' + str(ROOT / 'test/mocks'),
                '-I' + str(ROOT / 'include'),
                str(ROOT / 'test/helpers/motor_wire_harness.cpp'), '-o', str(binary)],
                capture_output=True, text=True, encoding='utf8', env=env, timeout=60)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            run = subprocess.run([str(binary)], capture_output=True, text=True, env=env, timeout=10)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)

    def test_production_profile_validation_and_roundtrip(self):
        compiler = shutil.which('g++') or 'D:/msys64/ucrt64/bin/g++.exe'
        json_header = next((ROOT / '.pio/libdeps').glob('*/ArduinoJson/src/ArduinoJson.h'))
        env = dict(os.environ)
        env['PATH'] = str(Path(compiler).parent) + os.pathsep + env.get('PATH', '')
        with tempfile.TemporaryDirectory(prefix='fastbee-motor-profile-') as temp:
            binary = Path(temp) / 'profile.exe'
            built = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                '-DARDUINOJSON_ENABLE_STD_STRING=1', '-DARDUINOJSON_ENABLE_ARDUINO_STRING=0',
                '-I' + str(ROOT / 'include'), '-I' + str(json_header.parent),
                str(ROOT / 'test/helpers/motor_protocol_harness.cpp'), '-o', str(binary)],
                capture_output=True, text=True, encoding='utf-8', env=env, timeout=60)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            run = subprocess.run([str(binary)], capture_output=True, text=True, env=env, timeout=10)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)

    def test_all_config_paths_preserve_profiles(self):
        routes = (ROOT / 'src/network/handlers/ProtocolRouteHandler.cpp').read_text(encoding='utf8')
        self.assertIn('copyJsonMember(out, in, "motorProtocol")', routes)
        self.assertIn('saveMotorProtocol(deviceOut, device.motorProtocol)', routes)
        self.assertIn('devObj["motorProtocol"].set(dv["motorProtocol"])', routes)
        self.assertLess(routes.index('loadMotorProtocol(device, profile)'), routes.index('if (next.size() >= previous.size())'))
        for file in ('ModbusHandler.cpp', 'ProtocolManager.cpp'):
            source = (ROOT / 'src/protocols' / file).read_text(encoding='utf8')
            self.assertIn('loadMotorProtocol(d, dev.motorProtocol)', source)
        self.assertIn('saveMotorProtocol(d, dev.motorProtocol)',
                      (ROOT / 'src/protocols/ModbusHandler.cpp').read_text(encoding='utf8'))


if __name__ == '__main__':
    unittest.main()
