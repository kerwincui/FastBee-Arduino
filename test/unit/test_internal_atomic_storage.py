"""Real atomic/route/ControlOutput logic with explicit ESP memory-region mocks.

The host SDK substitutes validate placement and OOM behavior; they do not prove
ESP instruction timing. SDK disassembly and the firmware matrix are separate.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SDK = ROOT / 'test/unit/cpp/internal_atomic_mocks'


def function(source, signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


class InternalAtomicStorageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise RuntimeError('C++17 compiler required')
        cls.compiler = compiler
        cls.directory = tempfile.TemporaryDirectory(prefix='fastbee-internal-atomic-')
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binaries = {}
        flags = ['-std=c++17', '-pthread', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                 '-DARDUINO_ARCH_ESP32=1', '-DCONFIG_IDF_TARGET_ESP32S3=1',
                 '-DFASTBEE_ENABLE_MQTT=1', '-DFASTBEE_ENABLE_MODBUS=0',
                 '-DFASTBEE_ENABLE_PERIPH_EXEC=0', '-I'+str(SDK)]
        route = (ROOT/'src/network/handlers/MqttRouteHandler.cpp').read_text(encoding='utf-8')
        methods = []
        for name, boundary in (
            ('handleTestMqttConnection', 'FastBee::MqttClientLifetime::Write testGuard'),
            ('handleGetMqttStatus', '// 检查网络状态'),
            ('handleMqttReconnect', '// 先读取持久化配置'),
            ('handleMqttDisconnect', 'bool absent = false;'),
            ('handleMqttNtpSync', 'bool absent = false;'),
        ):
            body = function(route, 'void MqttRouteHandler::'+name+'(')
            # Actual entry logic through unavailable-storage return. Everything
            # afterwards is explicitly replaced by one observable sentinel.
            methods.append(body[:body.index(boundary)]+'++continued;\n}')
        protocol = (ROOT/'src/protocols/ProtocolManager.cpp').read_text(encoding='utf-8')
        methods.append(function(protocol, 'void ProtocolManager::shutdown()'))
        handle = function(protocol, 'void ProtocolManager::handle()')
        lifecycle = handle.index('if (!mqttLifetime.available()) {')
        # Real loop lifecycle block including its old config-reboot fallback.
        methods.append('void ProtocolManager::handleLifecycleForTest() {\n'+
                       handle[lifecycle:handle.index('\n#endif', lifecycle)]+'\n}')
        for signature in ('bool ProtocolManager::stopMQTT()', 'bool ProtocolManager::finishMqttStop()',
                          'void ProtocolManager::requestMqttRestartAsync()',
                          'FastBee::MqttClientLifetime::Write ProtocolManager::prepareMqttNetworkSwitch()'):
            methods.append(function(protocol, signature))
        for signature, boundary in (
            ('bool ProtocolManager::restartMQTT()', '// MQTT 启用检查'),
            ('bool ProtocolManager::restartMQTTDeferred(', '// 统一入口守卫'),
        ):
            body = function(protocol, signature)
            methods.append(body[:body.index(boundary)]+'++rebuilds; return true;\n}')
        network = (ROOT/'src/network/NetworkManager.cpp').read_text(encoding='utf-8')
        body = function(network, 'bool FBNetworkManager::restartNetwork(bool* deferred)')
        methods.append(body[:body.index('char modeBuf[64];')]+'++mutations; return true;\n}')
        health = (ROOT/'src/systems/HealthMonitor.cpp').read_text(encoding='utf-8')
        for signature in ('bool HealthMonitor::refreshMqttRecoveryAdmission()',
                          'void HealthMonitor::reconcilePendingMemoryRecovery()',
                          'void HealthMonitor::restoreMemoryRecovery()'):
            methods.append(function(health, signature))
        header = (ROOT/'include/protocols/ProtocolManager.h').read_text(encoding='utf-8')
        fixture = (ROOT/'test/unit/cpp/internal_atomic_routes_harness.cpp.in').read_text(encoding='utf-8')
        fixture = fixture.replace('@METHODS@', '\n'.join(methods)).replace(
            '@AVAILABLE@', function(header, 'bool mqttLifetimeAvailable() const'))
        route_cpp = Path(cls.directory.name)/'routes.cpp'
        route_cpp.write_text(fixture, encoding='utf-8')
        for name, sources, extra in (
            ('storage', [ROOT/'test/unit/cpp/internal_atomic_storage_harness.cpp'], []),
            ('routes', [route_cpp], []),
            ('control', [ROOT/'test/unit/cpp/control_output_harness.cpp',
                         ROOT/'src/core/PeripheralControlOutput.cpp'],
             ['-I'+str(ROOT/path) for path in ('test/unit/cpp/control_output_mocks',
              'test/unit/cpp/mcp23017_device_mocks', 'test/mocks')]),
        ):
            binary = Path(cls.directory.name)/(name+'.exe')
            result = subprocess.run([compiler, *flags, *extra, '-I'+str(ROOT/'include'),
                                     *map(str, sources), '-o', str(binary)],
                                    capture_output=True, text=True, timeout=90)
            if result.returncode:
                raise AssertionError(result.stdout+result.stderr)
            cls.binaries[name] = binary

    def check(self, program, mode):
        result = subprocess.run([str(self.binaries[program]), mode],
                                capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)

    def test_zero_target_macros_rejected(self):
        result = subprocess.run([self.compiler, '-std=c++17', '-fsyntax-only',
            '-DARDUINO_ARCH_ESP32=1', '-DCONFIG_IDF_TARGET_ESP32=0',
            '-DCONFIG_IDF_TARGET_ESP32S3=0', '-DCONFIG_IDF_TARGET_ESP32C3=0',
            '-DCONFIG_IDF_TARGET_ESP32C6=0', '-I'+str(SDK), '-I'+str(ROOT/'include'),
            str(ROOT/'test/unit/cpp/internal_atomic_storage_harness.cpp')],
            capture_output=True, text=True, timeout=30)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Non-waiting atomic storage must be reviewed for this ESP target', result.stderr)


for _program, _cases in (
    ('storage', ('inline', 'external', 'threads', 'oom', 'allocator_region')),
    ('routes', ('unavailable', 'available', 'unauthorized', 'shutdown', 'permanent', 'health')),
    ('control', ('external_retirement',)),
):
    for _case in _cases:
        setattr(InternalAtomicStorageTests, 'test_'+_program+'_'+_case,
                lambda self, program=_program, case=_case: self.check(program, case))


if __name__ == '__main__':
    unittest.main()
