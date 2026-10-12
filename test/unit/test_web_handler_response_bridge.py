"""Compile actual response methods and compare original envelope behavior."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[2]
FIXTURES=ROOT/'test/unit/cpp'
END='// ============ 内置页面 ============'

def response_methods():
    source=(ROOT/'src/network/WebHandlerContext.cpp').read_text(encoding='utf-8')
    return source[source.index('void WebHandlerContext::sendSuccess(AsyncWebServerRequest* request, const JsonDocument& data)'):source.index(END)]

class WebHandlerResponseBridgeTests(unittest.TestCase):
    def test_actual_response_methods_preserve_status_escaping_truncation_and_headers(self):
        oracle=json.loads((FIXTURES/'web_handler_response_before.json').read_text(encoding='utf-8'))
        methods=response_methods()
        self.assertTrue(methods.startswith(oracle['definitions']), 'Original response methods changed')
        header=(ROOT/'include/network/WebHandlerContext.h').read_text(encoding='utf-8')
        start=header.index('    void sendSuccess(AsyncWebServerRequest* request, const JsonDocument& data)')
        declarations=header[start:header.index('\n\n    '+END,start)]
        harness=(FIXTURES/'web_handler_response_bridge_harness.cpp.in').read_text(encoding='utf-8')
        harness=harness.replace('@ORIGINAL_DECL@',oracle['declarations']).replace('@PRODUCTION_DECL@',declarations)
        harness=harness.replace('@ORIGINAL_METHODS@',oracle['definitions'].replace('WebHandlerContext::','OriginalWebHandlerContext::').replace('jsonEscapeToBuffer','originalJsonEscapeToBuffer'))
        harness=harness.replace('@PRODUCTION_METHODS@',methods)
        libdeps=Path(os.environ.get('PLATFORMIO_LIBDEPS_DIR',str(ROOT/'.pio/libdeps')))
        json_header=next(libdeps.glob('*/ArduinoJson/src/ArduinoJson.h'),None)
        self.assertIsNotNone(json_header,'Real ArduinoJson dependency required')
        compiler=shutil.which('g++') or shutil.which('clang++')
        self.assertTrue(compiler,'Real C++17 compiler required')
        with tempfile.TemporaryDirectory(prefix='fastbee-web-handler-responses-') as directory:
            temp=Path(directory);source=temp/'responses.cpp'
            source.write_text(harness,encoding='utf-8')
            exe=temp/('responses.exe' if os.name=='nt' else 'responses')
            build=subprocess.run([compiler,'-std=c++17','-Wall','-Wextra','-Werror','-Os','-I'+str(ROOT/'test/mocks'),'-I'+str(json_header.parent),str(source),'-o',str(exe)],capture_output=True,text=True,timeout=120)
            self.assertEqual(build.returncode,0,build.stdout+build.stderr)
            run=subprocess.run([str(exe)],capture_output=True,text=True,timeout=30)
            self.assertEqual(run.returncode,0,run.stdout+run.stderr)
            self.assertIn('PASS compared=1209 actual production response methods',run.stdout)

    def test_three_literal_bridges_reuse_original_string_and_document_response_methods(self):
        oracle=json.loads((FIXTURES/'web_handler_response_before.json').read_text(encoding='utf-8'))
        methods=response_methods()
        self.assertTrue(methods.startswith(oracle['definitions']))
        added=methods[len(oracle['definitions']):]
        self.assertIn('__attribute__((noinline, noclone))',added)
        self.assertEqual(added.count('\nFASTBEE_ERROR_LITERAL_BRIDGE\n'),3)
        self.assertIn('sendError(request, code, String(message));',added)
        self.assertIn('sendBadRequest(request, String(message));',added)
        self.assertIn('sendSuccess(request, String(message));',added)
        self.assertIn('#if defined(__GNUC__) && !defined(__clang__)',added)
        self.assertIn('#elif defined(__clang__)',added)
        for bypass in ('request->','jsonEscapeToBuffer(','millis(','new '):
            self.assertNotIn(bypass,added)

if __name__=='__main__':unittest.main()
