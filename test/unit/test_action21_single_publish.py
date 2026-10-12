"""Actual action21 branch, Manager/Scheduler routing and offline MQTT callback sink."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from test_periph_exec_config_persistence import cpp_function

ROOT = Path(__file__).resolve().parents[2]
SUPPORT = Path(os.environ.get('FASTBEE_TEST_SUPPORT_ROOT', ROOT))


def program():
    manager = (ROOT / 'src/core/PeriphExecManager.cpp').read_text(encoding='utf-8')
    scheduler = (ROOT / 'src/core/PeriphExecScheduler.cpp').read_text(encoding='utf-8')
    executor = (ROOT / 'src/core/PeriphExecExecutor.cpp').read_text(encoding='utf-8')
    methods = '\n'.join(cpp_function(text, marker) for text, marker in (
        (manager, 'void PeriphExecManager::triggerEvent('),
        (manager, 'void PeriphExecManager::triggerEventById('),
        (manager, 'bool PeriphExecManager::notifyMqttEventPublish('),
        (scheduler, 'void PeriphExecScheduler::triggerEvent('),
        (scheduler, 'void PeriphExecScheduler::triggerEventById('),
        (scheduler, 'void PeriphExecScheduler::triggerPeriphExecEvent('),
    ))
    branch = cpp_function(executor, '} else if (action.actionType == static_cast<uint8_t>(ExecActionType::ACTION_TRIGGER_EVENT))')
    branch = branch[branch.index('{') + 1:branch.rfind('}')]
    fixture = (ROOT / 'test/unit/cpp/action21_single_publish_harness.cpp.in').read_text(encoding='utf-8')
    frozen = (ROOT / 'test/unit/cpp/action21_executor_before.inc').read_text(encoding='utf-8')
    return fixture.replace('// ACTUAL_ROUTING_METHODS', methods).replace(
        '// ACTUAL_ACTION21_BODY', branch).replace('// FROZEN_ACTION21_BODY', frozen)


class Action21SinglePublishTests(unittest.TestCase):
    def test_actual_executor_to_scheduler_to_mqtt_sink_and_mutants(self):
        source = program()
        compiler = shutil.which('g++') or 'D:/msys64/ucrt64/bin/g++.exe'
        environment = dict(os.environ)
        environment['PATH'] = str(Path(compiler).parent) + os.pathsep + environment.get('PATH', '')
        with tempfile.TemporaryDirectory(prefix='fastbee-action21-publish-') as directory:
            cpp = Path(directory) / 'action21.cpp'
            binary = Path(directory) / 'action21.exe'
            command = [compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                '-I' + str(SUPPORT / 'test/mocks'), '-I' + str(ROOT / 'include'),
                str(cpp), str(ROOT / 'src/core/PeripheralExecution.cpp'), '-o', str(binary)]
            def run(text, expected, label):
                cpp.write_text(text, encoding='utf-8')
                built = subprocess.run(command, capture_output=True, text=True, timeout=60, env=environment)
                self.assertEqual(built.returncode, 0, label + built.stdout + built.stderr)
                tested = subprocess.run([str(binary)], capture_output=True, text=True, timeout=15, env=environment)
                self.assertEqual(tested.returncode, expected, label + tested.stdout + tested.stderr)
                print(label + ': ' + (tested.stdout.strip() or 'behavior assertion rejected mutant'))
            run(source, 0, 'action21 publication matrix')
            mutations = {
                'allow legacy dynamic event target': (
                    'if (action.targetPeriphId.isEmpty() && action.useReceivedValue) {',
                    'if (false && action.targetPeriphId.isEmpty() && action.useReceivedValue) {'),
                'restore implicit action21 publish': (
                    'triggerEventById(triggerEventId, effectiveValue, eventOrigin, false)',
                    'triggerEventById(triggerEventId, effectiveValue, eventOrigin, true)'),
                'ignore report suppression': (
                    'if (rule.reportAfterExec && !suppressReport) {',
                    'if ((static_cast<void>(suppressReport), rule.reportAfterExec)) {'),
                'ignore notifier source retirement': (
                    'if (!PeripheralManager::getInstance().isUartEventOriginCurrent(uartOrigin)) return false;',
                    '(void)uartOrigin; // mutant bypasses callback provenance'),
            }
            for label, (before, after) in mutations.items():
                # The frozen Executor branch intentionally contains the old report
                # condition too. Mutate the first/current function only.
                self.assertIn(before, source, label)
                run(source.replace(before, after, 1), 86, label)


if __name__ == '__main__':
    unittest.main()
