"""Production resource graph checks with network and peripheral candidates."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[2]
class NetworkResourcePreflightTests(unittest.TestCase):
    def compile_graph(self, harness):
        compiler=shutil.which('g++') or shutil.which('clang++')
        if not compiler: self.skipTest('C++17 compiler required')
        with tempfile.TemporaryDirectory(prefix='fastbee-network-preflight-') as folder:
            binary=Path(folder)/('preflight.exe' if os.name=='nt' else 'preflight')
            command=[compiler,'-std=c++17','-DUNIT_TEST','-DARDUINOJSON_ENABLE_STD_STRING=1',
                     '-DARDUINOJSON_ENABLE_ARDUINO_STRING=0',
                     '-I'+str(ROOT/'test/unit/cpp/mocks'),'-I'+str(ROOT/'test/mocks'),
                     '-I'+str(ROOT/'include'),str(ROOT/'test/unit/cpp'/harness),
                     str(ROOT/'src/core/BoardProfile.cpp'),str(ROOT/'src/core/PeripheralResourcePlanner.cpp'),
                     str(ROOT/'src/core/PeripheralNetworkResources.cpp'),'-o',str(binary)]
            result=subprocess.run(command,capture_output=True,text=True,timeout=90)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            result=subprocess.run([str(binary)],capture_output=True,text=True,timeout=15)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)

    def test_network_resource_graph_and_context_lifetime(self):
        self.compile_graph('network_resource_preflight_harness.cpp')

    def test_cellular_gpio_uart_and_dormant_graph_on_all_profiles(self):
        self.compile_graph('cellular_resource_preflight_harness.cpp')

    def compile_guard(self, harness, actual_json=False):
        compiler=shutil.which('g++') or shutil.which('clang++')
        if not compiler: self.skipTest('C++17 compiler required')
        with tempfile.TemporaryDirectory(prefix='fastbee-network-guard-') as folder:
            binary=Path(folder)/('guard.exe' if os.name=='nt' else 'guard')
            command=[compiler,'-std=c++17','-DUNIT_TEST','-DCONFIG_IDF_TARGET_ESP32S3=1',
                     '-DARDUINOJSON_ENABLE_STD_STRING=1','-DARDUINOJSON_ENABLE_ARDUINO_STRING=0']
            if actual_json:
                json_root=os.environ.get('FASTBEE_TEST_ARDUINOJSON_INCLUDE')
                if not json_root:
                    installed=list((ROOT/'.pio/libdeps').glob('*/ArduinoJson/src/ArduinoJson.h'))
                    if not installed: self.skipTest('Installed ArduinoJson headers required')
                    json_root=str(installed[0].parent)
                command+=['-I'+json_root]
            command+=['-I'+str(ROOT/'test/unit/cpp/network_resource_mocks'),
                      '-I'+str(ROOT/'test/unit/cpp/mocks'),'-I'+str(ROOT/'test/mocks'),
                      '-I'+str(ROOT/'include'),str(ROOT/'test/unit/cpp'/harness)]
            if not actual_json:
                command += [str(ROOT/p) for p in ('src/core/BoardProfile.cpp',
                    'src/core/PeripheralResourcePlanner.cpp','src/core/PeripheralNetworkResources.cpp',
                    'src/network/NetworkResourceConfig.cpp')]
            command+=['-o',str(binary)]
            result=subprocess.run(command,capture_output=True,text=True,timeout=90)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            result=subprocess.run([str(binary)],capture_output=True,text=True,timeout=15)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)

    def test_production_guard_conflicts_capacity_and_allocation_failure(self):
        self.compile_guard('network_resource_guard_harness.cpp')

    def test_pins_validate_types_before_narrowing_and_commit_atomically(self):
        self.compile_guard('network_resource_pins_harness.cpp',True)

    def test_all_mutations_use_configured_network_claims_before_commit(self):
        manager=(ROOT/'src/network/NetworkManager.cpp').read_text(encoding='utf-8')
        update=manager[manager.index('bool FBNetworkManager::updateConfig('):manager.index('bool FBNetworkManager::updateConfigFromJSON(')]
        self.assertLess(update.index('RecursiveMutexGuard resourceLock'),update.index('validateNetworkResourceConfig'))
        self.assertLess(update.index('validateNetworkResourceConfig'),update.index('captureUserConfig'))
        self.assertLess(update.index('validateNetworkResourceConfig'),update.index('wifiConfig = effectiveConfig;'))
        route=(ROOT/'src/network/handlers/PeripheralRouteHandler.cpp').read_text(encoding='utf-8')
        self.assertIn('context.ethernet = FastBee::ethernetResourceConfig(',route)
        for name in ('handleGetPeripheralResources', 'handlePreflightPeripheral',
                     'handleAddPeripheral', 'handleUpdatePeripheral', 'handleEnablePeripheral',
                     'handleImportConfiguration'):
            body=route.split('void PeripheralRouteHandler::'+name+'(',1)[1].split('\nvoid PeripheralRouteHandler::',1)[0]
            self.assertIn('ctx->networkManager',body,name)
        system=(ROOT/'src/network/handlers/SystemRouteHandler.cpp').read_text(encoding='utf-8')
        self.assertIn('parseEthernetResourcePins',system)
        self.assertIn('parseCellularResourcePins',system)
        self.assertIn('parseCellularResourcePins',manager)
        self.assertIn('context.cellular = FastBee::cellularResourceConfig(',route)
        self.assertIn('FastBee::setFixedJsonField(data, "networkClaims", "w5500,cellular")',route)
        self.assertIn('effectiveConfig.networkType != NetworkType::NET_WIFI',update)
        self.assertIn('wifiConfig.networkType != NetworkType::NET_WIFI',update)
        self.assertIn('updateConfig(cfg, true, &resourceError)',system)
        self.assertIn('busy ? 503 : 422',system)

    def test_cellular_adapter_and_planner_share_fixed_uart_contract(self):
        adapter=(ROOT/'src/network/CellularAdapter.cpp').read_text(encoding='utf-8')
        graph=(ROOT/'src/core/PeripheralNetworkResources.cpp').read_text(encoding='utf-8')
        self.assertIn('cellularUartPort(',graph)
        self.assertIn('static_assert(FastBee::cellularUartPort(CHIP_UART_COUNT) == 2',adapter)
        self.assertIn('static_assert(FastBee::cellularUartPort(CHIP_UART_COUNT) == 1',adapter)
        self.assertIn('_serial = &Serial2;',adapter)
        self.assertIn('_serial = &Serial1;',adapter)

if __name__=='__main__': unittest.main()
