#define millis realMillis
#include <Arduino.h>
#include "network/LiteralStringMatch.h"
#undef millis
#include <cassert>
#include <cctype>
#include "security/OutputActorIdentity.h"
static uint32_t clockMs = 1000;
uint32_t millis() { return clockMs; }
struct SessionsLock { explicit SessionsLock(void*) {} };
struct AuthResult { bool success=false; String username,sessionId,errorMessage; uint64_t outputActor=0; };
struct SessionStatus { bool valid=false,expired=false,locked=false; String username; };
struct User { bool enabled=true; };
struct Users { User user; User* getUser(const String&) { return &user; } };
struct UserSession { String username; uint32_t lastAccessTime=0,expireTime=0; bool isActive=true; uint64_t outputActor=0; };
PRODUCTION_SECURITY_CONFIG
struct AsyncWebServerResponse {
    int status=200; bool started=false; std::map<String,String> headers;
    int code() const { return status; }
    bool _started() const { return started; }
    const String* getHeader(const char* name) const { auto i=headers.find(name); return i==headers.end()?nullptr:&i->second; }
    bool hasHeader(const char* name) const { return headers.count(name)!=0; }
    void addHeader(const char* name,const String& value) { headers[name]=value; }
};
enum class AsyncAuthType { AUTH_NONE, AUTH_BEARER };
struct HeaderName : String {
    explicit HeaderName(const String& name):String(name) {}
    bool equalsIgnoreCase(const char* other) const {
        std::string a=c_str(),b=other;
        auto lower=[](unsigned char c){return static_cast<char>(std::tolower(c));};
        std::transform(a.begin(),a.end(),a.begin(),lower);
        std::transform(b.begin(),b.end(),b.begin(),lower);
        return a==b;
    }
};
struct Header {
    String key,val; HeaderName name() const {return HeaderName(key);} String value() const {return val;}
};
struct AsyncWebServerRequest {
    AsyncWebServerResponse response; bool authenticated=false,hasResponse=true;
    std::vector<Header> headers={{"Cookie","sessionId=test-session"}};
    String bearer, path="/api/system/info";
    AsyncAuthType authType() const {return bearer.isEmpty()?AsyncAuthType::AUTH_NONE:AsyncAuthType::AUTH_BEARER;}
    String authChallenge() const {return bearer;}
    const std::vector<Header>& getHeaders() const {return headers;}
    bool hasHeader(const char* key) const {for(const auto& h:headers) if(h.key==key) return true; return false;}
    String header(const char* key) const {for(const auto& h:headers) if(h.key==key) return h.val; return "";}
    void setAttribute(const char*,bool value) {authenticated=value;}
    bool getAttribute(const char*,bool) const { return authenticated; }
    AsyncWebServerResponse* getResponse() { return hasResponse ? &response : nullptr; }
    String url() const { return path; }
};
struct AuthManager {
    void* _sessionsMutex=nullptr; uint32_t lastSessionCleanup=0; bool locked=false;
    SecurityConfig securityConfig; Users* userManager=nullptr;
    std::map<String,UserSession> activeSessions;
    bool isAccountLocked(const String&) { return locked; }
    void logAuthEvent(const String&,const String&,const String&,bool) {}
    SessionStatus validateSession(const UserSession&);
    void cleanupExpiredSessions();
    AuthResult verifySession(const String&,bool=true);
    bool updateSessionAccessTime(const String&);
    SecurityConfig getSecurityConfig() const { return securityConfig; }
    static String extractSessionIdFromCookie(AsyncWebServerRequest*,const String&);
    static String extractSessionIdFromRequest(AsyncWebServerRequest*,const String&);
};
struct WebHandlerContext { AuthManager* authManager; void renewSessionCookie(AsyncWebServerRequest*); AuthResult authenticateRequest(AsyncWebServerRequest*); };
PRODUCTION_METHODS
PRODUCTION_RENEW
void loginCookie(AsyncWebServerResponse* response, SecurityConfig securityConfig) {
    AuthResult result; result.sessionId="test-session";
PRODUCTION_LOGIN_COOKIE
}
void seed(AuthManager& manager) {
    manager.activeSessions["test-session"]={"admin",clockMs,uint32_t(clockMs+manager.securityConfig.sessionTimeout),true};
}
int main(int argc,char** argv) {
    assert(argc==2); const std::string testCase=argv[1];
    AuthManager manager; WebHandlerContext ctx{&manager}; seed(manager);
    if(testCase=="output-actor") {
        const auto first=manager.verifySession("test-session",false);
        assert(first.success && first.outputActor);
        assert(manager.verifySession("test-session",true).outputActor==first.outputActor);
        assert(manager.verifySession("test-session",false).outputActor==first.outputActor);
        const auto missing=manager.verifySession("not-a-session");
        assert(!missing.success && !missing.outputActor);
        manager.activeSessions["other-session"]={"admin",clockMs,uint32_t(clockMs+3600000),true};
        const auto other=manager.verifySession("other-session");
        assert(other.success && other.outputActor>first.outputActor);
        manager.activeSessions.clear(); seed(manager);
        const auto relogin=manager.verifySession("test-session");
        assert(relogin.outputActor>other.outputActor);
        AuthManager recreated; seed(recreated);
        assert(recreated.verifySession("test-session").outputActor>relogin.outputActor);
        clockMs+=manager.securityConfig.sessionTimeout;
        const auto expired=manager.verifySession("test-session");
        assert(!expired.success && !expired.outputActor);
        FastBee::OutputActorSequence exhausted(UINT32_MAX-1);
        assert(exhausted.issue()==UINT32_MAX);
        assert(exhausted.issue()==0 && exhausted.issue()==0);
        return 0;
    }
    if(testCase=="emit-cookie") {
        AsyncWebServerResponse response;loginCookie(&response,manager.securityConfig);
        std::cout << response.headers["Set-Cookie"].c_str() << '\n';
        AsyncWebServerRequest request;assert(ctx.authenticateRequest(&request).success);
        ctx.renewSessionCookie(&request);
        std::cout << request.response.headers["Set-Cookie"].c_str() << '\n';return 0;
    }
    auto noCookie=[&](AsyncWebServerRequest request){ctx.renewSessionCookie(&request);assert(!request.response.hasHeader("Set-Cookie"));};
    if(testCase=="policy") {
        AsyncWebServerRequest request; noCookie(request);
        assert(ctx.authenticateRequest(&request).success);
        for(int status:{401,403,429,503}) {request.response.status=status;noCookie(request);}
        request.response.status=200;request.hasResponse=false;noCookie(request);request.hasResponse=true;
        request.response.started=true;noCookie(request);request.response.started=false;
        for(const char* path:{"/api/auth/login","/api/auth/logout","/api/auth/change-password"}){request.path=path;noCookie(request);}
        request.path="/api/system/info";
        request.response.headers["Set-Cookie"]="sessionId=; Max-Age=0";
        ctx.renewSessionCookie(&request);assert(request.response.headers["Set-Cookie"]=="sessionId=; Max-Age=0");request.response.headers.clear();
        request.bearer="different-token";noCookie(request);request.bearer="test-session";
        request.headers.clear();noCookie(request);request.bearer="";request.headers={{"Cookie","sessionId=test-session"}};
        manager.activeSessions.clear();noCookie(request);seed(manager);
        manager.locked=true;noCookie(request);manager.locked=false;seed(manager);
        Users users;manager.userManager=&users;users.user.enabled=false;noCookie(request);
        ctx.renewSessionCookie(nullptr);ctx.authManager=nullptr;noCookie(request);
        assert(!ctx.authenticateRequest(&request).success);return 0;
    }
    if(testCase=="parsing") {
        AsyncWebServerRequest request;manager.securityConfig.cookieName="custom";
        request.headers={{"cookie","other_custom=wrong; other_sessionId=wrong; custom=test-session"}};
        assert(ctx.authenticateRequest(&request).success);
        manager.securityConfig.cookieSecure=true;manager.securityConfig.cookieMaxAge=120;
        ctx.renewSessionCookie(&request);
        assert(request.response.headers["Set-Cookie"]=="custom=test-session; Path=/; Max-Age=120; SameSite=Strict; HttpOnly; Secure");
        assert(request.response.headers["Cache-Control"]=="no-store");
        request.headers={{"Cookie","other_custom=test-session; other_sessionId=test-session"}};
        assert(!ctx.authenticateRequest(&request).success);
        request.headers={{"Cookie","sessionId=test-session"},{"authorization","Bearer invalid"}};
        assert(!ctx.authenticateRequest(&request).success);
        request.headers.pop_back();assert(ctx.authenticateRequest(&request).success);
        request.bearer="invalid";assert(!ctx.authenticateRequest(&request).success);
        assert(!ctx.authenticateRequest(nullptr).success);return 0;
    }
    if(testCase=="idle") {
        clockMs+=manager.securityConfig.sessionTimeout;
        assert(!manager.updateSessionAccessTime("test-session"));
        assert(!manager.verifySession("test-session").success);return 0;
    }
    if(testCase=="wrap") {
        clockMs=UINT32_MAX-1000;manager.activeSessions.clear();seed(manager);
        assert(manager.verifySession("test-session",false).success);
        clockMs+=2000;manager.cleanupExpiredSessions();
        assert(manager.verifySession("test-session").success);
        clockMs+=manager.securityConfig.sessionTimeout;
        manager.cleanupExpiredSessions();assert(manager.activeSessions.empty());return 0;
    }
    AsyncWebServerResponse login; loginCookie(&login,manager.securityConfig);
    const String header=login.headers["Set-Cookie"];
    assert(header.indexOf("Max-Age=3600")>=0 && header.indexOf("HttpOnly")>=0);
    uint64_t cookieExpires=uint64_t(clockMs)+3600000;
    // Two clients' actual 30/60-second poll schedules cross the one-hour boundary.
    const uint32_t interval=testCase=="active-30"?30:60;
    for (uint32_t seconds=interval;seconds<=9*3600;seconds+=interval) {
        clockMs=1000+seconds*1000;
        if (clockMs>=cookieExpires) {
            assert(manager.verifySession("test-session",false).success);
            std::cerr << "Active cookie expired at " << seconds << " seconds while server session remained valid\n";
            return 1;
        }
        AsyncWebServerRequest request;
        assert(ctx.authenticateRequest(&request).success);
        ctx.renewSessionCookie(&request);
        if (request.response.hasHeader("Set-Cookie")) {
            assert(request.response.headers["Set-Cookie"]==header);
            cookieExpires=uint64_t(clockMs)+3600000;
        }
    }
    // Read-only verification must not postpone inactivity expiry.
    clockMs+=3600001;
    assert(!manager.verifySession("test-session",false).success);
    AsyncWebServerRequest expired; ctx.renewSessionCookie(&expired);
    assert(!expired.response.hasHeader("Set-Cookie"));
    std::cout << "active-cookie 9h and idle expiry passed\n";
}
