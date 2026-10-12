"""Request-local Modbus evidence, compiled against the production receive loop."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SOURCE = Path(os.environ.get('FASTBEE_TEST_MODBUS_SOURCE', ROOT))


class ModbusReadDiagnosticsTests(unittest.TestCase):
    def test_production_read_route(self):
        source = (SOURCE / 'src/network/handlers/ModbusRouteHandler.cpp').read_text(encoding='utf8')
        methods = []
        for signature in ('static void addModbusReadDiagnostics(', 'static void sendOneShotError(',
                          'static void addModbusDebug(', 'void ModbusRouteHandler::handleModbusRegisterRead('):
            start = source.index(signature)
            methods.append(source[start:source.index('\n}', start) + 2])
        compiler = shutil.which('g++') or 'D:/msys64/ucrt64/bin/g++.exe'
        json_header = next((ROOT / '.pio/libdeps').glob('*/ArduinoJson/src/ArduinoJson.h'))
        env = dict(os.environ)
        env['PATH'] = str(Path(compiler).parent) + os.pathsep + env.get('PATH', '')
        with tempfile.TemporaryDirectory(prefix='fastbee-modbus-read-route-') as temp:
            temp = Path(temp)
            (temp / 'modbus_read_route_production.inc').write_text('\n'.join(methods), encoding='utf8')
            binary = temp / ('route.exe' if os.name == 'nt' else 'route')
            built = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                '-Wno-unused-parameter', '-DARDUINOJSON_ENABLE_ARDUINO_STRING=0',
                '-DARDUINOJSON_ENABLE_STD_STRING=1', '-I' + str(temp),
                '-I' + str(SOURCE / 'include'), '-I' + str(json_header.parent),
                str(ROOT / 'test/helpers/modbus_read_route_harness.cpp'),
                str(SOURCE / 'src/network/FixedJsonField.cpp'), '-o', str(binary)],
                capture_output=True, text=True, encoding='utf8', env=env, timeout=60)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            ran = subprocess.run([str(binary)], capture_output=True, text=True, env=env, timeout=10)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)

    def test_production_transaction_diagnostics(self):
        source = (SOURCE / 'src/protocols/ModbusHandler.cpp').read_text(encoding='utf8')
        methods = []
        for signature in ('bool ModbusHandler::initializeSerial(',
                          'uint16_t ModbusHandler::calculateCRC(',
                          'bool ModbusHandler::validateFrame(',
                          'void ModbusHandler::_syncMotorTravel(',
                          'OneShotError ModbusHandler::_executeModbusTransactionRaw('):
            start = source.index(signature)
            methods.append(source[start:source.index('\n}', start) + 2])
        compiler = shutil.which('g++') or 'D:/msys64/ucrt64/bin/g++.exe'
        env = dict(os.environ)
        env['PATH'] = str(Path(compiler).parent) + os.pathsep + env.get('PATH', '')
        with tempfile.TemporaryDirectory(prefix='fastbee-modbus-diagnostics-') as temp:
            temp = Path(temp)
            (temp / 'modbus_read_diagnostics_production.inc').write_text('\n'.join(methods), encoding='utf8')
            binary = temp / ('diagnostics.exe' if os.name == 'nt' else 'diagnostics')
            built = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                '-I' + str(temp), '-I' + str(SOURCE / 'include'),
                str(ROOT / 'test/helpers/modbus_read_diagnostics_harness.cpp'), '-o', str(binary)],
                capture_output=True, text=True, encoding='utf8', env=env, timeout=60)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            ran = subprocess.run([str(binary)], capture_output=True, text=True, env=env, timeout=10)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)

    def test_read_api_uses_request_owned_evidence(self):
        source = (SOURCE / 'src/network/handlers/ModbusRouteHandler.cpp').read_text(encoding='utf8')
        start = source.index('void ModbusRouteHandler::handleModbusRegisterRead(')
        body = source[start:source.index('\n}', start)]
        self.assertIn('nullptr, &diagnostics)', body)
        self.assertIn('sendOneShotError(ctx, request, result, &diagnostics)', body)
        self.assertIn('addModbusReadDiagnostics(doc, diagnostics)', body)
        self.assertIn('const int slaveAddr', body)
        self.assertIn('quantity > Protocols::MODBUS_ONESHOT_BUFFER_SIZE', body)
        self.assertIn('quantity > 65536 - startAddr', body)
        error_start = source.index('case ONESHOT_EXCEPTION: {')
        exception_branch = source[error_start:source.index('case ONESHOT_NOT_INITIALIZED:', error_start)]
        self.assertIn('httpCode = 422', exception_branch)
        self.assertIn('request->send(httpCode, "application/json", out)', exception_branch)
        self.assertNotIn('sendJsonStream', exception_branch)


if __name__ == '__main__':
    unittest.main()
