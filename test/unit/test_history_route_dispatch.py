"""Compile production route registration against the installed async URI matcher.

Only Arduino String, request transport and callback collection are host shims.
Matcher constructors/matches, query splitting and setupRoutes come from the
actual dependency/firmware sources; reverting to a plain URI must fail dispatch.
"""

from __future__ import annotations

import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest

from test.unit.test_screen_panel_contract import braced_block


ROOT = pathlib.Path(__file__).resolve().parents[2]


class HistoryRouteDispatchTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("g++") or shutil.which("clang++")
        fallback = pathlib.Path("D:/msys64/ucrt64/bin/g++.exe")
        if not compiler and fallback.is_file():
            compiler = str(fallback)
        if not compiler:
            raise unittest.SkipTest("A C++17 host compiler is required")
        candidates = list((ROOT / ".pio/libdeps").glob("*/ESPAsyncWebServer/src"))
        if not candidates:
            raise unittest.SkipTest("Install PlatformIO ESPAsyncWebServer dependency first")
        dependency = next((p for p in candidates if p.parent.parent.name == "esp32s3-F16R8"), candidates[0])
        header = (dependency / "ESPAsyncWebServer.h").read_text(encoding="utf-8")
        server = (dependency / "WebServer.cpp").read_text(encoding="utf-8")
        request = (dependency / "WebRequest.cpp").read_text(encoding="utf-8")
        routes = (ROOT / "src/network/handlers/ScreenPanelRouteHandler.cpp").read_text(encoding="utf-8")
        matcher_header = header[header.index("\nclass AsyncURIMatcher {"):]
        matcher = "class AsyncURIMatcher {" + braced_block(matcher_header, "class AsyncURIMatcher") + "};"
        implementations = []
        for signature in (
            "AsyncURIMatcher::AsyncURIMatcher(String uri, uint16_t modifiers) : _value(std::move(uri))",
            "AsyncURIMatcher::AsyncURIMatcher(String uri, Type type, uint16_t modifiers) : _value(std::move(uri)), _flags(_toFlags(type, modifiers))",
            "bool AsyncURIMatcher::matches(AsyncWebServerRequest *request) const",
        ):
            implementations.append(signature + " {" + braced_block(server, signature) + "}")
        query_start = request.index("index = u.indexOf('?');")
        query_end = request.index("_url = urlDecode(u);", query_start) + len("_url = urlDecode(u);")
        query_split = request[query_start:query_end]
        setup_signature = "void ScreenPanelRouteHandler::setupRoutes(AsyncWebServer* server)"
        setup = setup_signature + " {" + braced_block(routes, setup_signature) + "}"
        exec_routes = (ROOT / "src/network/handlers/PeriphExecRouteHandler.cpp").read_text(encoding="utf-8")
        exec_signature = "void PeriphExecRouteHandler::setupRoutes(AsyncWebServer* server)"
        exec_setup = exec_signature + " {" + braced_block(exec_routes, exec_signature) + "}"
        source = r'''
#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdint>
#include <functional>
#include <iostream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>
#define FASTBEE_ENABLE_SCREEN_PANEL 0
#define FASTBEE_ENABLE_POINT_HISTORY 1
class String {
    std::string value;
public:
    String() = default;
    String(const char* s): value(s) {}
    String(std::string s): value(std::move(s)) {}
    size_t length() const { return value.size(); }
    char operator[](size_t i) const { return value[i]; }
    bool operator==(const String& r) const { return value == r.value; }
    String operator+(const char* r) const { return value + r; }
    bool startsWith(const String& r) const { return value.rfind(r.value, 0) == 0; }
    bool endsWith(const String& r) const {
        return value.size() >= r.value.size() && value.compare(value.size()-r.value.size(), r.value.size(), r.value) == 0;
    }
    int lastIndexOf(const char* r) const { auto i=value.rfind(r); return i==std::string::npos ? -1 : int(i); }
    int indexOf(char r) const { auto i=value.find(r); return i==std::string::npos ? -1 : int(i); }
    String substring(size_t begin) const { return value.substr(begin); }
    String substring(size_t begin, size_t end) const { return value.substr(begin, end-begin); }
    void concat(char c) { value += c; }
    void toLowerCase() { std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c){ return std::tolower(c); }); }
};
class AsyncWebServerRequest {
    String _url;
    static String urlDecode(String s) { return s; } // Test paths require no decoding.
public:
    int method = 1;
    bool json = false;
    explicit AsyncWebServerRequest(const char* raw, int methodValue = 1, bool jsonValue = false):
        method(methodValue), json(jsonValue) {
        String u(raw), g;
        int index;
        QUERY_SPLIT
    }
    const String& url() const { return _url; }
};
MATCHER
IMPLEMENTATIONS
enum { HTTP_GET = 1, HTTP_POST = 2, HTTP_DELETE = 4 };
struct JsonVariant {};
struct PanelJsonHandler {
    template<class Callback> PanelJsonHandler(void*, const char*, Callback, int) {}
};
struct AsyncCallbackJsonWebHandler {
    AsyncURIMatcher matcher;
    std::function<void(AsyncWebServerRequest*, JsonVariant&)> callback;
    int method = HTTP_POST;
    AsyncCallbackJsonWebHandler(AsyncURIMatcher uri,
        std::function<void(AsyncWebServerRequest*, JsonVariant&)> cb):
        matcher(std::move(uri)), callback(std::move(cb)) {}
    void setMethod(int value) { method = value; }
    void setMaxContentLength(int) {}
};
class AsyncWebServer {
    struct Route { AsyncURIMatcher matcher; int method; bool json;
        std::function<void(AsyncWebServerRequest*)> callback; };
    std::vector<Route> routes;
public:
    void on(AsyncURIMatcher matcher, int method, std::function<void(AsyncWebServerRequest*)> callback) {
        routes.push_back({std::move(matcher), method, false, std::move(callback)});
    }
    void addHandler(PanelJsonHandler* handler) { delete handler; } // POST is out of GET dispatch scope.
    void addHandler(AsyncCallbackJsonWebHandler* handler) {
        const auto callback = handler->callback;
        routes.push_back({std::move(handler->matcher), handler->method, true,
            [callback](AsyncWebServerRequest* request) { JsonVariant json; callback(request, json); }});
        delete handler;
    }
    void dispatch(AsyncWebServerRequest* request) {
        for (auto& route : routes) if (route.method == request->method &&
            (!route.json || request->json) && route.matcher.matches(request)) {
            route.callback(request); return;
        }
    }
};
class ScreenPanelRouteHandler {
    void* _ctx = nullptr;
public:
    std::string selected = "not-found";
    void setupRoutes(AsyncWebServer* server);
    void handleHistory(AsyncWebServerRequest*) { selected = "series"; }
    void handleGetHistoryConfig(AsyncWebServerRequest*) { selected = "config"; }
    void handleSaveHistoryConfig(AsyncWebServerRequest*, JsonVariant&) {}
};
class PeriphExecRouteHandler {
public:
    std::string selected = "not-found";
    void setupRoutes(AsyncWebServer* server);
    void handleGetControls(AsyncWebServerRequest*) { selected = "controls"; }
    void handleGetStaticEvents(AsyncWebServerRequest*) { selected = "static"; }
    void handleGetDynamicEvents(AsyncWebServerRequest*) { selected = "dynamic"; }
    void handleGetEventCategories(AsyncWebServerRequest*) { selected = "categories"; }
    void handleGetTriggerTypes(AsyncWebServerRequest*) { selected = "trigger-types"; }
    void handleGetRecentResults(AsyncWebServerRequest*) { selected = "results"; }
    void handlePreflightRuleJson(AsyncWebServerRequest*, JsonVariant&) { selected = "preflight-json"; }
    void handleUpdateRuleJson(AsyncWebServerRequest*, JsonVariant&) { selected = "update-json"; }
    void handleAddRuleJson(AsyncWebServerRequest*, JsonVariant&) { selected = "add-json"; }
    void handleUpdateRule(AsyncWebServerRequest*) { selected = "update-form"; }
    void handleEnableRule(AsyncWebServerRequest*) { selected = "enable"; }
    void handleDisableRule(AsyncWebServerRequest*) { selected = "disable"; }
    void handleRunOnce(AsyncWebServerRequest*) { selected = "run"; }
    void handleDeleteRule(AsyncWebServerRequest*) { selected = "delete"; }
    void handleGetRules(AsyncWebServerRequest*) { selected = "rules"; }
    void handleAddRule(AsyncWebServerRequest*) { selected = "add-form"; }
};
SETUP
EXEC_SETUP
int main(int argc, char** argv) {
    if (argc < 2 || argc > 4) return 2;
    AsyncWebServer server;
    ScreenPanelRouteHandler handler;
    if (argc < 3 || std::string(argv[2]) != "no-history") handler.setupRoutes(&server);
    PeriphExecRouteHandler exec;
    exec.setupRoutes(&server);
    const std::string method = argc == 4 ? argv[3] : "GET";
    AsyncWebServerRequest request(argv[1], method == "GET" ? HTTP_GET :
        method == "DELETE" ? HTTP_DELETE : HTTP_POST, method == "JSON");
    server.dispatch(&request);
    std::cout << (handler.selected == "not-found" ? exec.selected : handler.selected);
}
'''
        for marker, value in (("QUERY_SPLIT", query_split), ("MATCHER", matcher),
                              ("IMPLEMENTATIONS", "\n".join(implementations)),
                              ("EXEC_SETUP", exec_setup), ("SETUP", setup)):
            source = source.replace(marker, value)
        cls.temporary = tempfile.TemporaryDirectory(prefix="fastbee-history-dispatch-")
        cls.addClassCleanup(cls.temporary.cleanup)
        directory = pathlib.Path(cls.temporary.name)
        cls.process_env = dict(os.environ)
        cls.process_env["PATH"] = str(pathlib.Path(compiler).parent) + os.pathsep + cls.process_env.get("PATH", "")
        cls.executables = {}
        for name, body in (("production", source), ("plain-uri-regression", source.replace(
                'AsyncURIMatcher::exact("/api/periph-exec/history")', '"/api/periph-exec/history"')),
                ("plain-root-regression", source.replace(
                    'AsyncURIMatcher::exact("/api/periph-exec")', '"/api/periph-exec"'))):
            cpp = directory / f"{name}.cpp"
            cpp.write_text(body, encoding="utf-8")
            executable = directory / (name + (".exe" if os.name == "nt" else ""))
            build = subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(executable)],
                                   capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=60, env=cls.process_env)
            if build.returncode:
                raise AssertionError(build.stdout + build.stderr)
            cls.executables[name] = executable

    def dispatch(self, path, executable="production", history=True, method="GET"):
        result = subprocess.run([str(self.executables[executable]), path,
                                 "history" if history else "no-history", method], capture_output=True,
                                text=True, timeout=10, env=self.process_env)
        self.assertEqual(result.returncode, 0, result.stderr)
        return result.stdout

    def test_history_config_reaches_configuration_handler(self):
        self.assertEqual(self.dispatch("/api/periph-exec/history/config"), "config")
        self.assertEqual(self.dispatch("/api/periph-exec/history/config?unused=1"), "config")

    def test_series_query_parameters_preserve_route(self):
        self.assertEqual(self.dispatch("/api/periph-exec/history?sourceRef=periph.1.temperature"), "series")
        self.assertEqual(self.dispatch("/api/periph-exec/history?sourceRef=a%2Eb&count=10"), "series")
        self.assertEqual(self.dispatch("/api/periph-exec/history"), "series")

    def test_unknown_children_do_not_reach_series_or_configuration(self):
        for suffix in ("/unknown", "/config/unknown", "/", "/config/", "-other"):
            with self.subTest(suffix=suffix):
                self.assertEqual(self.dispatch("/api/periph-exec/history" + suffix), "not-found")

    def test_plain_uri_mutation_reproduces_device_regression(self):
        self.assertEqual(self.dispatch("/api/periph-exec/history/config", "plain-uri-regression"), "series")
        self.assertEqual(self.dispatch("/api/periph-exec/history/unknown", "plain-uri-regression"), "series")

    def test_compiled_out_history_never_falls_into_rule_list_or_create(self):
        for method in ("GET", "FORM", "JSON"):
            for suffix in ("/history", "/history/config", "/unknown", "/controls/unknown"):
                with self.subTest(method=method, suffix=suffix):
                    self.assertEqual(self.dispatch("/api/periph-exec" + suffix,
                                     history=False, method=method), "not-found")

    def test_rule_endpoints_and_delete_query_remain_available(self):
        for suffix, method, expected in (("?page=1", "GET", "rules"),
                ("", "JSON", "add-json"), ("", "FORM", "add-form"),
                ("/update", "JSON", "update-json"), ("/update", "FORM", "update-form"),
                ("/preflight", "JSON", "preflight-json"), ("/run", "FORM", "run"),
                ("/?id=rule-123", "DELETE", "delete"), ("/controls", "GET", "controls")):
            with self.subTest(suffix=suffix, method=method):
                self.assertEqual(self.dispatch("/api/periph-exec" + suffix,
                                 history=False, method=method), expected)

    def test_plain_root_mutation_reproduces_c3_history_alias(self):
        for method, expected in (("GET", "rules"), ("JSON", "add-json"), ("FORM", "add-form")):
            self.assertEqual(self.dispatch("/api/periph-exec/history/config", "plain-root-regression",
                             history=False, method=method), expected)


if __name__ == "__main__":
    unittest.main()
