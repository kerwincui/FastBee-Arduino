"""Compare actual parameter bridge methods against the original String implementation."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[2]
PARAM='// ============ 参数处理辅助方法 ============'
AUTH='// ============ 认证辅助方法 ============'

def parameter_segment(path):
    text=path.read_text(encoding='utf-8')
    return text[text.index(PARAM)+len(PARAM):text.index(AUTH)].strip()

class WebHandlerParameterBridgeTests(unittest.TestCase):
    def test_actual_methods_preserve_post_query_null_empty_and_mixed_overloads(self):
        oracle=json.loads((ROOT/'test/unit/cpp/web_handler_parameter_before.json').read_text(encoding='utf-8'))
        declarations=parameter_segment(ROOT/'include/network/WebHandlerContext.h')
        methods=parameter_segment(ROOT/'src/network/WebHandlerContext.cpp')
        self.assertTrue(methods.startswith(oracle['definitions']), 'Original String parsing body changed')
        harness=(ROOT/'test/unit/cpp/web_handler_parameter_bridge_harness.cpp.in').read_text(encoding='utf-8')
        harness=harness.replace('@ORIGINAL_DECLARATIONS@',oracle['declarations'])
        harness=harness.replace('@PRODUCTION_DECLARATIONS@',declarations)
        harness=harness.replace('@ORIGINAL_METHODS@',oracle['definitions'].replace('WebHandlerContext::','OriginalWebHandlerContext::'))
        harness=harness.replace('@PRODUCTION_METHODS@',methods)
        compiler=shutil.which('g++') or shutil.which('clang++')
        self.assertTrue(compiler, 'Real C++17 compiler required')
        with tempfile.TemporaryDirectory(prefix='fastbee-web-handler-parameters-') as directory:
            temp=Path(directory);source=temp/'parameters.cpp'
            source.write_text(harness,encoding='utf-8')
            exe=temp/('parameters.exe' if os.name=='nt' else 'parameters')
            build=subprocess.run([compiler,'-std=c++17','-Wall','-Wextra','-Werror','-Os',
                                  '-I'+str(ROOT/'test/mocks'),str(source),'-o',str(exe)],
                                 capture_output=True,text=True,timeout=120)
            self.assertEqual(build.returncode,0,build.stdout+build.stderr)
            run=subprocess.run([str(exe)],capture_output=True,text=True,timeout=30)
            self.assertEqual(run.returncode,0,run.stdout+run.stderr)
            self.assertIn('PASS compared=164 actual production methods',run.stdout)

    def test_literal_bridges_keep_shared_string_paths_and_compiler_boundaries(self):
        methods=parameter_segment(ROOT/'src/network/WebHandlerContext.cpp')
        oracle=json.loads((ROOT/'test/unit/cpp/web_handler_parameter_before.json').read_text(encoding='utf-8'))
        self.assertTrue(methods.startswith(oracle['definitions']))
        added=methods[len(oracle['definitions']):]
        self.assertIn('__attribute__((noinline, noclone))',added)
        self.assertEqual(added.count('\nFASTBEE_PARAM_LITERAL_BRIDGE\n'),5)
        self.assertNotIn('request->',added)
        self.assertNotIn('.toInt()',added)
        self.assertNotIn('.equalsIgnoreCase(',added)
        self.assertIn('return getParamValue(request, String(paramName), String(defaultValue));',added)
        self.assertIn('return getParamValue(request, String(paramName), defaultValue);',added)
        self.assertIn('return getParamValue(request, paramName, String(defaultValue));',added)
        self.assertIn('return getParamBool(request, String(paramName), defaultValue);',added)
        self.assertIn('return getParamInt(request, String(paramName), defaultValue);',added)

if __name__=='__main__':unittest.main()
