"""Exercise SSE admission/broadcast with scarce internal RAM and ample PSRAM."""
import os
import re
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test_sensor_publication import definition

ROOT = Path(__file__).resolve().parents[2]


class SseAdmissionTests(unittest.TestCase):
    def test_resource_authentication_and_capacity_statuses(self):
        source = (ROOT / 'src/network/handlers/SSERouteHandler.cpp').read_text(encoding='utf8')
        header = (ROOT / 'include/network/handlers/SSERouteHandler.h').read_text(encoding='utf8')
        threshold = re.search(r'LOW_MEMORY_THRESHOLD\s*=\s*(\d+);', header)
        self.assertIsNotNone(threshold, 'Production SSE threshold must be available')
        self.assertEqual(8192, int(threshold.group(1)), 'Keep the existing 8 KiB admission threshold')
        methods = '\n'.join(definition(source, signature) for signature in (
            'void SSERouteHandler::setupRoutes(',
            'bool SSERouteHandler::shouldSkipBroadcast(',
            'void SSERouteHandler::broadcastSensorData(',
            'void SSERouteHandler::setReason(',
        ))
        cpp = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>
#include "core/MemoryBudget.h"
#define LOG_INFOF(...) ((void)0)
#define LOG_INFO(...) ((void)0)
#define LOG_WARNINGF(...) ((void)0)
#define LOG_WARNING(...) ((void)0)
using String=std::string;
unsigned long millis(){return 1000;}
struct AsyncWebServerRequest{std::vector<int> statuses;void send(int code,const char*,const char*){statuses.push_back(code);}};
using ArMiddlewareNext=std::function<void()>;
struct AsyncEventSourceClient{bool connected(){return true;}};
struct Events{
 unsigned clients=1,sent=0;
 std::function<void(AsyncWebServerRequest*,ArMiddlewareNext)> middleware;
 void addMiddleware(decltype(middleware) value){middleware=value;}
 void onConnect(std::function<void(AsyncEventSourceClient*)>){}
 void onDisconnect(std::function<void(AsyncEventSourceClient*)>){}
 unsigned count(){return clients;}
 void send(const char*,const char*,unsigned){++sent;}
};
struct AsyncWebServer{void addHandler(Events*){}};
struct AuthResult{bool success;};
struct WebHandlerContext{
 void* authManager=this;bool authenticated=true,needsAuth=true;unsigned requests=0;
 bool requiresAuth(AsyncWebServerRequest*){return needsAuth;}
 AuthResult authenticateRequest(AsyncWebServerRequest*){return {authenticated};}
 void noteWebRequestActivity(){++requests;}
};
struct HealthMonitor{bool severe=false,critical=false;bool isMemorySevere(){return severe;}bool isMemoryCritical(){return critical;}} health;
struct SSERouteHandler{
 WebHandlerContext* ctx;Events _events;unsigned _messageId=0;unsigned long _lastSkipLogMs=0;
 static constexpr unsigned LOW_MEMORY_THRESHOLD=PRODUCTION_THRESHOLD,MAX_SSE_CLIENTS=2;
 struct Slot{AsyncEventSourceClient* client=nullptr;} _slots[2];
 struct Stats{unsigned acceptedConnections=0,lastConnectAtMs=0,rejectedLowMemory=0,
  lastRejectAtMs=0,rejectedGuard=0,rejectedCapacity=0,skippedBroadcastGuard=0,
  skippedBroadcastLowMemory=0,lastSkipBroadcastAtMs=0;
  char lastRejectReason[24]{},lastSkipReason[24]{};} _stats;
 explicit SSERouteHandler(WebHandlerContext* context):ctx(context){}
 void setupRoutes(AsyncWebServer*);
 void trackClient(AsyncEventSourceClient*,unsigned long){}
 void untrackClient(AsyncEventSourceClient*){}
 void reportClientCount(){}
 void setReason(char*,size_t,const char*) const;
 bool shouldSkipBroadcast();
 void broadcastSensorData(const String&);
 HealthMonitor* getHealthMonitor(){return &health;}
};
PRODUCTION
int main(){
 WebHandlerContext context;SSERouteHandler handler(&context);AsyncWebServer server;
 handler.setupRoutes(&server);unsigned allowed=0;
 auto probe=[&](int expected){AsyncWebServerRequest request;unsigned before=allowed;
  handler._events.middleware(&request,[&](){++allowed;});
  if(expected){assert(request.statuses==std::vector<int>{expected});assert(allowed==before);}
  else{assert(request.statuses.empty());assert(allowed==before+1);}
 };
 probe(0);assert(context.requests==1);
 context.authenticated=false;probe(401);
 // Resource failures keep 503 even before auth, with plentiful PSRAM and
 // internal 32-bit-only RAM. Both admission and actual queueing must stop.
 for (const size_t value : {size_t(0),size_t(4096),size_t(8191)}) {
  internalLargest=value;probe(503);
  assert(std::strcmp(handler._stats.lastRejectReason,"low_memory")==0);
  handler.broadcastSensorData("{}");
  assert(handler._events.sent==0);
  assert(std::strcmp(handler._stats.lastSkipReason,"low_memory")==0);
 }
 assert(handler._stats.rejectedLowMemory==3);
 assert(handler._stats.skippedBroadcastLowMemory==3);
 assert(handler._stats.lastRejectAtMs==millis());
 assert(handler._stats.lastSkipBroadcastAtMs==millis());
 internalLargest=8192;probe(401);context.authenticated=true;probe(0);
 handler.broadcastSensorData("{}");assert(handler._events.sent==1);
#if FASTBEE_ENABLE_HEALTH_MONITOR
 health.severe=true;probe(503);health.severe=false;
 health.critical=true;probe(503);health.critical=false;assert(handler._stats.rejectedGuard==2);
 for (unsigned level=0;level<2;++level) {
  health.severe=level==0;health.critical=level==1;
  handler.broadcastSensorData("{}");assert(handler._events.sent==1);
  assert(std::strcmp(handler._stats.lastSkipReason,"guard_active")==0);
 }
 health.severe=false;health.critical=false;
 assert(handler._stats.skippedBroadcastGuard==2);
 assert(std::strcmp(handler._stats.lastRejectReason,"guard_active")==0);
#else
 // Compile-time removal of HealthMonitor never removes the DRAM gate.
 assert(handler._stats.rejectedGuard==0&&handler._stats.skippedBroadcastGuard==0);
#endif
 AsyncEventSourceClient client;handler._slots[0].client=&client;handler._slots[1].client=&client;
 probe(503);assert(handler._stats.rejectedCapacity==1);
 assert(std::strcmp(handler._stats.lastRejectReason,"capacity")==0);
 handler._slots[0].client=nullptr;handler._slots[1].client=nullptr;probe(0);
 handler.broadcastSensorData("{}");assert(handler._events.sent==2);
 handler._events.clients=0;handler.broadcastSensorData("{}");assert(handler._events.sent==2);
 handler.ctx=nullptr;probe(503);
 assert(context.requests==3);
}
'''.replace('PRODUCTION_THRESHOLD', threshold.group(1)).replace('PRODUCTION', methods)
        compiler = shutil.which('g++') or shutil.which('clang++') or 'D:/msys64/ucrt64/bin/g++.exe'
        self.assertTrue(Path(compiler).exists(), 'C++17 compiler required')
        environment = dict(os.environ)
        environment['PATH'] = str(Path(compiler).parent) + os.pathsep + environment.get('PATH', '')
        with tempfile.TemporaryDirectory(prefix='fastbee-sse-admission-') as temp:
            temp = Path(temp)
            (temp / 'esp_heap_caps.h').write_text(
                (ROOT / 'test/unit/cpp/memory_gate_heap_mock.h').read_text(encoding='utf8'), encoding='utf8')
            path = temp / 'admission.cpp'; path.write_text(cpp, encoding='utf8')
            for enabled in (0, 1):
                with self.subTest(health_monitor=enabled):
                    executable = temp / (f'admission-{enabled}.exe' if os.name == 'nt' else f'admission-{enabled}')
                    built = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                            '-DESP_PLATFORM=1', f'-DFASTBEE_ENABLE_HEALTH_MONITOR={enabled}',
                                            f'-I{temp}', f'-I{ROOT / "include"}',
                                            str(path), '-o', str(executable)], capture_output=True, text=True,
                                           timeout=60, env=environment)
                    self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
                    ran = subprocess.run([str(executable)], capture_output=True, text=True, timeout=10, env=environment)
                    self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)


if __name__ == '__main__':
    unittest.main()
