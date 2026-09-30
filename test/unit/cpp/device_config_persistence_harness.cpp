#include <Arduino.h>
#include <ArduinoJson.h>
#include <network/DeviceConfigValidation.h>
#include <cassert>
#include <map>
#include <string>

static const char* DEVICE_CONFIG_FILE="/config/device.json";
struct FakeFS {
    std::map<std::string,std::string> files;
    bool exists(const char* path) { return files.count(path); }
} LittleFS;
struct ConfigStorage {
    bool fail=false;
    int writes=0;
    static ConfigStorage& getInstance() { static ConfigStorage storage; return storage; }
    bool loadJSONConfig(const char* path, JsonDocument& doc) { return !deserializeJson(doc,LittleFS.files[path]); }
    bool saveJSONConfig(const char* path, const JsonDocument& doc) {
        ++writes;
        if(fail) return false;
        std::string text;serializeJson(doc,text);LittleFS.files[path]=text;return true;
    }
};
enum LogLevel { LOG_DEBUG, LOG_INFO, LOG_WARNING, LOG_ERROR };
struct Logger {
    LogLevel level=LOG_INFO;
    void setLogLevel(LogLevel v) { level=v; }
    template<class... T> void infof(T...) {}
    void info(const char*) {}
} LOGGER;
struct Wifi { String macAddress() { return "14:C1:9F:C1:52:D8"; } } WiFi;
#define ESP DeviceConfigTestESP
struct Esp { uint64_t getEfuseMac() { return 0x123456789abc; } } ESP;
struct UserManager {
    struct Policy { uint8_t maxLoginAttempts=5;uint32_t loginLockoutTime=300000;uint8_t minPasswordLength=6;bool requireStrongPasswords=false; } config;
    bool saveFails=false;
    int saves=0;
    bool saveUsersToStorage() { ++saves;return !saveFails; }
    bool updatePasswordPolicy(uint8_t,uint32_t,uint8_t,bool);
};
struct AsyncWebServerRequest {
    std::map<std::string,std::string> params;
    int status=0;
    std::string response;
    bool hasParam(const char* key,bool=false) { return params.count(key); }
};
struct Context {
    bool authorized=true;
    uint32_t cacheDuration=86400;
    UserManager* userManager=nullptr;
    bool requireAuth(AsyncWebServerRequest* request) { if(!authorized)request->status=401;return authorized; }
    void sendError(AsyncWebServerRequest* request,int code,const char* error) { request->status=code;request->response=error; }
    void sendSuccess(AsyncWebServerRequest* request,const char*) { request->status=200; }
    String getParamValue(AsyncWebServerRequest* request,const char* key,const char*) { return request->params[key].c_str(); }
};
namespace HandlerUtils { void sendJsonStream(AsyncWebServerRequest* request,JsonDocument& doc) { request->status=200;serializeJson(doc,request->response); } }
struct DeviceRouteHandler {
    Context* ctx;
    void handleSaveDeviceConfigJson(AsyncWebServerRequest*,JsonVariant&);
    void handleSaveDeviceConfig(AsyncWebServerRequest*);
};
#include "device_save.inc"

int main() {
    UserManager users;Context context;context.userManager=&users;
    DeviceRouteHandler route{&context};
    auto& storage=ConfigStorage::getInstance();
    const std::string original=R"({"deviceName":"original","ntpServer1":"saved.example","timezone":"CUSTOM0","cacheDuration":86400,"developerModeEnabled":true})";
    LittleFS.files[DEVICE_CONFIG_FILE]=original;
    auto save=[&](const char* text){JsonDocument doc;assert(!deserializeJson(doc,text));JsonVariant body=doc.as<JsonVariant>();AsyncWebServerRequest request;route.handleSaveDeviceConfigJson(&request,body);return request.status;};
    for(const char* invalid:{"{}","[]","null",R"({"productNumber":1.5})",R"({"productNumber":-1})",R"({"productNumber":"12x"})",R"({"productNumber":"999999999999999999999999"})",R"({"syncInterval":1})",R"({"cacheDuration":-1})",R"({"enableNTP":"garbage"})",R"({"logLevel":"TRACE"})",R"({"maxLoginAttempts":256})",R"({"loginLockoutTime":-1})",R"({"minPasswordLength":33})",R"({"requireStrongPasswords":1})",R"({"developerModeEnabled":true})",R"({"deviceName":"mixed","minPasswordLength":8})"}) {
        assert(save(invalid)==400);assert(LittleFS.files[DEVICE_CONFIG_FILE]==original);assert(storage.writes==0);
    }
    storage.fail=true;
    assert(save(R"({"cacheDuration":0,"logLevel":"ERROR"})")==500);
    assert(context.cacheDuration==86400 && LOGGER.level==LOG_INFO && LittleFS.files[DEVICE_CONFIG_FILE]==original);
    storage.fail=false;
    assert(save(R"({"cacheDuration":0,"enableNTP":"false"})")==200);
    JsonDocument committed;assert(!deserializeJson(committed,LittleFS.files[DEVICE_CONFIG_FILE]));
    assert(context.cacheDuration==0 && committed["enableNTP"].as<bool>()==false);
    assert(committed["ntpServer1"]=="saved.example" && committed["developerModeEnabled"].as<bool>());
    AsyncWebServerRequest form;form.params["deviceName"]="legacy-name";route.handleSaveDeviceConfig(&form);assert(form.status==200);
    assert(!deserializeJson(committed,LittleFS.files[DEVICE_CONFIG_FILE]));assert(committed["deviceName"]=="legacy-name" && committed["timezone"]=="CUSTOM0");
    const int deviceWrites=storage.writes;
    users.saveFails=true;
    assert(save(R"({"maxLoginAttempts":3,"minPasswordLength":8})")==500);
    assert(users.config.maxLoginAttempts==5 && users.config.minPasswordLength==6 && storage.writes==deviceWrites);
    users.saveFails=false;
    assert(save(R"({"maxLoginAttempts":"3","loginLockoutTime":"60000","minPasswordLength":"8","requireStrongPasswords":"false"})")==200);
    assert(users.config.maxLoginAttempts==3 && users.config.loginLockoutTime==60000 && users.config.minPasswordLength==8 && !users.config.requireStrongPasswords);
    assert(storage.writes==deviceWrites);
    context.authorized=false;assert(save(R"({"deviceName":"unauthorized"})")==401);context.authorized=true;
    LittleFS.files[DEVICE_CONFIG_FILE]="broken";assert(save(R"({"deviceName":"new"})")==500);assert(LittleFS.files[DEVICE_CONFIG_FILE]=="broken");
    assert(!users.updatePasswordPolicy(0,60000,8,false));
    return 0;
}
