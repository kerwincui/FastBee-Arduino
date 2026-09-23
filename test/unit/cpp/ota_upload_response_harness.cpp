#include <cstdlib>
#include <iostream>

#define CHECK(condition) do { if (!(condition)) { std::cerr << "check failed at " << __LINE__ << ": " << #condition << "\n"; std::exit(1); } } while (0)
static const char* T_text_plain = "text/plain";
class AsyncWebServerRequest;
class AsyncWebServerResponse {
public:
    explicit AsyncWebServerResponse(int status) : code(status) {}
    int code;
    bool _sourceValid() const { return true; }
    void _respond(AsyncWebServerRequest* request);
};
struct TestClient { void setRxTimeout(unsigned) {} };
class AsyncWebServerRequest {
public:
    bool _sent = false, _paused = false;
    AsyncWebServerResponse* _response = nullptr;
    TestClient client;
    TestClient* _client = &client;
    int wireCode = 0;
    ~AsyncWebServerRequest() { delete _response; }
    bool isSent() const { return _sent; }
    AsyncWebServerResponse* getResponse() const { return _response; }
    void send(int status, const char*, const char*) { send(new AsyncWebServerResponse(status)); }
    void send(AsyncWebServerResponse* response);
    void _send();
};
void AsyncWebServerResponse::_respond(AsyncWebServerRequest* request) { request->wireCode = code; }
#include "production_library_send.inc"

struct TestContext {
    unsigned errors = 0;
    void sendError(AsyncWebServerRequest* request, int code, const char* message) {
        ++errors;
        request->send(code, "application/json", message);
    }
};
static void finishMultipart(TestContext* ctx, AsyncWebServerRequest* request) {
#include "production_ota_completion.inc"
}

int main() {
    // Run the actual library send/_send implementation: send queues the
    // response during upload; multipart completion has not sent it yet.
    for (const int status : {200, 500, 401}) {
        TestContext context;
        AsyncWebServerRequest request;
        request.send(status, "application/json", "upload result");
        AsyncWebServerResponse* queued = request.getResponse();
        CHECK(queued && !request.isSent() && request.wireCode == 0);
        finishMultipart(&context, &request);
        CHECK(context.errors == 0 && request.getResponse() == queued);
        request._send();
        CHECK(request.isSent() && request.wireCode == status);
        finishMultipart(&context, &request);
        CHECK(context.errors == 0 && request.wireCode == status);
    }
    {
        TestContext context;
        AsyncWebServerRequest empty;
        finishMultipart(&context, &empty);
        CHECK(context.errors == 1 && empty.getResponse());
        CHECK(!empty.isSent() && empty.getResponse()->code == 400);
        empty._send();
        CHECK(empty.isSent() && empty.wireCode == 400);
    }
    {
        TestContext context;
        AsyncWebServerRequest alreadyClosed;
        alreadyClosed._sent = true;
        finishMultipart(&context, &alreadyClosed);
        CHECK(context.errors == 0 && !alreadyClosed.getResponse());
    }
    std::cout << "Production OTA completion: queued 200/500/401 preserved, empty request 400, already sent unchanged\n";
}
