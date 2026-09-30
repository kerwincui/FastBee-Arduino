#include <Arduino.h>
#include <ArduinoJson.h>
#include <cassert>
#include <ctime>
#include "protocols/RemoteResourceModel.h"
using namespace FastBee::WeatherSchedule;
static bool tlsPsram=false;
namespace FastBee { namespace TlsMemory { inline bool ensureAllocator() { return true; } inline bool usesPsram() { return tlsPsram; } }}
static uint32_t clockMs=1000;
static time_t clockUtc=1700000000;
#define millis() clockMs
#define time(pointer) clockUtc
enum class NetworkType { NET_WIFI, NET_4G };
struct FBNetworkManager {
    bool connected=true;
    NetworkType type=NetworkType::NET_WIFI;
    bool isNetworkConnected() const { return connected; }
    NetworkType getNetworkType() const { return type; }
};
struct FastBeeFramework {
    FBNetworkManager network;
    static FastBeeFramework* getInstance() { static FastBeeFramework f; return &f; }
    FBNetworkManager* getNetworkManager() { return &network; }
};
constexpr int MALLOC_CAP_INTERNAL=1, MALLOC_CAP_8BIT=2, HTTPC_DISABLE_FOLLOW_REDIRECTS=0;
static size_t freeMemory=100000, largestBlock=60000;
static size_t heap_caps_get_free_size(int) { return freeMemory; }
static size_t heap_caps_get_largest_free_block(int) { return largestBlock; }
static bool ota=false;
static bool otaBusy() { return ota; }
class Stream {
public:
    virtual ~Stream()=default;
    virtual size_t write(uint8_t)=0;
    virtual size_t write(const uint8_t*,size_t)=0;
    virtual int available()=0;
    virtual int read()=0;
    virtual int peek()=0;
    virtual void flush()=0;
};
static const uint8_t certificateRange[]={1,2};
#define x509_crt_bundle_start certificateRange
#define x509_crt_bundle_end (certificateRange+1)
static bool caConfigured=false;
struct NetworkClient {};
struct WiFiClientSecure : NetworkClient {
    void setCACertBundle(const uint8_t* begin,size_t length) { assert(begin==certificateRange && length==1); caConfigured=true; }
    void setHandshakeTimeout(unsigned seconds) { assert(seconds==10); }
};
static std::string response=R"({"results":[{"location":{"name":"上海"},"now":{"text":"晴","code":"0","temperature":"23"}}]})";
static int httpStatus=200, announced=-1;
static bool incomplete=false, slow=false;
static unsigned getCalls=0, ends=0;
static String sentUrl;
struct HTTPClient {
    bool begin(NetworkClient&,const String& url) { sentUrl=url; return true; }
    void setConnectTimeout(unsigned ms) { assert(ms==5000); }
    void setTimeout(unsigned ms) { assert(ms==8000); }
    void setFollowRedirects(int mode) { assert(mode==HTTPC_DISABLE_FOLLOW_REDIRECTS); }
    void setReuse(bool reuse) { assert(!reuse); }
    void addHeader(const char*,const char*) {}
    int GET() { assert(caConfigured); ++getCalls; return httpStatus; }
    int getSize() { return announced; }
    int writeToStream(Stream* stream) {
        if (slow) clockMs+=16000;
        size_t count=stream->write(reinterpret_cast<const uint8_t*>(response.data()),response.size());
        return incomplete ? -1 : static_cast<int>(count);
    }
    void end() { ++ends; }
};
#include "fetch.inc"
int main() {
    JsonDocument source, normalized;
    source["provider"]="seniverse"; source["apiKey"]="a&b"; source["location"]="shanghai";
    source["url"]="https://untrusted.test";
    Reason reason=Reason::None; Diagnostics memory;
    auto fetch=[&]() { normalized.clear(); return fetchWeather(source.as<JsonObjectConst>(),normalized,reason,memory); };
    assert(fetch()); assert(memory.phase==std::string("request_completed") && memory.requestFree==100000U); assert(normalized["temperature"]==23 && normalized["humidity"].isNull());
    assert(sentUrl.startsWith("https://api.seniverse.com/v3/weather/now.json?key=a%26b"));
    assert(sentUrl.indexOf("language=zh-Hans&unit=c")>=0);
    clockUtc=0; assert(!fetch() && reason==Reason::Time && getCalls==1);
    assert(memory.phase==std::string("worker_blocked") && memory.workerLargest==60000U);
    assert(memory.requiredFree==65000U && memory.requiredLargest==32768U);
    clockUtc=1700000000; freeMemory=64999; assert(!fetch() && reason==Reason::Memory && getCalls==1);
    freeMemory=100000; largestBlock=32767; assert(!fetch() && reason==Reason::Memory && getCalls==1);
    largestBlock=60000; ota=true; assert(!fetch() && reason==Reason::Ota && getCalls==1); ota=false;
    auto* net=FastBeeFramework::getInstance()->getNetworkManager();
    net->connected=false; assert(!fetch() && reason==Reason::Offline && getCalls==1);
    net->connected=true; net->type=NetworkType::NET_4G;
    assert(!fetch() && reason==Reason::UnsupportedTransport && getCalls==1); net->type=NetworkType::NET_WIFI;
    // Replay the device's post-stack memory through the actual HTTP fetch path.
    tlsPsram=true; freeMemory=30152; largestBlock=9204;
    assert(fetch() && getCalls==2);
    assert(memory.requiredFree==24000U && memory.requiredLargest==8192U);
    assert(memory.workerFree==30152U && memory.workerLargest==9204U);
    freeMemory=24000; largestBlock=8192; assert(fetch() && getCalls==3);
    largestBlock=8191; assert(!fetch() && reason==Reason::Memory && getCalls==3);
    largestBlock=9204; freeMemory=23999; assert(!fetch() && reason==Reason::Memory && getCalls==3);
    tlsPsram=false; freeMemory=30152; assert(!fetch() && reason==Reason::Memory && getCalls==3);
    freeMemory=65000; largestBlock=32768; assert(fetch() && getCalls==4);
    freeMemory=100000; largestBlock=60000;
    httpStatus=-1; assert(!fetch() && reason==Reason::Transport);
    httpStatus=302; assert(!fetch() && reason==Reason::Provider);
    httpStatus=403; response=R"({"status_code":"AP010014","status":"private echoed key"})";
    assert(!fetch() && reason==Reason::RateLimit);
    response=R"({"status_code":"AP010003"})"; assert(!fetch() && reason==Reason::Authentication);
    httpStatus=404; response=R"({"status_code":"AP010010"})"; assert(!fetch() && reason==Reason::Location);
    httpStatus=429; response="not-json"; assert(!fetch() && reason==Reason::RateLimit);
    httpStatus=200; announced=8193; assert(!fetch() && reason==Reason::Response); announced=-1;
    response=std::string(8193,'a'); assert(!fetch() && reason==Reason::Response);
    response="{}"; incomplete=true; assert(!fetch() && reason==Reason::Response); incomplete=false;
    slow=true; assert(!fetch() && reason==Reason::Response); slow=false;
    response="malformed"; assert(!fetch() && reason==Reason::Response);
    assert(ends==getCalls);
}
