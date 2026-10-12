"""Real serializer/readback/commit paths: bounded RAM and failure preservation."""
from pathlib import Path
import hashlib
import json
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def method(source, signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 0
    for index in range(opening, len(source)):
        depth += (source[index] == '{') - (source[index] == '}')
        if not depth:
            return source[start:index + 1]
    raise AssertionError(signature)


class ConfigStorageSavePeakTests(unittest.TestCase):
    def test_complete_readback_faults_atomic_commit_and_measured_peak(self):
        compiler = shutil.which('g++')
        headers = Path(os.environ.get('FASTBEE_JSON_HEADERS', str(ROOT / '.pio/libdeps/esp32-F4R0/ArduinoJson/src')))
        self.assertTrue(compiler and (headers / 'ArduinoJson.h').exists())
        path = ROOT / 'src/systems/ConfigStorage.cpp'
        before = hashlib.sha256(path.read_bytes()).hexdigest()
        source = path.read_text(encoding='utf8')
        helper = '\n'.join(method(source, signature) for signature in (
            'bool verifyJsonFile(', 'bool requiresParsedJsonValidation(', 'struct JsonFileCompareWriter',
            'bool verifySerializedJsonFile(', 'bool atomicReplaceFromTmp('))
        # Method extraction returns the closing brace; structs need a semicolon.
        helper = helper.replace('\n}\nbool verifySerializedJsonFile', '\n};\nbool verifySerializedJsonFile')
        tracker = (ROOT / 'test/unit/cpp/config_storage_load_ownership_harness.cpp.in').read_text(encoding='utf8')
        tracker = tracker[tracker.index('extern "C" void* __real_malloc'):tracker.index('using String =')]
        template = (ROOT / 'test/unit/cpp/config_save_peak_harness.cpp.in').read_text(encoding='utf8')
        cpp_text = template.replace('@TRACKER@', tracker).replace('@HELPERS@', helper).replace('@SAVE@', method(source, 'bool ConfigStorage::saveJSONConfigImpl(')).replace('@FLUSH@', method(source, 'bool ConfigStorage::flushToDisk('))
        with tempfile.TemporaryDirectory(prefix='fastbee-config-save-peak-') as temporary:
            directory = Path(temporary)
            environment = dict(os.environ)
            environment['PATH'] = str(Path(compiler).parent) + os.pathsep + environment.get('PATH', '')
            cpp, binary = directory / 'save.cpp', directory / ('save.exe' if os.name == 'nt' else 'save')
            cpp.write_text(cpp_text, encoding='utf8')
            built = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                '-DARDUINOJSON_ENABLE_ARDUINO_STRING=0', '-DARDUINOJSON_ENABLE_STD_STRING=1',
                '-I' + str(headers), '-I' + str(ROOT / 'include'), str(cpp),
                str(ROOT / 'src/utils/SharedJsonSerializer.cpp'),
                '-Wl,--wrap=malloc,--wrap=realloc,--wrap=free', '-o', str(binary)],
                capture_output=True, text=True, env=environment, timeout=60)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            ran = subprocess.run([str(binary)], capture_output=True, text=True, env=environment, timeout=20)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
            data = json.loads(ran.stdout)
            self.assertEqual(data['cases'], 43)
            self.assertEqual(data['verifyHeapBytes'], 0)
            self.assertLess(data['authNewPeakBytes'], data['authOldPeakBytes'])
            self.assertLess(data['largeNewPeakBytes'], data['largeOldPeakBytes'])
            print(ran.stdout.strip())
            destination = os.environ.get('FASTBEE_CONFIG_SAVE_PEAK_RESULT')
            if destination:
                Path(destination).write_text(json.dumps(data, indent=2)+'\n', encoding='utf8')
        self.assertEqual(before, hashlib.sha256(path.read_bytes()).hexdigest())

    def test_both_document_saves_are_pinned_to_the_borrowed_source(self):
        source = (ROOT / 'src/systems/ConfigStorage.cpp').read_text(encoding='utf8')
        for signature in ('bool ConfigStorage::saveJSONConfigImpl(', 'bool ConfigStorage::flushToDisk('):
            body = method(source, signature)
            self.assertIn('if (config.overflowed()) return false;', body)
            self.assertIn('!verifySerializedJsonFile(tmpPath, config)', body)
            self.assertIn('atomicReplaceFromTmp(tmpPath, filename, &config)', body)
            self.assertLess(body.index('file.flush()'), body.index('!verifySerializedJsonFile'))
            self.assertLess(body.index('file.close()'), body.index('!verifySerializedJsonFile'))
        raw = method(source, 'bool ConfigStorage::flushRawToDisk(')
        self.assertIn('!verifyJsonFile(tmpPath)', raw)
        self.assertIn('atomicReplaceFromTmp(tmpPath, filename)', raw)
        self.assertNotIn('verifySerializedJsonFile', raw)
        # Existing cache lock and publish-after-commit ordering are retained.
        save = method(source, 'bool ConfigStorage::saveJSONConfigImpl(')
        self.assertLess(save.index('config.overflowed()'), save.index('CacheLock lock(_cacheMutex)'))
        self.assertLess(save.index('CacheLock lock(_cacheMutex)'), save.index('flushToDisk(filename, config)'))
        self.assertLess(save.index('flushToDisk(filename, config)'), save.index('entry->dirty = false'))
        shared = (ROOT / 'src/utils/SharedJsonSerializer.cpp').read_text(encoding='utf8')
        self.assertNotIn('Semaphore', shared)
        self.assertNotIn('mutex', shared)


if __name__ == '__main__':
    unittest.main()
