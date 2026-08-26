#ifndef PROVISION_ROUTE_HANDLER_H
#define PROVISION_ROUTE_HANDLER_H

#include <ESPAsyncWebServer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

class WebHandlerContext;

/**
 * @brief WiFi 引导配网路由处理器
 * 
 * 处理 /setup 引导页与 /api/wifi/* 配网相关路由；
 * 支持手机/移动端通过 AP 热点调用接口完成 WiFi 配置和设备参数下发。
 */
class ProvisionRouteHandler {
public:
    explicit ProvisionRouteHandler(WebHandlerContext* ctx);

    void setupRoutes(AsyncWebServer* server);

    /**
     * @brief 将配网下发的扩展参数（userId/deviceNum/extra）写入 device.json
     * @details AP 配网接口与 BLE 配网共用的持久化路径：
     *          userId → device.json.userId，deviceNum → device.json.deviceId，
     *          extra → device.json.productNumber（仅有效正整数）
     */
    static void _updateDeviceConfig(const String& userId,
                                    const String& deviceNum,
                                    const String& extra);

private:
    WebHandlerContext* ctx;

    void handleSetupPage(AsyncWebServerRequest* request);
    void handleWiFiScan(AsyncWebServerRequest* request);
    void handleWiFiConnect(AsyncWebServerRequest* request);

    // AP 配网公开接口（无需身份认证，供手机端直接调用）
    void handleWiFiStatus(AsyncWebServerRequest* request);   // GET  /api/wifi/status  设备状态检测
    void handleWiFiConfig(AsyncWebServerRequest* request);   // POST /api/wifi/config  配网下发配置

    // BLE 蓝牙配网管理接口（WebUI 调用，需身份认证）
    void handleBleProvisionStatus(AsyncWebServerRequest* request);  // GET  /api/ble/provision/status
    void handleBleProvisionStart(AsyncWebServerRequest* request);   // POST /api/ble/provision/start
    void handleBleProvisionStop(AsyncWebServerRequest* request);    // POST /api/ble/provision/stop

    // ===== WiFi 扫描后台任务 =====
    // 扫描（含 WiFi API 调用与 JSON 构建）运行在独立 FreeRTOS 任务上，
    // async_tcp 任务上的 chunked 回调仅轮询 scanResultReady 标志并拷贝结果，
    // 避免在 async_tcp 小栈上执行重活导致连接中断（ERR_INCOMPLETE_CHUNKED_ENCODING）。
    // STA 模式下扫描会导致射频跳频、中断当前 WiFi 连接（ERR_CONNECTION_RESET），
    // 因此使用结果缓存（SCAN_CACHE_TTL_MS）避免频繁触发扫描。
    static void _wifiScanTask(void* arg);
    void _launchScanTask();

    static constexpr unsigned long SCAN_CACHE_TTL_MS = 60000;  // 扫描结果缓存有效期

    SemaphoreHandle_t scanMutex = nullptr;      // 保护下方扫描共享状态
    String scanResultJson;                      // 扫描完成后构建好的响应体
    volatile bool scanResultReady = false;      // 结果就绪可被 HTTP 回调取走
    volatile bool scanTaskRunning = false;      // 扫描任务正在运行（去重并发请求）
    volatile unsigned long scanLastCompleteMs = 0;  // 上次扫描完成时间戳（millis）
};

#endif // PROVISION_ROUTE_HANDLER_H
