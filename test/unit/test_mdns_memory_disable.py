"""Execute production mDNS stop/disable/health-gate bodies on host mocks."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test_sensor_publication import definition

ROOT = Path(__file__).resolve().parents[2]


class MdnsMemoryDisableTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler: raise unittest.SkipTest('C++17 compiler required')
        dns = Path(os.environ.get('FASTBEE_DNS_SOURCE',ROOT/'src/network/DNSManager.cpp')).read_text(encoding='utf-8')
        network = Path(os.environ.get('FASTBEE_NETWORK_SOURCE',ROOT/'src/network/NetworkManager.cpp')).read_text(encoding='utf-8')
        bodies = '\n'.join(definition(dns, sig) for sig in (
            'void DNSManager::stopMDNS()', 'void DNSManager::setMDNSEnabled(bool enabled)',
            'bool DNSManager::checkMDNSHealth()'))
        bodies += '\n' + definition(network, 'bool FBNetworkManager::shouldRunMDNS() const')
        lifecycle = dns[dns.index('namespace {'):dns.index('\nFastBee::FeatureInstanceSnapshot DNSManager::observeFeatureInstance')]
        harness = r'''
#include <cassert>
#include <string>
#include <atomic>
#include <mutex>
#include "core/FeatureInstanceRegistry.h"
using SemaphoreHandle_t=std::recursive_mutex*;
constexpr int pdTRUE=1,portMAX_DELAY=-1;
SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(){return new std::recursive_mutex;}
int xSemaphoreTakeRecursive(SemaphoreHandle_t mutex,int timeout){if(timeout==0)return mutex->try_lock();mutex->lock();return pdTRUE;}
void xSemaphoreGiveRecursive(SemaphoreHandle_t mutex){mutex->unlock();}
void vSemaphoreDelete(SemaphoreHandle_t mutex){delete mutex;}
// @lifecycle@
#define FASTBEE_ENABLE_ETHERNET 0
#define LOG_INFO(...) ((void)0)
#define LOG_WARNING(...) ((void)0)
struct String { bool isEmpty() const {return true;} };
struct IPAddress { int value; IPAddress(int a,int b,int c,int d):value(a+b+c+d){} };
bool operator!=(const IPAddress& a,const IPAddress& b){return a.value!=b.value;}
const IPAddress INADDR_NONE(255,255,255,255);
using WiFiMode_t=int;
constexpr int WIFI_MODE_STA=1,WIFI_MODE_AP=2,WIFI_MODE_APSTA=3,WL_CONNECTED=1;
struct { int getMode(){return WIFI_MODE_STA;} int status(){return WL_CONNECTED;}
IPAddress localIP(){return IPAddress(192,168,5,24);} IPAddress softAPIP(){return IPAddress(192,168,4,1);} } WiFi;
struct {int ends=0; void end(){++ends;} } MDNS;
struct DNSManager {
bool mdnsStarted=false,mdnsEnabled=true; int starts=0; String actualHostname,customDomain;
void stopMDNS(); void setMDNSEnabled(bool); bool checkMDNSHealth();
bool startMDNS(const String&){++starts;mdnsStarted=true;return true;}
};
namespace FastBee { struct RuntimeFeatureManager {
bool enabled=true; static RuntimeFeatureManager& instance(){static RuntimeFeatureManager x;return x;}
bool shouldEnable(const char* key){assert(std::string(key)=="mdns");return enabled;}
}; }
struct FBNetworkManager {struct {bool enableMDNS=true;} wifiConfig; bool shouldRunMDNS() const;};
''' + bodies + r'''
int main(int argc,char** argv){
assert(argc==2); std::string mode=argv[1]; DNSManager dns; FBNetworkManager network;
if(mode=="stop-active"){dns.mdnsStarted=true;dns.setMDNSEnabled(false);assert(MDNS.ends==1&&!dns.mdnsStarted&&!dns.mdnsEnabled);}
if(mode=="stop-idempotent"){dns.mdnsStarted=true;dns.setMDNSEnabled(false);dns.setMDNSEnabled(false);dns.stopMDNS();assert(MDNS.ends==1);}
if(mode=="stop-inactive"){dns.setMDNSEnabled(false);assert(MDNS.ends==0&&!dns.mdnsEnabled);}
if(mode=="health-disabled"){dns.setMDNSEnabled(false);for(int n=0;n<3;++n)assert(!dns.checkMDNSHealth());assert(dns.starts==0);}
if(mode=="health-enabled"){assert(dns.checkMDNSHealth());assert(dns.starts==1);}
if(mode=="reenable"){dns.mdnsStarted=true;dns.setMDNSEnabled(false);dns.setMDNSEnabled(true);assert(dns.starts==0);assert(dns.checkMDNSHealth());assert(dns.starts==1&&MDNS.ends==1);}
if(mode=="two-gates"){for(int a=0;a<2;++a)for(int b=0;b<2;++b){network.wifiConfig.enableMDNS=a;FastBee::RuntimeFeatureManager::instance().enabled=b;assert(network.shouldRunMDNS()==(a&&b));}}
}
'''
        harness = harness.replace('// @lifecycle@', lifecycle)
        cls.temp=tempfile.TemporaryDirectory(prefix='fastbee-mdns-disable-')
        cls.addClassCleanup(cls.temp.cleanup)
        path=Path(cls.temp.name); (path/'test.cpp').write_text(harness,encoding='utf-8')
        cls.exe=path/'test.exe'
        built=subprocess.run([compiler,'-std=c++17','-Wall','-Wextra','-Werror','-pthread',
                              '-I'+str(ROOT/'include'),str(path/'test.cpp'),'-o',str(cls.exe)],capture_output=True,text=True,timeout=60)
        if built.returncode: raise AssertionError(built.stdout+built.stderr)


def case_test(case):
    def test(self):
        result=subprocess.run([str(self.exe),case],capture_output=True,text=True,timeout=10)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
    return test


for case in ['stop-active','stop-idempotent','stop-inactive','health-disabled','health-enabled','reenable','two-gates']:
    setattr(MdnsMemoryDisableTests,'test_'+case.replace('-','_'),case_test(case))

if __name__=='__main__': unittest.main(verbosity=2)
