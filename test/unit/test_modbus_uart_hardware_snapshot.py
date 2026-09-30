"""Compile the real read-only telemetry against register and UART getter fakes."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SOURCE = Path(os.environ.get('FASTBEE_TEST_MODBUS_SOURCE', ROOT))

class ModbusUartHardwareSnapshotTests(unittest.TestCase):
    def compile_and_run(self, unsupported):
        compiler = shutil.which('g++') or 'D:/msys64/ucrt64/bin/g++.exe'
        json_header = next((ROOT / '.pio/libdeps').glob('*/ArduinoJson/src/ArduinoJson.h'))
        env = dict(os.environ)
        env['PATH'] = str(Path(compiler).parent) + os.pathsep + env.get('PATH', '')
        with tempfile.TemporaryDirectory(prefix='fastbee-uart-hardware-') as temp:
            temp = Path(temp)
            for header in ['Arduino.h', 'HardwareSerial.h', 'esp_idf_version.h',
                           'driver/gpio.h', 'driver/uart.h', 'soc/gpio_struct.h',
                           'soc/io_mux_reg.h', 'soc/uart_periph.h', 'soc/system_reg.h', 'hal/gpio_ll.h']:
                path = temp / header
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text('#pragma once\n', encoding='ascii')
            binary = temp / ('snapshot.exe' if os.name == 'nt' else 'snapshot')
            args = [compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-DARDUINOJSON_ENABLE_ARDUINO_STRING=0', '-DARDUINOJSON_ENABLE_STD_STRING=1',
                    '-I' + str(temp), '-I' + str(SOURCE / 'include'), '-I' + str(json_header.parent)]
            if unsupported:
                args.append('-DTEST_UNSUPPORTED=1')
            args += [str(ROOT / 'test/helpers/modbus_uart_hardware_snapshot_harness.cpp'), '-o', str(binary)]
            built = subprocess.run(args, capture_output=True, text=True, encoding='utf8', env=env, timeout=60)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            ran = subprocess.run([str(binary)], capture_output=True, text=True, env=env, timeout=10)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)

    def test_register_snapshot_and_failures_without_hardware_writes(self):
        self.compile_and_run(False)

    def test_unsupported_target_compiles_and_does_not_claim_measurement(self):
        self.compile_and_run(True)

    def test_route_is_authenticated_explicit_opt_in(self):
        source = (SOURCE / 'src/network/handlers/ModbusRouteHandler.cpp').read_text(encoding='utf8')
        start = source.index('void ModbusRouteHandler::handleGetModbusStatus(')
        body = source[start:source.index('\n}', start)]
        self.assertLess(body.index('requireAuth(request)'), body.index('appendUartHardwareSnapshot'))
        self.assertIn('request->hasParam("hardware") && request->getParam("hardware")->value() == "1"', body)
        self.assertIn('if (modbus) modbus->appendUartHardwareSnapshot(hardware)', body)
        self.assertIn('MODBUS_NOT_RUNNING', body)
        for forbidden in ['readRegistersOnce(', 'writeSingleRegister(', 'initializeSerial(', 'setTransmitMode(']:
            self.assertNotIn(forbidden, body)

if __name__ == '__main__':
    unittest.main()
