#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include <utility>
struct String : std::string {
    using std::string::string;
    String(const std::string& value):std::string(value){}
    bool isEmpty() const {return empty();}
};
static unsigned long clockNow=100;
unsigned long millis(){return clockNow;}
struct Param {String text;const String& value()const{return text;}};
struct JsonDocument {
    std::map<std::string,size_t> values;
    size_t& operator[](const char* name){return values[name];}
};
struct AsyncWebServerRequest {
    std::map<std::string,Param> params;
    int status=0;String error;JsonDocument result;
    bool hasParam(const char* key,bool)const{return params.count(key)!=0;}
    Param* getParam(const char* key,bool){return &params.at(key);}
};
struct WebHandlerContext {
    bool authorized=true,developer=true;
    bool requireAuth(AsyncWebServerRequest* r){if(!authorized)r->status=401;return authorized;}
    bool requireDeveloperMode(AsyncWebServerRequest* r){if(!developer)r->status=403;return developer;}
    void sendError(AsyncWebServerRequest* r,int code,const char* error){r->status=code;r->error=error;}
    void sendSuccess(AsyncWebServerRequest* r,const JsonDocument& result){r->status=200;r->result=result;}
};
enum class MemoryGuardLevel {SEVERE};
namespace HandlerUtils {
bool pressure=false,lowMemory=false;
bool rejectHeavyRequestOnPressure(AsyncWebServerRequest* r,const char*,MemoryGuardLevel,int){if(pressure)r->status=503;return pressure;}
bool checkLowMemory(AsyncWebServerRequest* r,int){if(lowMemory)r->status=503;return lowMemory;}
void sendJsonError(AsyncWebServerRequest* r,int code,const char* message,int retryAfter){r->status=code;r->error=message;r->result.values["retryAfter"]=retryAfter;}
}
struct Files {
    std::map<std::string,std::string> data;
    size_t writeLimit=static_cast<size_t>(-1),writes=0,opens=0;
    bool openFailure=false;
} files;
class File {
    std::string path;size_t cursor=0;bool valid=false;
public:
    File()=default;
    File(std::string p,bool v):path(std::move(p)),valid(v){}
    explicit operator bool()const{return valid;}
    void close(){valid=false;}
    void flush(){}
    size_t size()const{return valid?files.data.at(path).size():0;}
    bool seek(size_t offset){if(!valid||offset>size())return false;cursor=offset;return true;}
    int read(){if(!valid||cursor>=size())return -1;return static_cast<uint8_t>(files.data.at(path)[cursor++]);}
    size_t print(const String& text){if(!valid)return 0;++files.writes;const auto n=std::min(files.writeLimit,text.size());files.data[path].append(text.data(),n);return n;}
};
struct LittleFs {
    File open(const char* path,const char* mode){
        ++files.opens;if(files.openFailure)return {};
        if(mode[0]=='w')files.data[path].clear();
        if(mode[0]=='a'&&!files.data.count(path))files.data[path]="";
        return File(path,files.data.count(path)!=0);
    }
    bool remove(const char* path){return files.data.erase(path)>0;}
} LittleFS;
constexpr const char* primary="/config/periph_exec.json";
constexpr const char* staging="/config/periph_exec.upload.tmp";
class PeriphExecManager {
public:
    bool pending=false,commitSuccess=true;size_t commits=0;
    String failure="mock validation or atomic commit failure";
    static PeriphExecManager& getInstance(){static PeriphExecManager m;return m;}
    bool isConfigurationRestartPending()const{return pending;}
    bool importConfigurationFile(const char* path,String& error){
        ++commits;if(!commitSuccess){error=failure;return false;}
        files.data[primary]=files.data.at(path);files.data.erase(path);pending=true;return true;
    }
};
class PeriphExecRouteHandler {
public:
    WebHandlerContext* ctx;
#include "production_import_fields.inc"
    explicit PeriphExecRouteHandler(WebHandlerContext* context):ctx(context){}
    void handleImportChunk(AsyncWebServerRequest* request);
};
#include "production_import_chunk.inc"
static String sessionA(32,'a'),sessionB(32,'b');
static AsyncWebServerRequest request(const String& session,size_t offset,size_t total,const char* chunk){
    AsyncWebServerRequest r;r.params["session"].text=session;
    r.params["offset"].text=std::to_string(offset);r.params["size"].text=std::to_string(total);r.params["chunk"].text=chunk;return r;
}
static void reset(){
    files={};files.data[primary]="original-primary";
    PeriphExecManager::getInstance()={};HandlerUtils::pressure=false;HandlerUtils::lowMemory=false;clockNow=100;
}
static void gates(){
    reset();WebHandlerContext ctx;PeriphExecRouteHandler h(&ctx);
    auto r=request(sessionA,0,6,"abc");ctx.authorized=false;h.handleImportChunk(&r);assert(r.status==401&&files.opens==0);
    ctx.authorized=true;ctx.developer=false;h.handleImportChunk(&r);assert(r.status==403&&files.opens==0);
    ctx.developer=true;HandlerUtils::pressure=true;h.handleImportChunk(&r);assert(r.status==503&&files.opens==0);
    HandlerUtils::pressure=false;HandlerUtils::lowMemory=true;h.handleImportChunk(&r);assert(r.status==503&&files.opens==0);
    HandlerUtils::lowMemory=false;r.params.erase("chunk");h.handleImportChunk(&r);assert(r.status==400&&files.opens==0);
    for(auto invalid: {request("bad",0,6,"abc"),request(sessionA,0,0,"abc"),request(sessionA,0,98305,"abc"),
                       request(sessionA,7,6,"abc"),request(sessionA,5,6,"ab"),request(sessionA,0,6,"")}) {
        h.handleImportChunk(&invalid);assert(invalid.status==400&&files.opens==0);
    }
    auto negative=request(sessionA,0,6,"abc");negative.params["offset"].text="-1";
    h.handleImportChunk(&negative);assert(negative.status==400&&files.opens==0);
    auto overChunk=request(sessionA,0,2000,"a");overChunk.params["chunk"].text=String(1537,'z');
    h.handleImportChunk(&overChunk);assert(overChunk.status==400&&files.opens==0);
    assert(files.data.at(primary)=="original-primary");
}
static void retryAndCommit(){
    reset();WebHandlerContext ctx;PeriphExecRouteHandler h(&ctx);auto& mgr=PeriphExecManager::getInstance();
    auto r=request(sessionA,0,6,"abc");h.handleImportChunk(&r);
    assert(r.status==200&&r.result.values["received"]==3&&!r.result.values["saved"]);
    assert(files.data.at(primary)=="original-primary");
    const auto writes=files.writes;h.handleImportChunk(&r);assert(r.status==200&&files.writes==writes);
    auto wrong=request(sessionA,0,6,"xyz");h.handleImportChunk(&wrong);assert(wrong.status==409&&files.data.at(staging)=="abc");
    auto gap=request(sessionA,5,6,"z");h.handleImportChunk(&gap);assert(gap.status==409&&files.writes==writes);
    auto sizeChange=request(sessionA,3,7,"def");h.handleImportChunk(&sizeChange);assert(sizeChange.status==409);
    auto other=request(sessionB,0,6,"xyz");h.handleImportChunk(&other);assert(other.status==409&&files.data.at(staging)=="abc");
    auto last=request(sessionA,3,6,"def");h.handleImportChunk(&last);
    assert(last.status==200&&last.result.values["saved"]&&last.result.values["restartRequired"]&&!last.result.values["applied"]);
    assert(files.data.at(primary)=="abcdef"&&mgr.commits==1&&mgr.pending);
    h.handleImportChunk(&last);assert(last.status==200&&mgr.commits==1);
    h.handleImportChunk(&r);assert(r.status==200&&mgr.commits==1);
    auto differentFinal=request(sessionA,3,6,"XYZ");h.handleImportChunk(&differentFinal);
    if(differentFinal.status!=409)std::exit(86); // explicit legacy-branch negative control
    assert(differentFinal.status==409&&mgr.commits==1&&files.data.at(primary)=="abcdef");
    clockNow+=60001;h.handleImportChunk(&other);assert(other.status==409&&mgr.commits==1);
}
static void failuresAndTimeout(){
    reset();WebHandlerContext ctx;PeriphExecRouteHandler h(&ctx);auto& mgr=PeriphExecManager::getInstance();
    auto r=request(sessionA,0,6,"abc");files.writeLimit=1;h.handleImportChunk(&r);
    assert(r.status==500&&h.importSession.empty()&&!files.data.count(staging)&&files.data.at(primary)=="original-primary"&&mgr.commits==0);
    files.writeLimit=static_cast<size_t>(-1);h.handleImportChunk(&r);assert(r.status==200);
    auto other=request(sessionB,0,6,"XYZ");clockNow+=59999;h.handleImportChunk(&other);assert(other.status==409);
    clockNow+=2;h.handleImportChunk(&other);assert(other.status==200&&files.data.at(staging)=="XYZ");
    auto last=request(sessionB,3,6,"123");mgr.commitSuccess=false;h.handleImportChunk(&last);
    assert(last.status==422&&h.importSession.empty()&&!files.data.count(staging)&&files.data.at(primary)=="original-primary");
    assert(mgr.commits==1&&!mgr.pending);
    mgr.pending=true;h.handleImportChunk(&r);assert(r.status==409&&files.data.at(primary)=="original-primary");
    reset();PeriphExecRouteHandler unavailable(&ctx);files.openFailure=true;
    unavailable.handleImportChunk(&r);assert(r.status==500&&files.data.at(primary)=="original-primary");
}
static void transientCommitPressure(){
    for (const char* error : {"CONFIG_MEMORY_PRESSURE", "RULE_SERVICE_BUSY"}) {
        reset();WebHandlerContext ctx;PeriphExecRouteHandler h(&ctx);
        auto& mgr=PeriphExecManager::getInstance();
        auto first=request(sessionA,0,6,"abc");h.handleImportChunk(&first);
        auto last=request(sessionA,3,6,"def");mgr.commitSuccess=false;mgr.failure=error;
        h.handleImportChunk(&last);
        assert(last.status==503 && last.result.values["retryAfter"]==5);
        assert(files.data.at(staging)=="abcdef" && files.data.at(primary)=="original-primary");
        assert(h.importReceived==6 && !h.importCommitted && mgr.commits==1);
        const auto writes=files.writes;mgr.commitSuccess=true;h.handleImportChunk(&last);
        assert(last.status==200 && files.writes==writes && mgr.commits==2 && h.importCommitted);
        assert(files.data.at(primary)=="abcdef");
    }
}
int main(){gates();retryAndCommit();failuresAndTimeout();transientCommitPressure();std::cout<<"import chunk production route gates/ranges/retries/short-write/session/commit passed\n";}
