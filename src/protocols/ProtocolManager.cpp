/**
 *@description: 协议管理器实现
 *@author: kerwincui
 *@copyright:FastBee All rights reserved.
 *@date: 2025-12-02 17:31:25
 */

#include "protocols/ProtocolManager.h"
#include "systems/LoggerSystem.h"
#include "systems/ConfigStorage.h"
#include "core/FastBeeFramework.h"
#include "core/PeriphExecManager.h"
#include "core/FeatureFlags.h"
#include "core/MemoryBudget.h"
#include "core/FeatureBudget.h"
#include "core/RuntimeFeatureManager.h"
#include "network/NetworkManager.h"
#include "network/handlers/MqttRouteHandler.h"
#include "protocols/FastBeeOTAProtocol.h"
#if FASTBEE_ENABLE_REMOTE_RESOURCES
#include "protocols/RemoteResourceManager.h"
#endif
#if FASTBEE_ENABLE_OTA
#include "network/OTAManager.h"
#include "network/OtaConfigStorage.h"
#endif
#include "systems/SystemRebooter.h"
#include "utils/StringUtils.h"
#include "utils/CheckedText.h"
#include "core/ActionReportPayload.h"
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <memory>
#include <new>

#if FASTBEE_ENABLE_MODBUS
namespace {
class ModbusLifecycleGuard {
public:
    ModbusLifecycleGuard(SemaphoreHandle_t mutex, TickType_t timeout)
        : _mutex(mutex), _locked(false) {
        if (_mutex) _locked = xSemaphoreTakeRecursive(_mutex, timeout) == pdTRUE;
    }
    ~ModbusLifecycleGuard() {
        if (_locked && _mutex) xSemaphoreGiveRecursive(_mutex);
    }
    bool isLocked() const { return _locked; }

    ModbusLifecycleGuard(const ModbusLifecycleGuard&) = delete;
    ModbusLifecycleGuard& operator=(const ModbusLifecycleGuard&) = delete;

private:
    SemaphoreHandle_t _mutex;
    bool _locked;
};
}
#endif

#if FASTBEE_ENABLE_MQTT
#include "protocols/MqttSecureIntent.h"
static bool configureMqttTransport(MQTTClient* mqttClient) {
    if (!mqttClient) return false;

    FastBeeFramework* fw = FastBeeFramework::getInstance();
    FBNetworkManager* netMgr = fw ? static_cast<FBNetworkManager*>(fw->getNetworkManager()) : nullptr;
    if (!netMgr || netMgr->getNetworkType() == NetworkType::NET_WIFI) {
        return mqttClient->setTransportClient(nullptr);
    }

    if (!netMgr->isNetworkConnected()) {
        LOG_WARNING("Protocol Manager: MQTT transport unavailable, active network is offline");
        return false;
    }

    bool wantsMqtts = false;
    {
        JsonDocument doc;
        if (!ConfigStorage::getInstance().loadProtocolSection("mqtt", doc)) return false;
        JsonVariant mqttSection = doc["mqtt"];
        JsonVariant cfg = mqttSection.is<JsonObject>() ? mqttSection : doc.as<JsonVariant>();
        wantsMqtts = FastBee::mqttRequiresTls(cfg);
        if ((cfg["downgraded_by_memory"] | false) && !FastBee::mqttLegacySecurePort(cfg))
            return false;
    }

    Client* transport = nullptr;
    if (wantsMqtts && netMgr->getNetworkType() == NetworkType::NET_ETHERNET) {
        // WiFiClientSecure is backed by Arduino-ESP32 NetworkClientSecure and
        // can use the active Ethernet route. Injecting the plain W5500 client
        // here would send cleartext MQTT to port 8883 and timeout.
        LOG_INFO("Protocol Manager: Using internal secure transport for Ethernet MQTTS");
        return mqttClient->setTransportClient(nullptr);
    }
#if FASTBEE_ENABLE_CELLULAR
    if (wantsMqtts && netMgr->getNetworkType() == NetworkType::NET_4G) {
        CellularAdapter* cell = netMgr->getCellularAdapter();
        transport = cell ? cell->getSecureClient() : nullptr;
        if (!transport) {
            LOG_WARNING("Protocol Manager: MQTTS rejected, cellular TLS transport unavailable");
            return false;
        }
        LOG_INFO("Protocol Manager: Using cellular secure transport for MQTTS");
        return mqttClient->setTransportClient(transport, true);
    }
#endif
    if (wantsMqtts) {
        LOG_WARNING("Protocol Manager: MQTTS rejected, active network has no TLS provider");
        return false;
    }
    transport = netMgr->getActiveClient();
    if (!transport) {
        LOG_WARNING("Protocol Manager: MQTT transport unavailable, active Client is null");
        return false;
    }

    return mqttClient->setTransportClient(transport);
}
#endif

static String formatAnalogReportValue(const PeripheralConfig& config,
                                      PeripheralManager& pm,
                                      AnalogInputQuality* quality = nullptr) {
    AnalogReadSample sample;
    if (!pm.readAnalogSample(config.id, sample)) {
        if (quality) *quality = AnalogInputQuality::READ_FAILED;
        return String();
    }
    if (quality) *quality = sample.quality;
    if (sample.conversionEnabled && sample.signalMode != static_cast<uint8_t>(AnalogSignalMode::RAW)) {
        const float value = sample.value;
        if (isnan(value) || isinf(value)) return String();
        const uint8_t decimals = sample.decimalPlaces;
        char buf[32];
        snprintf(buf, sizeof(buf), "%.*f", decimals, value);
        return String(buf);
    }
    return String(sample.raw);
}

ProtocolManager::ProtocolManager()
    : isInitialized(false)
#if FASTBEE_ENABLE_MODBUS
    , modbusRestartPending(false)
#endif
{
#if FASTBEE_ENABLE_MODBUS
    modbusLifecycleMutex = xSemaphoreCreateRecursiveMutex();
    if (!modbusLifecycleMutex) {
        LOG_ERROR("Protocol Manager: Failed to create Modbus lifecycle mutex");
    }
#endif
#if FASTBEE_ENABLE_MQTT
    if (!mqttLifetime.available()) {
        LOG_ERROR("MQTT admission storage unavailable; manager recreation/restart required");
    }
#endif
}

ProtocolManager::~ProtocolManager() {
    shutdown();
#if FASTBEE_ENABLE_MQTT
#endif
#if FASTBEE_ENABLE_MODBUS
    if (modbusLifecycleMutex) {
        vSemaphoreDelete(modbusLifecycleMutex);
        modbusLifecycleMutex = nullptr;
    }
#endif
}

#if FASTBEE_ENABLE_MODBUS
void ProtocolManager::ModbusHandlerLease::release() {
    if (_owner) _owner->releaseModbusHandlerLease();
    _owner = nullptr;
    _handler = nullptr;
}

ProtocolManager::ModbusHandlerLease ProtocolManager::acquireModbusHandler(
    TickType_t timeout) const {
    return acquireModbusHandlerInternal(timeout, false);
}

ProtocolManager::ModbusHandlerLease ProtocolManager::acquireModbusHandlerInternal(
    TickType_t timeout, bool allowPending) const {
    ModbusLifecycleGuard lifecycle(modbusLifecycleMutex, timeout);
    if (!lifecycle.isLocked() || !modbusHandler) return ModbusHandlerLease();
    if (modbusShutdownPending) return ModbusHandlerLease();
    if (!allowPending && (modbusRestartPending || modbusStopPending)) {
        return ModbusHandlerLease();
    }

    portENTER_CRITICAL(&modbusLeaseMux);
    ++modbusActiveLeases;
    portEXIT_CRITICAL(&modbusLeaseMux);
    return ModbusHandlerLease(const_cast<ProtocolManager*>(this),
                              modbusHandler.get());
}

void ProtocolManager::releaseModbusHandlerLease() {
    portENTER_CRITICAL(&modbusLeaseMux);
    if (modbusActiveLeases > 0) --modbusActiveLeases;
    portEXIT_CRITICAL(&modbusLeaseMux);
}

uint16_t ProtocolManager::getActiveModbusLeaseCount() const {
    portENTER_CRITICAL(&modbusLeaseMux);
    const uint16_t count = modbusActiveLeases;
    portEXIT_CRITICAL(&modbusLeaseMux);
    return count;
}
#endif

#if FASTBEE_ENABLE_MQTT
FastBee::MqttClientLease ProtocolManager::acquireMQTTClient(bool* absent) const {
    if (absent) *absent = false; // Contention/pending mutation is not a missing client.
    if (!mqttLifetime.available()) return {};
    if (mqttShutdownPending.load() || mqttStopPending.load() || mqttRestartPending.load()) return {};
    FastBee::MqttClientLifetime::Read read(mqttLifetime);
    if (!read || mqttShutdownPending.load() || mqttStopPending.load() || mqttRestartPending.load()) return {};
    if (!mqttClient) { if (absent) *absent = true; return {}; }
    return FastBee::MqttClientLease(std::move(read), mqttClient.get());
}
#endif

bool ProtocolManager::initialize() {
    if (isInitialized) {
        return true;
    }

    LOG_INFO("Protocol Manager: Initializing...");
    shutdownComplete = false;
#if FASTBEE_ENABLE_MQTT
    mqttShutdownPending = false;
#endif
#if FASTBEE_ENABLE_MODBUS
    modbusShutdownPending = false;
#endif
    isInitialized = true;
    return true;
}

bool ProtocolManager::addProtocol(ProtocolType type, const String& name, bool enabled) {
    ProtocolConfig config;
    config.type    = type;
    config.name    = name;
    config.enabled = enabled;
    config.config  = nullptr;

    protocols.push_back(config);

    char buf[64];
    snprintf(buf, sizeof(buf), "Protocol Manager: Added [%s]", name.c_str());
    LOG_INFO(buf);
    return true;
}

bool ProtocolManager::setProtocolConfig(ProtocolType type, void* config) {
    for (auto& protocol : protocols) {
        if (protocol.type == type) {
            protocol.config = config;

            // 根据协议类型初始化
            switch (type) {
#if FASTBEE_ENABLE_MQTT
                case ProtocolType::MQTT:
                    return initMQTT(config);
#endif
#if FASTBEE_ENABLE_MODBUS
                case ProtocolType::MODBUS:
                    return initModbus(config);
#endif
                default:
                    return false;
            }
        }
    }
    return false;
}

bool ProtocolManager::startAll() {
    LOG_INFO("Protocol Manager: Starting all protocols...");

    bool allStarted = true;
    for (const auto& protocol : protocols) {
        if (!protocol.enabled) continue;

        bool started = false;
        switch (protocol.type) {
#if FASTBEE_ENABLE_MQTT
            case ProtocolType::MQTT: {
                {
                    FastBee::MqttClientLifetime::Write lifecycle(mqttLifetime);
                    if (lifecycle && !mqttShutdownPending && !mqttStopPending &&
                        !mqttRestartPending && mqttClient) {
                        // initMQTT may already have completed one-shot begin.
                        started = !mqttClient->isStopped() || mqttClient->begin();
                        if (!started && mqttClient->isStopped()) {
                            mqttClient->setReportRetirementReason(FastBee::MqttRetirementReason::InitializationFailed);
                            mqttClient.reset();
                        }
                    }
                }
                if (started) PeriphExecManager::getInstance().triggerEvent(EventType::EVENT_MQTT_ENABLED, "");
                break;
            }
#endif
#if FASTBEE_ENABLE_MODBUS
            case ProtocolType::MODBUS:
            {
                auto lease = acquireModbusHandler(portMAX_DELAY);
                started = lease && lease->begin();
                if (started) PeriphExecManager::getInstance().triggerEvent(EventType::EVENT_MODBUS_RTU_ENABLED, "");
                break;
            }
#endif
        }

        if (!started) {
            char buf[64];
            snprintf(buf, sizeof(buf), "Protocol Manager: Failed to start [%s]", protocol.name.c_str());
            LOG_WARNING(buf);
            allStarted = false;
        }
    }

    return allStarted;
}

void ProtocolManager::stopAll() {
    LOG_INFO("Protocol Manager: Stopping all protocols...");
#if FASTBEE_ENABLE_MQTT
    stopMQTT();
#endif
#if FASTBEE_ENABLE_MODBUS
    stopModbus();
#endif
}

void ProtocolManager::shutdown() {
    if (shutdownComplete) return;
    LOG_INFO("Protocol Manager: Shutting down...");
#if FASTBEE_ENABLE_MQTT
    // Shutdown is a terminal drain, invoked after producers are asked to stop.
    // Never call it while holding an MQTT lease on the same task.
    mqttShutdownPending = true;
    mqttRestartPending = false;
    FastBee::MqttClientLifetime::Write mqttShutdown(mqttLifetime);
    // Construction failure admitted no client/read/write lease. It cannot
    // recover while waiting, and must not turn shutdown into an infinite wait.
    while (mqttLifetime.available() && !mqttShutdown.tryAcquire()) vTaskDelay(pdMS_TO_TICKS(5));
#endif

#if FASTBEE_ENABLE_MODBUS
    // Prevent new public operations, then let already admitted leases drain.
    // Lease release uses a short critical section rather than this mutex, so
    // waiting here cannot deadlock an in-flight worker.
    {
        ModbusLifecycleGuard lifecycle(modbusLifecycleMutex, portMAX_DELAY);
        if (!lifecycle.isLocked()) {
            LOG_ERROR("Protocol Manager: Cannot lock Modbus lifecycle for shutdown");
            return;
        }
        modbusShutdownPending = true;
        modbusStopPending = true;
        modbusRestartPending = false;
    }
    while (getActiveModbusLeaseCount() > 0) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
#endif

#if FASTBEE_ENABLE_MQTT
    // MQTTClient's destructor owns worker shutdown before transport disconnect.
    // Disconnecting here first would race an internal reconnect operation.
    if (mqttClient) mqttClient->setReportRetirementReason(FastBee::MqttRetirementReason::Shutdown);
    mqttClient.reset();
    mqttStopPending = false;
#endif
#if FASTBEE_ENABLE_MODBUS
    {
        ModbusLifecycleGuard lifecycle(modbusLifecycleMutex, portMAX_DELAY);
        PeriphExecManager::getInstance().setModbusReadCallback(nullptr);
        PeriphExecManager::getInstance().setModbusRawSendCallback(nullptr);
        unregisterModbusSubDevices();
        if (modbusHandler) modbusHandler->end();
        modbusHandler.reset();
        modbusStopPending = false;
    }
#endif

    protocols.clear();
    messageCallback = nullptr;
    txCallback = nullptr;
    rxCallback = nullptr;
    sseCallback = nullptr;
    isInitialized   = false;
    shutdownComplete = true;

    LOG_INFO("Protocol Manager: Shutdown complete");
}

bool ProtocolManager::sendData(ProtocolType type, const String& topic, const String& data) {
    switch (type) {
        case ProtocolType::MQTT:
#if FASTBEE_ENABLE_MQTT
        {
            auto lease = acquireMQTTClient();
            return lease && lease->publish(topic, data);
        }
#else
            return false;
#endif
        case ProtocolType::MODBUS:
#if FASTBEE_ENABLE_MODBUS
        {
            auto lease = acquireModbusHandler(pdMS_TO_TICKS(2000));
            if (!lease) return false;
            if (lease->getMode() == MODBUS_MASTER) {
                // This generic API has no authenticated output context. Do not
                // create a second master-write path or narrow numeric strings
                // into a different slave/register/value. Use a bound entry.
                return false;
            }
            // Slave模式: topic为寄存器地址
#if FASTBEE_MODBUS_SLAVE_ENABLE
            return lease->writeData(topic.toInt(), data);
#else
            return false;
#endif
        }
#else
            return false;
#endif
        default:
            return false;
    }
}

void ProtocolManager::setMessageCallback(MessageCallback callback) {
    this->messageCallback = callback;
}

String ProtocolManager::getProtocolStatus(ProtocolType type) {
    switch (type) {
        case ProtocolType::MQTT:
#if FASTBEE_ENABLE_MQTT
        {
            auto lease = acquireMQTTClient();
            return lease ? lease->getStatus() : "Not initialized or busy";
        }
#else
            return "Disabled";
#endif
        case ProtocolType::MODBUS:
#if FASTBEE_ENABLE_MODBUS
        {
            auto lease = acquireModbusHandler(pdMS_TO_TICKS(100));
            return lease ? lease->getStatus() : "Not initialized or busy";
        }
#else
            return "Disabled";
#endif
        default:
            return "Unknown protocol";
    }
}

void ProtocolManager::handle() {
    // 无条件周期调试（每10秒），确认 handle() 被调用
    // heap 统一用 MALLOC_CAP_8BIT（真实可 malloc DRAM，与 METRICS free8 同口径）；
    // ESP.getFreeHeap() 为 INTERNAL 口径，含 IRAM/RTC 虚高约 28KB，易误读内存水位
    {
        static unsigned long _pmAliveMs = 0;
        if (millis() - _pmAliveMs > 10000) {
            _pmAliveMs = millis();
#if FASTBEE_ENABLE_MQTT
            auto mqttDebug = acquireMQTTClient();
            if (mqttDebug) {
                ets_printf("[PROTO-ALIVE] handle called heap=%lu mqtt=%p conn=%u\n",
                    (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                    (void*)mqttDebug.get(),
                    mqttDebug->getIsConnected() ? 1U : 0U);
            } else {
                // 区分“配置禁用”（预期行为）与“启动失败”（需排查），避免裸空指针被一律误读为故障
                ets_printf("[PROTO-ALIVE] handle called heap=%lu mqtt=%s\n",
                    (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                    mqttDisabledByConfig ? "disabled" : "NULL(start-failed)");
            }
#else
            ets_printf("[PROTO-ALIVE] handle called heap=%lu mqtt=off\n",
                (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
#endif
        }
    }
    // 内存保护：堆低于安全阈值时跳过重型协议处理（JSON 序列化、Modbus 等），避免 OOM
    // 但 MQTT handle() 必须始终运行：它负责连接状态维护和后台重连调度，
    // 内部已有堆保护（doReconnect() 检查 8KB），不会在堆不足时强行连接
    // PSRAM 设备稳态堆常在 10-12KB（WiFi/lwIP/FreeRTOS 必须在内部 DRAM），
    // 30KB 阈值会导致重型协议永远跳过，MQTT 仍可运行（8KB 阈值足够）
    bool networkReady = true;
    {
        FastBeeFramework* fw = FastBeeFramework::getInstance();
        FBNetworkManager* netMgr = fw ? static_cast<FBNetworkManager*>(fw->getNetworkManager()) : nullptr;
        if (netMgr) {
            networkReady = netMgr->isNetworkConnected();
        }
    }

    uint32_t freeHeap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    uint32_t largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    bool heapSufficient =
        (freeHeap >= FastBee::MemoryBudget::GUARD_WARN_DRAM_FREE) &&
        (largestBlock >= FastBee::MemoryBudget::GUARD_WARN_LARGEST_BLOCK);

    // MQTT handle() 始终运行：内部有堆保护，不会在内存不足时 OOM
#if FASTBEE_ENABLE_MQTT
    {
        auto mqttRead = acquireMQTTClient();
        if (mqttRead) {
            mqttClient->setMinReportInterval(mqttMinReportInterval.load());
            if (networkReady || mqttClient->getIsConnected()) {
                mqttClient->handle();
            } else {
                static unsigned long lastMqttNetworkSkipMs = 0;
                unsigned long now = millis();
                if (now - lastMqttNetworkSkipMs > 10000) {
                    lastMqttNetworkSkipMs = now;
                    LOG_INFO("Protocol Manager: Network not connected, MQTT handle skipped");
                }
            }
#if FASTBEE_ENABLE_REMOTE_RESOURCES
            FastBee::RemoteResourceManager::instance().handleMqtt(mqttClient.get());
#endif
            // 周期性 ets_printf 调试（每10秒），直接写UART不经过Serial缓冲区
            static unsigned long _pmDbgMs = 0;
            if (mqttClient->getIsConnected() && millis() - _pmDbgMs > 10000) {
                _pmDbgMs = millis();
                ets_printf("[PROTO] MQTT handle alive heap=%lu\n", (unsigned long)freeHeap);
            }
        }

        // OTA 下载在独立任务中运行，回调只写入定长队列。所有 MQTT 发布都在
        // loopTask 中完成，避免跨任务并发访问 PubSubClient。
        processFastBeeOtaReplies();
    } // Release the loop's reference before consuming lifecycle requests.
    // Restore can request stop/rebuild; never retain the loop lease across it.
    MqttRouteHandler::checkPendingTestRestore();

    // MQTT 热重建：保存协议配置后由 HTTP handler 置标志，在 loopTask 大栈中执行。
    // 故意放在 heapSufficient 早退之前：低内存时也要消费标志，
    // 由 restartMQTTDeferred 内部门槛判定，失败则回退调度重启保证新配置生效
    if (!mqttLifetime.available()) {
        mqttStopPending = false;
        mqttRestartPending = false;
    }
    if (mqttStopPending) finishMqttStop();
    if (!mqttStopPending && mqttRestartPending.exchange(false)) {
        LOG_INFO("[MQTT] Executing deferred hot-restart in loopTask...");
        if (!restartMQTTDeferred(true) && !mqttRestartPending && !mqttStopPending && !mqttShutdownPending &&
            mqttLifetime.available()) {
            // restartMQTTDeferred 在配置已禁用时也返回 false（属正常停止，
            // 如连续两次保存先启后禁的竞态），此时不应回退重启
            bool stillEnabled = false;
            {
                JsonDocument d;
                if (ConfigStorage::getInstance().loadProtocolSection("mqtt", d)) {
                    stillEnabled = d["mqtt"]["enabled"].as<bool>();
                }
            }
            if (stillEnabled) {
                // 热重建失败（DRAM 不足/碎片化/无可用传输）：
                // 回退到设备重启获得干净堆，与旧版"保存即重启"行为等价，
                // 确保配置必定生效且不会因碎片化堆强行 TLS 握手导致长期不可用
                LOG_WARNING("[MQTT] Hot-restart failed, falling back to device reboot");
                SystemRebooter::scheduleConfigReboot("MQTT hot-restart failed");
            }
        }
    }
#endif

    // Modbus 延迟恢复必须在通用重型协议早退之前消费。通用闸门采用
    // 30KB/16KB 的保守阈值，而 Modbus 重建只需受控的 JSON/UART 资源；
    // 在满足专用准入线时仍可安全重建，否则保留 pending 供下一轮重试。
#if FASTBEE_ENABLE_MODBUS
    if (modbusStopPending && getActiveModbusLeaseCount() == 0) {
        stopModbus();
    }

    if (modbusRestartPending) {
        const uint32_t modbusFree = FastBee::MemoryBudget::dram8Free();
        const uint32_t modbusLargest = FastBee::MemoryBudget::dram8Largest();
        const bool modbusRestartReady =
            modbusFree >= FastBee::MemoryBudget::MODBUS_RESTART_MIN_DRAM &&
            modbusLargest >= FastBee::MemoryBudget::MODBUS_RESTART_MIN_LARGEST;
        if (modbusRestartReady) {
            modbusRestartPending = false;
            LOG_INFO("[Modbus] Executing deferred restart in loopTask...");
            const ModbusRestartResult result = restartModbus();
            if (result == ModbusRestartResult::DeferredBusy) {
                LOG_INFO("[Modbus] Deferred restart waiting for active operation to finish");
            } else if (result == ModbusRestartResult::Failed) {
                // HealthMonitor will retain/requeue its recovery state when a
                // configuration or runtime feature gate still rejects start.
                LOG_WARNING("[Modbus] Deferred restart was rejected; recovery remains pending");
            } else if (result == ModbusRestartResult::IdleNoEnabledTargets) {
                LOG_INFO("[Modbus] Deferred restart completed: no enabled targets, transport remains idle");
            }
        } else {
            static unsigned long lastModbusRestartWaitMs = 0;
            const unsigned long now = millis();
            if (now - lastModbusRestartWaitMs > 10000UL) {
                lastModbusRestartWaitMs = now;
                LOG_WARNINGF("[Modbus] Deferred restart waiting for DRAM (free=%lu largest=%lu)",
                             (unsigned long)modbusFree,
                             (unsigned long)modbusLargest);
            }
        }
    }
#endif

    // 堆保护：低于阈值时跳过其余重型协议处理
    if (!heapSufficient) {
        static unsigned long lastWarnMs = 0;
        unsigned long now = millis();
        if (now - lastWarnMs > 10000) {
            lastWarnMs = now;
            LOG_WARNINGF("[PROTO] Heap low (%lu), skipping heavy protocol handlers", (unsigned long)freeHeap);
        }
        return;
    }

#if FASTBEE_ENABLE_MODBUS
    {
        // Maintenance remains available while a stop/restart is waiting for a
        // long operation to drain. This keeps software-timed coil shutoffs and
        // control-priority handling responsive during that window.
        auto lease = acquireModbusHandlerInternal(0, true);
        if (lease) lease->handle();
    }
#endif
}

void ProtocolManager::handleMessage(ProtocolType type, const String& topic, const String& message) {
    if (rxCallback) rxCallback();
    if (messageCallback) {
        messageCallback(type, topic, message);
    }
}

#if FASTBEE_ENABLE_MQTT
void ProtocolManager::handleMqttMessage(const String& topic,
                                        const String& message,
                                        MqttTopicType topicType) {
    if (topicType == MqttTopicType::OTA_UPGRADE ||
        topicType == MqttTopicType::OTA_BINARY) {
        handleFastBeeOtaCommand(message, topicType);
        return;
    }

    handleMessage(ProtocolType::MQTT, topic, message);
}

void ProtocolManager::handleFastBeeOtaCommand(const String& message,
                                              MqttTopicType topicType) {
    auto mqttRead = acquireMQTTClient();
    if (!mqttRead) return;
    if (topicType == MqttTopicType::OTA_BINARY) {
        LOG_WARNING("FastBee OTA: Binary package protocol is not supported; command ignored");
        return;
    }

    FastBeeOtaProtocol::Command command;
    String error;
    if (!FastBeeOtaProtocol::parseCommand(message, command, error)) {
        LOG_WARNINGF("FastBee OTA: Invalid command (%s)", error.c_str());
        return;
    }

    // Do not claim a task or write Flash unless its status channel is usable.
    // A disconnected broker or disabled/misconfigured canonical reply topic
    // would otherwise leave FastBee waiting forever for status 3/4.
    if (!mqttClient) {
        LOG_WARNING("FastBee OTA: MQTT client unavailable; command ignored before Flash write");
        return;
    }
    if (!mqttClient->getIsConnected()) {
        LOG_WARNING("FastBee OTA: MQTT disconnected; command ignored before Flash write");
        return;
    }
    if (!mqttClient->hasUsableFastBeeOtaReplyTopic()) {
        LOG_WARNING("FastBee OTA: Canonical reply topic disabled or missing; command ignored before Flash write");
        return;
    }
    auto replyStorage = fastBeeOtaReplyQueues.ensureAndAcquire();
    if (!replyStorage) {
        LOG_WARNING("FastBee OTA: Reply storage unavailable or busy; command ignored before Flash write");
        return;
    }
    const auto fastBeeOtaActiveTerminalQueue = replyStorage.terminal();
    const auto fastBeeOtaReplyQueue = replyStorage.replies();
    const auto fastBeeOtaProgressQueue = replyStorage.progress();
    if (!fastBeeOtaReplyQueue || !fastBeeOtaProgressQueue) {
        LOG_WARNING("FastBee OTA: Reply queues unavailable; command ignored before Flash write");
        return;
    }
    if (!fastBeeOtaActiveTerminalQueue) {
        LOG_WARNING("FastBee OTA: Active terminal queue unavailable; command ignored before Flash write");
        return;
    }
    uint8_t cachedTerminalStatus = 0;
    char cachedTerminalVersion[32] = {};
    bool cachedTerminalStillActive = false;
    bool duplicateActiveTask = false;
    bool anotherTaskActive = false;
    portENTER_CRITICAL(&fastBeeOtaStateMux);
    if (fastBeeOtaLastTerminalTaskId == command.taskId) {
        cachedTerminalStatus = fastBeeOtaLastTerminalStatus;
        strlcpy(cachedTerminalVersion, fastBeeOtaLastTerminalVersion,
                sizeof(cachedTerminalVersion));
        cachedTerminalStillActive = fastBeeOtaActiveTaskId == command.taskId;
        anotherTaskActive = fastBeeOtaActiveTaskId != 0 &&
                            fastBeeOtaActiveTaskId != command.taskId;
    } else if (fastBeeOtaActiveTaskId == command.taskId) {
        duplicateActiveTask = true;
    } else if (fastBeeOtaActiveTaskId != 0) {
        anotherTaskActive = true;
    } else {
        fastBeeOtaActiveTaskId = command.taskId;
    }
    portEXIT_CRITICAL(&fastBeeOtaStateMux);

    if (cachedTerminalStatus == FastBeeOtaProtocol::STATUS_SUCCESS ||
        cachedTerminalStatus == FastBeeOtaProtocol::STATUS_FAILED) {
        LOG_INFOF("FastBee OTA: Duplicate delivery for terminal task %ld, status=%u",
                  static_cast<long>(command.taskId),
                  static_cast<unsigned>(cachedTerminalStatus));
        // While the task still owns the OTA slot, its original terminal event
        // is queued or retained for retry. Enqueueing the same reply again can
        // consume the slot reserved for that original event under redelivery.
        if (cachedTerminalStillActive || anotherTaskActive) {
            LOG_INFOF("FastBee OTA: Terminal replay for task %ld deferred while an OTA claim is active",
                      static_cast<long>(command.taskId));
            return;
        }
        queueFastBeeOtaReply(
            command.taskId, String(cachedTerminalVersion), cachedTerminalStatus,
            cachedTerminalStatus == FastBeeOtaProtocol::STATUS_SUCCESS ? 100 : 0);
        return;
    }
    if (duplicateActiveTask) {
        LOG_INFOF("FastBee OTA: Duplicate delivery for active task %ld ignored",
                  static_cast<long>(command.taskId));
        return;
    }
    if (anotherTaskActive) {
        queueFastBeeOtaReply(command.taskId, command.version,
                             FastBeeOtaProtocol::STATUS_FAILED, 0,
                             "Another FastBee OTA task is active");
        return;
    }
#if !FASTBEE_ENABLE_OTA
    queueFastBeeOtaReply(command.taskId, command.version,
                         FastBeeOtaProtocol::STATUS_FAILED, 0,
                         "OTA feature is disabled in this firmware");
    return;
#else
    const MQTTConfig& mqttConfig = mqttClient->getConfig();
    // Read the saved base at command time: changing only OTA settings must
    // take effect without rebuilding the MQTT connection or rebooting.
    String otaBaseUrl;
    if (!command.url.startsWith("http://") && !command.url.startsWith("https://")) {
        JsonDocument otaConfig;
        if (!FastBee::OtaConfigStorage::load(otaConfig, error)) {
            queueFastBeeOtaReply(command.taskId, command.version,
                                 FastBeeOtaProtocol::STATUS_FAILED, 0, error);
            return;
        }
        otaBaseUrl = otaConfig["mqtt"]["otaBaseUrl"] | "";
    }
    String firmwareUrl;
    if (!FastBeeOtaProtocol::resolveFirmwareUrl(
            command.url, otaBaseUrl, mqttConfig.server,
            firmwareUrl, error)) {
        LOG_WARNINGF("FastBee OTA: URL rejected (%s)", error.c_str());
        queueFastBeeOtaReply(command.taskId, command.version,
                             FastBeeOtaProtocol::STATUS_FAILED, 0, error);
        return;
    }

    FastBeeFramework* framework = FastBeeFramework::getInstance();
    OTAManager* otaManager = framework ? framework->getOTAManager() : nullptr;
    if (!otaManager) {
        queueFastBeeOtaReply(command.taskId, command.version,
                             FastBeeOtaProtocol::STATUS_FAILED, 0,
                             "OTA manager is unavailable");
        return;
    }

    const int32_t taskId = command.taskId;
    const String version = command.version;
    queueFastBeeOtaReply(taskId, version,
                         FastBeeOtaProtocol::STATUS_UPGRADING, 0);
    const bool started = otaManager->startOTAAsync(
        firmwareUrl,
        [this, taskId, version](uint8_t progress) {
            if (progress == 0) return;
            queueFastBeeOtaReply(taskId, version,
                                 FastBeeOtaProtocol::STATUS_UPGRADING,
                                 progress);
        },
        [this, taskId, version](bool success, const String& completionError) {
            queueFastBeeOtaReply(
                taskId, version,
                success ? FastBeeOtaProtocol::STATUS_SUCCESS
                        : FastBeeOtaProtocol::STATUS_FAILED,
                success ? 100 : 0,
                completionError);
        },
        false);

    if (!started) {
        String startError = otaManager->getErrorMessage();
        if (startError.isEmpty()) startError = "OTA is already in progress";
        LOG_WARNINGF("FastBee OTA: Failed to start task %ld (%s)",
                     static_cast<long>(taskId), startError.c_str());
        queueFastBeeOtaReply(taskId, version,
                             FastBeeOtaProtocol::STATUS_FAILED, 0,
                             startError);
        return;
    }

    LOG_INFOF("FastBee OTA: Task %ld accepted, version=%s",
              static_cast<long>(taskId), version.c_str());
#endif
}

void ProtocolManager::queueFastBeeOtaReply(int32_t taskId,
                                           const String& version,
                                           uint8_t status,
                                           uint8_t progress,
                                           const String& error) {
    // A claimed OTA task retains the owner until its terminal reply is sent.
    // This lease also keeps storage safe across idle retirement/destruction.
    auto replyStorage = fastBeeOtaReplyQueues.acquire();
    if (!replyStorage) {
        LOG_ERROR("FastBee OTA: Reply storage unavailable");
        return;
    }
    const auto fastBeeOtaActiveTerminalQueue = replyStorage.terminal();
    const auto fastBeeOtaReplyQueue = replyStorage.replies();
    const auto fastBeeOtaProgressQueue = replyStorage.progress();
    const bool terminal = status == FastBeeOtaProtocol::STATUS_SUCCESS ||
                          status == FastBeeOtaProtocol::STATUS_FAILED;
    bool staleProgress = false;
    bool activeTerminal = false;
    if (terminal || status == FastBeeOtaProtocol::STATUS_UPGRADING) {
        // Cache only the terminal result of the task that actually claimed the
        // OTA slot. A temporary busy rejection for another task must not erase
        // the active task's result. Cache before enqueueing so a concurrent
        // duplicate can never append status=2 after status=3/4.
        portENTER_CRITICAL(&fastBeeOtaStateMux);
        if (status == FastBeeOtaProtocol::STATUS_UPGRADING &&
            fastBeeOtaLastTerminalTaskId == taskId &&
            (fastBeeOtaLastTerminalStatus == FastBeeOtaProtocol::STATUS_SUCCESS ||
             fastBeeOtaLastTerminalStatus == FastBeeOtaProtocol::STATUS_FAILED)) {
            staleProgress = true;
        } else if (terminal && fastBeeOtaActiveTaskId == taskId) {
            activeTerminal = true;
            fastBeeOtaLastTerminalTaskId = taskId;
            fastBeeOtaLastTerminalStatus = status;
            strlcpy(fastBeeOtaLastTerminalVersion, version.c_str(),
                    sizeof(fastBeeOtaLastTerminalVersion));
        }
        portEXIT_CRITICAL(&fastBeeOtaStateMux);
    }

    if (staleProgress) {
        LOG_DEBUGF("FastBee OTA: Late progress dropped for terminal task %ld",
                   static_cast<long>(taskId));
        return;
    }

    FastBeeOtaReplyEvent event{};
    event.taskId = taskId;
    event.status = status;
    event.progress = progress > 100 ? 100 : progress;
    strlcpy(event.version, version.c_str(), sizeof(event.version));
    strlcpy(event.error, error.c_str(), sizeof(event.error));

    if (!terminal) {
        if (!fastBeeOtaProgressQueue ||
            xQueueOverwrite(fastBeeOtaProgressQueue, &event) != pdTRUE) {
            LOG_WARNING("FastBee OTA: Progress queue unavailable, event dropped");
        }
        return;
    }

    if (activeTerminal) {
        // The active OTA owner's terminal result must not compete with busy or
        // replay responses. A one-entry overwrite queue is safe because only
        // one task can own the OTA slot, and repeated events are idempotent.
        if (!fastBeeOtaActiveTerminalQueue ||
            xQueueOverwrite(fastBeeOtaActiveTerminalQueue, &event) != pdTRUE) {
            LOG_ERROR("FastBee OTA: Active terminal queue unavailable");
            return;
        }
    } else {
        if (!fastBeeOtaReplyQueue ||
            xQueueSend(fastBeeOtaReplyQueue, &event, 0) != pdTRUE) {
            LOG_WARNING("FastBee OTA: Non-owner terminal reply queue full, event dropped");
            return;
        }
    }

    // Progress has lossy/latest-value semantics. Once a terminal status is
    // queued, discard an older progress event for the same task.
    if (fastBeeOtaProgressQueue) {
        FastBeeOtaReplyEvent queuedProgress{};
        if (xQueuePeek(fastBeeOtaProgressQueue, &queuedProgress, 0) == pdTRUE &&
            queuedProgress.taskId == taskId) {
            xQueueReceive(fastBeeOtaProgressQueue, &queuedProgress, 0);
        }
    }
}

void ProtocolManager::processFastBeeOtaReplies() {
    // Empty, unclaimed storage is reclaimable even while MQTT is disabled.
    // Queued/pending replies and admitted producers prevent retirement.
    fastBeeOtaReplyQueues.releaseIfIdle([this] {
        portENTER_CRITICAL(&fastBeeOtaStateMux);
        const bool idle = fastBeeOtaActiveTaskId == 0;
        portEXIT_CRITICAL(&fastBeeOtaStateMux);
        return idle && !fastBeeOtaHasPendingReply;
    });
    auto mqttRead = acquireMQTTClient();
    if (!mqttRead) return;
    auto replyStorage = fastBeeOtaReplyQueues.acquire();
    if (!replyStorage) return;
    const auto fastBeeOtaActiveTerminalQueue = replyStorage.terminal();
    const auto fastBeeOtaReplyQueue = replyStorage.replies();
    const auto fastBeeOtaProgressQueue = replyStorage.progress();
    if (!fastBeeOtaActiveTerminalQueue || !fastBeeOtaReplyQueue || !mqttClient ||
        !mqttClient->getIsConnected()) {
        return;
    }

    // The active task's terminal result has absolute priority. It replaces a
    // retained progress or non-owner reply, so a full/broken auxiliary queue
    // can never delay status=3/4 or keep a failed owner claimed indefinitely.
    FastBeeOtaReplyEvent activeTerminalEvent{};
    if (xQueueReceive(fastBeeOtaActiveTerminalQueue,
                      &activeTerminalEvent, 0) == pdTRUE) {
        const bool samePendingTerminal =
            fastBeeOtaHasPendingReply &&
            fastBeeOtaPendingReply.taskId == activeTerminalEvent.taskId &&
            fastBeeOtaPendingReply.status == activeTerminalEvent.status;
        if (!samePendingTerminal) {
            if (fastBeeOtaHasPendingReply) {
                LOG_WARNINGF(
                    "FastBee OTA: Active terminal task %ld preempted pending task %ld status=%u",
                    static_cast<long>(activeTerminalEvent.taskId),
                    static_cast<long>(fastBeeOtaPendingReply.taskId),
                    static_cast<unsigned>(fastBeeOtaPendingReply.status));
            }
            fastBeeOtaPendingReply = activeTerminalEvent;
            fastBeeOtaHasPendingReply = true;
            fastBeeOtaLastPublishAttemptMs = 0;
        }
    }

    // A terminal status must supersede a progress item that was retained after
    // a failed publish. Otherwise a recovered connection could report stale
    // status=2 before the already-completed status=3/4.
    if (fastBeeOtaHasPendingReply &&
        fastBeeOtaPendingReply.status == FastBeeOtaProtocol::STATUS_UPGRADING) {
        FastBeeOtaReplyEvent terminalEvent{};
        if (xQueueReceive(fastBeeOtaReplyQueue, &terminalEvent, 0) == pdTRUE) {
            fastBeeOtaPendingReply = terminalEvent;
            fastBeeOtaLastPublishAttemptMs = 0;
        } else {
            bool terminalAlreadyQueued = false;
            portENTER_CRITICAL(&fastBeeOtaStateMux);
            terminalAlreadyQueued =
                fastBeeOtaLastTerminalTaskId == fastBeeOtaPendingReply.taskId &&
                (fastBeeOtaLastTerminalStatus == FastBeeOtaProtocol::STATUS_SUCCESS ||
                 fastBeeOtaLastTerminalStatus == FastBeeOtaProtocol::STATUS_FAILED);
            portEXIT_CRITICAL(&fastBeeOtaStateMux);
            if (terminalAlreadyQueued) {
                fastBeeOtaHasPendingReply = false;
                fastBeeOtaLastPublishAttemptMs = 0;
                return;
            }
        }
    }

    if (!fastBeeOtaHasPendingReply) {
        if (xQueueReceive(fastBeeOtaReplyQueue,
                          &fastBeeOtaPendingReply, 0) != pdTRUE) {
            if (!fastBeeOtaProgressQueue ||
                xQueueReceive(fastBeeOtaProgressQueue,
                              &fastBeeOtaPendingReply, 0) != pdTRUE) {
                return;
            }

            bool terminalAlreadyQueued = false;
            portENTER_CRITICAL(&fastBeeOtaStateMux);
            terminalAlreadyQueued =
                fastBeeOtaLastTerminalTaskId == fastBeeOtaPendingReply.taskId &&
                (fastBeeOtaLastTerminalStatus == FastBeeOtaProtocol::STATUS_SUCCESS ||
                 fastBeeOtaLastTerminalStatus == FastBeeOtaProtocol::STATUS_FAILED);
            portEXIT_CRITICAL(&fastBeeOtaStateMux);
            if (terminalAlreadyQueued) {
                return;
            }
        }
        fastBeeOtaHasPendingReply = true;
    }
    const FastBeeOtaReplyEvent& event = fastBeeOtaPendingReply;

    const unsigned long now = millis();
    if (fastBeeOtaLastPublishAttemptMs != 0 &&
        now - fastBeeOtaLastPublishAttemptMs < 1000UL) {
        return;
    }

    const String payload = FastBeeOtaProtocol::buildReply(
        event.taskId, String(event.version), event.status, event.progress);
    fastBeeOtaLastPublishAttemptMs = now;
    if (!mqttClient->publishOtaReply(payload)) {
        LOG_WARNINGF("FastBee OTA: Reply publish deferred for task %ld",
                     static_cast<long>(event.taskId));
        return;
    }

    const bool otaSucceeded =
        event.status == FastBeeOtaProtocol::STATUS_SUCCESS;
    fastBeeOtaLastPublishAttemptMs = 0;
    LOG_INFOF("FastBee OTA: Published task=%ld status=%u progress=%u",
              static_cast<long>(event.taskId),
              static_cast<unsigned>(event.status),
              static_cast<unsigned>(event.progress));
    if (event.status == FastBeeOtaProtocol::STATUS_FAILED && event.error[0]) {
        LOG_WARNINGF("FastBee OTA: Task %ld failed (%s)",
                     static_cast<long>(event.taskId), event.error);
    }

    // FastBee 必须先收到 status=3，设备才能重启。publish() 成功后仍留出
    // 3 秒给 TCP/MQTT 发送缓冲完成传输。
    fastBeeOtaHasPendingReply = false;
    if (event.status == FastBeeOtaProtocol::STATUS_FAILED) {
        // A failed task remains occupied until status=4 has actually reached
        // the broker, preventing a later task from overwriting its final reply.
        portENTER_CRITICAL(&fastBeeOtaStateMux);
        if (fastBeeOtaActiveTaskId == event.taskId) {
            fastBeeOtaActiveTaskId = 0;
        }
        portEXIT_CRITICAL(&fastBeeOtaStateMux);
    }
    if (otaSucceeded) {
        SystemRebooter::scheduleReboot(
            "FastBee MQTT OTA completed", 3000, RestartReason::OTA_UPDATE);
    }
}
#endif

void ProtocolManager::setTxCallback(CounterCallback cb) { txCallback = cb; }
void ProtocolManager::setRxCallback(CounterCallback cb) { rxCallback = cb; }
void ProtocolManager::setSSECallback(SSEBroadcastCallback callback) { sseCallback = callback; }
void ProtocolManager::setMQTTStatusSSECallback(SimpleSSECallback callback) { mqttStatusSSECallback = callback; }
void ProtocolManager::setModbusStatusSSECallback(SimpleSSECallback callback) { modbusStatusSSECallback = callback; }

// ========== Modbus 数据统一分发出口 ==========

void ProtocolManager::dispatchModbusData(uint8_t slaveAddress, const String& data, ModbusDataSource source) {
#if FASTBEE_ENABLE_MODBUS
    size_t firstPayloadByte = 0;
    while (firstPayloadByte < data.length()) {
        const char c = data[firstPayloadByte];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
        ++firstPayloadByte;
    }
    const bool structuredPayload = firstPayloadByte < data.length() &&
        (data[firstPayloadByte] == '[' || data[firstPayloadByte] == '{');
    // Snapshot the wire encoding while the producing Modbus handler is still
    // alive. Queue consumers must not infer it again after a hot restart.
    // Read only the encoding byte: getConfig() copies the full polling table
    // onto the worker stack and overflows it when a poll returns valid data.
    const bool rawRtuPayload = modbusHandler &&
        modbusHandler->getTransferType() == 1 && !structuredPayload;

    // 1. MQTT 上报（所有来源都上报）
#if FASTBEE_ENABLE_MQTT
    auto mqttRead = acquireMQTTClient();
    if (mqttRead && mqttRead->getIsConnected()) {
        // 统一上报日志：无论 transferType=0(JSON) 或 1(HEX) 都打印上报主题、格式、内容
        // 使用 Serial.printf 避免 LoggerSystem 文件 I/O 加剧异步任务栈压力
        const char* fmt = "JSON";
        if (rawRtuPayload) {
            fmt = "HEX";
        }
        const char* srcName = (source == ModbusDataSource::PeriphExecPoll) ? "PeriphExecPoll"
                            : (source == ModbusDataSource::LiveCallback)   ? "LiveCallback"
                                                                           : "Unknown";
        Serial.printf("[Modbus] TX ▲ src=%s format=%s slave=%d len=%u\n",
                      srcName, fmt, slaveAddress, (unsigned)data.length());
        // PeriphExecPoll 来自异步任务栈，用 queueReportData 避免 TCP/IP 深栈调用导致栈溢出
        // LiveCallback 来自主循环，可直接 publish
        if (source == ModbusDataSource::PeriphExecPoll) {
            if (rawRtuPayload) mqttClient->queueModbusRtuReport(data);
            else mqttClient->queueReportData(data);
        } else {
            if (rawRtuPayload) mqttClient->publishModbusRtuReport(data);
            else mqttClient->publishReportData(data);
        }
    } else {
        Serial.printf("[Modbus] TX ▲ DROPPED (MQTT offline) slave=%d len=%u\n",
                      slaveAddress, (unsigned)data.length());
    }
#endif
    // 2. SSE 实时推送（所有来源都推送）
    if (sseCallback) {
        sseCallback(slaveAddress, data);
    }
    // 3. 规则匹配和轮询数据分发（仅 LiveCallback 来源）
    //    PeriphExecPoll 不分发，因为 PeriphExec 有自己的数据处理流程
    if (source == ModbusDataSource::LiveCallback) {
        if (data.length() > 0 && data[0] == '[') {
            PeriphExecManager::getInstance().handleMqttMessage("modbus/data", data);
            PeriphExecManager::getInstance().handlePollData("modbus", data);
        }
    }
    // 4. 全局消息路由
    handleMessage(ProtocolType::MODBUS, String(slaveAddress), data);
#else
    (void)slaveAddress; (void)data; (void)source;
#endif
}

#if FASTBEE_ENABLE_MQTT
bool ProtocolManager::restartMQTT() {
    if (!mqttLifetime.available()) {
        mqttRestartPending = false;
        return false;
    }
    if (mqttShutdownPending) return false;
    FastBee::MqttClientLifetime::Write lifecycle(mqttLifetime);
    if (!lifecycle) { mqttRestartPending = true; return false; }
    if (mqttShutdownPending) return false;
    if (mqttStopPending) { mqttRestartPending = true; return false; }
    mqttRestartPending.exchange(false); // Consume requests predating this rebuild only.
    // MQTT 启用检查：fail-closed —— 禁用或配置不可用时一律不创建客户端
    {
        JsonDocument enabledDoc;
        bool sectionOk = ConfigStorage::getInstance().loadProtocolSection("mqtt", enabledDoc);
        bool mqttEnabled = sectionOk && enabledDoc["mqtt"]["enabled"].as<bool>();
        if (!sectionOk) {
            // 配置节缺失/解析失败：按禁用处理。若放行，MQTT 会以空 host 反复
            // DNS 重连，每次数 KB 堆分配，最终耗尽内存并锁死 WebMemoryGuard
            LOG_WARNING("Protocol Manager: MQTT config section missing/unreadable, fail-closed as disabled");
        } else if (mqttEnabled && enabledDoc["mqtt"]["server"].as<String>().length() == 0) {
            // 启用但 server 为空同样是死循环重连，第二道防线一并按禁用处理
            LOG_WARNING("Protocol Manager: MQTT enabled but server is empty, fail-closed as disabled");
            mqttEnabled = false;
        }
        if (!mqttEnabled) {
            mqttDisabledByConfig = true;
            if (mqttClient) {
                if (!mqttClient->stop()) { mqttRestartPending = true; return false; }
                mqttClient->setReportRetirementReason(FastBee::MqttRetirementReason::ConfigurationDisabled);
                mqttClient.reset();
            }
            return false;
        }
        mqttDisabledByConfig = false;
    }

    LOG_INFO("Protocol Manager: Restarting MQTT...");

    // 断开现有连接
    if (mqttClient) {
        if (!mqttClient->stop()) { mqttRestartPending = true; return false; }
        mqttClient->setReportRetirementReason(FastBee::MqttRetirementReason::Rebuild);
        mqttClient.reset();
    }

    // 重新创建并初始化。低内存/碎片化场景下即使通过了堆门槛，
    // 构造函数内部的 mutex/queue 分配仍可能失败；使用 nothrow 让本次
    // 重建安全失败，由上层状态接口报告原因，而不是触发异常/abort。
    MQTTClient* freshClient = new (std::nothrow) MQTTClient();
    if (!freshClient) {
        LOG_WARNING("Protocol Manager: MQTT restart allocation failed (nothrow)");
        return false;
    }
    mqttClient.reset(freshClient);
    mqttClient->setMessageCallback([this](const String& topic, const String& message, MqttTopicType tType) {
        handleMqttMessage(topic, message, tType);
    });
    if (!configureMqttTransport(mqttClient.get())) {
        LOG_WARNING("Protocol Manager: MQTT restart aborted, no usable transport");
        mqttClient->setReportRetirementReason(FastBee::MqttRetirementReason::InitializationFailed);
        mqttClient.reset();
        return false;
    }

    // 设置状态变化回调（SSE 推送用）
    if (mqttStatusSSECallback) {
        mqttClient->setStatusChangeCallback([this](const String& data) {
            if (mqttStatusSSECallback) mqttStatusSSECallback(data);
        });
    }

    // 注册实时监测数据提供者（组合 Modbus 缓存数据 + 本地外设数据）
    mqttClient->setMonitorDataProvider([this]() -> String {
#if FASTBEE_ENABLE_MODBUS
        auto lease = acquireModbusHandler(pdMS_TO_TICKS(100));
        String modbusData = lease ? lease->collectCachedPollData() : "[]";
#else
        String modbusData = "[]";
#endif
        String localData = collectLocalSensorData();
        bool hasMb = modbusData.length() > 2;
        bool hasLc = localData.length() > 2;
        if (!hasMb && !hasLc) return "[]";
        if (!hasMb) return localData;
        if (!hasLc) return modbusData;
        // 预分配足够容量，避免多次重新分配
        String result;
        result.reserve(modbusData.length() + localData.length());
        result.concat('[');
        // 直接引用内部 c_str 指针+偏移，避免 substring() 创建临时 String
        result.concat(modbusData.c_str() + 1, modbusData.length() - 2);
        result.concat(',');
        result.concat(localData.c_str() + 1, localData.length() - 2);
        result.concat(']');
        return result;
    });

    mqttClient->setMinReportInterval(mqttMinReportInterval.load());
    if (!mqttClient->begin()) {
        LOG_WARNING("Protocol Manager: MQTT restart begin() failed");
        mqttClient->setReportRetirementReason(FastBee::MqttRetirementReason::InitializationFailed);
        mqttClient.reset();
        return false;
    }

    // 尝试连接
    bool ok = mqttClient->connect();
    if (ok) {
        LOG_INFO("Protocol Manager: MQTT restarted and connected");
    } else {
        LOG_WARNING("Protocol Manager: MQTT restarted but connect failed (will auto-retry)");
    }
    return ok;
}

bool ProtocolManager::restartMQTTDeferred(bool forceRebuild, FastBee::MqttRestartResult* result) {
    if (result) *result = FastBee::MqttRestartResult::NotStarted;
    if (!mqttLifetime.available()) {
        mqttRestartPending = false;
        if (result) *result = FastBee::MqttRestartResult::StorageUnavailable;
        return false;
    }
    if (mqttShutdownPending) return false;
    FastBee::MqttClientLifetime::Write lifecycle(mqttLifetime);
    if (!lifecycle) {
        mqttRestartPending = true;
        if (result) *result = FastBee::MqttRestartResult::Deferred;
        return false;
    }
    if (mqttShutdownPending) return false;
    if (mqttStopPending) {
        mqttRestartPending = true;
        if (result) *result = FastBee::MqttRestartResult::Deferred;
        return false;
    }
    mqttRestartPending.exchange(false); // Requests arriving during rebuild stay pending.
    // 统一入口守卫：禁用配置时停止并清理客户端，不创建或连接 MQTT。
    {
        JsonDocument enabledDoc;
        bool sectionOk = ConfigStorage::getInstance().loadProtocolSection("mqtt", enabledDoc);
        bool mqttEnabled = sectionOk && enabledDoc["mqtt"]["enabled"].as<bool>();
        if (!sectionOk) {
            // 配置节缺失/解析失败：按禁用处理（fail-closed），避免空 host 重连环
            LOG_WARNING("Protocol Manager: MQTT config section missing/unreadable, fail-closed as disabled (deferred)");
        } else if (mqttEnabled && enabledDoc["mqtt"]["server"].as<String>().length() == 0) {
            LOG_WARNING("Protocol Manager: MQTT enabled but server is empty, fail-closed as disabled (deferred)");
            mqttEnabled = false;
        }
        if (!mqttEnabled) {
            mqttDisabledByConfig = true;
            // 如果已有运行中的客户端，停止它
            if (mqttClient) {
                if (!mqttClient->stop()) {
                    mqttRestartPending = true;
                    if (result) *result = FastBee::MqttRestartResult::Deferred;
                    return false;
                }
                mqttClient->setReportRetirementReason(FastBee::MqttRetirementReason::ConfigurationDisabled);
                mqttClient.reset();
            }
            return false;
        }
        mqttDisabledByConfig = false;
    }

    LOG_INFO("Protocol Manager: Restarting MQTT (deferred connect)...");

    // Preserve an active client unless a forced rebuild is requested.
    if (!forceRebuild && mqttClient && !mqttClient->isStopped()) {
        MQTTConfig currentConfig = mqttClient->getConfig();
        if (!currentConfig.server.isEmpty() && currentConfig.port > 0) {
            ets_printf("[MQTT] Deferred restart skipped: client already active (connected=%u)\n",
                       mqttClient->getIsConnected() ? 1U : 0U);
            LOG_INFO("Protocol Manager: MQTT deferred restart skipped, active client preserved");
            if (result) *result = FastBee::MqttRestartResult::Ready;
            return true;
        }
    }

    // 先释放旧客户端，然后再检查堆内存
    // 之前在释放前检查导致误判（旧客户端占用 ~12KB，释放后堆足够）
    if (mqttClient) {
        if (!mqttClient->stop()) {
            mqttRestartPending = true;
            if (result) *result = FastBee::MqttRestartResult::Deferred;
            return false;
        }
        mqttClient->setReportRetirementReason(FastBee::MqttRetirementReason::Rebuild);
        mqttClient.reset();
    }

    // FreeRTOS vTaskDelete 不会立即释放任务栈内存，需等待 idle 任务清理
    // 短暂延迟让 idle 任务有机会回收内存，避免堆内存检查误判
    delay(10);

    // 堆保护：释放旧客户端后仍不足，放弃重建
    // MQTTS 需要更多内存（SSL/TLS 握手约 42KB），MQTT 仅需约 6KB
    // 检查配置中的 scheme 字段动态调整阈值
    uint32_t freeHeap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    uint32_t largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    
    // 尝试从配置读取 scheme（如果 MQTT 客户端未创建，从配置文件读取）
    bool isMqtts = false;
    {
        JsonDocument doc;
        if (ConfigStorage::getInstance().loadProtocolSection("mqtt", doc)) {
            isMqtts = FastBee::mqttRequiresTls(doc["mqtt"]);
        }
    }
    
    // 无 PSRAM 时 mbedTLS 大缓冲全部落内部 DRAM，使用严格门槛
#if defined(BOARD_HAS_PSRAM) && defined(MALLOC_CAP_SPIRAM)
    const bool tlsUsesPsram = psramFound();
#else
    const bool tlsUsesPsram = false;
#endif
    uint32_t minHeap = isMqtts
        ? (tlsUsesPsram ? FastBee::MemoryBudget::MQTTS_MIN_DRAM_FREE
                        : FastBee::MemoryBudget::MQTTS_NO_PSRAM_MIN_DRAM_FREE)
        : FastBee::MemoryBudget::MQTT_MIN_HEAP;
    uint32_t minLargestBlock = isMqtts
        ? (tlsUsesPsram ? FastBee::MemoryBudget::MQTTS_MIN_LARGEST_BLOCK
                        : FastBee::MemoryBudget::MQTTS_NO_PSRAM_MIN_LARGEST_BLOCK)
        : FastBee::MemoryBudget::MQTT_MIN_LARGEST_BLOCK;
    if (freeHeap < minHeap || largestBlock < minLargestBlock) {
        if (isMqtts &&
            freeHeap >= FastBee::MemoryBudget::GUARD_CRITICAL_DRAM_FREE &&
            largestBlock >= FastBee::MemoryBudget::GUARD_CRITICAL_LARGEST_BLOCK) {
            LOG_WARNINGF("Protocol Manager: DRAM marginal for MQTTS deferred restart (dram=%lu largest=%lu need=%lu/%lu), creating client and deferring reconnect",
                         (unsigned long)freeHeap,
                         (unsigned long)largestBlock,
                         (unsigned long)minHeap,
                         (unsigned long)minLargestBlock);
        } else {
            LOG_WARNINGF("Protocol Manager: DRAM too low for %s deferred restart (dram=%lu largest=%lu need=%lu/%lu), skipping",
                         isMqtts ? "MQTTS" : "MQTT",
                         (unsigned long)freeHeap,
                         (unsigned long)largestBlock,
                         (unsigned long)minHeap,
                         (unsigned long)minLargestBlock);
            return false;
        }
    }

    // 重新创建并初始化（仅加载配置，不阻塞连接）。即使堆门槛通过，
    // 构造函数内部的小块分配也可能失败，必须避免 throwing new 在
    // C3/经典 ESP32 上把低内存问题升级为异常重启。
    MQTTClient* freshClient = new (std::nothrow) MQTTClient();
    if (!freshClient) {
        LOG_WARNING("Protocol Manager: MQTT deferred restart allocation failed (nothrow)");
        return false;
    }
    mqttClient.reset(freshClient);
    mqttClient->setMessageCallback([this](const String& topic, const String& message, MqttTopicType tType) {
        handleMqttMessage(topic, message, tType);
    });
    if (!configureMqttTransport(mqttClient.get())) {
        LOG_WARNING("Protocol Manager: MQTT deferred restart aborted, no usable transport");
        mqttClient->setReportRetirementReason(FastBee::MqttRetirementReason::InitializationFailed);
        mqttClient.reset();
        return false;
    }

    // 设置状态变化回调（SSE 推送用）
    if (mqttStatusSSECallback) {
        mqttClient->setStatusChangeCallback([this](const String& data) {
            if (mqttStatusSSECallback) mqttStatusSSECallback(data);
        });
    }

    // 注册实时监测数据提供者（组合 Modbus 缓存数据 + 本地外设数据）
    mqttClient->setMonitorDataProvider([this]() -> String {
#if FASTBEE_ENABLE_MODBUS
        auto lease = acquireModbusHandler(pdMS_TO_TICKS(100));
        String modbusData = lease ? lease->collectCachedPollData() : "[]";
#else
        String modbusData = "[]";
#endif
        String localData = collectLocalSensorData();
        bool hasMb = modbusData.length() > 2;
        bool hasLc = localData.length() > 2;
        if (!hasMb && !hasLc) return "[]";
        if (!hasMb) return localData;
        if (!hasLc) return modbusData;
        // 预分配足够容量，避免多次重新分配
        String result;
        result.reserve(modbusData.length() + localData.length());
        result.concat('[');
        // 直接引用内部 c_str 指针+偏移，避免 substring() 创建临时 String
        result.concat(modbusData.c_str() + 1, modbusData.length() - 2);
        result.concat(',');
        result.concat(localData.c_str() + 1, localData.length() - 2);
        result.concat(']');
        return result;
    });

    mqttClient->setMinReportInterval(mqttMinReportInterval.load());
    if (!mqttClient->begin(500)) {  // deferred 重启：Web 已在运行，仅留 500ms 让任务初始化
        LOG_WARNING("Protocol Manager: MQTT deferred restart begin() failed");
        mqttClient->setReportRetirementReason(FastBee::MqttRetirementReason::InitializationFailed);
        mqttClient.reset();
        return false;
    }

    // 不调用 connect()，由 MQTTClient::handle() 的 auto-reconnect 在 loop 中异步连接
    LOG_INFO("Protocol Manager: MQTT config reloaded, will auto-connect in loop (500ms startup)");
    if (result) *result = FastBee::MqttRestartResult::Ready;
    return true;
}

FastBee::MqttClientLifetime::Write ProtocolManager::prepareMqttNetworkSwitch() {
    FastBee::MqttClientLifetime::Write lifecycle(mqttLifetime);
    // Preserve permanent failure for the network caller; an empty default
    // lease still means an ordinary temporary rejection elsewhere.
    if (lifecycle.storageUnavailable()) return lifecycle;
    if (!lifecycle || mqttShutdownPending) return {};
    if (mqttClient) {
        const bool resume = !mqttClient->isStopped() && !mqttStopPending;
        // Publish before the potentially slow stop so a later manual stop can
        // cancel this intent, rather than having it overwritten after draining.
        if (resume) mqttRestartPending = true;
        // Destroy while the old transport still exists. A stopped MQTTClient
        // retains _externalClient, and its destructor disconnects it again.
        // Merely calling stop() would leave that pointer dangling on rebuild.
        if (!mqttClient->stop()) return {}; // Keep old network transport alive while worker drains.
        mqttClient->setReportRetirementReason(FastBee::MqttRetirementReason::NetworkSwitch);
        mqttClient.reset();
    }
    // Writers/readers remain excluded until the network caller leaves scope.
    // The loop may service the queued rebuild only after this lease releases.
    return lifecycle;
}

bool ProtocolManager::stopMQTT() {
    if (!mqttLifetime.available()) {
        mqttStopPending = false;
        mqttRestartPending = false;
        return false;
    }
    mqttStopPending = true;
    mqttRestartPending = false;
    return finishMqttStop();
}

bool ProtocolManager::finishMqttStop() {
    if (!mqttLifetime.available()) {
        mqttStopPending = false;
        return false;
    }
    FastBee::MqttClientLifetime::Write lifecycle(mqttLifetime);
    if (!lifecycle) return false;
    LOG_INFO("Protocol Manager: Stopping MQTT...");
    if (mqttClient) {
        if (!mqttClient->stop()) return false;
    }
    mqttStopPending = false;
    return true;
}

void ProtocolManager::requestMqttRestartAsync() {
    if (!mqttLifetime.available()) {
        mqttRestartPending = false;
        return;
    }
    // 仅置标志：实际重建由 handle() 在 loopTask（16KB 栈）中执行，
    // 避免在 async_tcp 小栈任务里做 TLS 销毁/重建
    LOG_INFO("Protocol Manager: MQTT restart requested (deferred to loopTask)");
    mqttRestartPending = true;
}
#endif // FASTBEE_ENABLE_MQTT (restartMQTT + restartMQTTDeferred + stopMQTT)

#if FASTBEE_ENABLE_MODBUS
ModbusRestartResult ProtocolManager::restartModbus() {
    LOG_INFO("Protocol Manager: Restarting Modbus...");
    modbusRestartResultAvailable = false;
    const auto completeRestart = [this](ModbusRestartResult result) {
        modbusLastRestartResult = result;
        modbusRestartResultAvailable = true;
        return result;
    };

    // Worker/HTTP Modbus operations retain an active lease without holding the
    // lifecycle mutex. A hot restart therefore checks the lease count while
    // holding the lifecycle lock and defers handler destruction until zero.
    ModbusLifecycleGuard lifecycle(modbusLifecycleMutex, pdMS_TO_TICKS(100));
    if (!lifecycle.isLocked()) {
        modbusRestartPending = true;
        LOG_INFO("Protocol Manager: Modbus restart deferred; operation still active");
        return ModbusRestartResult::DeferredBusy;
    }
    if (modbusShutdownPending) {
        LOG_INFO("Protocol Manager: Modbus restart rejected during shutdown");
        return completeRestart(ModbusRestartResult::Failed);
    }
    if (getActiveModbusLeaseCount() > 0) {
        modbusRestartPending = true;
        LOG_INFO("Protocol Manager: Modbus restart deferred; active handler lease");
        return ModbusRestartResult::DeferredBusy;
    }
    modbusStopPending = false;
    modbusRestartPending = false;

    // 运行时功能门控：用户可在 Web UI 禁用 modbus
    if (!FastBee::RuntimeFeatureManager::instance().shouldEnable("modbus")) {
        LOG_WARNING("Protocol Manager: Modbus disabled by runtime feature config, skipping");
        return completeRestart(ModbusRestartResult::Failed);
    }

    // 内存预算门控：编译启用但启动时 DRAM 不足则自动降级
    if (FastBee::FeatureBudgetRegistry::instance().isDenied("modbus")) {
        LOG_WARNING("Protocol Manager: Modbus disabled by memory budget, skipping");
        return completeRestart(ModbusRestartResult::Failed);
    }

    // 堆保护：防止低内存时 JSON 解析/对象创建触发 abort()。
    // 使用实际可分配的内部 8-bit DRAM，避免 PSRAM/IRAM 统计放行边缘重建。
    const uint32_t freeHeap = FastBee::MemoryBudget::dram8Free();
    const uint32_t largestBlock = FastBee::MemoryBudget::dram8Largest();
    if (freeHeap < FastBee::MemoryBudget::MODBUS_RESTART_MIN_DRAM ||
        largestBlock < FastBee::MemoryBudget::MODBUS_RESTART_MIN_LARGEST) {
        LOG_WARNINGF("Protocol Manager: Heap too low for Modbus restart (free=%lu largest=%lu), skipping",
                     (unsigned long)freeHeap, (unsigned long)largestBlock);
        return completeRestart(ModbusRestartResult::Failed);
    }

    // 停止现有实例
    if (modbusHandler) {
        // Remove every callback which can reach the old instance before its
        // storage is released. New callbacks are installed only after the new
        // handler has begun successfully.
        PeriphExecManager::getInstance().setModbusReadCallback(nullptr);
        PeriphExecManager::getInstance().setModbusRawSendCallback(nullptr);
        unregisterModbusSubDevices();
        modbusHandler->end();
        modbusHandler.reset();
    }

    // 读取 protocol.json 配置
    if (!LittleFS.exists("/config/protocol.json")) {
        LOG_WARNING("Protocol Manager: No protocol config found for Modbus");
        return completeRestart(ModbusRestartResult::Failed);
    }

    // 优化：使用 Filter 仅反序列化 modbusRtu 子节点，堆峰值从 ~15KB 降至 ~12KB
    // （modbusRtu 占比最大，但掉 mqtt/publishTopics/subscribeTopics 仍可省数 KB）
    JsonDocument doc;
    if (!ConfigStorage::getInstance().loadProtocolSection("modbusRtu", doc)) {
        LOG_WARNING("Protocol Manager: modbusRtu section missing/invalid, fallback to full load");
        File f = LittleFS.open("/config/protocol.json", "r");
        if (!f) {
            LOG_ERROR("Protocol Manager: Failed to open protocol config");
            return completeRestart(ModbusRestartResult::Failed);
        }
        DeserializationError err = deserializeJson(doc, f);
        f.close();
        if (err) {
            LOG_ERRORF("Protocol Manager: JSON parse error: %s", err.c_str());
            return completeRestart(ModbusRestartResult::Failed);
        }
    }

    JsonVariant rtu = doc["modbusRtu"];
    if (!rtu.is<JsonObject>()) {
        LOG_WARNING("Protocol Manager: No modbusRtu config found");
        return completeRestart(ModbusRestartResult::Failed);
    }

    // 从嵌套的 modbusRtu 配置构建 ModbusConfig
    ModbusConfig modbusConfig;

    // 从外设管理器获取串口配置（引脚 + 波特率）
    String peripheralId = rtu["peripheralId"] | "";
    if (peripheralId.isEmpty()) {
        LOG_ERROR("Modbus RTU: No peripheralId configured, cannot start");
        return completeRestart(ModbusRestartResult::Failed);
    }

    PeripheralConfig periph;
    if (!PeripheralManager::getInstance().getPeripheralSnapshot(peripheralId, periph)) {
        LOG_ERRORF("Modbus RTU: Peripheral '%s' not found", peripheralId.c_str());
        return completeRestart(ModbusRestartResult::Failed);
    }

    if (periph.type != PeripheralType::UART) {
        LOG_ERRORF("Modbus RTU: Peripheral '%s' is not UART type", peripheralId.c_str());
        return completeRestart(ModbusRestartResult::Failed);
    }

    // 引脚约定: pins[0]=RX, pins[1]=TX
    modbusConfig.rxPin = periph.pins[0];
    modbusConfig.txPin = periph.pins[1];
    modbusConfig.baudRate = periph.params.uart.baudRate > 0 ? periph.params.uart.baudRate : 9600;

    LOG_INFOF("Modbus RTU: Using peripheral '%s' - RX:%d, TX:%d, Baud:%d",
              peripheralId.c_str(), modbusConfig.rxPin, modbusConfig.txPin, modbusConfig.baudRate);

    // dePin 保留从 protocol.json 读取（RS485 方向控制是 Modbus 特有配置）
    int dePin = rtu["dePin"] | -1;
    modbusConfig.dePin = (dePin < 0) ? 255 : (uint8_t)dePin;
    modbusConfig.slaveAddress = rtu["slaveAddress"] | (uint8_t)1;
    modbusConfig.responseTimeout = 1000;  // 固定默认值，实际由 master.responseTimeout 控制
    modbusConfig.transferType = rtu["transferType"] | (uint8_t)0;
    // workMode 已移除：由 ModbusHandler::getWorkMode() 根据轮询任务配置动态推导

    String modeStr = rtu["mode"] | "slave";
    modbusConfig.mode = (modeStr == "master") ? MODBUS_MASTER : MODBUS_SLAVE;

    // 解析 Master 配置
    bool hasEnabledPollTask = false;
    bool hasEnabledSubDevice = false;
    if (rtu.containsKey("master")) {
        JsonObject masterObj = rtu["master"];
        modbusConfig.master.responseTimeout = masterObj["responseTimeout"] | (uint16_t)1000;
        modbusConfig.master.maxRetries = masterObj["maxRetries"] | (uint8_t)2;
        modbusConfig.master.interPollDelay = masterObj["interPollDelay"] | (uint16_t)100;

        if (masterObj.containsKey("tasks")) {
            JsonArray tasksArr = masterObj["tasks"];
            modbusConfig.master.taskCount = 0;
            for (JsonVariant taskVar : tasksArr) {
                if (modbusConfig.master.taskCount >= Protocols::MODBUS_MAX_POLL_TASKS) break;
                JsonObject t = taskVar.as<JsonObject>();
                PollTask& task = modbusConfig.master.tasks[modbusConfig.master.taskCount];
                task.slaveAddress = t["slaveAddress"] | (uint8_t)1;
                task.functionCode = t["functionCode"] | (uint8_t)0x03;
                task.startAddress = t["startAddress"] | (uint16_t)0;
                task.quantity     = t["quantity"] | (uint16_t)10;
                task.pollInterval = t["pollInterval"] | (uint16_t)Protocols::MODBUS_DEFAULT_POLL_INTERVAL;
                task.enabled      = t["enabled"] | true;
                // 向后兼容：优先读取 name，回退到 label
                const char* taskName = t["name"] | (t["label"] | "");
                strlcpy(task.name, taskName, sizeof(task.name));

                // Invalid saved mappings disable this task; never silently reinterpret them.
                if (!FastBee::validModbusTaskMappings(t)) task.enabled = false;
                // 解析寄存器映射
                task.mappingCount = 0;
                if (t.containsKey("mappings")) {
                    JsonArray mappingsArr = t["mappings"];
                    for (JsonVariant mv : mappingsArr) {
                        if (task.mappingCount >= Protocols::MODBUS_MAX_MAPPINGS_PER_TASK) break;
                        JsonObject m = mv.as<JsonObject>();
                        RegisterMapping& mapping = task.mappings[task.mappingCount];
                        if (!FastBee::readModbusRegisterMapping(m, task.quantity, mapping)) {
                            task.enabled = false;
                            continue;
                        }
                        task.mappingCount++;
                    }
                }

                if (task.enabled) hasEnabledPollTask = true;
                modbusConfig.master.taskCount++;
            }
        }

        // 解析子设备
        if (masterObj.containsKey("devices")) {
            JsonArray devicesArr = masterObj["devices"];
            modbusConfig.master.deviceCount = 0;
            for (JsonVariant dv : devicesArr) {
                if (modbusConfig.master.deviceCount >= Protocols::MODBUS_MAX_SUB_DEVICES) break;
                JsonObject d = dv.as<JsonObject>();
                ModbusSubDevice& dev = modbusConfig.master.devices[modbusConfig.master.deviceCount];
                const char* dName = d["name"] | "Device";
                StringUtils::copyUtf8(dev.name, sizeof(dev.name), dName);
                const char* sid = d["sensorId"] | "";
                StringUtils::copyUtf8(dev.sensorId, sizeof(dev.sensorId), sid);
                const char* dType = d["deviceType"] | "relay";
                StringUtils::copyUtf8(dev.deviceType, sizeof(dev.deviceType), dType);
                dev.slaveAddress    = d["slaveAddress"] | (uint8_t)1;
                dev.channelCount    = d["channelCount"] | (uint8_t)2;
                dev.coilBase        = d["coilBase"] | (uint16_t)0;
                dev.ncMode          = d["ncMode"] | false;
                dev.controlProtocol = d["controlProtocol"] | (uint8_t)0;
                dev.batchRegister   = d["batchRegister"] | (uint16_t)0;
                dev.batchRegType    = d["batchRegType"] | (uint8_t)0;
                dev.delayMode       = d["delayMode"] | (uint8_t)0;
                dev.baudRateMode    = d["baudRateMode"] | (uint8_t)0;
                dev.baudRateReg     = d["baudRateReg"] | (uint16_t)0;
                dev.addressReg      = d["addressReg"] | (uint16_t)0;
                dev.pwmRegBase      = d["pwmRegBase"] | (uint16_t)0;
                dev.pwmResolution   = d["pwmResolution"] | (uint8_t)8;
                dev.pidDecimals     = d["pidDecimals"] | (uint8_t)1;
                dev.motorDecimals   = d["motorDecimals"] | (uint8_t)0;
                dev.motorMinPosition = d["motorMinPosition"] | (int32_t)0;
                dev.motorMaxPosition = d["motorMaxPosition"] | (int32_t)0;
                dev.motorCurrentPosition = d["motorCurrentPosition"] | (int32_t)0;
                dev.motorMoveStep = d["motorMoveStep"] | (int32_t)0;
                dev.motorLastPulse = d["motorLastPulse"] | (uint16_t)0;
                dev.enabled         = d["enabled"] | true;
                if (dev.enabled) hasEnabledSubDevice = true;
                if (d.containsKey("pidAddrs") && d["pidAddrs"].is<JsonArray>()) {
                    JsonArray pa = d["pidAddrs"].as<JsonArray>();
                    for (int i = 0; i < 6 && i < (int)pa.size(); i++)
                        dev.pidAddrs[i] = pa[i] | (uint16_t)0;
                }
                if (d.containsKey("motorRegs") && d["motorRegs"].is<JsonArray>()) {
                    JsonArray mr = d["motorRegs"].as<JsonArray>();
                    for (int i = 0; i < 5 && i < (int)mr.size(); i++)
                        dev.motorRegs[i] = mr[i] | (uint16_t)0;
                }
                if (const char* error = loadMotorProtocol(d, dev.motorProtocol))
                    LOG_WARNINGF("Modbus motor %u: %s; control blocked", dev.slaveAddress, error);
                if (const char* error = loadMotorTravelDraft(d, dev.motorTravelDraft))
                    LOG_WARNINGF("Modbus motor %u: %s; travel draft invalid", dev.slaveAddress, error);
                modbusConfig.master.deviceCount++;
            }
        }
    }

    // A structured-data master can intentionally contain only disabled
    // templates. Raw passthrough is different: it still needs a live handler
    // and UART even when there are no polling tasks or registered devices.
    if (modbusConfig.mode == MODBUS_MASTER &&
        modbusConfig.transferType != 1 &&
        !hasEnabledPollTask && !hasEnabledSubDevice) {
        LOG_INFO("Protocol Manager: Modbus master has no enabled tasks/devices; keeping transport stopped");
        return completeRestart(ModbusRestartResult::IdleNoEnabledTargets);
    }

    ModbusHandler::sanitizeConfig(modbusConfig);

    // 创建新实例并设置回调。低内存恢复路径必须以可控失败结束，
    // 不能让 throwing new 把设备直接带入异常重启。
    ModbusHandler* freshModbus = new (std::nothrow) ModbusHandler();
    if (!freshModbus) {
        LOG_WARNING("Protocol Manager: Modbus allocation failed (nothrow)");
        return completeRestart(ModbusRestartResult::Failed);
    }
    modbusHandler.reset(freshModbus);

    modbusHandler->setDataCallback([this](uint8_t address, const String& data) {
        // 统一通过 dispatchModbusData 分发（LiveCallback 路径：MQTT + SSE + 规则匹配）
        dispatchModbusData(address, data, ModbusDataSource::LiveCallback);
    });

    // 注册 Modbus 一次性读取回调到 PeriphExec
    PeriphExecManager::getInstance().setModbusReadCallback(
        [this](const String& params) -> String {
            return this->executeModbusRead(params);
        }
    );

    // 注册 Modbus 原始 HEX 帧透传回调到 PeriphExec
    PeriphExecManager::getInstance().setModbusRawSendCallback(
        [this](const String& hexPayload) -> String {
            return this->executeModbusRawSend(hexPayload);
        }
    );

    // 设置状态变化回调（SSE 推送用）
    if (modbusStatusSSECallback) {
        modbusHandler->setStatusChangeCallback([this](const String& data) {
            if (modbusStatusSSECallback) modbusStatusSSECallback(data);
        });
    }

    bool ok = modbusHandler->begin(modbusConfig);
    if (ok) {
        LOG_INFOF("Protocol Manager: Modbus restarted in %s mode, %d tasks, %d devices",
                  (modbusConfig.mode == MODBUS_MASTER) ? "Master" : "Slave",
                  modbusConfig.master.taskCount,
                  modbusConfig.master.deviceCount);

        // 注册 Modbus 子设备为标准外设并设置通信回调
        registerModbusSubDevices(modbusConfig);
    } else {
        LOG_WARNING("Protocol Manager: Modbus restart failed");
    }
    return completeRestart(ok ? ModbusRestartResult::Started
                              : ModbusRestartResult::Failed);
}

void ProtocolManager::stopModbus() {
    LOG_INFO("Protocol Manager: Stopping Modbus...");
    // Publish the stop intent before taking the lifecycle lock so new public
    // operations stop entering while an existing lease drains.
    modbusStopPending = true;
    modbusRestartPending = false;
    ModbusLifecycleGuard lifecycle(modbusLifecycleMutex, pdMS_TO_TICKS(100));
    if (!lifecycle.isLocked()) {
        LOG_INFO("Protocol Manager: Modbus stop deferred; lifecycle busy");
        return;
    }
    const uint16_t activeLeases = getActiveModbusLeaseCount();
    if (activeLeases > 0) {
        LOG_INFOF("Protocol Manager: Modbus stop deferred; %u active lease(s)",
                  static_cast<unsigned int>(activeLeases));
        return;
    }
    modbusRestartResultAvailable = false;
    PeriphExecManager::getInstance().setModbusReadCallback(nullptr);
    PeriphExecManager::getInstance().setModbusRawSendCallback(nullptr);

    // 注销 Modbus 外设和通信回调
    unregisterModbusSubDevices();

    if (modbusHandler) {
        modbusHandler->end();
        modbusHandler.reset();
    }
    modbusStopPending = false;
}

bool ProtocolManager::restartModbusDeferred() {
    LOG_INFO("Protocol Manager: Modbus restart deferred to loopTask");
    ModbusLifecycleGuard lifecycle(modbusLifecycleMutex, pdMS_TO_TICKS(100));
    if (!lifecycle.isLocked()) return false;
    if (modbusShutdownPending) return false;
    modbusStopPending = false;
    modbusRestartResultAvailable = false;
    modbusRestartPending = true;
    return true;
}

bool ProtocolManager::consumeModbusRestartResult(ModbusRestartResult& result) {
    if (!modbusRestartResultAvailable) {
        return false;
    }
    result = modbusLastRestartResult;
    modbusRestartResultAvailable = false;
    return true;
}

// ========== MQTT触发的Modbus一次性读取 ==========

String ProtocolManager::executeModbusRead(const String& paramsJson) {
    FastBeeJsonDoc resultDoc;
    JsonArray resultArr = resultDoc.to<JsonArray>();

    auto modbusLease = acquireModbusHandler(pdMS_TO_TICKS(2000));
    ModbusHandler* activeModbus = modbusLease.get();

    // 检查 Modbus 是否可用
    if (!activeModbus) {
        JsonObject err = resultArr.add<JsonObject>();
        err["id"] = "modbus_read";
        err["value"] = "0";
        err["remark"] = "error:not_initialized";
        String out;
        serializeJson(resultDoc, out);
        return out;
    }

    if (activeModbus->getMode() != MODBUS_MASTER) {
        JsonObject err = resultArr.add<JsonObject>();
        err["id"] = "modbus_read";
        err["value"] = "0";
        err["remark"] = "error:not_master_mode";
        String out;
        serializeJson(resultDoc, out);
        return out;
    }

    // 解析参数 JSON
    JsonDocument paramsDoc;
    DeserializationError parseErr = deserializeJson(paramsDoc, paramsJson.c_str(), paramsJson.length());
    if (parseErr) {
        LOG_WARNINGF("[Modbus] OneShot: JSON parse error: %s", parseErr.c_str());
        JsonObject err = resultArr.add<JsonObject>();
        err["id"] = "modbus_read";
        err["value"] = "0";
        err["remark"] = "error:invalid_params";
        String out;
        serializeJson(resultDoc, out);
        return out;
    }

    // 获取 tasks 数组（支持三种格式：{tasks:[...]}, [...], {slaveAddress:...}）
    JsonDocument tasksDoc;
    JsonArray tasks;
    if (paramsDoc.is<JsonObject>() && paramsDoc["tasks"].is<JsonArray>()) {
        tasks = paramsDoc["tasks"].as<JsonArray>();
    } else if (paramsDoc.is<JsonArray>()) {
        tasks = paramsDoc.as<JsonArray>();
    } else if (paramsDoc.is<JsonObject>() && paramsDoc.containsKey("slaveAddress")) {
        // 单个任务对象，包装为数组
        JsonArray arr = tasksDoc.to<JsonArray>();
        arr.add(paramsDoc);
        tasks = arr;
    }

    if (tasks.isNull() || tasks.size() == 0) {
        JsonObject err = resultArr.add<JsonObject>();
        err["id"] = "modbus_read";
        err["value"] = "0";
        err["remark"] = "error:invalid_params";
        String out;
        serializeJson(resultDoc, out);
        return out;
    }

    LOG_INFOF("[Modbus] OneShot: executing %d task(s) from MQTT command", tasks.size());
    const bool rawTransfer = activeModbus->getTransferType() == 1;

    // 逐任务执行
    for (JsonVariant tv : tasks) {
        JsonObject t = tv.as<JsonObject>();
        uint8_t  slaveAddr = t["slaveAddress"] | (uint8_t)1;
        uint8_t  fc        = t["functionCode"] | (uint8_t)0x03;
        uint16_t startAddr = t["startAddress"] | (uint16_t)0;
        uint16_t qty       = t["quantity"] | (uint16_t)1;

        // 执行一次性读取。RAW 模式同时保留事务层 CRC/地址校验后的原始 ADU，
        // 避免后续寄存器解析和重新编码改变设备实际响应。
        String rawResponseHex;
        OneShotResult readResult = activeModbus->readRegistersOnce(
            slaveAddr, fc, startAddr, qty, false, 0, 0xFF,
            rawTransfer ? &rawResponseHex : nullptr);

        if (rawTransfer && !rawResponseHex.isEmpty()) {
            dispatchModbusData(
                slaveAddr, rawResponseHex, ModbusDataSource::PeriphExecPoll);
            continue;
        }

        if (readResult.error != ONESHOT_SUCCESS) {
            // 生成错误响应
            JsonObject err = resultArr.add<JsonObject>();
            err["id"] = "modbus_read";
            err["value"] = "0";
            switch (readResult.error) {
                case ONESHOT_TIMEOUT:
                    err["remark"] = "error:timeout";
                    break;
                case ONESHOT_CRC_ERROR:
                    err["remark"] = "error:crc_error";
                    break;
                case ONESHOT_EXCEPTION: {
                    char buf[24];
                    snprintf(buf, sizeof(buf), "error:exception:0x%02X", readResult.exceptionCode);
                    err["remark"] = buf;
                    break;
                }
                case ONESHOT_BUSY:
                    err["remark"] = "error:busy";
                    break;
                default:
                    err["remark"] = "error:unknown";
                    break;
            }
            continue;
        }

        // 读取成功，根据传输类型决定上报格式
        if (rawTransfer) {
            // 原始字符串分配失败时不能用解析值伪造一个响应帧。
            LOG_WARNING("[Modbus] OneShot: validated raw ADU unavailable, report skipped");
        } else if (t.containsKey("mappings") && t["mappings"].is<JsonArray>()) {
            // JSON模式 + 有映射：应用映射转换
            JsonArray mappings = t["mappings"].as<JsonArray>();
            for (JsonVariant mv : mappings) {
                JsonObject m = mv.as<JsonObject>();
                RegisterMapping mapping;
                if (!FastBee::readModbusRegisterMapping(m, readResult.count, mapping) || !mapping.sensorId[0]) continue;
                char valBuf[96];
                if (!FastBee::formatModbusMapping(readResult.data, readResult.count, mapping, valBuf, sizeof(valBuf))) continue;
                const char* sensorId = mapping.sensorId;

                JsonObject item = resultArr.add<JsonObject>();
                item["id"] = sensorId;
                item["value"] = valBuf;
                item["remark"] = "";
            }
        } else {
            // 无映射：返回原始寄存器值
            for (uint16_t i = 0; i < readResult.count; i++) {
                JsonObject item = resultArr.add<JsonObject>();
                char idBuf[24];
                snprintf(idBuf, sizeof(idBuf), "reg_%d", startAddr + i);
                item["id"] = idBuf;
                item["value"] = String(readResult.data[i]);
                item["remark"] = "";
            }
        }
    }

    String out;
    serializeJson(resultDoc, out);
    LOG_INFOF("[Modbus] OneShot: result: %s", out.c_str());
    return out;
}

// ========== MQTT触发的Modbus原始HEX帧透传 ==========

static OneShotResult protocolModbusDenied(
    FastBee::OrdinaryControlResult& control, const char* reason) {
    control = {}; control.evaluated = true; control.reason = reason;
    OneShotResult result;
    result.error = ONESHOT_CONTROL_REJECTED; result.controlReason = reason;
    return result;
}

static OneShotResult submitProtocolModbusFrame(ModbusHandler& handler,
    const FastBee::ModbusOutputFrame& frame,
    const FastBee::OrdinaryControlContext& context, FastBee::OrdinaryControlResult& control,
    uint8_t* raw = nullptr, size_t capacity = 0, size_t* length = nullptr) {
    if (length) *length = 0;
    if (!context.request.operation)
        return protocolModbusDenied(control, "CONTROL_CONTEXT_UNAVAILABLE");
    FastBee::ModbusControlTarget target;
    if (!frame.valid() || !handler.snapshotOrdinaryFrame(frame, target))
        return protocolModbusDenied(control, "CONTROL_FRAME_MAPPING_UNCONFIRMED");
    return PeripheralManager::getInstance().submitOrdinaryModbusFrame(
        handler, target, frame, context, control, 0, raw, capacity, length);
}

String ProtocolManager::executeModbusRawSend(const String& hexPayload) {
    FastBee::OrdinaryControlResult control;
    // A rejected reserved command remains handled; it never becomes a second
    // rule/direct-output attempt or a fabricated HEX response.
    auto failed = [&](const char* reason) -> String {
        JsonDocument doc;
        JsonObject item = doc.to<JsonArray>().add<JsonObject>();
        item["id"] = "modbus_raw_send"; item["value"] = "";
        item["remark"] = reason; item["success"] = false;
        item["outputAttempted"] = control.attempted;
        item["commandAcknowledged"] = false; item["physicalOutputVerified"] = false;
        if (doc.overflowed()) return "[]";
        String report;
        const size_t bytes = measureJson(doc);
        if (!report.reserve(bytes) || serializeJson(doc, report) != bytes || report.length() != bytes)
            return "[]";
        return report;
    };
    const uint32_t sourceEpoch = FastBee::SensorConfigEpoch::current();
    auto modbusLease = acquireModbusHandler(pdMS_TO_TICKS(2000));
    ModbusHandler* activeModbus = modbusLease.get();
    // 检查 Modbus 是否可用
    if (!activeModbus) {
        LOG_WARNING("[Modbus] RawSend: not initialized");
        return failed("MODBUS_UNAVAILABLE");
    }

    if (activeModbus->getMode() != MODBUS_MASTER) {
        LOG_WARNING("[Modbus] RawSend: not master mode");
        return failed("MODBUS_MASTER_REQUIRED");
    }

    // 校验 HEX 字符串格式
    String hex = hexPayload;
    hex.trim();
    if (hex.length() < 8U || (hex.length() % 2U) != 0U ||
        hex.length() > Protocols::MODBUS_BUFFER_SIZE * 2U) {
        LOG_WARNINGF("[Modbus] RawSend: invalid hex length=%d", hex.length());
        return failed("MODBUS_HEX_INVALID");
    }

    // HEX 字符串解码为字节数组
    const size_t byteLen = hex.length() / 2U;
    uint8_t byteArray[Protocols::MODBUS_BUFFER_SIZE];
    for (size_t i = 0; i < byteLen; i++) {
        char hi = hex.charAt(i * 2);
        char lo = hex.charAt(i * 2 + 1);
        auto hexVal = [](char c) -> int8_t {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        int8_t hv = hexVal(hi), lv = hexVal(lo);
        if (hv < 0 || lv < 0) {
            LOG_WARNING("[Modbus] RawSend: invalid hex char");
            return failed("MODBUS_HEX_INVALID");
        }
        byteArray[i] = (uint8_t)((hv << 4) | lv);
    }

    uint8_t slaveAddr = byteArray[0];

    // FastBee MQTT Modbus passthrough carries a complete RTU ADU, including
    // CRC. Never reinterpret a damaged CRC as payload and append another CRC:
    // that would mutate the platform command before it reaches the UART.
    const uint16_t crcCalc = activeModbus->calculateCRC(byteArray, byteLen - 2U);
    const uint8_t crcLo = crcCalc & 0xFFU;
    const uint8_t crcHi = (crcCalc >> 8) & 0xFFU;
    if (byteArray[byteLen - 2U] != crcLo ||
        byteArray[byteLen - 1U] != crcHi) {
        LOG_WARNINGF("[Modbus] RawSend: invalid RTU CRC (len=%u), frame rejected",
                     static_cast<unsigned>(byteLen));
        return failed("MODBUS_CRC_INVALID");
    }
    const size_t effectiveLen = byteLen - 2U;
    LOG_INFOF("[Modbus] RawSend: slave=%d, %u bytes (without CRC)", slaveAddr,
              static_cast<unsigned>(effectiveLen));

    // 使用专用原始字节通道。OneShotResult 只有 32 个槽，不能承载最多
    // 256 字节的 RTU 响应。
    uint8_t response[Protocols::MODBUS_BUFFER_SIZE];
    size_t responseLength = 0;
    uint8_t exceptionCode = 0;
    OneShotError rawError = ONESHOT_CONTROL_REJECTED;
    const uint8_t function = byteArray[1];
    if (function >= 1 && function <= 4) {
        // Read-only traffic keeps its original raw response path and needs no
        // output permission, even if PM admission is currently unavailable.
        rawError = activeModbus->sendRawFrameBytesOnce(slaveAddr, byteArray, effectiveLen,
            response, sizeof(response), responseLength, exceptionCode, false);
    } else {
        FastBee::ModbusOutputFrame frame;
        FastBee::ModbusControlTarget target;
        bool valid = frame.assign(byteArray, effectiveLen, FastBee::ModbusFrameAccess::Standard);
        bool mapped = valid && activeModbus->snapshotOrdinaryFrame(frame, target);
        // FF00 can mean either a standard ON or a saved vendor timer (255
        // units). Resolve against actual saved mappings, never address guesses.
        // Snapshot fallback performs no write; there is no direct-UART fallback.
        if (!mapped && function == 5 && effectiveLen == 6 && byteArray[4] && !byteArray[5]) {
            valid = frame.assign(byteArray, effectiveLen, FastBee::ModbusFrameAccess::RelayVendor);
            mapped = valid && activeModbus->snapshotOrdinaryFrame(frame, target);
        }
        // MQTT cannot infer a developer-maintenance authorization. Unknown
        // vendor writes, broadcast writes and malformed requests fail closed.
        if (!valid) return failed("CONTROL_FRAME_INVALID");
        if (!mapped) return failed("CONTROL_FRAME_MAPPING_UNCONFIRMED");
        static const uint64_t mqttRawActor = FastBee::RuleOutputIdentity::allocateActor();
        FastBee::OrdinaryControlContext context;
        auto& pm = PeripheralManager::getInstance();
        if (!pm.beginOrdinaryControl(FastBee::OrdinaryControl::Source::Mqtt,
            mqttRawActor, 0, false, 0, String(), sourceEpoch, {}, context))
            return failed("CONTROL_CONTEXT_UNAVAILABLE");
        struct Terminal {
            PeripheralManager& pm; const FastBee::OrdinaryControlContext& context;
            ~Terminal() { pm.finishOrdinaryControl(context); }
        } terminal{pm, context};
        // The saved target includes any hardware timer window; PM retains it
        // even after an unknown ACK or this MQTT invocation's terminal return.
        const auto result = pm.submitOrdinaryModbusFrame(*activeModbus, target, frame,
            context, control, 0, response, sizeof(response), &responseLength);
        rawError = result.error; exceptionCode = result.exceptionCode;
    }

    // A slave exception is an error to structured/legacy callers, but it is
    // also a valid RTU response frame that raw passthrough must preserve.
    const bool forwardableException =
        rawError == ONESHOT_EXCEPTION &&
        responseLength >= 5U &&
        (response[1] & 0x80U) != 0U;
    if (rawError != ONESHOT_SUCCESS && !forwardableException) {
        LOG_WARNINGF("[Modbus] RawSend: failed, error=%d exception=%u",
                     rawError, static_cast<unsigned>(exceptionCode));
        return failed(control.evaluated ? control.reason : "MODBUS_RESPONSE_UNCONFIRMED");
    }
    if (responseLength < 4 || responseLength > sizeof(response))
        return failed("MODBUS_RESPONSE_UNCONFIRMED");

    // 成功：将原始响应字节格式化为 HEX 字符串
    String respHex;
    if (!respHex.reserve(responseLength * 2U)) return failed("MODBUS_RESPONSE_ALLOCATION_FAILED");
    for (size_t i = 0; i < responseLength; i++) {
        char buf[3];
        snprintf(buf, sizeof(buf), "%02X", response[i]);
        respHex += buf;
    }

    if (respHex.length() != responseLength * 2U) return failed("MODBUS_RESPONSE_ALLOCATION_FAILED");
    LOG_INFOF("[Modbus] RawSend: result hex=%s", respHex.c_str());
    // Route the response through the typed asynchronous outlet. Returning the
    // HEX string to processQueuedCommands would lose RAW metadata and could be
    // published as text if the Modbus handler restarts before consumption.
    dispatchModbusData(slaveAddr, respHex, ModbusDataSource::PeriphExecPoll);
    return "[]";
}
#endif // FASTBEE_ENABLE_MODBUS (restartModbus + stopModbus + restartModbusDeferred + executeModbusRead + executeModbusRawSend)

// 具体协议初始化实现
#if FASTBEE_ENABLE_MQTT
bool ProtocolManager::initMQTT(void* config) {
    if (!config) return false;
    FastBee::MqttClientLifetime::Write lifecycle(mqttLifetime);
    if (!lifecycle || mqttShutdownPending || mqttStopPending) return false;

    MQTTConfig* mqttConfig = static_cast<MQTTConfig*>(config);
    if (mqttClient) {
        if (!mqttClient->stop()) return false; // Initialization also retains an in-flight worker.
        mqttClient->setReportRetirementReason(FastBee::MqttRetirementReason::Rebuild);
        mqttClient.reset();
    }

    // 使用非抛出分配：启动期或内存降级期都应返回可读失败状态，
    // 而不是因 MQTT 客户端构造失败触发异常重启。
    MQTTClient* freshClient = new (std::nothrow) MQTTClient();
    if (!freshClient) {
        LOG_WARNING("Protocol Manager: MQTT init allocation failed (nothrow)");
        return false;
    }
    mqttClient.reset(freshClient);

    // 设置回调
    mqttClient->setMessageCallback([this](const String& topic, const String& message, MqttTopicType tType) {
        handleMqttMessage(topic, message, tType);
    });

    if (!configureMqttTransport(mqttClient.get())) {
        LOG_WARNING("Protocol Manager: MQTT init aborted, no usable transport");
        mqttClient->setReportRetirementReason(FastBee::MqttRetirementReason::InitializationFailed);
        mqttClient.reset();
        return false;
    }

    // 设置状态变化回调（SSE 推送用）
    if (mqttStatusSSECallback) {
        mqttClient->setStatusChangeCallback([this](const String& data) {
            if (mqttStatusSSECallback) mqttStatusSSECallback(data);
        });
    }

    // 注册实时监测数据提供者（组合 Modbus 缓存数据 + 本地外设数据）
    mqttClient->setMonitorDataProvider([this]() -> String {
#if FASTBEE_ENABLE_MODBUS
        auto lease = acquireModbusHandler(pdMS_TO_TICKS(100));
        String modbusData = lease ? lease->collectCachedPollData() : "[]";
#else
        String modbusData = "[]";
#endif
        String localData = collectLocalSensorData();
        bool hasMb = modbusData.length() > 2;
        bool hasLc = localData.length() > 2;
        if (!hasMb && !hasLc) return "[]";
        if (!hasMb) return localData;
        if (!hasLc) return modbusData;
        // 预分配足够容量，避免多次重新分配
        String result;
        result.reserve(modbusData.length() + localData.length());
        result.concat('[');
        // 直接引用内部 c_str 指针+偏移，避免 substring() 创建临时 String
        result.concat(modbusData.c_str() + 1, modbusData.length() - 2);
        result.concat(',');
        result.concat(localData.c_str() + 1, localData.length() - 2);
        result.concat(']');
        return result;
    });

    mqttClient->setMinReportInterval(mqttMinReportInterval.load());
    if (!mqttClient->begin()) {
        mqttClient->setReportRetirementReason(FastBee::MqttRetirementReason::InitializationFailed);
        mqttClient.reset();
        return false;
    }
    return true;
}
#endif

#if FASTBEE_ENABLE_MODBUS
bool ProtocolManager::initModbus(void* config) {
    if (!config) return false;

    ModbusLifecycleGuard lifecycle(modbusLifecycleMutex, portMAX_DELAY);
    if (!lifecycle.isLocked()) return false;

    // 运行时功能门控：用户可在 Web UI 禁用 modbus
    if (!FastBee::RuntimeFeatureManager::instance().shouldEnable("modbus")) {
        LOG_WARNING("Protocol Manager: Modbus disabled by runtime feature config, skipping");
        return false;
    }

    ModbusConfig* modbusConfig = static_cast<ModbusConfig*>(config);
    ModbusHandler* freshModbus = new (std::nothrow) ModbusHandler();
    if (!freshModbus) {
        LOG_WARNING("Protocol Manager: Modbus init allocation failed (nothrow)");
        return false;
    }
    modbusHandler.reset(freshModbus);

    // 设置回调
    modbusHandler->setDataCallback([this](uint8_t address, const String& data) {
        // 统一通过 dispatchModbusData 分发（LiveCallback 路径：MQTT + SSE + 规则匹配）
        dispatchModbusData(address, data, ModbusDataSource::LiveCallback);
    });

    // 设置状态变化回调（SSE 推送用）
    if (modbusStatusSSECallback) {
        modbusHandler->setStatusChangeCallback([this](const String& data) {
            if (modbusStatusSSECallback) modbusStatusSSECallback(data);
        });
    }

    bool initOk = modbusHandler->begin(*modbusConfig);
    if (initOk) {
        // 注册 Modbus 子设备为标准外设并设置通信回调
        registerModbusSubDevices(*modbusConfig);
    }
    return initOk;
}
#endif

// ========== Modbus 子设备注册到 PeripheralManager ==========

#if FASTBEE_ENABLE_MODBUS
void ProtocolManager::registerModbusSubDevices(const ModbusConfig& config) {
    PeripheralManager& pm = PeripheralManager::getInstance();

    // 先清理旧的 Modbus 外设（重启 Modbus 时需要先注销）
    unregisterModbusSubDevices();

    if (config.mode != MODBUS_MASTER || config.master.deviceCount == 0) {
        return;
    }

    // 透传模式下不注册子设备（纯转发，不需要控制类子设备）
    if (config.transferType == 1) {
        LOG_INFO("Protocol Manager: Transparent mode, skip sub-device registration");
        return;
    }

    // Resolve the active handler for every operation. Capturing the current
    // raw pointer would leave PeripheralManager holding a dangling callback
    // after a protocol hot restart.
    if (modbusHandler) {
        pm.setModbusCallbacks(
            // 线圈写入回调 (FC05) — 使用控制优先级通道，避免与轮询竞争
            [this](uint8_t slaveAddr, uint16_t coilAddr, bool value,
                   const FastBee::OrdinaryControlContext& context,
                   FastBee::OrdinaryControlResult& control) -> bool {
                auto lease = acquireModbusHandler(pdMS_TO_TICKS(2000));
                if (!lease) {
                    protocolModbusDenied(control, "MODBUS_UNAVAILABLE"); return false;
                }
                FastBee::ModbusOutputFrame frame;
                if (!frame.single(slaveAddr, 5, coilAddr, value ? 0xff00 : 0)) {
                    protocolModbusDenied(control, "CONTROL_FRAME_INVALID"); return false;
                }
                return submitProtocolModbusFrame(*lease.get(), frame, context, control).error == ONESHOT_SUCCESS;
            },
            // 寄存器写入回调 (FC06) — 使用控制优先级通道
            [this](uint8_t slaveAddr, uint16_t regAddr, uint16_t value,
                   const FastBee::OrdinaryControlContext& context,
                   FastBee::OrdinaryControlResult& control) -> bool {
                auto lease = acquireModbusHandler(pdMS_TO_TICKS(2000));
                if (!lease) {
                    protocolModbusDenied(control, "MODBUS_UNAVAILABLE"); return false;
                }
                FastBee::ModbusOutputFrame frame;
                if (!frame.single(slaveAddr, 6, regAddr, value)) {
                    protocolModbusDenied(control, "CONTROL_FRAME_INVALID"); return false;
                }
                return submitProtocolModbusFrame(*lease.get(), frame, context, control).error == ONESHOT_SUCCESS;
            }
        );
    }

    // 逐个注册子设备为标准外设
    for (uint8_t i = 0; i < config.master.deviceCount; i++) {
        const ModbusSubDevice& dev = config.master.devices[i];
        if (!dev.enabled) continue;

        PeripheralConfig pCfg;
        pCfg.id = "modbus:" + String(i);
        pCfg.name = String(dev.name);
        pCfg.type = PeripheralType::MODBUS_DEVICE;
        pCfg.enabled = true;
        pCfg.pinCount = 0;  // Modbus 外设不使用 GPIO 引脚

        // 设置 Modbus 特定参数
        pCfg.params.modbus.slaveAddress    = dev.slaveAddress;
        pCfg.params.modbus.channelCount    = dev.channelCount;
        pCfg.params.modbus.coilBase        = dev.coilBase;
        pCfg.params.modbus.ncMode          = dev.ncMode;
        pCfg.params.modbus.controlProtocol = dev.controlProtocol;
        pCfg.params.modbus.deviceIndex     = i;
        pCfg.params.modbus.batchRegister   = dev.batchRegister;
        pCfg.params.modbus.pwmRegBase      = dev.pwmRegBase;
        pCfg.params.modbus.pwmResolution   = dev.pwmResolution;
        strncpy(pCfg.params.modbus.sensorId, dev.sensorId, sizeof(pCfg.params.modbus.sensorId) - 1);
        pCfg.params.modbus.sensorId[sizeof(pCfg.params.modbus.sensorId) - 1] = '\0';

        // 确定设备类型
        String devType(dev.deviceType);
        if (devType == "relay")          pCfg.params.modbus.deviceType = 0;
        else if (devType == "pwm")      pCfg.params.modbus.deviceType = 1;
        else if (devType == "pid")      pCfg.params.modbus.deviceType = 2;
        else if (devType == "motor")    pCfg.params.modbus.deviceType = 3;
        else                            pCfg.params.modbus.deviceType = 0;

        // 电机参数
        memcpy(pCfg.params.modbus.motorRegs, dev.motorRegs, sizeof(dev.motorRegs));
        pCfg.params.modbus.motorDecimals = dev.motorDecimals;
        pCfg.params.modbus.motorMinPosition = dev.motorMinPosition;
        pCfg.params.modbus.motorMaxPosition = dev.motorMaxPosition;
        pCfg.params.modbus.motorCurrentPosition = dev.motorCurrentPosition;
        pCfg.params.modbus.motorMoveStep = dev.motorMoveStep;
        pCfg.params.modbus.motorLastPulse = dev.motorLastPulse;

        if (pm.addPeripheral(pCfg)) {
            LOG_INFOF("Protocol Manager: Registered Modbus sub-device '%s' as peripheral 'modbus:%d'",
                      dev.name, i);
        } else {
            LOG_WARNINGF("Protocol Manager: Failed to register Modbus sub-device '%s'", dev.name);
        }
    }
}

void ProtocolManager::unregisterModbusSubDevices() {
    PeripheralManager& pm = PeripheralManager::getInstance();

    // 清除通信回调
    pm.clearModbusCallbacks();

    // 移除所有 MODBUS_DEVICE 类型的外设
    auto allPeriphs = pm.getPeripheralsByType(PeripheralType::MODBUS_DEVICE);
    for (const auto& p : allPeriphs) {
        pm.removePeripheral(p.id);
        LOG_INFOF("Protocol Manager: Unregistered Modbus peripheral '%s'", p.id.c_str());
    }
}
#endif // FASTBEE_ENABLE_MODBUS (registerModbusSubDevices + unregisterModbusSubDevices)

String ProtocolManager::collectLocalSensorData() const {
    PeripheralManager& pm = PeripheralManager::getInstance();
    std::vector<PeripheralConfig> allPeriphs = pm.getAllPeripherals();

    String json;
    json.reserve(256);
    json = "[";
    bool first = true;

    for (const auto& config : allPeriphs) {
        if (!config.enabled) continue;

        // 实时监测仅上报输入类传感器数据，排除输出类控制外设
        // （如 GPIO_DIGITAL_OUTPUT=12 / GPIO_ANALOG_OUTPUT=16 / GPIO_PWM_OUTPUT=17 / DAC=27）
        if (!isInputType(config.type)) continue;

        // DI 的上报策略由配置控制：事件型按键默认由按键线程上报，
        // 选择“周期上报”后才纳入这里的定时采样；EVENT_ONLY 始终跳过。
        if (isGpioDigitalInputType(config.type)) {
            const uint8_t reportMode = config.params.gpio.reportMode;
            if (!shouldReportDigitalInputPeriodically(reportMode)) {
                continue;
            }
        }

        int typeVal = static_cast<int>(config.type);
        String value;
        AnalogInputQuality analogQuality = AnalogInputQuality::VALID;
        bool reportAnalogQuality = false;
        if (isGpioDigitalInputType(config.type)) {
            // PERIODIC 数字输入；ON_CHANGE 由 DI 去抖线程在稳定变化时上报。
            GPIOState state = pm.readPin(config.id);
            if (state == GPIOState::STATE_UNDEFINED) continue;
            value = (state == GPIOState::STATE_HIGH) ? "1" : "0";
        } else if (typeVal == 15 || typeVal == 26) {
            // 模拟输入/ADC：启用工程量转换时统一上报转换值，否则保持
            // 历史行为上报原始 ADC 计数。
            reportAnalogQuality = config.params.adc.conversionEnabled &&
                config.params.adc.signalMode != static_cast<uint8_t>(AnalogSignalMode::RAW);
            value = formatAnalogReportValue(config, pm, &analogQuality);
        } else {
            continue;
        }

        if (!value.isEmpty()) {
            if (!first) json += ",";
            json += "{\"id\":\"";
            json += config.id;
            json += "\",\"value\":\"";
            json += value;
            json += "\",\"remark\":\"local\"}";
            first = false;
        }

        // A separate property preserves the existing numeric property ID and
        // payload while making invalid, clamped and recovered states visible.
        if (reportAnalogQuality) {
            if (!first) json += ",";
            json += "{\"id\":\"";
            json += config.id;
            json += "_quality\",\"value\":\"";
            json += analogInputQualityName(analogQuality);
            json += "\",\"remark\":\"quality\"}";
            first = false;
        }
    }

#if FASTBEE_ENABLE_PERIPH_EXEC
    // 合并传感器读取缓存（DHT11/DHT22、DS18B20、SHT31/AHT20/BH1750/BMP280/MPU6050 等）：
    // ACTION_SENSOR_READ 采集规则每次执行都会写入该缓存（已完成 scale/offset 与小数位格式化），
    // 实时监测直接复用缓存值，避免在 loopTask 中同步读传感器（DHT 单次读取阻塞 250ms+）。
    // id 通用规则与 DATA_REPORT 上报路径一致：显式 dataField 优先（多字段外设），
    // 否则回退外设配置 id（与平台物模型标识符对应），最后才用完整缓存键兜底。
    {
        const auto sensorCache = PeriphExecManager::getInstance().getSensorReadCacheCopy();
        const int64_t nowUs = esp_timer_get_time();
        if (nowUs < 0) return "[]";
        const uint64_t now = static_cast<uint64_t>(nowUs) / 1000;
        for (const auto& entry : sensorCache) {
            const auto& cache = entry.second;
            if (cache.value.isEmpty()) continue;
            // 防御性 TTL 检查：周期性淘汰之外再拦截一次过期数据
            const uint32_t maxAge = cache.maxAgeMs ? cache.maxAgeMs : PeriphExecManager::SENSOR_CACHE_TTL_MS;
            if (now < cache.sampledAtMs || now - cache.sampledAtMs > maxAge) continue;
            // 旧缓存条目无 periphId 时回退用完整 key（periphId_dataField）
            String reportId = cache.dataField;
            if (reportId.isEmpty()) {
                reportId = cache.periphId.isEmpty() ? entry.first : cache.periphId;
            }
            ActionExecResult sample;
            if (!FastBee::captureLiteralReportIdentity(sample, reportId.c_str())) return "[]";
            FastBee::completeCapturedReport(sample, cache.value, true, String("sensor"));
            sample.hasSensorSample = true; sample.sampleUnit = cache.unit;
            if (sample.sampleUnit != cache.unit || cache.unit.length() > 32) return "[]";
            sample.sampleUnitBytes = static_cast<uint8_t>(cache.unit.length());
            sample.sensorSample = {cache.sampledAtMs, now, maxAge, FastBee::SensorSampleQuality::Good};
            JsonDocument row;
            JsonArray rows = row.to<JsonArray>();
            if (FastBee::appendCapturedReportRow(rows, sample) != FastBee::CapturedReportRow::Added || row.overflowed()) return "[]";
            String encoded;
            const size_t bytes = measureJson(rows[0]);
            if (!encoded.reserve(bytes) || serializeJson(rows[0], encoded) != bytes || encoded.length() != bytes ||
                (!first && !FastBee::appendCheckedText(json, ",", 1, UINT16_MAX)) ||
                !FastBee::appendCheckedText(json, encoded.c_str(), encoded.length(), UINT16_MAX)) return "[]";
            first = false;
        }
    }
#endif

    json += "]";
    return json;
}
