"""Prepared lifetime/admission tests. No MQTT connection, firmware build or device I/O."""
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
                return source[start:index+1]
    raise AssertionError('Unterminated function: '+signature)


class MqttClientLeaseTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise RuntimeError('C++17 compiler required')
        cls.directory = tempfile.TemporaryDirectory(prefix='fastbee-mqtt-lease-')
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = Path(cls.directory.name)/'lease.exe'
        source = (ROOT/'src/protocols/ProtocolManager.cpp').read_text(encoding='utf-8')
        methods = '\n'.join(function(source, signature) for signature in (
            'FastBee::MqttClientLease ProtocolManager::acquireMQTTClient(',
            'FastBee::MqttClientLifetime::Write ProtocolManager::prepareMqttNetworkSwitch()',
            'bool ProtocolManager::stopMQTT()', 'bool ProtocolManager::finishMqttStop()',
            'void ProtocolManager::requestMqttRestartAsync()'))
        # Execute the real admission prefix. Replace config/TLS construction
        # only after successful admission; no claim to test the rebuild body.
        for signature, boundary in (
            ('bool ProtocolManager::restartMQTT()', '// MQTT 启用检查'),
            ('bool ProtocolManager::restartMQTTDeferred(bool forceRebuild,', '// 统一入口守卫')):
            start = source.index(signature)
            prefix = source[start:source.index(boundary, start)]
            outcome = 'if (result) { *result = FastBee::MqttRestartResult::Ready; }\n' if 'Deferred' in signature else ''
            methods += '\n'+prefix+outcome+'++rebuilds; return true;\n}'
        route = (ROOT/'src/network/handlers/MqttRouteHandler.cpp').read_text(encoding='utf-8')
        methods += '\n'+function(route, 'static bool tryAutoStartMqttForStatus(')
        network = (ROOT/'src/network/NetworkManager.cpp').read_text(encoding='utf-8')
        start = network.index('bool FBNetworkManager::restartNetwork(bool* deferred)')
        # Real admission/defer path; adapter operations are an explicit fake.
        methods += '\n'+network[start:network.index('char modeBuf[64];', start)]
        methods += '++mutations; if (onAdapter) onAdapter(); return adapterOk;\n}'
        template = (ROOT/'test/unit/cpp/mqtt_client_lease_harness.cpp.in').read_text(encoding='utf-8')
        fixture = Path(cls.directory.name)/'lease.cpp'
        fixture.write_text(template.replace('@METHODS@', methods), encoding='utf-8')
        result = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
            '-Wno-unused-parameter', '-pthread', '-I'+str(ROOT/'include'), str(fixture),
            '-o', str(cls.binary)], capture_output=True, text=True, timeout=90)
        if result.returncode:
            raise AssertionError(result.stdout+result.stderr)

    def check(self, case):
        result = subprocess.run([str(self.binary), case], capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)


for _case in ('absent', 'available', 'writer', 'readers', 'move', 'move_assign', 'self_move',
              'pending_restart', 'pending_stop', 'shutdown', 'restart_busy', 'deferred_busy',
              'stop_busy', 'stop_then_restart', 'thread_drain', 'writer_exclusion', 'exceptions',
              'status_busy', 'status_absent', 'status_release', 'status_active',
              'status_memory_stop', 'status_rebuild_contention', 'direct_pending_restart',
              'deferred_outcome', 'stop_outcome', 'shutdown_outcome',
              'write_default', 'write_move', 'write_move_assign',
              'transport_busy', 'transport_ready', 'transport_stopped',
              'transport_stop_pending', 'transport_shutdown', 'transport_absent',
              'network_no_callback', 'network_mqtt_busy', 'network_reentry',
              'network_failure_release', 'network_thread_busy',
              'network_production_defer', 'network_production_failure',
              'bound_report_rebuild', 'bound_report_lease', 'worker_stop_pending',
              'worker_network_pending', 'status_cancelled'):
    setattr(MqttClientLeaseTests, 'test_'+_case, lambda self, case=_case:self.check(case))


class MqttClientLeaseIntegrationTests(unittest.TestCase):
    def test_retry_compares_binding_and_sends_under_one_lease(self):
        source = (ROOT/'src/core/FastBeeFramework.cpp').read_text(encoding='utf-8')
        callback = source[source.index('pem.setMqttQueueReportCallback('):].split('});', 1)[0]
        self.assertIn('uint32_t instanceId', callback)
        self.assertLess(callback.index('acquireMQTTClient()'), callback.index('replayBoundReport(*mqtt.get(), instanceId'))
        self.assertNotIn('mqtt = {}', callback)

    def test_report_producers_never_rebind_unknown_origin(self):
        for name in ('PeriphExecExecutor.cpp', 'PeriphExecScheduler.cpp'):
            source = (ROOT/'src/core'/name).read_text(encoding='utf-8')
            self.assertIn('FastBee::captureReportInstance(', source)
            self.assertIn('FastBee::submitBoundReport(mqtt.get(), ', source)
            self.assertNotIn('mqtt ? mqtt->getReportInstanceId() : 0', source)
        source = (ROOT/'src/core/PeriphExecManager.cpp').read_text(encoding='utf-8')
        self.assertIn('_mqttQueueReportCb(report, instanceId)', source)
        header = (ROOT/'include/protocols/MQTTClient.h').read_text(encoding='utf-8')
        self.assertIn('const uint32_t _reportInstanceId = FastBee::nextReportInstanceId()', header)

    def test_rule_and_boot_bind_before_collection(self):
        source = (ROOT/'src/core/PeriphExecExecutor.cpp').read_text(encoding='utf-8')
        rule = function(source, 'std::vector<ActionExecResult> PeriphExecExecutor::executeAllActions(')
        boot = function(source, 'size_t PeriphExecExecutor::reportRuleCurrentState(')
        for body in (rule, boot):
            self.assertLess(body.index('captureReportInstanceId()'), body.index('for ('))
            self.assertIn('reportActionResults(reportableResults, reportOrigin)', body)
        report = function(source, 'void PeriphExecExecutor::reportActionResults(')
        self.assertNotIn('getReportInstanceId()', report)
        self.assertIn('submitBoundReport(mqtt.get(), originInstanceId, payload', report)
        self.assertIn('queuePendingReport(value, origin, current)', report)

    def test_digital_change_binds_before_rule_callbacks(self):
        source = (ROOT/'src/core/PeriphExecScheduler.cpp').read_text(encoding='utf-8')
        body = function(source, 'void PeriphExecScheduler::handleStableDigitalInputChange(')
        self.assertLess(body.index('captureReportInstance('), body.index('handlePollData('))
        self.assertLess(body.index('handlePollData('), body.index('auto mqtt = getMqttClient('))
        self.assertIn('submitBoundReport(mqtt.get(), reportOrigin, reportPayload', body)
        self.assertIn('queuePendingReport(value, origin, current)', body)
        self.assertLess(body.index('reportPayload.length() != payloadLen'), body.index('submitBoundReport('))
        self.assertNotIn('getReportInstanceId()', body)

    def test_network_admission_covers_all_restart_branches(self):
        source = (ROOT/'src/network/NetworkManager.cpp').read_text(encoding='utf-8')
        body = function(source, 'bool FBNetworkManager::restartNetwork(bool* deferred)')
        self.assertLess(body.index('NetworkTransitionGuard transition'), body.index('setModeTransitioning(true)'))
        self.assertLess(body.index('if (!transition)'), body.index('NetworkMode newMode'))
        self.assertIn('pendingRestart = true', body)
        self.assertIn('pendingRestartTime = millis()', body)
        self.assertNotIn('_preNetworkSwitchCb()', body)
        self.assertIn('return restartNetwork(nullptr)', function(source, 'bool FBNetworkManager::restartNetwork()'))
        header = (ROOT/'include/network/NetworkManager.h').read_text(encoding='utf-8')
        self.assertIn('bool restartNetwork() override;', header)

    def test_deferred_network_retry_preserves_connection_budget(self):
        source = (ROOT/'src/network/NetworkManager.cpp').read_text(encoding='utf-8')
        body = function(source, 'void FBNetworkManager::update()')
        for counter in ('ethReconnectAttempts', 'cellReconnectAttempts'):
            position = body.index('--'+counter)
            context = body[position-90:position+150]
            self.assertIn('if (deferred)', context)
            self.assertIn('return;', context)

    def test_framework_binds_scoped_admission_and_clears_owner_callback(self):
        source = (ROOT/'src/core/FastBeeFramework.cpp').read_text(encoding='utf-8')
        callback = source[source.index('netMgr->setPreNetworkSwitchCallback('):].split('});', 1)[0]
        self.assertIn('return pm->prepareMqttNetworkSwitch()', callback)
        shutdown = function(source, 'void FastBeeFramework::shutdown()')
        self.assertLess(shutdown.index('protocolManager->shutdown()'),
                        shutdown.index('network->setPreNetworkSwitchCallback({})'))
        self.assertLess(shutdown.index('network->setPreNetworkSwitchCallback({})'),
                        shutdown.index('network->disconnect()'))

    def test_replacement_functions_enter_writer_before_client_access(self):
        source = (ROOT/'src/protocols/ProtocolManager.cpp').read_text(encoding='utf-8')
        for signature in ('bool ProtocolManager::restartMQTT()',
                          'bool ProtocolManager::restartMQTTDeferred(',
                          'bool ProtocolManager::initMQTT('):
            with self.subTest(signature=signature):
                body = function(source, signature)
                self.assertLess(body.index('Write lifecycle(mqttLifetime)'), body.index('mqttClient'))
        shutdown = function(source, 'void ProtocolManager::shutdown()')
        self.assertLess(shutdown.index('mqttShutdownPending = true'), shutdown.index('mqttShutdown.tryAcquire()'))
        self.assertLess(shutdown.index('mqttShutdown.tryAcquire()'), shutdown.index('mqttClient.reset()'))

    def test_report_consumers_no_longer_return_unowned_pointer(self):
        for filename in ('PeriphExecExecutor.cpp', 'PeriphExecScheduler.cpp'):
            source = (ROOT/'src/core'/filename).read_text(encoding='utf-8')
            self.assertNotIn('getMQTTClient()', source)
            self.assertIn('mqttAbsent', source)
        source = (ROOT/'src/core/FastBeeFramework.cpp').read_text(encoding='utf-8')
        for marker in ('pem.setMqttIsConnectedCallback(', 'pem.setMqttQueueReportCallback(',
                       'pem.setMqttEventPublishCallback('):
            callback = source[source.index(marker):].split('});', 1)[0]
            self.assertIn('acquireMQTTClient()', callback)
            self.assertNotIn('getMQTTClient()', callback)

    def test_busy_restart_cannot_trigger_reboot_fallback(self):
        source = (ROOT/'src/protocols/ProtocolManager.cpp').read_text(encoding='utf-8')
        self.assertIn('!restartMQTTDeferred(true) && !mqttRestartPending && !mqttStopPending', source)
        self.assertIn('if (mqttStopPending) finishMqttStop();', source)

    def test_status_holds_lease_and_reports_busy_as_unknown(self):
        source = (ROOT/'src/network/handlers/MqttRouteHandler.cpp').read_text(encoding='utf-8')
        body = function(source, 'void MqttRouteHandler::handleGetMqttStatus(')
        self.assertNotIn('getMQTTClient()', body)
        self.assertIn('mqttLease = pm->acquireMQTTClient(&mqttAbsent)', body)
        self.assertIn('FastBee::setFixedJsonField(data, "stateAvailable", !lifecycleBusy)', body)
        self.assertIn('writeMqttDiagnostic(data, MqttDiagnostic::LifecycleBusy', body)
        writer = (ROOT/'src/protocols/MqttDiagnosticJson.cpp').read_text(encoding='utf-8')
        self.assertIn('{0, "MQTT_LIFECYCLE_BUSY", "pending",', writer)
        self.assertIn('autoStartAttempted && !autoStartStarted && !lifecycleBusy', body)

    def test_all_production_callers_use_scoped_admission(self):
        for base in ('src', 'include'):
            for filename in (ROOT/base).rglob('*'):
                if filename.suffix in ('.cpp', '.h'):
                    with self.subTest(file=filename.relative_to(ROOT)):
                        self.assertNotIn('getMQTTClient(', filename.read_text(encoding='utf-8'))

    def test_http_stop_does_not_claim_completion_while_deferred(self):
        source = (ROOT/'src/network/handlers/MqttRouteHandler.cpp').read_text(encoding='utf-8')
        body = function(source, 'void MqttRouteHandler::handleMqttDisconnect(')
        self.assertLess(body.index('mqtt = {}'), body.index('pm->stopMQTT()'))
        self.assertIn('FastBee::setFixedJsonField(resp, "data", "disconnected", stopped);', body)
        self.assertIn('FastBee::setFixedJsonField(resp, "data", "lifecyclePending", !stopped);', body)


if __name__ == '__main__':
    unittest.main()
