"""Execute production device-save handlers and password-policy rollback offline."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest
from test_sensor_publication import definition

ROOT=Path(__file__).resolve().parents[2]

class DeviceConfigPersistenceTests(unittest.TestCase):
    def test_production_validation_commit_and_rollback(self):
        compiler=shutil.which('g++') or shutil.which('clang++')
        self.assertIsNotNone(compiler)
        headers=list((ROOT/'.pio/libdeps').glob('*/ArduinoJson/src/ArduinoJson.h'))
        self.assertTrue(headers)
        route=(ROOT/'src/network/handlers/DeviceRouteHandler.cpp').read_text(encoding='utf-8')
        users=(ROOT/'src/security/UserManager.cpp').read_text(encoding='utf-8')
        with tempfile.TemporaryDirectory(prefix='fastbee-device-config-') as directory:
            temp=Path(directory)
            arduino=(ROOT/'test/mocks/Arduino.h').read_text(encoding='utf-8')
            arduino=arduino.replace('void reserve(size_t size) { _value.reserve(size); }',
                'bool reserve(size_t size) { _value.reserve(size); return true; }\n'
                'bool concat(const char* s, unsigned int n) { _value.append(s,n); return true; }\n'
                'bool concat(const char* s) { _value.append(s); return true; }\n'
                'bool concat(char c) { _value.push_back(c); return true; }')
            (temp/'Arduino.h').write_text(arduino,encoding='utf-8')
            (temp/'WString.h').write_text('#include <Arduino.h>\n',encoding='utf-8')
            (temp/'device_save.inc').write_text(definition(users,'bool UserManager::updatePasswordPolicy(')+'\n'+
                definition(route,'void DeviceRouteHandler::handleSaveDeviceConfigJson(')+'\n'+
                definition(route,'void DeviceRouteHandler::handleSaveDeviceConfig('),encoding='utf-8')
            exe=temp/('device-config.exe' if os.name=='nt' else 'device-config')
            result=subprocess.run([compiler,'-std=c++17','-Wall','-Wextra','-Werror',
                '-DARDUINOJSON_ENABLE_ARDUINO_STRING=1','-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0',
                '-DARDUINOJSON_ENABLE_ARDUINO_PRINT=0','-DARDUINOJSON_ENABLE_PROGMEM=0',
                f'-I{temp}',f'-I{headers[0].parent}',f'-I{ROOT / "include"}',
                str(ROOT/'test/unit/cpp/device_config_persistence_harness.cpp'),'-o',str(exe)],
                capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=60)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            result=subprocess.run([str(exe)],capture_output=True,text=True,timeout=15)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
