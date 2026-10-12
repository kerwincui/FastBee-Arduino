"""Run real Manager config methods with ArduinoJson and byte-level storage faults."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest
import hashlib
import json

ROOT = Path(__file__).resolve().parents[2]
SUPPORT = Path(os.environ.get('FASTBEE_TEST_SUPPORT_ROOT', ROOT))


def cpp_function(source, marker):
    start = source.index(marker)
    masked = re.sub(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
                    lambda m: ' ' * len(m.group()), source)
    opening = masked.index('{', start)
    depth = 0
    for at in range(opening, len(masked)):
        if masked[at] == '{': depth += 1
        elif masked[at] == '}':
            depth -= 1
            if depth == 0: return source[start:at + 1]
    raise AssertionError('unterminated ' + marker)


class PeriphExecConfigPersistenceTests(unittest.TestCase):
    def test_real_file_validation_load_migration_and_pending_restart(self):
        source = (ROOT / 'src/core/PeriphExecManager.cpp').read_text(encoding='utf-8')
        store = (ROOT / 'src/core/PeriphExecConfigStore.cpp').read_text(encoding='utf-8')
        constants = '\n'.join(line for line in source.splitlines() if line.startswith('constexpr ') and
                              'PERIPH_EXEC_RULE_MUTATION_LOCK_TIMEOUT_MS' not in line)
        source += '\n' + store
        source += "\n" + (ROOT / "src/core/PeriphExecRuleQueries.cpp").read_text(encoding="utf-8")
        source += "\n" + (ROOT / "include/core/PeriphExecRemoteBindings.h").read_text(encoding="utf-8")
        header = (ROOT / 'include/core/PeriphExecManager.h').read_text(encoding='utf-8')
        helpers = constants + '\n' + store[store.index('namespace {') + len('namespace {'):
                                            store.index('} // namespace')]
        helpers += '\n' + cpp_function(source, 'bool hasRetiredRemoteSource(')
        helpers += '\n' + cpp_function((SUPPORT / 'src/utils/FileUtils.cpp').read_text(encoding='utf-8'),
                                       'bool FileUtils::commitAtomicWrite(')
        methods = [cpp_function(source, 'bool PeriphExecManager::' + method + '(') for method in (
            'saveConfiguration', 'saveConfigurationLocked', 'writeConfigurationLocked',
            'validateConfigurationFile', 'importConfigurationFile', 'loadConfiguration',
            'copyIrKeysJson', 'setIrKeysJson', 'removeRuleLocked', 'addRuleLocked', 'updateRuleLocked', 'setRuleEnabledLocked',
            'sanitizeRuleForSafety', 'sanitizeTriggerForSafety')]
        methods.append(cpp_function(header, 'const char* configurationWriteError()').replace(
            'configurationWriteError()', 'PeriphExecManager::configurationWriteError()'))
        program = (ROOT / 'test/unit/cpp/periph_exec_config_harness.cpp').read_text(encoding='utf-8')
        program = program.replace('// PRODUCTION_HELPERS', helpers).replace('// PRODUCTION_METHODS', '\n'.join(methods))
        compiler = shutil.which('g++') or 'D:/msys64/ucrt64/bin/g++.exe'
        includes = [ROOT / 'test/unit/fixtures/periph_exec_config', SUPPORT / 'test/mocks',
                    SUPPORT / '.pio/libdeps/native/ArduinoJson/src', ROOT / 'include', SUPPORT / 'include']
        env = dict(os.environ)
        env['PATH'] = str(Path(compiler).parent) + os.pathsep + env.get('PATH', '')
        with tempfile.TemporaryDirectory(prefix='fastbee-config-persistence-') as directory:
            # Use ArduinoJson's actual Arduino String writer (which clears the
            # destination); the repository's generic host String lacks concat.
            arduino = (SUPPORT / 'test/mocks/Arduino.h').read_text(encoding='utf-8')
            arduino = arduino.replace('void clear() { _value.clear(); }',
                'void clear() { _value.clear(); }\n    bool concat(const char* p) { _value += p ? p : ""; return true; }')
            (Path(directory) / 'Arduino.h').write_text(arduino, encoding='utf-8')
            cpp = Path(directory) / 'config.cpp'
            binary = Path(directory) / 'config.exe'
            cpp.write_text(program, encoding='utf-8')
            for target in os.environ.get('FASTBEE_CONFIG_TEST_TARGETS', 'ESP32C3,ESP32,ESP32S3,ESP32C6').split(','):
                with self.subTest(target=target):
                    command = [compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-const-variable', '-Wno-unused-function',
                        '-DARDUINOJSON_ENABLE_ARDUINO_STRING=1', '-DCONFIG_IDF_TARGET_' + target,
                        '-I' + directory, *['-I' + str(p) for p in includes], str(cpp),
                        str(ROOT / 'src/network/FixedJsonField.cpp'),
                        str(ROOT / 'src/utils/SharedJsonSerializer.cpp'), str(ROOT / 'src/utils/SharedJsonDeserializer.cpp'), '-o', str(binary)]
                    built = subprocess.run(command, text=True, capture_output=True, timeout=60, env=env)
                    self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
                    result = subprocess.run([str(binary)], text=True, capture_output=True, timeout=15, env=env)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    if target == 'ESP32C3':
                        guard = 'if (!visitConfigurationFile(stagedPath, checkReferences, &importedIds, nullptr, errorMsg)) return false;'
                        self.assertEqual(program.count(guard), 1)
                        mutant = program.replace(guard, '(void)checkReferences;', 1)
                        try:
                            cpp.write_text(mutant, encoding='utf-8')
                            built_mutant = subprocess.run(command, text=True, capture_output=True, timeout=60, env=env)
                            self.assertEqual(built_mutant.returncode, 0, built_mutant.stdout + built_mutant.stderr)
                            rejected = subprocess.run([str(binary)], text=True, capture_output=True, timeout=15, env=env)
                            self.assertEqual(rejected.returncode, 86, rejected.stdout + rejected.stderr)
                            self.assertIn('!m.importConfigurationFile', rejected.stderr)
                            print('reference guard removed: C3 mutant correctly rejected, exit=86')
                        finally:
                            cpp.write_text(program, encoding='utf-8')
                        rebuilt = subprocess.run(command, text=True, capture_output=True, timeout=60, env=env)
                        self.assertEqual(rebuilt.returncode, 0, rebuilt.stdout + rebuilt.stderr)
                    port = {'ESP32C3': 'COM4', 'ESP32': 'COM16'}.get(target)
                    snapshot = SUPPORT / f'artifacts/resume-20260922-r12-{port}-write/live-filesystem-before.bin'
                    if port and snapshot.is_file():
                        from littlefs import LittleFS
                        image = snapshot.read_bytes()
                        fs = LittleFS(block_size=4096, block_count=len(image)//4096, mount=False)
                        fs.context.buffer = bytearray(image)
                        fs.mount()
                        try:
                            with fs.open('/config/periph_exec.json', 'rb') as stream:
                                configuration = stream.read()
                        finally:
                            fs.unmount()
                        fixture = Path(directory) / 'live-config.json'
                        fixture.write_bytes(configuration)
                        checked = subprocess.run([str(binary), str(fixture)], text=True,
                            capture_output=True, timeout=15, env=env)
                        self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)
                        print(json.dumps({'port': port, 'rules': len(json.loads(configuration)['rules']),
                            'sha256': hashlib.sha256(configuration).hexdigest(), 'passed': True}))


if __name__ == '__main__':
    unittest.main()


