"""Prepared worker retirement regressions; RTOS/socket operations are explicit fakes."""
import hashlib
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def function(source, signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == '{':
            depth += 1
        elif source[index] == '}':
            depth -= 1
            if not depth:
                return source[start:index + 1]
    raise AssertionError(signature)


class ReconnectLifetimeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise RuntimeError('C++17 compiler required')
        cls.inputs = [ROOT / p for p in (
            'include/protocols/MqttReconnectLifetime.h', 'src/protocols/MQTTClient.cpp',
            'test/unit/cpp/mqtt_reconnect_lifetime_harness.cpp.in',
            'test/unit/test_mqtt_reconnect_lifetime.py', 'include/core/AsyncExecTypes.h')]
        cls.hashes = [hashlib.sha256(p.read_bytes()).hexdigest() for p in cls.inputs]
        cls.directory = tempfile.TemporaryDirectory(prefix='fastbee-mqtt-worker-')
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = Path(cls.directory.name) / 'worker.exe'
        source = cls.inputs[1].read_text(encoding='utf-8')
        methods = '\n'.join(function(source, signature) for signature in (
            'void MQTTClient::shutdown()', 'bool MQTTClient::stop()',
            'bool MQTTClient::ensureReconnectTask()',
    'void MQTTClient::recoverStuckReconnect(', 'void MQTTClient::reconnectTaskEntry('))
        template = cls.inputs[2].read_text(encoding='utf-8')
        guards = cls.inputs[4].read_text(encoding='utf-8')
        guard = guards[guards.index('class RecursiveMutexGuard {'):]
        template = template.replace('@GUARD@', guard[:guard.index('\n};') + 3])
        fixture = Path(cls.directory.name) / 'worker.cpp'
        fixture.write_text(template.replace('@METHODS@', methods), encoding='utf-8')
        result = subprocess.run([compiler, '-std=c++17', '-pthread', '-Wall', '-Wextra',
            '-Werror', '-Wno-unused-parameter', '-I' + str(ROOT / 'include'), str(fixture),
            '-o', str(cls.binary)], capture_output=True, text=True, timeout=90)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    @classmethod
    def tearDownClass(cls):
        if cls.hashes != [hashlib.sha256(p.read_bytes()).hexdigest() for p in cls.inputs]:
            raise AssertionError('Worker test inputs changed during execution')

    def check(self, case):
        result = subprocess.run([str(self.binary), case], capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


for _case in ('gate', 'contended_reserve', 'create_failure', 'early_exit', 'stop_idle', 'owner_busy_stop', 'owner_busy_cleanup',
              'cancel_startup', 'terminal_drain', 'running_stop', 'watchdog'):
    setattr(ReconnectLifetimeTests, 'test_' + _case, lambda self, case=_case: self.check(case))


class ReconnectIntegrationTests(unittest.TestCase):
    def test_no_foreign_task_delete_and_ack_is_last_object_access(self):
        source = (ROOT / 'src/protocols/MQTTClient.cpp').read_text(encoding='utf-8')
        self.assertEqual(source.count('vTaskDelete('), 1)
        entry = function(source, 'void MQTTClient::reconnectTaskEntry(')
        tail = entry.split('client->_reconnectLifetime.finish();', 1)[1]
        self.assertNotIn('client->', tail)
        self.assertIn('vTaskDelete(nullptr)', tail)

    def test_handshake_cancellation_precedes_connected_effects(self):
        source = (ROOT / 'src/protocols/MQTTClient.cpp').read_text(encoding='utf-8')
        reconnect = function(source, 'bool MQTTClient::reconnect()')
        after_connect = reconnect[reconnect.rindex('ok = mqttClient.connect('):]
        self.assertLess(after_connect.index('_reconnectLifetime.stopping()'),
                        after_connect.index('isConnected = true'))
        handle = function(source, 'void MQTTClient::handle()')
        self.assertLess(handle.index('_reconnectLifetime.active()'), handle.index('bool activeNetworkIsWifi'))


if __name__ == '__main__':
    unittest.main()
