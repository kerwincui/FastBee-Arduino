#ifndef COAPHANDLER_H
#define COAPHANDLER_H

#include <WiFi.h>
#include <WiFiUdp.h>
#include <functional>
#include <map>
#include <core/ConfigDefines.h>

enum class CoAPType {
    CON = 0,  // Confirmable
    NON = 1,  // Non-confirmable
    ACK = 2,  // Acknowledgement
    RST = 3   // Reset
};

enum class CoAPMethod {
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
    std::map<String, String> resourceMap; // 资源路径映射
};

class CoAPHandler {
public:
    CoAPHandler();
    ~CoAPHandler();
    
    // 从LittleFS加载配置
    bool loadConfigFromLittleFS(const char* configPath = CONFIG_FILE_COAP);
    
    // 初始化方法
    bool begin(const CoAPConfig& config);
    bool beginFromConfig(const char* configPath = CONFIG_FILE_COAP);
    
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
    
    CoAPConfig config;
    WiFiUDP udp;
    bool isInitialized;
    uint16_t messageId;
    std::function<void(const String&, const String&)> messageCallback;
};

#endif