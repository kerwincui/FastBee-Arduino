#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

static void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "%s\n", message);
        std::fflush(stderr);
        std::_Exit(1);
    }
}

// Track every String instance, including empty strings. Long payloads ensure
// all three paths exercise dynamic ownership, without claiming ESP heap sizes.
class String {
public:
    static std::set<const String*> live;
    static unsigned destroyed;
    std::string value;
    String() { live.insert(this); }
    String(const char* text) : value(text) { live.insert(this); }
    String(const std::string& text) : value(text) { live.insert(this); }
    String(uint32_t number) : value(std::to_string(number)) { live.insert(this); }
    String(const String& other) : value(other.value) { live.insert(this); }
    String& operator=(const String& other) { value = other.value; return *this; }
    ~String() { require(live.erase(this) == 1, "String destroyed twice"); ++destroyed; }
    bool isEmpty() const { return value.empty(); }
    String& operator+=(const char* text) { value += text; return *this; }
    friend String operator+(const String& left, const String& right) {
        return String(left.value + right.value);
    }
};
std::set<const String*> String::live;
unsigned String::destroyed = 0;

PRODUCTION_AUTH_RESULT
PRODUCTION_SECURITY_CONFIG
PRODUCTION_STACK

static std::string testCase;
static const std::string token(44, 'T');
static const std::string account = "lifecycle-test-account-with-long-name";
static const std::string rejection = "Rejected credentials with a dynamically owned error message";
static unsigned loginCalls = 0, successResponses = 0, errorResponses = 0;
static unsigned slotReturns = 0, notifications = 0, metrics = 0;
static size_t baselineStrings = 0;
static std::string sentCookie, sentBody, sentError;

struct JsonDocument {
    struct Value {
        std::string value;
        Value& operator=(bool flag) { value = flag ? "true" : "false"; return *this; }
        Value& operator=(const String& text) { value = text.value; return *this; }
    };
    std::map<std::string, Value> fields;
    Value& operator[](const char* key) { return fields[key]; }
};
static void serializeJson(const JsonDocument& document, String& output) {
    std::string result;
    for (const auto& field : document.fields) result += field.first + "=" + field.second.value + ";";
    output = String(result);
}

struct AsyncWebServerResponse {
    std::map<std::string, std::string> headers;
    void addHeader(const char* name, const String& text) { headers[name] = text.value; }
};
struct AsyncWebServerRequest {
    std::unique_ptr<AsyncWebServerResponse> response;
    AsyncWebServerResponse* beginResponse(int code, const char* mime, const String& body) {
        require(code == 200 && std::string(mime) == "application/json", "Wrong success response");
        sentBody = body.value;
        response = std::make_unique<AsyncWebServerResponse>();
        return response.get();
    }
    void send(AsyncWebServerResponse* sent) {
        require(sent == response.get(), "Wrong response ownership");
        require(sent->headers["Connection"] == "close", "Missing connection close");
        sentCookie = sent->headers["Set-Cookie"];
        ++successResponses;
    }
};
using AsyncWebServerRequestPtr = std::weak_ptr<AsyncWebServerRequest>;

struct AuthManager {
    AuthResult login(const String& username, const String& password,
                     const String& ip, const String& userAgent) {
        require(notifications == 1, "Login ran before task notification");
        require(username.value == account && password.value.size() > 16 &&
                !ip.isEmpty() && userAgent.value.size() > 80, "Missing owned login inputs");
        ++loginCalls;
        AuthResult result;
        result.success = testCase != "rejected";
        result.username = String(account);
        if (result.success) result.sessionId = String(token);
        else result.errorMessage = String(rejection);
        return result;
    }
    SecurityConfig getSecurityConfig() { return SecurityConfig(); }
};
struct WebHandlerContext {
    AuthManager* authManager;
    void sendError(AsyncWebServerRequest* request, int code, const String& error) {
        require(request && code == 401, "Wrong rejection response");
        sentError = error.value;
        ++errorResponses;
    }
};

using SemaphoreHandle_t = void*;
using UBaseType_t = unsigned;
static constexpr bool pdTRUE = true;
static constexpr uint32_t portMAX_DELAY = UINT32_MAX;
static void ulTaskNotifyTake(bool clear, uint32_t timeout) {
    require(clear && timeout == portMAX_DELAY, "Incorrect task wait");
    ++notifications;
}
static void xSemaphoreGive(SemaphoreHandle_t slot) {
    require(slot != nullptr, "Missing login slot");
    ++slotReturns;
}
static UBaseType_t uxTaskGetStackHighWaterMark(void* task) {
    require(!task, "Wrong stack metrics task");
    return 2048;
}
struct SerialStub {
    void printf(const char*, unsigned long stack, unsigned long freeBytes, unsigned long used) {
        require(stack == LOGIN_TASK_STACK_BYTES && freeBytes == 2048 &&
                used == stack - freeBytes, "Incorrect login stack metrics");
        ++metrics;
    }
} Serial;

[[noreturn]] static void vTaskDelete(void* task) {
    require(!task, "Task must delete itself");
    // Check while the task frame still exists. _Exit deliberately does not
    // return, throw or unwind: the old production task must fail right here.
    if (String::live.size() != baselineStrings) {
        size_t retainedPayload = 0;
        for (const auto* text : String::live) retainedPayload += text->value.size();
        std::fprintf(stderr, "ownership remains at task deletion: strings=%zu baseline=%zu payload=%zu\n",
                     String::live.size(), baselineStrings, retainedPayload);
        std::fflush(stderr);
        std::_Exit(1);
    }
    require(String::destroyed >= 7, "Job/result strings were not destroyed");
    require(loginCalls == 1 && slotReturns == 1 && notifications == 1 && metrics == 1,
            "Incomplete or duplicate task cleanup");
    if (testCase == "success") {
        require(successResponses == 1 && errorResponses == 0, "Incorrect success path");
        require(sentCookie == "sessionId=" + token + "; Path=/; Max-Age=3600; SameSite=Strict; HttpOnly",
                "Changed login cookie behavior");
        require(sentBody.find("sessionId=" + token) != std::string::npos &&
                sentBody.find("username=" + account) != std::string::npos &&
                sentBody.find("success=true") != std::string::npos, "Missing API login fields");
    } else if (testCase == "rejected") {
        require(successResponses == 0 && errorResponses == 1 && sentError == rejection,
                "Incorrect rejection path");
    } else {
        require(successResponses == 0 && errorResponses == 0 && sentCookie.empty(),
                "Disconnected request received a response");
    }
    std::puts("ownership released before task deletion");
    std::fflush(stdout);
    std::_Exit(0);
}

PRODUCTION_LOGIN_JOB
PRODUCTION_LOGIN_TASK

int main(int argc, char** argv) {
    require(argc == 2, "Expected one test case");
    testCase = argv[1];
    require(testCase == "success" || testCase == "rejected" || testCase == "disconnected",
            "Unknown test case");
    AuthManager auth;
    WebHandlerContext context{&auth};
    auto request = std::make_shared<AsyncWebServerRequest>();
    baselineStrings = String::live.size();
    auto* job = new LoginJob();
    job->ctx = &context;
    job->held = request;
    job->slot = &context;
    job->username = String(account);
    job->password = String("long-test-password-owned-by-login-job");
    job->ipAddress = String("192.0.2.123");
    job->userAgent = String(std::string(128, 'U'));
    job->apiClient = true;
    if (testCase == "disconnected") request.reset();
    loginTask(job);
    require(false, "Self-deleting task returned");
}
