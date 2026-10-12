"""Prepared transport-owner regressions; real methods/guard, fake sockets and RTOS."""
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


class MqttTransportOwnerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise RuntimeError('C++17 compiler required')
        cls.inputs = [ROOT / path for path in (
            'src/protocols/MQTTClient.cpp', 'include/core/AsyncExecTypes.h',
            'include/protocols/MqttReconnectLifetime.h',
            'test/unit/cpp/mqtt_transport_owner_harness.cpp.in',
            'test/unit/test_mqtt_transport_owner.py')]
        cls.hashes = [hashlib.sha256(p.read_bytes()).hexdigest() for p in cls.inputs]
        cls.directory = tempfile.TemporaryDirectory(prefix='fastbee-mqtt-owner-')
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = Path(cls.directory.name) / 'owner.exe'
        source = cls.inputs[0].read_text(encoding='utf-8')
        methods = '\n'.join(function(source, signature) for signature in (
            'bool MQTTClient::disconnect()', 'bool MQTTClient::stop()',
            'bool MQTTClient::subscribeExact(', 'bool MQTTClient::unsubscribeExact(',
            'bool MQTTClient::publishExact(', 'void MQTTClient::yieldToWeb()',
            'bool MQTTClient::setTransportClient(', 'bool MQTTClient::publishMonitorData()',
            'void MQTTClient::setMessageCallback(', 'void MQTTClient::setStatusChangeCallback(',
    'void MQTTClient::setMonitorDataProvider('))
        guard = function(cls.inputs[1].read_text(encoding='utf-8'), 'class RecursiveMutexGuard {') + ';'
        template = cls.inputs[3].read_text(encoding='utf-8').replace('@GUARD@', guard).replace('@METHODS@', methods)
        fixture = Path(cls.directory.name) / 'owner.cpp'
        fixture.write_text(template, encoding='utf-8')
        result = subprocess.run([compiler, '-std=c++17', '-pthread', '-Wall', '-Wextra', '-Werror',
            '-I' + str(ROOT / 'include'), str(fixture), '-o', str(cls.binary)],
            capture_output=True, text=True, timeout=90)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    @classmethod
    def tearDownClass(cls):
        if cls.hashes != [hashlib.sha256(p.read_bytes()).hexdigest() for p in cls.inputs]:
            raise AssertionError('Transport owner inputs changed during execution')

    def check(self, case):
        result = subprocess.run([str(self.binary), case], capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


for _case in ('contention', 'recursive', 'null_mutex', 'stopped', 'test_isolation',
              'disconnect', 'stop_busy', 'stop_worker', 'transport_frozen', 'callbacks_frozen',
              'yield_busy', 'yield_worker', 'yield_healthy', 'monitor_missing', 'monitor_sample'):
    setattr(MqttTransportOwnerTests, 'test_' + _case, lambda self, case=_case: self.check(case))


class OwnerIntegrationTests(unittest.TestCase):
    def test_loop_and_reconnect_claim_owner_before_transport(self):
        source = (ROOT / 'src/protocols/MQTTClient.cpp').read_text(encoding='utf-8')
        for signature, mutation in (
            ('void MQTTClient::handle()', 'bool activeNetworkIsWifi'),
            ('bool MQTTClient::reconnect()', 'ntpConnectionEpoch.fetch_add'),
            ('void MQTTClient::doReconnect()', '_reconnectPending = false'),
            ('bool MQTTClient::begin(', '_beginAttempted.exchange'),
            ('bool MQTTClient::connect()', '_reconnectLifetime.reopen'),
            ('bool MQTTClient::subscribe(', 'mqttClient.subscribe'),
            ('bool MQTTClient::subscribeAll()', 'mqttClient.subscribe')):
            body = function(source, signature)
            self.assertLess(body.index('RecursiveMutexGuard lock(_publishMutex, 0)'), body.index(mutation))
            self.assertLess(body.index('lock.isLocked()'), body.index(mutation))

    def test_transport_configuration_failure_propagates(self):
        source = (ROOT / 'src/protocols/ProtocolManager.cpp').read_text(encoding='utf-8')
        body = function(source, 'static bool configureMqttTransport(')
        self.assertEqual(body.count('return mqttClient->setTransportClient('), 4)


if __name__ == '__main__':
    unittest.main()
