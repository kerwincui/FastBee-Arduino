"""Compile the production SSE admission path: resource failures must not become 401."""
import os
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
        method = definition(source, 'void SSERouteHandler::setupRoutes(')
        cpp = r'''
#include <cassert>
#include <cstdint>
#include <functional>
#include <vector>
#define FASTBEE_ENABLE_HEALTH_MONITOR 1
#define LOG_INFOF(...) ((void)0)
#define LOG_INFO(...) ((void)0)
#define LOG_WARNINGF(...) ((void)0)
#define LOG_WARNING(...) ((void)0)
constexpr int MALLOC_CAP_DEFAULT=0;
static uint32_t largest=32768;
uint32_t heap_caps_get_largest_free_block(int){return largest;}
unsigned long millis(){return 1000;}
struct AsyncWebServerRequest{std::vector<int> statuses;void send(int code,const char*,const char*){statuses.push_back(code);}};
using ArMiddlewareNext=std::function<void()>;
struct AsyncEventSourceClient{bool connected(){return true;}};
struct Events{
 std::function<void(AsyncWebServerRequest*,ArMiddlewareNext)> middleware;
 void addMiddleware(decltype(middleware) value){middleware=value;}
 void onConnect(std::function<void(AsyncEventSourceClient*)>){}
 void onDisconnect(std::function<void(AsyncEventSourceClient*)>){}
 unsigned count(){return 0;}
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
 WebHandlerContext* ctx;Events _events;
 static constexpr unsigned LOW_MEMORY_THRESHOLD=8192,MAX_SSE_CLIENTS=2;
 struct Slot{AsyncEventSourceClient* client=nullptr;} _slots[2];
 struct Stats{unsigned acceptedConnections=0,lastConnectAtMs=0,rejectedLowMemory=0,
  lastRejectAtMs=0,rejectedGuard=0,rejectedCapacity=0;char lastRejectReason[24];} _stats;
 explicit SSERouteHandler(WebHandlerContext* context):ctx(context){}
 void setupRoutes(AsyncWebServer*);
 void trackClient(AsyncEventSourceClient*,unsigned long){}
 void untrackClient(AsyncEventSourceClient*){}
 void reportClientCount(){}
 void setReason(char*,unsigned,const char*){}
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
 context.authenticated=false;probe(401);context.authenticated=true;
 largest=4096;probe(503);assert(handler._stats.rejectedLowMemory==1);largest=32768;
 health.severe=true;probe(503);health.severe=false;
 health.critical=true;probe(503);health.critical=false;assert(handler._stats.rejectedGuard==2);
 AsyncEventSourceClient client;handler._slots[0].client=&client;handler._slots[1].client=&client;
 probe(503);assert(handler._stats.rejectedCapacity==1);
 handler._slots[0].client=nullptr;handler._slots[1].client=nullptr;probe(0);
 handler.ctx=nullptr;probe(503);
 assert(context.requests==2);
}
'''.replace('PRODUCTION', method)
        compiler = shutil.which('g++') or shutil.which('clang++')
        self.assertIsNotNone(compiler, 'C++17 compiler required')
        with tempfile.TemporaryDirectory(prefix='fastbee-sse-admission-') as temp:
            temp = Path(temp)
            path = temp / 'admission.cpp'; path.write_text(cpp, encoding='utf8')
            executable = temp / ('admission.exe' if os.name == 'nt' else 'admission')
            built = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                    str(path), '-o', str(executable)], capture_output=True, text=True, timeout=60)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            ran = subprocess.run([str(executable)], capture_output=True, text=True, timeout=10)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)


if __name__ == '__main__':
    unittest.main()
