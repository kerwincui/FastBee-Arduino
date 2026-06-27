/**
 * @file CellularAdapter.h
 * @brief 4G 蜂窝模块适配器 (EC801E-CN)
 * @author kerwincui
 * @date 2026-06-02
 * 
 * 通过 UART AT 指令控制中移 EC801E-CN 模块，
 * 使用 TinyGSM 库实现 PPP/TCP 透传，提供标准 Arduino Client 接口。
 */

#ifndef CELLULAR_ADAPTER_H
#define CELLULAR_ADAPTER_H

#include "core/FeatureFlags.h"

#if FASTBEE_ENABLE_CELLULAR

#include <Arduino.h>
#include <HardwareSerial.h>
#include "network/WiFiManager.h"
#include "core/interfaces/INetworkAdapter.h"

// TinyGSM 配置 - 在 include 之前定义
// EC801E-CN 的 AT 指令集与 SIM7600 系列（LTE Cat-4）兼容
#define TINY_GSM_MODEM_BG96
#define TINY_GSM_RX_BUFFER 1024

#include <TinyGsmClient.h>

class CellularTrackedClient;
class CellularSoftwareTlsClient;

class QuectelSslClient : public Client {
public:
    explicit QuectelSslClient(HardwareSerial& serial,
                              uint8_t contextId = 1,
                              uint8_t connectId = 1,
                              uint8_t sslContextId = 0);

    void setApn(const String& apn);
    bool isBusy() const { return _busy; }

    int connect(IPAddress ip, uint16_t port) override;
    int connect(const char* host, uint16_t port) override;
    size_t write(uint8_t value) override;
    size_t write(const uint8_t* buf, size_t size) override;
    int available() override;
    int read() override;
    int read(uint8_t* buf, size_t size) override;
    int peek() override;
    void flush() override;
    void stop() override;
    uint8_t connected() override;
    operator bool() override;

private:
    static constexpr size_t RX_BUFFER_SIZE = 768;

    HardwareSerial* _serial;
    String _apn;
    String _host;
    uint16_t _port = 0;
    uint8_t _contextId;
    uint8_t _connectId;
    uint8_t _sslContextId;
    bool _connected = false;
    bool _hasPendingData = false;
    bool _busy = false;
    unsigned long _lastPollMs = 0;
    uint8_t _rxBuffer[RX_BUFFER_SIZE];
    size_t _rxPos = 0;
    size_t _rxLen = 0;

    bool configureSsl();
    bool ensureInternetContext();
    bool queryInternetContextActive();
    bool sendCommandExpectOk(const char* cmd, uint32_t timeoutMs = 5000);
    bool sendCommandCollect(const char* cmd, String& response, uint32_t timeoutMs = 5000);
    bool waitForPrompt(uint32_t timeoutMs = 5000);
    bool waitForSendDone(uint32_t timeoutMs = 15000);
    bool waitForOpenResult(uint32_t timeoutMs = 90000);
    bool fetchRx(size_t wanted, uint32_t timeoutMs);
    int bufferedAvailable() const;
    void resetRx();
    void drainInput(uint32_t maxMs = 20);
    String readLine(uint32_t timeoutMs);
    void handleUrc(const String& line);
    static String sanitizeToken(String value);
    static bool parseLastInteger(const String& line, int& value);
};

/**
 * @class CellularAdapter
 * @brief 4G 蜂窝网络适配器 (EC801E-CN)
 * 
 * 通过 AT 指令激活 PDP 上下文，使用 TinyGSM 提供的 Client 接口
 * 与 PubSubClient 对接实现 MQTT over 4G。
 */
class CellularAdapter : public INetworkAdapter {
public:
    CellularAdapter();
    ~CellularAdapter();

    /**
     * @brief 初始化 4G 模块
     * @param config 网络配置（含 cellular 引脚配置）
     * @return 初始化是否成功
     */
    bool begin(const WiFiConfig& config) override;

    /**
     * @brief 检查是否已连接网络
     */
    bool isConnected() const override;

    /**
     * @brief 断开连接并关闭模块
     */
    void disconnect() override;

    /**
     * @brief 获取 Arduino Client 指针（用于 PubSubClient）
     */
    Client* getClient() override;

    /**
     * @brief 获取 TLS Arduino Client 指针（用于 MQTTS over 4G）
     */
    Client* getSecureClient();

    /**
     * @brief 获取信号质量 (CSQ)
     * @return 信号质量字符串 "CSQ: xx,yy"
     */
    String getSignalQuality();

    /**
     * @brief 获取 SIM 卡 ICCID
     */
    String getICCID();

    /**
     * @brief 获取 IMEI
     */
    String getIMEI();

    /**
     * @brief 获取运营商信息
     */
    String getOperator();

    /**
     * @brief 获取网络连接类型 (4G/3G/2G)
     */
    String getNetworkType();

    /**
     * @brief 获取信号质量 CSQ 值 (0-31, 99=未知)
     */
    int getSignalQualityCSQ();

    /**
     * @brief 检查SIM卡是否就绪
     */
    bool isSimReady();

    /**
     * @brief 获取连接状态字符串
     */
    String getStatusString() const override;

    /**
     * @brief 获取 4G 模块分配的 IP 地址
     */
    IPAddress localIP() const override;

    /**
     * @brief 更新状态（在主循环中调用）
     */
    void update() override;

    /**
     * @brief 重连网络
     */
    bool reconnect();

private:
    HardwareSerial* _serial = nullptr;
    TinyGsm* _modem = nullptr;
    TinyGsmClient* _gsmClient = nullptr;
    CellularTrackedClient* _trackedClient = nullptr;
    CellularSoftwareTlsClient* _softwareTlsClient = nullptr;
    QuectelSslClient* _qsslClient = nullptr;
    
    CellularConfig _pinConfig;
    String _apn;
    bool _initialized = false;
    bool _connected = false;
    bool _clientBusy = false;
    unsigned long _lastCheckTime = 0;

    /**
     * @brief 模块上电
     */
    void powerOn();

    /**
     * @brief 模块断电
     */
    void powerOff();

    /**
     * @brief 等待模块就绪
     */
    bool waitForReady(uint32_t timeoutMs = 10000);

    /**
     * @brief 激活 PDP 上下文
     */
    bool activateNetwork();

    /**
     * @brief 发送AT命令并打印响应（诊断用）
     */
    void sendATDiag(const char* cmd);

    /**
     * @brief 发送AT命令（不打印响应）
     */
    void sendATCmd(const char* cmd);

    /**
     * @brief 发送AT命令并返回响应字符串
     */
    String readATResponse(const char* cmd);

    /**
     * @brief 检查PDP上下文是否仍活跃（使用AT+CGPADDR）
     */
    bool checkPdpActive();
};

#endif // FASTBEE_ENABLE_CELLULAR
#endif // CELLULAR_ADAPTER_H
