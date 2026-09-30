// Compile the production startOTA body against observable transport doubles.
#include <Arduino.h>
#include <ArduinoJson.h>
#include <cassert>
#include <functional>
#include <memory>
#include "protocols/FastBeeOTAProtocol.h"

struct Scenario {
    bool network = true, memory = true, begin = true, secure = false;
    int created = 0, tlsCreated = 0, alive = 0, getCalls = 0, timeReads = 0;
    int gateCalls = 0, ended = 0, flashed = 0, scheduled = 0;
    int response = 200;
    long length = 4096;
    size_t received = 4096, offset = 0;
    time_t now = 1750000000;
} scenario;

enum class NetworkType { NET_WIFI, NET_4G };
class FBNetworkManager {
public:
    bool isNetworkConnected() { return scenario.network; }
    NetworkType getNetworkType() { return NetworkType::NET_WIFI; }
};
class FastBeeFramework {
public:
    static FastBeeFramework* getInstance() { static FastBeeFramework f; return &f; }
    FBNetworkManager* getNetworkManager() { static FBNetworkManager n; return &n; }
};
constexpr int WL_CONNECTED = 1;
struct { int status() { return scenario.network ? WL_CONNECTED : 0; } } WiFi;
enum { OTA_IDLE, OTA_DOWNLOADING, OTA_UPLOADING, OTA_COMPLETED, OTA_FAILED };
static int otaStatus = OTA_IDLE;
static String otaErrorMessage, otaFirmwareUrl;
#define LOG_INFO(message) ((void)0)
#define LOG_WARNING(message) ((void)0)
#define LOG_ERROR(message) ((void)0)
#define FASTBEE_ENABLE_CELLULAR 1

class NetworkClient {
public:
    NetworkClient() { ++scenario.created; ++scenario.alive; }
    virtual ~NetworkClient() { --scenario.alive; }
    virtual bool tls() const { return false; }
    size_t available() const { return scenario.received - scenario.offset; }
    int readBytes(uint8_t*, size_t count) {
        assert(count <= 1024);
        const size_t actual = std::min(count, available());
        scenario.offset += actual;
        return static_cast<int>(actual);
    }
};
class WiFiClientSecure : public NetworkClient {
public:
    WiFiClientSecure() { ++scenario.tlsCreated; }
    bool tls() const override { return true; }
    void setCACertBundle(const uint8_t*, size_t size) { assert(size > 0); scenario.secure = true; }
    void setHandshakeTimeout(int value) { assert(value == 30); }
};
const uint8_t bundle[] = {1, 2, 3};
const uint8_t* x509_crt_bundle_start = bundle;
const uint8_t* x509_crt_bundle_end = bundle + sizeof(bundle);
constexpr int HTTP_CODE_OK = 200, HTTPC_STRICT_FOLLOW_REDIRECTS = 1;
class HTTPClient {
    NetworkClient* client = nullptr;
public:
    bool begin(NetworkClient& stream, const String& url) {
        client = &stream;
        assert(stream.tls() == url.startsWith("https://"));
        return scenario.begin;
    }
    void setFollowRedirects(int mode) { assert(mode == HTTPC_STRICT_FOLLOW_REDIRECTS); }
    void setTimeout(int value) { assert(value == 30000); }
    int GET() { ++scenario.getCalls; return scenario.response; }
    long getSize() { return scenario.length; }
    NetworkClient* getStreamPtr() { return client; }
    bool connected() { return scenario.offset < scenario.received; }
    void end() { ++scenario.ended; }
};
struct { void end(bool) {} } Update;
enum class RestartReason { OTA_UPDATE };
struct SystemRebooter {
    static void scheduleReboot(const char*, int, RestartReason) { ++scenario.scheduled; }
};
class OTAManager {
public:
    using ProgressCallback = std::function<void(uint8_t)>;
    bool otaInProgress = false;
    bool otaUploadMode = false;
    uint8_t progress = 0;
    size_t currentSize = 0, expected = 0;
    unsigned long otaStartTime = 0;
    bool checkMemoryGate(String* error) {
        ++scenario.gateCalls;
        if (!scenario.memory) *error = "Insufficient memory";
        return scenario.memory;
    }
    bool beginOTA(size_t size) { expected = size; otaInProgress = true; otaStartTime = millis(); ++scenario.flashed; return true; }
    size_t writeData(uint8_t*, size_t count) { currentSize += count; progress = currentSize * 100 / expected; return count; }
    bool endOTA() { otaInProgress = false; return currentSize == expected; }
    bool startOTA(const String&, ProgressCallback, bool);
};
static time_t testClock(time_t*) { ++scenario.timeReads; return scenario.now; }
#define time testClock
#include "production_ota_download.inc"
#undef time

int main() {
    // HTTP works without a clock or TLS allocations and streams every byte.
    scenario = Scenario{};
    scenario.now = 0;
    OTAManager plain;
    assert(plain.startOTA("http://lan:8080/fw.bin", nullptr, false));
    assert(scenario.created == 1 && scenario.tlsCreated == 0 && scenario.timeReads == 0);
    assert(scenario.offset == 4096 && scenario.alive == 0 && scenario.scheduled == 0);

    scenario = Scenario{};
    OTAManager secure;
    assert(secure.startOTA("https://host/fw.bin", nullptr, true));
    assert(scenario.created == 1 && scenario.tlsCreated == 1 && scenario.secure);
    assert(scenario.timeReads == 1 && scenario.alive == 0 && scenario.scheduled == 1);

    // TLS validation/handshake failure must never try an HTTP client.
    scenario = Scenario{};
    scenario.response = -11;
    OTAManager tlsFailure;
    assert(!tlsFailure.startOTA("https://host/fw.bin", nullptr, true));
    assert(scenario.created == 1 && scenario.tlsCreated == 1 && scenario.getCalls == 1);
    assert(scenario.flashed == 0 && scenario.alive == 0 && scenario.scheduled == 0);

    scenario = Scenario{};
    scenario.now = 0;
    OTAManager clockFailure;
    assert(!clockFailure.startOTA("https://host/fw.bin", nullptr, true));
    assert(scenario.created == 0 && scenario.getCalls == 0);

    scenario = Scenario{};
    scenario.memory = false;
    OTAManager lowMemory;
    assert(!lowMemory.startOTA("https://host/fw.bin", nullptr, true));
    assert(scenario.created == 0 && scenario.getCalls == 0 && scenario.gateCalls == 1);

    for (const char* url : {"file:///fw.bin", "http://user@host/fw.bin", "https://", "http://host/fw#part"}) {
        scenario = Scenario{};
        OTAManager invalid;
        assert(!invalid.startOTA(url, nullptr, true));
        assert(scenario.created == 0 && scenario.getCalls == 0);
    }

    for (bool tls : {false, true}) {
        const char* url = tls ? "https://host/fw.bin" : "http://host/fw.bin";
        scenario = Scenario{};
        scenario.received = 2048;
        OTAManager truncated;
        assert(!truncated.startOTA(url, nullptr, true));
        assert(scenario.alive == 0 && scenario.scheduled == 0 && otaStatus == OTA_FAILED);

        scenario = Scenario{};
        scenario.length = -1;
        OTAManager noLength;
        assert(!noLength.startOTA(url, nullptr, true));
        assert(scenario.flashed == 0 && scenario.alive == 0);

        scenario = Scenario{};
        scenario.begin = false;
        OTAManager badBegin;
        assert(!badBegin.startOTA(url, nullptr, true));
        assert(scenario.alive == 0 && scenario.getCalls == 0);
    }
}
