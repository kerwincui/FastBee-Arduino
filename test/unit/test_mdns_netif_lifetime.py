"""Exercise production teardown bodies against a strict netif lifetime model.

COM16 matching-ELF crash: reconnect -> restartMDNS -> mdns_free ->
pcb_if_deinit_lwip -> join_group -> esp_netif_is_netif_up. Arduino WiFi
MODE_NULL destroys the default netifs, so mDNS must release them first.
These are host order/lifetime tests, not evidence of a physical reconnect.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test_sensor_publication import definition

ROOT = Path(__file__).resolve().parents[2]

class MdnsNetifLifetimeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise unittest.SkipTest('C++17 compiler required')
        path = Path(os.environ.get('FASTBEE_NETWORK_SOURCE', ROOT/'src/network/NetworkManager.cpp'))
        source = path.read_text(encoding='utf-8')
        bodies = '\n'.join(definition(source, signature) for signature in (
            'void FBNetworkManager::startBootStaRetryScan()',
            'void FBNetworkManager::disconnectWiFi()',
            'void FBNetworkManager::disconnectNetwork()'))
        cls.source = source
        cls.temp = tempfile.TemporaryDirectory(prefix='fastbee-mdns-netif-')
        cls.addClassCleanup(cls.temp.cleanup)
        directory = Path(cls.temp.name)
        harness = r'''
#include <cassert>
#include <cstdio>
#include <string>
bool live = true, mdns = true;
int stops = 0, teardowns = 0;
constexpr int WIFI_MODE_NULL = 0;
unsigned long millis() { return 12345; }
struct SerialMock {
  template<typename... T> void printf(const char*, T...) {}
  void println(const char*) {}
} Serial;
#define LOG_INFO(...) ((void)0)
#define LOG_INFOF(...) ((void)0)
struct Dns { void stopMDNS() {
  if (mdns) { assert(live && "mDNS accessed a destroyed netif"); ++stops; mdns=false; }
} };
struct WiFiMock { void mode(int value) {
  assert(value == WIFI_MODE_NULL);
  assert(!mdns && "WiFi destroyed netif before mDNS release");
  live=false; ++teardowns;
} } WiFi;
struct WifiManager { void disconnectWiFi() { WiFi.mode(WIFI_MODE_NULL); } };
enum class NetworkStatus { DISCONNECTED };
enum class BootStaRetryScanState { AUTH_COOLDOWN, DRIVER_NULL_SETTLE };
struct FBNetworkManager {
  Dns dns; Dns* dnsManager = &dns;
  WifiManager wifi; WifiManager* wifiManager=&wifi;
  int _bootStaRetryCandidateCount=3;
  unsigned long _bootStaRetryScanStartedAt=0, _authRetryCooldownMs=0;
  bool _bootStaRetryScanInProgress=false, connecting=true;
  BootStaRetryScanState _bootStaRetryScanState=BootStaRetryScanState::DRIVER_NULL_SETTLE;
  struct { NetworkStatus status=NetworkStatus::DISCONNECTED; } statusInfo;
  void triggerEvent(NetworkStatus, const char*) {}
  void startBootStaRetryScan();
  void disconnectWiFi();
  void disconnectNetwork();
};
''' + bodies + r'''
int main(int argc, char** argv) {
  assert(argc==2); std::string mode=argv[1]; FBNetworkManager network;
  if (mode=="no-mdns") mdns=false;
  if (mode=="null-manager") { mdns=false; network.dnsManager=nullptr; }
  if (mode=="cooldown") network._authRetryCooldownMs=60000;
  if (mode=="disconnect") network.disconnectWiFi();
  else if (mode=="disconnect-network") network.disconnectNetwork();
  else network.startBootStaRetryScan();
  assert(teardowns==1 && !live && !mdns);
  assert(stops==((mode=="no-mdns" || mode=="null-manager") ? 0 : 1));
  if (mode=="cooldown") assert(network._bootStaRetryScanState==BootStaRetryScanState::AUTH_COOLDOWN);
  if (mode=="repeat") {
    live=true; mdns=true; network.startBootStaRetryScan();
    assert(stops==2 && teardowns==2 && !mdns);
  }
}
'''
        (directory/'main.cpp').write_text(harness, encoding='utf-8')
        cls.exe = directory/'lifetime.exe'
        compiled = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
            str(directory/'main.cpp'), '-o', str(cls.exe)], capture_output=True, text=True, timeout=60)
        if compiled.returncode:
            raise AssertionError(compiled.stdout+compiled.stderr)

    def test_ap_fallback_stops_mdns_before_disconnect(self):
        body = definition(self.source, 'void FBNetworkManager::attemptReconnect()')
        disconnect = body.index('wifiManager->disconnectWiFi();')
        stop = body.find('dnsManager->stopMDNS();')
        self.assertGreaterEqual(stop, 0)
        self.assertLess(stop, disconnect)

def make_test(case):
    def test(self):
        ran = subprocess.run([str(self.exe), case], capture_output=True, text=True, timeout=10)
        self.assertEqual(ran.returncode, 0, ran.stdout+ran.stderr)
    return test

for case in ['retry', 'cooldown', 'no-mdns', 'null-manager', 'disconnect', 'disconnect-network', 'repeat']:
    setattr(MdnsNetifLifetimeTests, 'test_'+case.replace('-', '_'), make_test(case))

if __name__ == '__main__':
    unittest.main(verbosity=2)
