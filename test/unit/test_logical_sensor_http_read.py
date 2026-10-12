"""Compile the real HTTP read admission and reject an untyped logical source."""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class LogicalSensorHttpReadTests(unittest.TestCase):
    def test_logical_source_never_reports_a_successful_null_sample(self):
        source_path = ROOT / 'src/network/handlers/PeripheralReadRoutes.cpp'
        before = hashlib.sha256(source_path.read_bytes()).hexdigest()
        source = source_path.read_text(encoding='utf8')
        start = source.index('void PeripheralRouteHandler::handleReadPeripheral(')
        end = source.index('    JsonDocument doc;', start)
        admission = source[start:end] + '    request->continued = true;\n}\n'
        program = r'''
#include <Arduino.h>
#include <cassert>
#include <cstring>
#include <iostream>
#include "core/PeripheralTypes.h"
struct AsyncWebServerRequest { int status = 0; String message; bool continued = false; };
struct PeripheralConfig {
    PeripheralType type = PeripheralType::SENSOR_GENERIC;
    struct { struct { char driver[16] = {}; unsigned char sensorType = 0; } sensor; } params;
};
struct Context {
    bool authenticated = true;
    String id = "dht11";
    bool requireAuth(AsyncWebServerRequest* request) {
        if (!authenticated) request->status = 401;
        return authenticated;
    }
    String getParamValue(AsyncWebServerRequest*, const char*, const char*) { return id; }
    void sendError(AsyncWebServerRequest* request, int code, const char* message) {
        request->status = code; request->message = message;
    }
};
struct PeripheralManager {
    bool exists = true; unsigned snapshots = 0;
    PeripheralConfig selected;
    static PeripheralManager& getInstance() { static PeripheralManager manager; return manager; }
    bool getPeripheralSnapshot(const String&, PeripheralConfig& config, uint32_t* epoch) {
        ++snapshots; if (!exists) return false; config = selected; *epoch = 17; return true;
    }
};
struct PeripheralRouteHandler { Context* ctx; void handleReadPeripheral(AsyncWebServerRequest*); };
// @admission@
int main() {
    Context context; PeripheralRouteHandler handler{&context};
    auto& manager = PeripheralManager::getInstance();
    context.authenticated = false;
    { AsyncWebServerRequest request; handler.handleReadPeripheral(&request);
      assert(request.status == 401 && !request.continued && manager.snapshots == 0); }
    context.authenticated = true; context.id = "";
    { AsyncWebServerRequest request; handler.handleReadPeripheral(&request);
      assert(request.status == 400 && !request.continued && manager.snapshots == 0); }
    context.id = "dht11"; manager.exists = false;
    { AsyncWebServerRequest request; handler.handleReadPeripheral(&request);
      assert(request.status == 404 && !request.continued); }
    manager.exists = true;
    for (const char* id : {"dht11", "virtual-source"}) {
      context.id = id;
      for (unsigned char logicalType : {static_cast<unsigned char>(0), static_cast<unsigned char>(255)}) {
        manager.selected.params.sensor.sensorType = logicalType;
        AsyncWebServerRequest request; handler.handleReadPeripheral(&request);
        assert(request.status == 409 && !request.continued);
        assert(std::strstr(request.message.c_str(), "SENSOR_RULE_READ_REQUIRED:"));
        assert(std::strstr(request.message.c_str(), "sensor rule results"));
      }
    }
    std::strcpy(manager.selected.params.sensor.driver, "AHT20");
    { AsyncWebServerRequest request; handler.handleReadPeripheral(&request);
      assert(request.status == 0 && request.continued); }
    manager.selected.params.sensor.driver[0] = '\0';
    manager.selected.type = PeripheralType::UART;
    { AsyncWebServerRequest request; handler.handleReadPeripheral(&request);
      assert(request.status == 0 && request.continued); }
    std::cout << "logical sensor HTTP: authentication, missing ID/source, explicit 409, arbitrary type IDs and registered driver/other-type admission passed\n";
}
'''.replace('// @admission@', admission)
        compiler = shutil.which('g++') or shutil.which('clang++')
        self.assertIsNotNone(compiler, 'C++17 compiler required')
        json_header = next((ROOT/'.pio/libdeps').glob('*/ArduinoJson/src/ArduinoJson.h'))
        with tempfile.TemporaryDirectory(prefix='fastbee-logical-sensor-http-') as directory:
            temp = Path(directory)
            cpp = temp / 'read.cpp'; cpp.write_text(program, encoding='utf8')
            binary = temp / ('read.exe' if os.name == 'nt' else 'read')
            built = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                '-DARDUINOJSON_ENABLE_ARDUINO_STRING=0', '-DARDUINOJSON_ENABLE_STD_STRING=1',
                '-I'+str(ROOT/'test/mocks'), '-I'+str(ROOT/'include'),
                '-I'+str(json_header.parent), str(cpp), '-o', str(binary)],
                capture_output=True, text=True, timeout=90)
            self.assertEqual(built.returncode, 0, built.stdout+built.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
            print(result.stdout.strip())
        self.assertEqual(before, hashlib.sha256(source_path.read_bytes()).hexdigest())


if __name__ == '__main__': unittest.main()
