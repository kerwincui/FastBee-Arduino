"""Actual sendSuccess + fragment class, real JSON and tracked String ownership."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test.unit.test_screen_panel_contract import braced_block

ROOT=Path(__file__).resolve().parents[2]

class JsonSuccessResponseTests(unittest.TestCase):
    def test_real_success_envelope_peak_and_failure_atomicity(self):
        compiler=shutil.which("g++") or "D:/msys64/ucrt64/bin/g++.exe"
        json=next((ROOT/".pio/libdeps").glob("*/ArduinoJson/src/ArduinoJson.h"))
        source=(ROOT/"src/network/WebHandlerContext.cpp").read_text(encoding="utf-8")
        signature="void WebHandlerContext::sendSuccess(AsyncWebServerRequest* request, const JsonDocument& data)"
        environment=dict(os.environ)
        environment["PATH"]=str(Path(compiler).parent)+os.pathsep+environment.get("PATH","")
        with tempfile.TemporaryDirectory(prefix="fastbee-json-success-") as directory:
            temp=Path(directory)
            shutil.copyfile(ROOT/"test/unit/cpp/json_success_arduino_mock.h",temp/"Arduino.h")
            (temp/"success_under_test.inc").write_text(signature+" {"+braced_block(source,signature)+"}\n",encoding="utf-8")
            (temp/"ESPAsyncWebServer.h").write_text('''#pragma once
#include <Arduino.h>
class AsyncAbstractResponse {
public:
 virtual ~AsyncAbstractResponse()=default;
 virtual bool _sourceValid() const {return false;}
 virtual size_t _fillBuffer(uint8_t*,size_t)=0;
 int _code=0; String _contentType;
 size_t _contentLength=0,_sentLength=0;bool _sendContentLength=false;
 void addHeader(const char*,const char*) {}
 static void* operator new(size_t n,const std::nothrow_t&) noexcept {return failResponse?nullptr: ::operator new(n,std::nothrow);}
 static void operator delete(void* p) noexcept {::operator delete(p);}
 static void operator delete(void* p,const std::nothrow_t&) noexcept {::operator delete(p);}
};
''',encoding="utf-8")
            exe=temp/"test.exe"
            built=subprocess.run([compiler,"-std=c++17","-Wall","-Wextra","-Werror",
                "-DARDUINOJSON_ENABLE_ARDUINO_STRING=0","-DARDUINOJSON_ENABLE_STD_STRING=1",
                f"-I{temp}",f"-I{ROOT/'include'}",f"-I{json.parent}",
                str(ROOT/"test/unit/cpp/json_success_harness.cpp"),"-o",str(exe)],
                capture_output=True,text=True,env=environment,timeout=60)
            self.assertEqual(built.returncode,0,built.stdout+built.stderr)
            result=subprocess.run([str(exe)],capture_output=True,text=True,env=environment,timeout=15)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            print(result.stdout.strip())

if __name__=="__main__": unittest.main()
