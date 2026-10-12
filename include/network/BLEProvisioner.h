/**
 * @file BLEProvisioner.h
 * @brief BLE 蓝牙配网管理器
 *
 * 通过 BLE UART 透传服务（Nordic UART Service UUID）接收手机端下发的
 * WiFi 凭证与扩展参数（userId/deviceNum/extra），复用 AP 配网同一套
 * 持久化路径：扩展参数写入 device.json，WiFi 配置经 NetworkManager
 * updateConfig() 保存并触发 STA 模式切换。
 *
 * 编译开关：FASTBEE_ENABLE_BLE=1（需 NimBLE-Arduino 库，见 platformio.ini）
 */

#ifndef BLE_PROVISIONER_H
#define BLE_PROVISIONER_H

#include "core/FeatureFlags.h"
#include "core/FeatureOwnerLifetime.h"
#include "network/BLEProvisionTypes.h"

#if FASTBEE_ENABLE_BLE

#include <Arduino.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

class FBNetworkManager;
class NimBLEServer;
class NimBLECharacteristic;

/**
 * @brief BLE 配网状态枚举
 */
enum class BLEProvisionState : uint8_t {
    IDLE = 0,          // 未启动
    ADVERTISING = 1,   // 广播中，等待手机连接
    CONNECTED = 2,     // 手机已连接，等待配网数据
    PROVISIONED = 3    // 最近一次配网成功（结果通知排空中/已完成）
};

/**
 * @brief BLE 配网管理器
 *
 * 线程模型：
 *  - NimBLE 回调运行在 NimBLE 宿主任务（小栈），onWrite 仅拷贝原始
 *    载荷并置位标志；JSON 解析、device.json 写入与 NetworkManager
 *    配置保存全部延迟到主循环 update()（loopTask 24KB 栈）执行，
 *    与 ProvisionRouteHandler 扫描任务的栈安全设计保持一致。
 */
class BLEProvisioner {
public:
    /**
     * @brief 构造函数
     * @param netMgr 网络管理器指针（用于保存配网配置并触发模式切换）
     */
    explicit BLEProvisioner(FBNetworkManager* netMgr);
    ~BLEProvisioner();

    BLEProvisioner(const BLEProvisioner&) = delete;
    BLEProvisioner& operator=(const BLEProvisioner&) = delete;

    /**
     * @brief 启动 BLE 配网服务（初始化协议栈、创建 GATT 服务并开始广播）
     * @param manual 是否由用户手动启动（手动启动不受框架 AP 状态自动托管停止）
     * @return 是否启动成功
     */
    bool begin(bool manual = false);

    /**
     * @brief 停止 BLE 配网服务（停止广播并释放协议栈内存）
     */
    void stop();

    /**
     * @brief 主循环轮询（处理配网数据、排空结果通知、断连后恢复广播）
     */
    void update();

    bool isRunning() const { return running; }
    bool isClientConnected() const { return clientConnected; }
    bool isProvisioned() const { return lastResultOk; }
    bool isManualStarted() const { return manualStarted; }
    BLEProvisionState getState() const;
    const String& getAdvertisedName() const { return advName; }

    /**
     * @brief 填充状态 JSON（供 /api/ble/provision/status 使用）
     */
    void fillStatusJson(JsonObject& data) const;

    // ===== 供 NimBLE 回调写入（内部使用）=====
    void _onClientConnect();
    void _onClientDisconnect();
    void _onRxData(const char* data, size_t len);

private:
    FastBee::FeatureOwnerLifetime featureOwner_{FastBee::ObservedFeatureOwner::Ble};
    FBNetworkManager* netMgr;

    NimBLEServer* server = nullptr;
    NimBLECharacteristic* txChar = nullptr;   // TX：配网结果 notify
    NimBLECharacteristic* rxChar = nullptr;   // RX：配网数据 write

    String advName;                            // 广播名称（设备名，超长截断）
    bool running = false;
    bool manualStarted = false;                // 手动启动标志（自动托管不停止）
    volatile bool clientConnected = false;
    volatile bool restartAdvPending = false;   // 断连后延迟恢复广播

    // RX 载荷跨任务传递（NimBLE 任务写 → loopTask 读）
    SemaphoreHandle_t rxMutex = nullptr;
    String pendingRx;                          // 待处理的配网 JSON
    volatile bool rxPending = false;

    // 配网结果通知排空（与历史示例一致：最多 10 次、间隔 100ms）
    volatile bool lastResultOk = false;
    volatile bool resultNotifyActive = false;
    uint8_t resultNotifyRemain = 0;
    unsigned long lastNotifyMs = 0;
    bool stopAfterNotify = false;              // 结果排空后自动停止释放内存

    static constexpr uint8_t  RESULT_NOTIFY_COUNT = 10;
    static constexpr unsigned long RESULT_NOTIFY_INTERVAL_MS = 100;
    static constexpr unsigned long RESTART_ADV_DELAY_MS = 500;
    static constexpr size_t RX_MAX_LEN = 512;  // 配网 JSON 上限，防恶意大包

    unsigned long restartAdvAt = 0;

    /**
     * @brief 处理一条已接收的配网载荷（loopTask 上执行）
     */
    void _processProvisionPayload(const String& rx);

    /**
     * @brief 读取设备名用于广播名称（device.json.deviceName，默认 FastBee）
     */
    String _readAdvName();

    /**
     * @brief 发送配网结果并开始通知排空
     */
    void _startResultNotify(bool success, bool autoStop);
};

#endif // FASTBEE_ENABLE_BLE

#endif // BLE_PROVISIONER_H
