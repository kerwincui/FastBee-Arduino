import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest
from test.unit.test_screen_panel_contract import braced_block

ROOT = pathlib.Path(__file__).resolve().parents[2]
SUPPORT = pathlib.Path(os.environ.get('FASTBEE_TEST_SUPPORT_ROOT', ROOT))


class ImportChunkRouteTests(unittest.TestCase):
    def test_load_failure_is_not_reported_as_empty_rules_success(self):
        route = (ROOT / 'src/network/handlers/PeriphExecRouteHandler.cpp').read_text(encoding='utf-8')
        body = route.split('void PeriphExecRouteHandler::handleGetRules(', 1)[1].split(
            'void PeriphExecRouteHandler::handleAddRule(', 1)[0]
        guard = 'if (mgr.hasConfigurationLoadError())'
        # Both single-rule and list reads must hit the failure response before
        # accessing or serializing an empty in-memory collection.
        self.assertLess(body.index(guard), body.index('if (request->hasParam("id"))'))
        self.assertLess(body.index(guard), body.index('mgr.getRules()'))
        failure = braced_block(body, guard)
        self.assertRegex(failure, r'ctx->sendError\(request,\s*409,\s*"CONFIG_LOAD_FAILED:')
        self.assertIn('return;', failure)

    def test_production_route_preserves_primary_and_has_idempotent_commit(self):
        route = (ROOT / 'src/network/handlers/PeriphExecRouteHandler.cpp').read_text(encoding='utf-8')
        header = (ROOT / 'include/network/handlers/PeriphExecRouteHandler.h').read_text(encoding='utf-8')
        signature = 'void PeriphExecRouteHandler::handleImportChunk(AsyncWebServerRequest* request)'
        fields = header[header.index('    String importSession;'):header.index('    void handleImportChunk(')]
        compiler = shutil.which('g++') or 'D:/msys64/ucrt64/bin/g++.exe'
        with tempfile.TemporaryDirectory(prefix='fastbee-import-chunk-') as directory:
            temporary = pathlib.Path(directory)
            (temporary / 'production_import_fields.inc').write_text(fields, encoding='utf-8')
            (temporary / 'production_import_chunk.inc').write_text(signature + ' {' + braced_block(route, signature) + '}\n', encoding='utf-8')
            exe = temporary / 'route.exe'
            env = dict(os.environ)
            env['PATH'] = str(pathlib.Path(compiler).parent) + os.pathsep + env.get('PATH', '')
            compiled = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-I'+str(temporary),
                                       '-DARDUINOJSON_ENABLE_ARDUINO_STRING=0', '-DARDUINOJSON_ENABLE_STD_STRING=1',
                                       '-I'+str(ROOT / 'include'), '-I'+str(SUPPORT / '.pio/libdeps/native/ArduinoJson/src'),
                                       str(ROOT / 'test/unit/cpp/periph_exec_import_chunk_harness.cpp'),
                                       str(ROOT / 'src/network/FixedJsonField.cpp'), '-o', str(exe)],
                                      capture_output=True, text=True, timeout=60, env=env)
            self.assertEqual(0, compiled.returncode, compiled.stdout + compiled.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10, env=env)
            self.assertEqual(0, result.returncode, result.stdout + result.stderr)
            # The old committed branch acknowledged different retry bytes as
            # success. Remove only its validation to prove the test catches it.
            body = braced_block(route, signature)
            committed = braced_block(body, 'if (importCommitted)')
            mutant = body.replace(committed, '', 1)
            self.assertNotEqual(mutant, body)
            (temporary / 'production_import_chunk.inc').write_text(signature + ' {' + mutant + '}\n', encoding='utf-8')
            compiled = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-I'+str(temporary),
                                       '-DARDUINOJSON_ENABLE_ARDUINO_STRING=0', '-DARDUINOJSON_ENABLE_STD_STRING=1',
                                       '-I'+str(ROOT / 'include'), '-I'+str(SUPPORT / '.pio/libdeps/native/ArduinoJson/src'),
                                       str(ROOT / 'test/unit/cpp/periph_exec_import_chunk_harness.cpp'),
                                       str(ROOT / 'src/network/FixedJsonField.cpp'), '-o', str(exe)],
                                      capture_output=True, text=True, timeout=60, env=env)
            self.assertEqual(0, compiled.returncode, compiled.stdout + compiled.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10, env=env)
            self.assertEqual(86, result.returncode, result.stdout + result.stderr)
