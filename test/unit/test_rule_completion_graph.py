"""Actual Manager graph/transaction/file/dispatch paths; no devices or outputs."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from test_periph_exec_config_persistence import cpp_function

ROOT = Path(__file__).resolve().parents[2]
SUPPORT = Path(os.environ.get('FASTBEE_TEST_SUPPORT_ROOT', ROOT))


def production_program():
    manager = (ROOT / 'src/core/PeriphExecManager.cpp').read_text(encoding='utf-8')
    store = (ROOT / 'src/core/PeriphExecConfigStore.cpp').read_text(encoding='utf-8')
    queries = (ROOT / 'src/core/PeriphExecRuleQueries.cpp').read_text(encoding='utf-8')
    header = (ROOT / 'include/core/PeriphExecManager.h').read_text(encoding='utf-8')
    source = manager + '\n' + store + '\n' + queries
    constants = '\n'.join(line for line in manager.splitlines() if line.startswith('constexpr ') and
                          'PERIPH_EXEC_RULE_MUTATION_LOCK_TIMEOUT_MS' not in line)
    helpers = constants + '\n' + store[store.index('namespace {') + len('namespace {'):store.index('} // namespace')]
    helpers += '\n' + cpp_function((ROOT / 'include/core/PeriphExecRemoteBindings.h').read_text(encoding='utf-8'),
                                   'bool hasRetiredRemoteSource(')
    helpers += '\n' + cpp_function((SUPPORT / 'src/utils/FileUtils.cpp').read_text(encoding='utf-8'),
                                   'bool FileUtils::commitAtomicWrite(')
    for name in ('triggersMatch', 'actionsMatch', 'rulesMatch'):
        helpers += '\n' + cpp_function(manager, 'static bool ' + name + '(')
    methods = [cpp_function(source, 'bool PeriphExecManager::' + name + '(') for name in (
        'saveConfiguration', 'saveConfigurationLocked', 'writeConfigurationLocked',
        'validateConfigurationFile', 'reviewConfigurationFile', 'importConfigurationFile', 'loadConfiguration',
        'copyIrKeysJson', 'setIrKeysJson', 'removeRuleLocked', 'addRuleLocked', 'updateRuleLocked', 'setRuleEnabledLocked',
        'sanitizeRuleForSafety', 'sanitizeTriggerForSafety', 'addRuleAndSave', 'updateRuleAndSave',
        'removeRuleAndSave', 'enableRuleAndSave', 'disableRuleAndSave', 'applyRuleMutationTransactional',
        'restoreRuleMutationLocked')]
    methods.append(cpp_function(source, 'void PeriphExecManager::dispatchPeriphExecEvent('))
    methods.append(cpp_function(source, 'void PeriphExecManager::dispatchEventMatchedRules('))
    legacy = (ROOT / 'test/unit/cpp/action21_event_matcher_before.inc').read_text(encoding='utf-8')
    methods.append(legacy.replace('dispatchEventMatchedRules(', 'dispatchEventMatchedRulesLegacy(', 1))
    methods.append(cpp_function(header, 'const char* configurationWriteError()').replace(
        'configurationWriteError()', 'PeriphExecManager::configurationWriteError()'))
    fixture = (ROOT / 'test/unit/cpp/periph_exec_config_harness.cpp').read_text(encoding='utf-8').split('int main(')[0]
    fixture = fixture.replace('#include <Arduino.h>', '#include <Arduino.h>\n#include \"network/LiteralStringMatch.h\"', 1)
    # This fixture enables AJ's actual Arduino String adapter. The generic
    # config harness fallback would otherwise create an ambiguous ADL overload.
    fixture = fixture.replace('inline void convertFromJson(JsonVariantConst value, String& output) { output = value.as<std::string>(); }', '')
    members = (ROOT / 'test/unit/cpp/rule_completion_graph_members.inc').read_text(encoding='utf-8')
    members = members.replace('// PRODUCTION_MUTATION_TYPES',
        cpp_function(header, 'enum class RuleMutationType') + ';\n' + cpp_function(header, 'struct RuleMutationSnapshot') + ';')
    fixture = fixture.replace('static int failAfter = -1;',
        'static int failAfter = -1;\nstatic void (*graphAllocationHook)() = nullptr;')
    fixture = fixture.replace('    return p;',
        '    if (graphAllocationHook) graphAllocationHook();\n    return p;', 1)
    fixture = fixture.replace('void operator delete(void* p) noexcept',
        'void* operator new[](size_t n) { return ::operator new(n); }\n'
        'void* operator new[](size_t n, const std::nothrow_t&) noexcept {\n'
        '    try { return ::operator new[](n); } catch (...) { return nullptr; }\n'
        '}\n'
        'void operator delete[](void* p, const std::nothrow_t&) noexcept { free(p); }\n'
        'void operator delete[](void* p) noexcept { free(p); }\n'
        'void operator delete[](void* p, size_t) noexcept { free(p); }\n'
        'void operator delete(void* p) noexcept')
    fixture = fixture.replace('    String generateUniqueId() const', members + '\n    String generateUniqueId() const')
    fixture = fixture.replace('// PRODUCTION_HELPERS', helpers)
    fixture = fixture.replace('// PRODUCTION_METHODS', '\n'.join(methods))
    # Only the external UART observation check is mocked; actual dispatch matching,
    # mutex/cycle/cooldown/revision forwarding remain the production function.
    peripheral = '''
#include "core/UartEventOrigin.h"
struct PeripheralManager {
    static inline bool current = true;
    static PeripheralManager& getInstance() { static PeripheralManager value; return value; }
    bool isUartEventOriginCurrent(const FastBee::UartEventOrigin&) const { return current; }
};
'''
    fixture = fixture.replace('// PRODUCTION_HELPERS', '')
    fixture = fixture.replace('class PeriphExecManager {', peripheral + '\nclass PeriphExecManager {')
    cases = (ROOT / 'test/unit/cpp/rule_completion_graph_cases.inc').read_text(encoding='utf-8')
    extra = (ROOT / 'test/unit/cpp/action21_event_graph_cases.inc').read_text(encoding='utf-8')
    executor = (ROOT / 'src/core/PeriphExecExecutor.cpp').read_text(encoding='utf-8')
    branch = cpp_function(executor, '} else if (action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_TRIGGER_EVENT))')
    branch = branch[branch.index('{') + 1:branch.rfind('}')]
    extra = extra.replace('// ACTUAL_ACTION21_BODY', branch)
    cases = cases.replace('int main() {', extra + '\nint main() {')
    cases = cases.replace('exerciseGraphAndMutations(); exerciseFiles(); exerciseDispatch();',
        'exerciseGraphAndMutations(); exerciseFiles(); exerciseDispatch(); exerciseAction21Graph();')
    return fixture + cases


class RuleCompletionGraphTests(unittest.TestCase):
    def test_actual_config_transaction_and_completion_paths(self):
        program = production_program()
        compiler = shutil.which('g++') or 'D:/msys64/ucrt64/bin/g++.exe'
        includes = [ROOT / 'test/unit/fixtures/periph_exec_config', SUPPORT / 'test/mocks',
                    Path(os.environ.get('PLATFORMIO_LIBDEPS_DIR',str(SUPPORT/'.pio/libdeps'))) / 'native/ArduinoJson/src', ROOT / 'include', SUPPORT / 'include']
        environment = dict(os.environ)
        environment['PATH'] = str(Path(compiler).parent) + os.pathsep + environment.get('PATH', '')
        with tempfile.TemporaryDirectory(prefix='fastbee-completion-graph-') as directory:
            arduino = (SUPPORT / 'test/mocks/Arduino.h').read_text(encoding='utf-8').replace('void clear() { _value.clear(); }',
                'void clear() { _value.clear(); }\n    bool concat(const char* p) { _value += p ? p : ""; return true; }')
            (Path(directory) / 'Arduino.h').write_text(arduino, encoding='utf-8')
            cpp = Path(directory) / 'completion.cpp'
            binary = Path(directory) / 'completion.exe'
            targets = os.environ.get('FASTBEE_CONFIG_TEST_TARGETS', 'ESP32C3,ESP32,ESP32S3,ESP32C6').split(',')
            for target in targets:
                with self.subTest(target=target):
                    cpp.write_text(program, encoding='utf-8')
                    command = [compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-const-variable', '-Wno-unused-function', '-Wno-error=deprecated-declarations',
                        '-DARDUINOJSON_ENABLE_ARDUINO_STRING=1', '-DCONFIG_IDF_TARGET_' + target,
                        '-I' + directory, *['-I' + str(p) for p in includes], str(cpp),
                        str(ROOT / 'src/network/LiteralStringMatch.cpp'),
                        str(ROOT / 'src/network/FixedJsonField.cpp'),
                        str(ROOT / 'src/utils/SharedJsonSerializer.cpp'),
                        str(ROOT / 'src/utils/SharedJsonDeserializer.cpp'), '-o', str(binary)]
                    built = subprocess.run(command, text=True, encoding='utf-8', errors='replace', capture_output=True, timeout=90, env=environment)
                    self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
                    result = subprocess.run([str(binary)], text=True, encoding='utf-8', errors='replace', capture_output=True, timeout=20, env=environment)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    print(target + ': ' + result.stdout.strip())
                    if target != targets[0]:
                        continue
                    mutations = {
                        'add admission': ('if (const char* issue = FastBee::validateRuleCompletionGraph(rules, &newRule))',
                                          'if (const char* issue = static_cast<const char*>(nullptr))'),
                        'import commit': ('if (const char* issue = importedIds.completion.error())',
                                          'if (const char* issue = static_cast<const char*>(nullptr))'),
                        'legacy publish': ('if (const char* issue = FastBee::validateRuleCompletionGraph(candidate))',
                                           'if (const char* issue = static_cast<const char*>(nullptr))'),
                        'event fanout': ('if (const char* eventIssue = FastBee::validateRuleCompletionGraph(rules))',
                                          'if (const char* eventIssue = static_cast<const char*>(nullptr))'),
                        'emission allocation admission': ('if (!configMemoryAvailable(bytes, bytes)) return nullptr;',
                                                          'if (false) return nullptr;'),
                        'emission allocation reserve': ('if (!copy || !configMemoryAvailable(0, 0)) return nullptr;',
                                                        'if (!copy) return nullptr;'),
                        'completion fanout': ('if (const char* issue = FastBee::validateRuleCompletionGraph(rules))',
                                              'if (const char* issue = static_cast<const char*>(nullptr))')
                    }
                    for label, (before, after) in mutations.items():
                        self.assertEqual(program.count(before), 1, label)
                        cpp.write_text(program.replace(before, after, 1), encoding='utf-8')
                        changed = subprocess.run(command, text=True, encoding='utf-8', errors='replace', capture_output=True, timeout=90, env=environment)
                        self.assertEqual(changed.returncode, 0, label + changed.stdout + changed.stderr)
                        rejected = subprocess.run([str(binary)], text=True, encoding='utf-8', errors='replace', capture_output=True, timeout=20, env=environment)
                        self.assertEqual(rejected.returncode, 86, label + rejected.stdout + rejected.stderr)
                        print('removed ' + label + ': actual behavior regression rejected mutant')
