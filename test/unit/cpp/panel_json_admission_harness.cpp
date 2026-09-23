#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <string>
#include <type_traits>
#include <vector>

using String = std::string;

struct AsyncWebServerResponse {
    int status = 0;
    String message;
};

struct AsyncWebServerRequest {
    AsyncWebServerResponse responseStorage;
    AsyncWebServerResponse* response = nullptr;
    int responseWrites = 0;
    bool delegateBufferAllocated = false;
    bool delegateBodyComplete = false;

    AsyncWebServerResponse* getResponse() const { return response; }

    void respond(int status, const String& message) {
        responseStorage.status = status;
        responseStorage.message = message;
        response = &responseStorage;
        ++responseWrites;
    }
};

class AsyncWebHandler {
public:
    virtual ~AsyncWebHandler() = default;
    virtual bool canHandle(AsyncWebServerRequest*) const { return false; }
    virtual void handleBody(AsyncWebServerRequest*, uint8_t*, size_t, size_t,
                            size_t) {}
    virtual void handleRequest(AsyncWebServerRequest*) {}
};

class JsonVariant {};
using ArJsonRequestHandlerFunction =
    std::function<void(AsyncWebServerRequest*, JsonVariant&)>;

constexpr int HTTP_POST = 2;

struct DelegateMetrics {
    int canHandleCalls = 0;
    int bodyCalls = 0;
    int allocations = 0;
    int requestCalls = 0;
    int parses = 0;
    int callbacks = 0;
    int method = 0;
    size_t maxContentLength = 0;
};

static DelegateMetrics gDelegate;

class AsyncCallbackJsonWebHandler final : public AsyncWebHandler {
public:
    AsyncCallbackJsonWebHandler(const char*, ArJsonRequestHandlerFunction callback)
        : callback_(std::move(callback)) {}

    void setMethod(int method) { gDelegate.method = method; }
    void setMaxContentLength(size_t limit) {
        gDelegate.maxContentLength = limit;
    }

    bool canHandle(AsyncWebServerRequest*) const override {
        ++gDelegate.canHandleCalls;
        return gDelegate.method == HTTP_POST;
    }

    void handleBody(AsyncWebServerRequest* request, uint8_t*, size_t len,
                    size_t index, size_t total) override {
        ++gDelegate.bodyCalls;
        if (total > gDelegate.maxContentLength) return;
        if (index == 0) {
            ++gDelegate.allocations;
            request->delegateBufferAllocated = true;
        }
        if (request->delegateBufferAllocated && index + len >= total) {
            request->delegateBodyComplete = true;
        }
    }

    void handleRequest(AsyncWebServerRequest* request) override {
        ++gDelegate.requestCalls;
        if (!request->delegateBufferAllocated ||
            !request->delegateBodyComplete) {
            request->respond(400, "delegate missing body");
            return;
        }
        ++gDelegate.parses;
        JsonVariant json;
        ++gDelegate.callbacks;
        callback_(request, json);
    }

private:
    ArJsonRequestHandlerFunction callback_;
};

class WebHandlerContext {
public:
    bool authenticated = true;
    bool developerMode = true;
    int authChecks = 0;
    int developerChecks = 0;
    int errors = 0;

    bool requireAuth(AsyncWebServerRequest* request) {
        ++authChecks;
        if (authenticated) return true;
        sendError(request, 401, "Unauthorized");
        return false;
    }

    bool requireDeveloperMode(AsyncWebServerRequest* request) {
        ++developerChecks;
        if (developerMode) return true;
        sendError(request, 403, "Developer mode is disabled");
        return false;
    }

    void sendError(AsyncWebServerRequest* request, int status,
                   const String& message) {
        ++errors;
        request->respond(status, message);
    }
};

namespace HandlerUtils {
static bool rejectForLowMemory = false;
static int checks = 0;
static size_t lastThreshold = 0;

bool checkLowMemory(AsyncWebServerRequest* request, size_t threshold) {
    ++checks;
    lastThreshold = threshold;
    if (!rejectForLowMemory) return false;
    request->respond(503, "Low memory");
    return true;
}
}  // namespace HandlerUtils

struct EspMock {
    size_t maxAllocHeap = 1024 * 1024;
    size_t getMaxAllocHeap() const { return maxAllocHeap; }
};

static EspMock ESP;

// This include is generated verbatim from ScreenPanelRouteHandler.cpp.
#include "panel_json_handler_under_test.inc"

static_assert(std::is_base_of_v<AsyncWebHandler, PanelJsonHandler>);
static_assert(std::is_same_v<decltype(&PanelJsonHandler::canHandle),
                             bool (PanelJsonHandler::*)(
                                 AsyncWebServerRequest*) const>);

static int gApplicationCallbacks = 0;

static void resetMetrics() {
    gDelegate = {};
    HandlerUtils::rejectForLowMemory = false;
    HandlerUtils::checks = 0;
    HandlerUtils::lastThreshold = 0;
    ESP.maxAllocHeap = 1024 * 1024;
    gApplicationCallbacks = 0;
}

static PanelJsonHandler makeHandler(WebHandlerContext& context, size_t limit) {
    return PanelJsonHandler(
        &context, "/api/screen-panel/config",
        [](AsyncWebServerRequest* request, JsonVariant&) {
            ++gApplicationCallbacks;
            request->respond(200, "saved");
        },
        limit);
}

static void feedFirstChunk(PanelJsonHandler& handler,
                           AsyncWebServerRequest& request, size_t total) {
    uint8_t data[] = {'{', '}'};
    handler.handleBody(&request, data, sizeof(data), 0, total);
}

static void assertRejectedBeforeDelegate(PanelJsonHandler& handler,
                                         AsyncWebServerRequest& request,
                                         int expectedStatus) {
    assert(request.getResponse() != nullptr);
    assert(request.getResponse()->status == expectedStatus);
    assert(gDelegate.bodyCalls == 0);
    assert(gDelegate.allocations == 0);
    assert(gDelegate.requestCalls == 0);
    assert(gDelegate.parses == 0);
    assert(gDelegate.callbacks == 0);
    assert(gApplicationCallbacks == 0);

    AsyncWebServerResponse* rejectedResponse = request.getResponse();
    const String rejectedMessage = rejectedResponse->message;
    const int responseWrites = request.responseWrites;
    handler.handleRequest(&request);

    assert(request.getResponse() == rejectedResponse);
    assert(request.getResponse()->status == expectedStatus);
    assert(request.getResponse()->message == rejectedMessage);
    assert(request.responseWrites == responseWrites);
    assert(gDelegate.requestCalls == 0);
    assert(gDelegate.parses == 0);
    assert(gApplicationCallbacks == 0);
}

static void unauthenticatedRequestIsRejected() {
    resetMetrics();
    WebHandlerContext context;
    context.authenticated = false;
    auto handler = makeHandler(context, 64);
    AsyncWebServerRequest request;

    feedFirstChunk(handler, request, 2);

    assert(context.authChecks == 1);
    assert(context.developerChecks == 0);
    assert(HandlerUtils::checks == 0);
    assertRejectedBeforeDelegate(handler, request, 401);
}

static void nonDeveloperRequestIsRejected() {
    resetMetrics();
    WebHandlerContext context;
    context.developerMode = false;
    auto handler = makeHandler(context, 64);
    AsyncWebServerRequest request;

    feedFirstChunk(handler, request, 2);

    assert(context.authChecks == 1);
    assert(context.developerChecks == 1);
    assert(HandlerUtils::checks == 0);
    assertRejectedBeforeDelegate(handler, request, 403);
}

static void oversizedBodyIsRejected() {
    resetMetrics();
    WebHandlerContext context;
    auto handler = makeHandler(context, 64);
    AsyncWebServerRequest request;

    feedFirstChunk(handler, request, 65);

    assert(context.authChecks == 1);
    assert(context.developerChecks == 1);
    assert(HandlerUtils::checks == 0);
    assertRejectedBeforeDelegate(handler, request, 413);
}

static void lowMemoryBodyIsRejected() {
    resetMetrics();
    WebHandlerContext context;
    auto handler = makeHandler(context, 64);
    AsyncWebServerRequest request;
    HandlerUtils::rejectForLowMemory = true;

    feedFirstChunk(handler, request, 24);

    assert(HandlerUtils::checks == 1);
    assert(HandlerUtils::lastThreshold == 24 * 2 + 8192);
    assertRejectedBeforeDelegate(handler, request, 503);
}

static void unavailableContiguousBufferIsRejected() {
    resetMetrics();
    WebHandlerContext context;
    auto handler = makeHandler(context, 64);
    AsyncWebServerRequest request;
    ESP.maxAllocHeap = 24;

    feedFirstChunk(handler, request, 24);

    assert(HandlerUtils::checks == 1);
    assertRejectedBeforeDelegate(handler, request, 503);
}

static void validBodyReachesDelegateAndApplication() {
    resetMetrics();
    WebHandlerContext context;
    auto handler = makeHandler(context, 64);
    AsyncWebServerRequest request;
    const PanelJsonHandler& constHandler = handler;
    assert(constHandler.canHandle(&request));
    assert(gDelegate.canHandleCalls == 1);
    assert(gDelegate.method == HTTP_POST);
    assert(gDelegate.maxContentLength == 64);

    uint8_t first[] = {'{', '"', 'x', '"'};
    uint8_t second[] = {':', '1', '}'};
    handler.handleBody(&request, first, sizeof(first), 0, 7);
    handler.handleBody(&request, second, sizeof(second), 4, 7);

    assert(request.getResponse() == nullptr);
    assert(context.authChecks == 1);
    assert(context.developerChecks == 1);
    assert(HandlerUtils::checks == 1);
    assert(gDelegate.bodyCalls == 2);
    assert(gDelegate.allocations == 1);
    assert(gDelegate.parses == 0);

    handler.handleRequest(&request);

    assert(context.authChecks == 2);
    assert(context.developerChecks == 2);
    assert(gDelegate.requestCalls == 1);
    assert(gDelegate.parses == 1);
    assert(gDelegate.callbacks == 1);
    assert(gApplicationCallbacks == 1);
    assert(request.getResponse() != nullptr);
    assert(request.getResponse()->status == 200);
    assert(request.getResponse()->message == "saved");
}

int main() {
    unauthenticatedRequestIsRejected();
    nonDeveloperRequestIsRejected();
    oversizedBodyIsRejected();
    lowMemoryBodyIsRejected();
    unavailableContiguousBufferIsRejected();
    validBodyReachesDelegateAndApplication();
    std::cout << "panel JSON admission checks passed\n";
}
