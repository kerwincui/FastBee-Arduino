"""Production weather routes and save/refresh handlers with real URI matching."""
import ast
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test.unit.test_screen_panel_contract import braced_block
from test.unit.test_periph_exec_config_persistence import cpp_function

ROOT=Path(__file__).resolve().parents[2]

class WeatherRouteDispatchTests(unittest.TestCase):
    def test_exact_routes_refresh_get_post_query_and_invalid_save(self):
        dependency=next((ROOT/'.pio/libdeps').glob('*/ESPAsyncWebServer/src'))
        header=(dependency/'ESPAsyncWebServer.h').read_text(encoding='utf-8')
        server=(dependency/'WebServer.cpp').read_text(encoding='utf-8')
        request=(dependency/'WebRequest.cpp').read_text(encoding='utf-8')
        tree=ast.parse((ROOT/'test/unit/test_history_route_dispatch.py').read_text(encoding='utf-8'))
        # Reuse transport shim only; every matcher and weather method below is production.
        shim=next(node.value.value for node in ast.walk(tree) if isinstance(node,ast.Assign) and
            any(isinstance(t,ast.Name) and t.id=='source' for t in node.targets) and isinstance(node.value,ast.Constant))
        shim=shim[:shim.index('enum { HTTP_GET')]
        shim=shim.replace('    size_t length() const', '    const char* c_str() const { return value.c_str(); }\n    size_t length() const')
        shim=shim.replace('    int method = 1;', '    void* _tempObject=nullptr;\n    size_t contentLength() const { return 0; }\n    int method = 1;')
        matcher='class AsyncURIMatcher {'+braced_block(header[header.index('\nclass AsyncURIMatcher {'):],'class AsyncURIMatcher')+'};'
        implementations=[]
        for signature in ('AsyncURIMatcher::AsyncURIMatcher(String uri, uint16_t modifiers) : _value(std::move(uri))',
            'AsyncURIMatcher::AsyncURIMatcher(String uri, Type type, uint16_t modifiers) : _value(std::move(uri)), _flags(_toFlags(type, modifiers))',
            'bool AsyncURIMatcher::matches(AsyncWebServerRequest *request) const'):
            implementations.append(signature+' {'+braced_block(server,signature)+'}')
        begin=request.index("index = u.indexOf('?');"); end=request.index('_url = urlDecode(u);',begin)+len('_url = urlDecode(u);')
        shim=shim.replace('QUERY_SPLIT',request[begin:end]).replace('MATCHER',matcher).replace('IMPLEMENTATIONS','\n'.join(implementations))
        routes=(ROOT/'src/network/handlers/RemoteResourceRouteHandler.cpp').read_text(encoding='utf-8')
        methods='\n'.join(cpp_function(routes,sig) for sig in ('const char* weatherSaveMessage(',
            'void RemoteResourceRouteHandler::setupRoutes(', 'void RemoteResourceRouteHandler::weatherTestStatus(',
            'void RemoteResourceRouteHandler::requestWeatherTest(', 'void RemoteResourceRouteHandler::save('))
        harness=r'''
#include <ArduinoJson.h>
#include <cstdlib>
#include <cstring>
// Negative route checks are expected failures, not crashes. In particular,
// abort/assert can launch Windows Error Reporting and retain the executable.
static void require(bool condition,const char* check,int exitCode=42) {
 if (!condition) { std::cerr << check << std::endl; std::_Exit(exitCode); }
}
enum { HTTP_GET=1, HTTP_POST=2 };
struct Body { size_t total; size_t used; char bytes[1]; };
struct Context {
 int code=0; unsigned success=0;
 bool requireAuth(void*) { return true; } bool requireDeveloperMode(void*) { return true; }
 void sendError(void*,int value,const char*) { code=value; }
 void sendBadRequest(void*,const char*) { code=400; }
 void sendSuccess(void*,JsonDocument&) { code=200; ++success; }
 String getParamValue(void*,const char*,const char*) { return "home_weather"; }
};
namespace HandlerUtils { bool checkLowMemory(void*,int) { return false; } }
namespace FastBee {
namespace RemoteResourceModel { constexpr size_t MAX_BODY=8192; bool identifier(const char* id) { return *id!=0; } }
struct RemoteResourceManager {
 unsigned configReads=0,refreshReads=0,refreshWrites=0,configWrites=0;
 static RemoteResourceManager& instance() { static RemoteResourceManager m; return m; }
 void getScreenWeather(JsonObject) { ++configReads; }
 bool getWeatherTestStatus(const String&,JsonObject) { ++refreshReads; return true; }
 bool requestRead(const String&,String&) { ++refreshWrites; return true; }
 bool saveScreenWeather(JsonVariantConst,String&) { ++configWrites; return true; }
}; }
class AsyncWebServer {
 struct Route { AsyncURIMatcher uri; int method; std::function<void(AsyncWebServerRequest*)> callback; };
 std::vector<Route> routes;
public:
 void on(AsyncURIMatcher uri,int method,std::function<void(AsyncWebServerRequest*)> callback,
         void* =nullptr,std::function<void(AsyncWebServerRequest*,uint8_t*,size_t,size_t,size_t)> ={}) {
   routes.push_back({std::move(uri),method,callback});
 }
 void dispatch(const char* path,int method) {
   AsyncWebServerRequest request(path,method);
   for (auto& route:routes) if(route.method==method && route.uri.matches(&request)) {route.callback(&request);return;}
 }
};
class RemoteResourceRouteHandler {
public:
 Context context; Context* ctx_=&context;
 void setupRoutes(AsyncWebServer*);
 void save(AsyncWebServerRequest*);
 void weatherTestStatus(AsyncWebServerRequest*);
 void requestWeatherTest(AsyncWebServerRequest*);
};
PRODUCTION
int main() {
 AsyncWebServer server; RemoteResourceRouteHandler route; route.setupRoutes(&server);
 auto& manager=FastBee::RemoteResourceManager::instance();
 server.dispatch("/api/screen-panel/weather/refresh?id=home_weather",HTTP_POST);
 require(manager.refreshWrites==1 && manager.configWrites==0 && route.context.code==200,"refresh_post_dispatch",41);
 server.dispatch("/api/screen-panel/weather/refresh?id=home_weather",HTTP_GET);
 require(manager.refreshReads==2 && manager.configReads==0,"refresh_get_dispatch");
 server.dispatch("/api/screen-panel/weather",HTTP_GET); require(manager.configReads==1,"config_get_dispatch");
 server.dispatch("/api/screen-panel/weather",HTTP_POST);
 require(route.context.code==400 && manager.configWrites==0,"missing_json_rejected");
 unsigned replies=route.context.success;
 server.dispatch("/api/screen-panel/weather/refresh/unknown",HTTP_GET);
 server.dispatch("/api/screen-panel/weather/unknown",HTTP_POST);
 require(route.context.success==replies && manager.refreshWrites==1,"unknown_routes_rejected");
}
'''.replace('PRODUCTION',methods)
        compiler=shutil.which('g++') or 'D:/msys64/ucrt64/bin/g++.exe'
        jsonHeader=next((ROOT/'.pio/libdeps').glob('*/ArduinoJson/src/ArduinoJson.h'))
        env=dict(os.environ);env['PATH']=str(Path(compiler).parent)+os.pathsep+env.get('PATH','')
        with tempfile.TemporaryDirectory(prefix='weather-dispatch-') as folder:
            temp=Path(folder)
            for mutation in (False,True):
                body=shim+harness
                if mutation:
                    original=body
                    body=body.replace('AsyncURIMatcher::exact("/api/screen-panel/weather")','"/api/screen-panel/weather"')
                    self.assertNotEqual(original,body,'Mutation must change the production route matcher')
                cpp=temp/'route.cpp';binary=temp/'route.exe';cpp.write_text(body,encoding='utf-8')
                built=subprocess.run([compiler,'-std=c++17','-Wall','-Wextra','-Werror',f'-I{jsonHeader.parent}',str(cpp),'-o',str(binary)],capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=60,env=env)
                self.assertEqual(0,built.returncode,built.stdout+built.stderr)
                run=subprocess.run([str(binary)],capture_output=True,text=True,timeout=15,env=env)
                if mutation:
                    self.assertEqual(41,run.returncode,'Plain URI must reproduce refresh shadowing, not an unrelated crash: '+run.stdout+run.stderr)
                    self.assertEqual('refresh_post_dispatch',run.stderr.strip())
                else:self.assertEqual(0,run.returncode,run.stdout+run.stderr)

if __name__=='__main__':unittest.main()
