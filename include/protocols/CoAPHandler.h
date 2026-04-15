#ifndef COAPHANDLER_H
#define COAPHANDLER_H

#include "core/FeatureFlags.h"

#if FASTBEE_ENABLE_COAP

#include <WiFi.h>
#include <WiFiUdp.h>
#include <functional>
#include <map>
#include <core/SystemConstants.h>
#include "systems/LoggerSystem.h"

// CoAP 协议常量 (RFC 7252)
static constexpr uint8_t COAP_VERSION = 1;
static constexpr uint8_t COAP_DEFAULT_TOKEN_SIZE = 4;
static constexpr uint8_t COAP_MAX_TOKEN_SIZE = 8;
static constexpr uint8_t MAX_PENDING_MESSAGES = 4;
static constexpr uint16_t COAP_ACK_TIMEOUT = 2000;   // ms (RFC 7252 default)
static constexpr uint16_t COAP_MAX_RETRANSMIT = 4;

// CoAP Option 编号
static constexpr uint16_t COAP_OPTION_URI_PATH = 11;
static constexpr uint16_t COAP_OPTION_CONTENT_FORMAT = 12;
static constexpr uint16_t COAP_OPTION_URI_QUERY = 15;

// CoAP Content-Format
static constexpr uint16_t COAP_CT_TEXT_PLAIN = 0;
static constexpr uint16_t COAP_CT_APP_JSON = 50;

enum class CoAPType {
    CON = 0,  // Confirmable
    NON = 1,  // Non-confirmable
    ACK = 2,  // Acknowledgement
    RST = 3   // Reset
};

enum class CoAPMethod {
    EMPTY = 0,
    GET = 1,
    POST = 2,
    PUT = 3,
    DELETE = 4
};

struct CoAPConfig {
    String server = "coap.me";
    uint16_t port = 5683;
    uint16_t localPort = 5683;
    String defaultMethod = "POST";
    unsigned long timeout = 5000;
    uint8_t retransmitCount = 3;
    std::map<String, String> resourceMap;
};

// 待确认消息（用于重传追踪）
struct PendingMessage {
    uint16_t messageId;
    uint8_t token[COAP_DEFAULT_TOKEN_SIZE];
    uint8_t tokenLength;
    uint8_t packet[Protocols::COAP_BUFFER_SIZE];
    uint16_t packetLength;
    unsigned long sentTime;
    uint8_t retryCount;
    unsigned long nextRetryTime;
    bool active;
};

class CoAPHandler {
public:
    CoAPHandler();
    ~CoAPHandler();
    
    // 从LittleFS加载配置
    bool loadConfigFromLittleFS(const char* configPath = FileSystem::PROTOCOL_CONFIG_FILE);
    
    // 初始化方法
    bool begin(const CoAPConfig& config);
    bool beginFromConfig(const char* configPath = FileSystem::PROTOCOL_CONFIG_FILE);
    
    // 清理资源
    void end();
    
    // CoAP消息发送
    bool send(const String& resource, const String& data);
    bool get(const String& resource, const String& query = "");
    bool put(const String& resource, const String& data);
    bool post(const String& resource, const String& data);
    bool del(const String& resource);
    
    // 处理循环
    void handle();
    
    // 状态获取
    String getStatus() const;
    const CoAPConfig& getConfig() const;
    
    // 回调设置
    void setMessageCallback(std::function<void(const String&, const String&)> callback);

private:
    bool sendCoAPMessage(CoAPType type, CoAPMethod method, const String& resource, const String& payload);
    void processCoAPPacket();
    uint16_t generateMessageId();
    String getMethodString(CoAPMethod method);
    
    // RFC 7252 Option 编解码
    uint16_t encodeOption(uint8_t* buffer, uint16_t prevOptionNumber,
                          uint16_t optionNumber, const uint8_t* value, uint16_t valueLength);
    
    // Token 管理
    void generateToken(uint8_t* token, uint8_t length);
    
    // 重传引擎
    bool addPendingMessage(uint16_t msgId, const uint8_t* token, uint8_t tokenLen,
                           const uint8_t* packet, uint16_t packetLen);
    void removePendingMessage(uint16_t msgId);
    void checkRetransmissions();
    
    // 消息去重
    bool isDuplicateMessage(uint16_t msgId);
    void recordMessageId(uint16_t msgId);
    
    // ACK 发送
    void sendAck(uint16_t msgId);
    
    CoAPConfig config;
    WiFiUDP udp;
    bool isInitialized;
    uint16_t messageId;
    std::function<void(const String&, const String&)> messageCallback;
    
    // 重传追踪（静态数组避免堆碎片）
    PendingMessage pendingMessages[MAX_PENDING_MESSAGES];
    
    // 消息去重环形缓冲区
    uint16_t recentMessageIds[16];
    uint8_t recentMsgIdIndex;
};

#else // !FASTBEE_ENABLE_COAP

// 空桩类，避免编译错误
class CoAPHandler {
public:
    bool initialize() { return true; }
    void shutdown() {}
    void handle() {}
    bool isRunning() const { return false; }
};

#endif // FASTBEE_ENABLE_COAP

#endif
