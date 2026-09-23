/**
 * @description: MQTT 客户端实现
 * @author: kerwincui
 * @copyright: FastBee All rights reserved.
 * @date: 2025-12-02 17:31:14
 *
 * 修复/优化说明：
 *  1. 修复 loadMqttConfig() 中 !!LittleFS.exists() 双重取反 Bug
 *  2. mqttCallback() 改为一次性 String(payload, length)，消除逐字符 += 循环
 *  3. 所有 Serial.println 替换为 LOG 宏统一输出
 *  4. getStatus() 返回 const char*，避免 String 临时对象
 */

#include "protocols/MQTTClient.h"
#include "protocols/ProtocolManager.h"
#include "protocols/ModbusHandler.h"
#include "systems/LoggerSystem.h"
#include "systems/ConfigStorage.h"
#include "core/FeatureFlags.h"
#include "core/MemoryBudget.h"
#include "core/FastBeeFramework.h"
#include "network/WebConfigManager.h"
#if FASTBEE_ENABLE_HEALTH_MONITOR
#include "systems/HealthMonitor.h"
#endif
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <HTTPClient.h>
#if FASTBEE_ENABLE_REMOTE_RESOURCES
#include "protocols/RemoteResourceManager.h"
#include "protocols/RemoteResourceModel.h"
#endif
#include <esp_heap_caps.h>
#include <mbedtls/aes.h>
#include <mbedtls/platform.h>
#include "network/NetworkManager.h"
#include "network/handlers/SSERouteHandler.h"
#if FASTBEE_ENABLE_PERIPH_EXEC
#include <core/PeriphExecManager.h>
#endif
#if FASTBEE_ENABLE_RULE_SCRIPT
#include <core/RuleScriptManager.h>
#endif
#include <mbedtls/base64.h>
#include <new>
#include "core/StringBufferPool.h"
#include "core/RuntimeFeatureManager.h"

#if FASTBEE_ENABLE_MQTT

namespace {

extern const uint8_t x509_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t x509_crt_bundle_end[] asm("_binary_x509_crt_bundle_end");

constexpr size_t MQTTS_MBEDTLS_PSRAM_MIN_ALLOC = 256U;
constexpr size_t MQTTS_MBEDTLS_PSRAM_RESERVE = 64U * 1024U;
constexpr const char* FASTBEE_OTA_REPLY_TOPIC = "/{deviceNum}/http/upgrade/reply";
constexpr const char* FASTBEE_OTA_COMMAND_TOPIC = "/{deviceNum}/http/upgrade/set";
constexpr const char* FASTBEE_OTA_REPLY_LEGACY = "/http/upgrade/reply";
constexpr const char* FASTBEE_OTA_COMMAND_LEGACY = "/http/upgrade/set";

void ensureFastBeeOtaPublishTopic(std::vector<MqttPublishTopic>& topics) {
    bool hasCanonical = false;
    for (auto& topic : topics) {
        if (topic.topicType != MqttTopicType::OTA_UPGRADE) continue;
        String configured = topic.topic;
        configured.trim();
        if (configured != FASTBEE_OTA_REPLY_TOPIC) continue;
        topic.topic = FASTBEE_OTA_REPLY_TOPIC;
        topic.autoPrefix = false;
        hasCanonical = true;
    }
    if (!hasCanonical) {
        for (auto& topic : topics) {
            if (topic.topicType != MqttTopicType::OTA_UPGRADE || !topic.autoPrefix) continue;
            String configured = topic.topic;
            configured.trim();
            if (configured != FASTBEE_OTA_REPLY_LEGACY) continue;
            topic.topic = FASTBEE_OTA_REPLY_TOPIC;
            topic.autoPrefix = false;
            hasCanonical = true;
            break;
        }
    }
    if (hasCanonical) return;

    MqttPublishTopic topic;
    topic.topic = FASTBEE_OTA_REPLY_TOPIC;
    topic.autoPrefix = false;
    topic.topicType = MqttTopicType::OTA_UPGRADE;
    topics.push_back(topic);
    LOG_INFO("MQTT: Added missing FastBee HTTPS OTA reply topic");
}

void ensureFastBeeOtaSubscribeTopic(std::vector<MqttSubscribeTopic>& topics) {
    bool hasCanonical = false;
    for (auto& topic : topics) {
        if (topic.topicType != MqttTopicType::OTA_UPGRADE) continue;
        String configured = topic.topic;
        configured.trim();
        if (configured != FASTBEE_OTA_COMMAND_TOPIC) continue;
        topic.topic = FASTBEE_OTA_COMMAND_TOPIC;
        topic.autoPrefix = false;
        hasCanonical = true;
    }
    if (!hasCanonical) {
        for (auto& topic : topics) {
            if (topic.topicType != MqttTopicType::OTA_UPGRADE || !topic.autoPrefix) continue;
            String configured = topic.topic;
            configured.trim();
            if (configured != FASTBEE_OTA_COMMAND_LEGACY) continue;
            topic.topic = FASTBEE_OTA_COMMAND_TOPIC;
            topic.autoPrefix = false;
            hasCanonical = true;
            break;
        }
    }
    if (hasCanonical) return;

    MqttSubscribeTopic topic;
    topic.topic = FASTBEE_OTA_COMMAND_TOPIC;
    topic.autoPrefix = false;
    topic.topicType = MqttTopicType::OTA_UPGRADE;
    topics.push_back(topic);
    LOG_INFO("MQTT: Added missing FastBee HTTPS OTA command topic");
}

// MQTT 发布路径共享缓冲池：避免 processQueuedReports 中频繁 String(payload, length) 分配
// 512 字节 × 4 槽 = 2KB DRAM 预分配，与 MqttSlot.payload 尺寸对齐
FastBee::StringBufferPool<512, 4> g_mqttPublishPool;

bool g_mqttsMbedtlsAllocatorInstalled = false;

// Keep the SSE payload self-describing.  The REST status route has the same
// mapping, but MQTTClient is also the producer for asynchronous status events;
// callers must not have to scrape serial output or wait for the next REST poll
// to learn why a connection failed.
const char* mqttStatusErrorName(int code) {
    switch (code) {
        case 0:  return "OK";
        case -1: return "MQTT_DISCONNECTED";
        case -2: return "MQTT_CONNECT_FAILED";
        case -3: return "MQTT_CONNECTION_LOST";
        case -4: return "MQTT_CONNECTION_TIMEOUT";
        case -5: return "MQTTS_FEATURE_DISABLED";
        case -11: return "MQTT_CLIENT_INIT_FAILED";
        case -13: return "MQTT_DISABLED_BY_MEMORY";
        case -14: return "MQTT_STOPPED_BY_MEMORY";
        case 1:  return "MQTT_BAD_PROTOCOL";
        case 2:  return "MQTT_BAD_CLIENT_ID";
        case 3:  return "MQTT_UNAVAILABLE";
        case 4:  return "MQTT_BAD_CREDENTIALS";
        case 5:  return "MQTT_UNAUTHORIZED";
        default: return "MQTT_UNKNOWN";
    }
}

const char* mqttStatusErrorClass(int code, bool tlsMemoryFailure) {
    if (code == 0) return "none";
    if (code == -5) return "feature";
    if (code == -11 || code == -13 || code == -14) return "resource";
    if (tlsMemoryFailure) return "memory";
    if (code >= 1 && code <= 3) return "broker";
    if (code == 4 || code == 5) return "auth";
    if (code >= -4 && code <= -1) return "network";
    return "unknown";
}

const char* mqttStatusErrorMessage(int code, bool networkConnected,
                                   bool tlsMemoryFailure) {
    switch (code) {
        case 0:  return "";
        case -1: return "MQTT 客户端未连接";
        case -2: return networkConnected
                         ? "无法连接 MQTT Broker（DNS/TCP）"
                         : "网络未连接，无法访问 MQTT Broker";
        case -3: return "MQTT 连接已断开，正在重连";
        case -4: return tlsMemoryFailure
                         ? "TLS 内存不足，已进入慢速重试；建议切换 mqtt"
                         : "MQTT 连接超时，请检查 Broker 地址和端口";
        case -5: return "MQTTS 功能未启用，请在功能配置中启用";
        case -11: return "MQTT 客户端启动失败，可能是内存不足或配置无效";
        case -13: return "设备内存紧张，MQTT 已自动禁用，请恢复内存后重新启用";
        case -14: return "设备内存紧张，MQTT 已暂时停止，内存恢复后将自动重连";
        case 1:  return "Broker 拒绝：协议版本不支持";
        case 2:  return "Broker 拒绝：客户端 ID 无效或重复";
        case 3:  return "Broker 当前不可用";
        case 4:  return "Broker 拒绝：用户名或密码错误";
        case 5:  return "Broker 拒绝：客户端未授权";
        default: return "MQTT 连接失败，请查看错误码";
    }
}

NetworkType getActiveMqttNetworkType() {
    FastBeeFramework* fw = FastBeeFramework::getInstance();
    FBNetworkManager* netMgr = fw ? static_cast<FBNetworkManager*>(fw->getNetworkManager()) : nullptr;
    return netMgr ? netMgr->getNetworkType() : NetworkType::NET_WIFI;
}

// TLS 大缓冲是否可路由到 PSRAM：无 PSRAM 时 mbedTLS 全部落内部 DRAM，
// 握手准入必须使用 MemoryBudget 的无 PSRAM 严格门槛
bool mqttsTlsUsesPsram() {
#if defined(BOARD_HAS_PSRAM) && defined(MALLOC_CAP_SPIRAM)
    return psramFound();
#else
    return false;
#endif
}

// 深度暂停决策：前台 Web 请求活跃时不应“跳过暂停继续握手”（历史 panic
// 现场条件），而应推迟握手；连续推迟超限后强制深度暂停，防止前台窗口
// 被前端轮询无限续期导致 MQTTS 永久无法上线
enum class MqttsWebPauseDecision : uint8_t { NO_PAUSE, PAUSE, FORCE_PAUSE, DEFER };
constexpr uint8_t MQTTS_FOREGROUND_DEFER_MAX = 6;
uint8_t g_mqttsForegroundDeferCount = 0;

MqttsWebPauseDecision decideWebPauseForMqtts(uint32_t dramFree, uint32_t largestBlock) {
    if (!FastBee::MemoryBudget::shouldPauseWebBeforeMqtts(dramFree, largestBlock)) {
        g_mqttsForegroundDeferCount = 0;
        return MqttsWebPauseDecision::NO_PAUSE;
    }

    FastBeeFramework* fw = FastBeeFramework::getInstance();
    WebConfigManager* wcm = fw ? fw->getWebConfigManager() : nullptr;
    if (wcm && wcm->isForegroundRequestActive()) {
        if (g_mqttsForegroundDeferCount < MQTTS_FOREGROUND_DEFER_MAX) {
            ++g_mqttsForegroundDeferCount;
            ets_printf("[MQTT] MQTTS deep Web pause deferred: foreground Web request active (%u/%u)\n",
                       (unsigned)g_mqttsForegroundDeferCount,
                       (unsigned)MQTTS_FOREGROUND_DEFER_MAX);
            return MqttsWebPauseDecision::DEFER;
        }
        ets_printf("[MQTT] MQTTS deep Web pause FORCED after %u foreground defers\n",
                   (unsigned)MQTTS_FOREGROUND_DEFER_MAX);
        g_mqttsForegroundDeferCount = 0;
        return MqttsWebPauseDecision::FORCE_PAUSE;
    }
    g_mqttsForegroundDeferCount = 0;
    return MqttsWebPauseDecision::PAUSE;
}

void* fastbeeMbedtlsCalloc(size_t n, size_t size) {
    if (size != 0 && n > (static_cast<size_t>(-1) / size)) {
        return nullptr;
    }

    const size_t bytes = n * size;
    void* ptr = nullptr;

#if defined(BOARD_HAS_PSRAM) && defined(MALLOC_CAP_SPIRAM)
    if (bytes >= MQTTS_MBEDTLS_PSRAM_MIN_ALLOC) {
        const size_t psramFree = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        if (psramFree > bytes + MQTTS_MBEDTLS_PSRAM_RESERVE) {
            ptr = heap_caps_calloc_prefer(
                n,
                size,
                2,
                MALLOC_CAP_DEFAULT | MALLOC_CAP_SPIRAM,
                MALLOC_CAP_DEFAULT | MALLOC_CAP_INTERNAL);
        }
    }
#endif

    if (!ptr) {
#if defined(BOARD_HAS_PSRAM) && defined(MALLOC_CAP_SPIRAM)
        ptr = heap_caps_calloc_prefer(
            n,
            size,
            2,
            MALLOC_CAP_DEFAULT | MALLOC_CAP_INTERNAL,
            MALLOC_CAP_DEFAULT | MALLOC_CAP_SPIRAM);
#else
        ptr = heap_caps_calloc(n, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#endif
    }

    if (!ptr) {
        ptr = heap_caps_calloc(n, size, MALLOC_CAP_8BIT);
    }
    return ptr;
}

void fastbeeMbedtlsFree(void* ptr) {
    heap_caps_free(ptr);
}

void ensureMbedtlsAllocatorForMqtts() {
#if defined(MBEDTLS_PLATFORM_MEMORY)
    if (g_mqttsMbedtlsAllocatorInstalled) {
        return;
    }
    int rc = mbedtls_platform_set_calloc_free(fastbeeMbedtlsCalloc, fastbeeMbedtlsFree);
    if (rc == 0) {
        g_mqttsMbedtlsAllocatorInstalled = true;
        ets_printf("[MQTT] mbedTLS allocator installed (psram_min=%u reserve=%u)\n",
                   (unsigned)MQTTS_MBEDTLS_PSRAM_MIN_ALLOC,
                   (unsigned)MQTTS_MBEDTLS_PSRAM_RESERVE);
    } else {
        LOG_WARNINGF("[MQTT] mbedTLS allocator install failed rc=%d", rc);
    }
#endif
}

}  // namespace

static double jsonVariantToDoubleSafe(JsonVariantConst v) {
    if (v.isNull()) return 0.0;
    if (v.is<double>()) return v.as<double>();
    if (v.is<float>()) return static_cast<double>(v.as<float>());
    if (v.is<long long>()) return static_cast<double>(v.as<long long>());
    if (v.is<unsigned long long>()) return static_cast<double>(v.as<unsigned long long>());
    if (v.is<long>()) return static_cast<double>(v.as<long>());
    if (v.is<unsigned long>()) return static_cast<double>(v.as<unsigned long>());
    if (v.is<int>()) return static_cast<double>(v.as<int>());
    if (v.is<unsigned int>()) return static_cast<double>(v.as<unsigned int>());
    if (v.is<const char*>()) {
        const char* s = v.as<const char*>();
        return (s && s[0] != '\0') ? String(s).toDouble() : 0.0;
    }
    if (v.is<String>()) return v.as<String>().toDouble();
    return 0.0;
}

static bool mqttPayloadIsStructuredJson(const uint8_t* payload, size_t length) {
    if (!payload || length == 0) return false;

    size_t first = 0;
    while (first < length) {
        char c = static_cast<char>(payload[first]);
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
        ++first;
    }
    if (first >= length || (payload[first] != '[' && payload[first] != '{')) {
        return false;
    }

    JsonDocument probe;
    DeserializationError error = deserializeJson(probe, payload, length);
    return !error && (probe.is<JsonArray>() || probe.is<JsonObject>());
}

static uint16_t mqttModbusCrc16(const uint8_t* data, size_t length) {
    uint16_t crc = 0xFFFF;
    for (size_t pos = 0; pos < length; ++pos) {
        crc ^= static_cast<uint16_t>(data[pos]);
        for (uint8_t bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x0001U) != 0U
                ? static_cast<uint16_t>((crc >> 1U) ^ 0xA001U)
                : static_cast<uint16_t>(crc >> 1U);
        }
    }
    return crc;
}

static bool mqttModbusFrameHasValidCrc(const uint8_t* frame, size_t length) {
    if (!frame || length < 4U || length > Protocols::MODBUS_BUFFER_SIZE) {
        return false;
    }
    const uint16_t expectedCrc = mqttModbusCrc16(frame, length - 2U);
    return frame[length - 2U] == static_cast<uint8_t>(expectedCrc & 0xFFU) &&
           frame[length - 1U] == static_cast<uint8_t>((expectedCrc >> 8U) & 0xFFU);
}

static bool decodeModbusRtuHex(const char* payload, size_t hexLength,
                               uint8_t* frame, size_t frameCapacity,
                               size_t& frameLength) {
    frameLength = 0;
    if (!payload || !frame || hexLength < 8U || (hexLength % 2U) != 0U) {
        return false;
    }

    const size_t byteLength = hexLength / 2U;
    if (byteLength > frameCapacity || byteLength > Protocols::MODBUS_BUFFER_SIZE) {
        return false;
    }

    auto hexValue = [](char c) -> int8_t {
        if (c >= '0' && c <= '9') return static_cast<int8_t>(c - '0');
        if (c >= 'a' && c <= 'f') return static_cast<int8_t>(c - 'a' + 10);
        if (c >= 'A' && c <= 'F') return static_cast<int8_t>(c - 'A' + 10);
        return -1;
    };
    for (size_t i = 0; i < byteLength; ++i) {
        const int8_t high = hexValue(payload[i * 2U]);
        const int8_t low = hexValue(payload[i * 2U + 1U]);
        if (high < 0 || low < 0) return false;
        frame[i] = static_cast<uint8_t>((high << 4U) | low);
    }

    if (!mqttModbusFrameHasValidCrc(frame, byteLength)) {
        return false;
    }
    frameLength = byteLength;
    return true;
}

enum class MqttModbusPayloadEncoding : uint8_t {
    INVALID,
    HEX_ASCII,
    BINARY
};

static MqttModbusPayloadEncoding decodeModbusRtuPayload(
        const uint8_t* payload, size_t length,
        uint8_t* frame, size_t frameCapacity, size_t& frameLength) {
    frameLength = 0;
    if (!payload || !frame || length == 0U) {
        return MqttModbusPayloadEncoding::INVALID;
    }

    // Evaluate both wire representations by CRC. A binary RTU frame may
    // consist entirely of ASCII HEX characters, so character shape alone is
    // not enough. A valid binary frame takes precedence; otherwise accept a
    // CRC-valid decoded HEX-ASCII frame.
    const bool hexAsciiValid = decodeModbusRtuHex(
        reinterpret_cast<const char*>(payload), length,
        frame, frameCapacity, frameLength);
    if (mqttModbusFrameHasValidCrc(payload, length)) {
        if (length > frameCapacity) {
            frameLength = 0;
            return MqttModbusPayloadEncoding::INVALID;
        }
        memcpy(frame, payload, length);
        frameLength = length;
        return MqttModbusPayloadEncoding::BINARY;
    }
    if (hexAsciiValid) {
        return MqttModbusPayloadEncoding::HEX_ASCII;
    }

    frameLength = 0;
    return MqttModbusPayloadEncoding::INVALID;
}

static uint8_t currentModbusTransferType() {
#if FASTBEE_ENABLE_MODBUS
    FastBeeFramework* fw = FastBeeFramework::getInstance();
    ProtocolManager* pm = fw ? fw->getProtocolManager() : nullptr;
    auto lease = pm ? pm->acquireModbusHandler(pdMS_TO_TICKS(100))
                    : ProtocolManager::ModbusHandlerLease();
    ModbusHandler* handler = lease.get();
    return handler ? handler->getTransferType() : 0;
#else
    return 0;
#endif
}

static bool mqttHasInternalDram(uint32_t minFree, uint32_t minLargest, const char* operation) {
    uint32_t dramFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    uint32_t dramLargest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    if (dramFree >= minFree && dramLargest >= minLargest) {
        return true;
    }
    LOG_WARNINGF("MQTT: DRAM low, skip %s (dram=%lu largest=%lu need=%lu/%lu)",
                 operation,
                 (unsigned long)dramFree,
                 (unsigned long)dramLargest,
                 (unsigned long)minFree,
                 (unsigned long)minLargest);
    return false;
}

MQTTClient::MQTTClient()
    : isConnected(false), stopped(false), lastReconnectAttempt(0),
      lastConnectedTime(0), lastErrorCode(0), reconnectCount(0),
      reconnectInterval(5000), consecutiveTimeouts(0), lastLoopTime(0),
      _reconnectPending(false), _reconnectRunning(false), _reconnectTaskHandle(nullptr),
      _taskStartupDelayMs(3000),
      _slotWriteIndex(0), _slotReadIndex(0), _slotCount(0) {
    _publishMutex = xSemaphoreCreateRecursiveMutex();
    _dataCommandQueue = xQueueCreate(4, sizeof(String*));
}

MQTTClient::~MQTTClient() {
    // 先关闭后台任务，避免 disconnect 后任务仍在尝试重连
    shutdown();
    disconnect();
    if (_publishMutex) {
        vSemaphoreDelete(_publishMutex);
        _publishMutex = nullptr;
    }
    // 排空并删除 DATA_COMMAND 队列
    if (_dataCommandQueue) {
        String* cmd = nullptr;
        while (xQueueReceive(_dataCommandQueue, &cmd, 0) == pdTRUE) { delete cmd; }
        vQueueDelete(_dataCommandQueue);
        _dataCommandQueue = nullptr;
        _cmdQueueTotalBytes = 0;
    }
    // 清空上报环形缓冲区
    for (uint8_t i = 0; i < MQTT_REPORT_SLOTS; i++) {
        _reportSlots[i].clear();
    }
    _pendingReportSlot.clear();
    _slotWriteIndex = 0;
    _slotReadIndex = 0;
    _slotCount = 0;
}

bool MQTTClient::loadMqttConfig(const String& filename) {
    // 修复：原代码 !!LittleFS.exists() 是双重取反，逻辑错误（文件存在时反而返回 false）
    if (!LittleFS.exists(filename)) {
        char buf[64];
        snprintf(buf, sizeof(buf), "MQTT: Config file not found: %s", filename.c_str());
        LOG_WARNING(buf);
        return false;
    }

    JsonDocument doc;
    // 优化：优先走分段加载 (Filter) 仅反序列化 "mqtt" 子节点，堆峰值 ~15KB → ~3KB
    // 退化路径：当 filename 不是标准 protocol.json 或 section 不存在时，回滚到全量反序列化（兼容旧配置）
    bool sectionLoaded = false;
    if (filename == String(FileSystem::PROTOCOL_CONFIG_FILE)) {
        sectionLoaded = ConfigStorage::getInstance().loadProtocolSection("mqtt", doc);
    }
    if (!sectionLoaded) {
        File file = LittleFS.open(filename, "r");
        if (!file) {
            char buf[64];
            snprintf(buf, sizeof(buf), "MQTT: Failed to open config: %s", filename.c_str());
            LOG_ERROR(buf);
            return false;
        }

        DeserializationError err = deserializeJson(doc, file);
        file.close();

        if (err) {
            char buf[80];
            snprintf(buf, sizeof(buf), "MQTT: Config parse error: %s", err.c_str());
            LOG_ERROR(buf);
            return false;
        }
    }

    // protocol.json 结构为 { "mqtt": { ... }, "modbusRtu": { ... }, ... }
    // 优先从 doc["mqtt"] 子对象读取；若不存在则兼容旧配置从根级别读取
    JsonVariant mqttObj = doc["mqtt"];
    bool nested = !mqttObj.isNull() && mqttObj.is<JsonObject>();
    JsonVariant cfg = nested ? mqttObj : doc.as<JsonVariant>();

    config.scheme      = cfg["scheme"]      | "mqtt";
    config.server      = cfg["server"]      | "";
    config.port        = cfg["port"]        | 1883;
    // Cache the persisted memory-protection marker without another filesystem
    // read, keeping status callbacks allocation-light on constrained targets.
    _disabledByMemory = (cfg["disabled_by_memory"] | false) ||
                        (cfg["disable_mqtt_by_memory"] | false);

    // 向后兼容：如果配置中没有 scheme 字段但端口为 8883，自动推断为 mqtts
    // 旧版固件没有保存 scheme，用户通过 Web UI 仅更新了 port，此时应自动启用 TLS
    if (!cfg["scheme"].is<const char*>() && config.port == 8883) {
        config.scheme = "mqtts";
        LOG_INFO("MQTT: Auto-detected mqtts from port 8883");
    }
    config.username    = cfg["username"]    | "";
    config.password    = cfg["password"]    | "";
    config.topicPrefix = cfg["topicPrefix"] | "";
    config.otaBaseUrl  = cfg["otaBaseUrl"]  | "";
    config.subscribeTopic = cfg["subscribeTopic"] | "";
    config.keepAlive   = cfg["keepAlive"]   | 60;
    config.autoReconnect     = cfg["autoReconnect"]     | true;
    // 遗嘱消息配置
    config.willTopic   = cfg["willTopic"]   | "";
    config.willPayload = cfg["willPayload"] | "";
    config.willQos     = cfg["willQos"]     | 0;
    config.willRetain  = cfg["willRetain"]  | false;
    
    // 认证配置
    config.authType    = static_cast<MqttAuthType>(cfg["authType"] | 0);
    config.deviceNum   = cfg["deviceNum"]   | "";
    // productId 只从 device.json 获取，不从 protocol.json 读取
    config.userId      = cfg["userId"]      | "";
    config.mqttSecret  = cfg["mqttSecret"]  | "";
    config.authCode    = cfg["authCode"]    | "";
    config.ntpServer   = cfg["ntpServer"]   | "";
    // Card 高级配置（设备信息发布用）
    config.longitude      = cfg["longitude"]      | 0.0;
    config.latitude       = cfg["latitude"]       | 0.0;
    config.iccid          = cfg["iccid"]          | "";
    config.cardPlatformId = cfg["cardPlatformId"] | 0;
    config.summary        = cfg["summary"]        | "";

    // 如果deviceNum/userId/ntpServer为空，尝试从device.json中读取
    // productId 始终从 device.json 的 productNumber 获取
    if (config.deviceNum.isEmpty() || config.productId.isEmpty() || config.userId.isEmpty()
        || config.ntpServer.isEmpty()) {
        if (LittleFS.exists("/config/device.json")) {
            File devFile = LittleFS.open("/config/device.json", "r");
            if (devFile) {
                JsonDocument devDoc;
                if (!deserializeJson(devDoc, devFile)) {
                    if (config.deviceNum.isEmpty()) {
                        config.deviceNum = devDoc["deviceId"] | "";
                    }
                    if (config.productId.isEmpty()) {
                        String pn = String(devDoc["productNumber"] | 0);
                        if (pn != "0") config.productId = pn;
                    }
                    if (config.userId.isEmpty()) {
                        config.userId = devDoc["userId"] | "";
                    }
                    if (config.ntpServer.isEmpty()) {
                        config.ntpServer = devDoc["ntpServer1"] | "";
                    }
                }
                devFile.close();
            }
        }
    }

    // 加载发布主题配置（支持多组）
    config.publishTopics.clear();
    JsonArray topicsArr = cfg["publishTopics"].as<JsonArray>();
    if (!topicsArr.isNull()) {
        for (JsonVariant v : topicsArr) {
            MqttPublishTopic topic;
            topic.topic = v["topic"] | "";
            topic.qos = v["qos"] | 0;
            topic.retain = v["retain"] | false;
            topic.enabled = v["enabled"] | true;
            topic.autoPrefix = v["autoPrefix"] | false;
            topic.topicType = static_cast<MqttTopicType>(v["topicType"] | 0);
            if (topic.topicType == MqttTopicType::OTA_BINARY) {
                topic.enabled = false;
                LOG_WARNING("MQTT: OTA_BINARY publish is not supported; topic forced disabled");
            }
            config.publishTopics.push_back(topic);
        }
    }
    // 兼容旧配置：如果有单独的 publishTopic，添加到数组
    if (config.publishTopics.empty() && !cfg["publishTopic"].isNull()) {
        MqttPublishTopic topic;
        topic.topic = cfg["publishTopic"] | "";
        topic.qos = cfg["publishQos"] | 0;
        topic.retain = cfg["publishRetain"] | false;
        config.publishTopics.push_back(topic);
    }
    // 确保至少有一个默认配置
    if (config.publishTopics.empty()) {
        MqttPublishTopic topic;
        config.publishTopics.push_back(topic);
    }
    
    // 加载订阅主题配置（支持多组）
    config.subscribeTopics.clear();
    JsonArray subTopicsArr = cfg["subscribeTopics"].as<JsonArray>();
    if (!subTopicsArr.isNull()) {
        for (JsonVariant v : subTopicsArr) {
            MqttSubscribeTopic topic;
            topic.topic = v["topic"] | "";
            topic.qos = v["qos"] | 0;
            topic.enabled = v["enabled"] | true;
            topic.autoPrefix = v["autoPrefix"] | false;
            topic.topicType = static_cast<MqttTopicType>(v["topicType"] | 1);
            if (topic.topicType == MqttTopicType::OTA_BINARY) {
                topic.enabled = false;
                LOG_WARNING("MQTT: OTA_BINARY subscribe is not supported; topic forced disabled");
            }
            config.subscribeTopics.push_back(topic);
        }
    }
    // 兼容旧配置：如果有单独的 subscribeTopic，添加到数组
    if (config.subscribeTopics.empty() && !config.subscribeTopic.isEmpty()) {
        MqttSubscribeTopic topic;
        topic.topic = config.subscribeTopic;
        topic.qos = 0;
        config.subscribeTopics.push_back(topic);
    }

    // 无论 clientId 是否已配置，确保 deviceNum/productId/userId 都有有效默认值
    // （修复：之前这些默认值仅在 clientId 为空时才设置，导致已有 clientId 时 productId 为空，
    //  主题前缀缺失 productId，平台下发的消息无法匹配设备订阅的主题）
    if (config.deviceNum.isEmpty()) {
        String mac = WiFi.macAddress();
        mac.replace(":", "");
        config.deviceNum = "FBE" + mac;
        LOG_WARNINGF("[MQTT] deviceNum empty after loadConfig, fallback to %s (check device.json)",
                     config.deviceNum.c_str());
    }
    if (config.productId.isEmpty()) {
        config.productId = "1";
    }
    if (config.userId.isEmpty()) {
        config.userId = "1";
    }

#if FASTBEE_ENABLE_OTA
    // Resolve topics only after deviceNum/productId defaults are available.
    // This also recognizes an already-expanded canonical path even when an
    // older configuration stored the wrong topicType.
    normalizeFastBeeOtaTopics();
#endif

    // clientId 若配置文件未指定，按 FastBee 认证格式自动生成：认证类型&设备编号&产品编号&用户ID
    if (cfg["clientId"].isNull() || cfg["clientId"].as<String>().isEmpty()) {
        // 根据认证类型构建 clientId
        String prefix = (config.authType == MqttAuthType::ENCRYPTED) ? "E" : "S";
        config.clientId = prefix + "&" + config.deviceNum + "&" + config.productId + "&" + config.userId;
    } else {
        config.clientId = cfg["clientId"].as<String>();
    }

    char buf[128];
    snprintf(buf, sizeof(buf), "MQTT: Config loaded scheme=%s server=%s:%d client=%s",
             config.scheme.c_str(), config.server.c_str(), config.port, config.clientId.c_str());
    LOG_INFO(buf);
    return true;
}

bool MQTTClient::begin(uint32_t taskStartupDelayMs) {
    stopped = false;
    _taskStartupDelayMs = taskStartupDelayMs;
    loadMqttConfig();

    // 运行时 PSRAM 检测：无 PSRAM 时 mqtts 可能因内存不足失败
    if (config.scheme == "mqtts" && !psramFound()) {
        ets_printf("[MQTT] *** WARNING: No PSRAM detected. MQTTS(TLS) connection may fail due to insufficient memory.\n");
        ets_printf("[MQTT] *** SUGGESTION: Switch to mqtt:// if mqtts connection fails.\n");
        LOG_WARNING("[MQTT] No PSRAM detected. MQTTS(TLS) may fail due to insufficient memory. Suggest switching to mqtt://");
    }

    // 根据是否有外部 Client 注入来选择传输层
    if (_externalClient) {
        mqttClient.setClient(*_externalClient);
        LOG_INFO("MQTT: Using external transport client");
    } else if (config.scheme == "mqtts") {
        // MQTTS (TLS): 延迟创建 WiFiClientSecure，在 doReconnect() 中按需分配
        mqttClient.setClient(wifiClient);
        LOG_INFO("MQTT: MQTTS transport configured; TLS client will be created on reconnect");
    } else {
        mqttClient.setClient(wifiClient);
        LOG_INFO("MQTT: Using WiFiClient (mqtt://) transport");
    }
    mqttClient.setServer(config.server.c_str(), config.port);
    // MQTT/MQTTS both require 1024 bytes. This covers a complete 256-byte
    // Modbus RTU frame plus the configured topic and MQTT publish header.
    bool isMqttsInit = (config.scheme == "mqtts");
    if (!isMqttsInit) {
        _mqttsMemoryBackoffActive = false;
        _lastMqttsTlsMemoryFailure = false;
    }
    uint16_t mqttBufferSize = 1024;
    if (!mqttClient.setBufferSize(mqttBufferSize)) {
        lastErrorCode = -11;
        LOG_ERRORF("MQTT: Failed to allocate %u-byte packet buffer (current=%u)",
                   mqttBufferSize, mqttClient.getBufferSize());
        return false;
    }
    mqttClient.setKeepAlive(config.keepAlive);
    // MQTTS (TLS) 握手在 ESP32-C6 等低主频芯片上需要 5~15 秒
    // 使用较长的连接超时，连接成功后在 reconnect() 中恢复短超时
    if (isMqttsInit) {
        mqttClient.setSocketTimeout(30);  // TLS 握手需要更长时间
        LOG_INFO("MQTT: Socket timeout set to 30s for MQTTS TLS handshake");
    } else {
        mqttClient.setSocketTimeout(5);
    }
    mqttClient.setCallback([this](char* topic, byte* payload, unsigned int length) {
        this->mqttCallback(topic, payload, length);
    });

    _reconnectPending = false;
    _reconnectRunning = false;

    // 注册 MQTT 发布缓冲池到 HealthMonitor（碎片预防监控）
#if FASTBEE_ENABLE_HEALTH_MONITOR
    {
        HealthMonitor* hm = FastBeeFramework::getInstance()->getHealthMonitor();
        if (hm) {
            HealthMonitor::PoolStatsEntry entry;
            entry.name = "mqtt_pub";
            entry.blockSize = 512;
            entry.capacity = 4;
            entry.getUsed = []() -> uint8_t { return g_mqttPublishPool.usedCount(); };
            entry.getExhaust = []() -> uint32_t { return g_mqttPublishPool.exhaustTotal(); };
            hm->registerPool(entry);
        }
    }
#endif

    LOG_INFO("MQTT: Client initialized");
    return true;
}

void MQTTClient::shutdown() {
    _reconnectPending = false;
    TaskHandle_t reconnectTask = _reconnectTaskHandle;
    if (reconnectTask) {
        if (xTaskGetCurrentTaskHandle() == reconnectTask) {
            return;
        }

        unsigned long waitStart = millis();
        while (_reconnectRunning && (millis() - waitStart) < 1500UL) {
            delay(10);
        }

        reconnectTask = _reconnectTaskHandle;
        if (reconnectTask) {
            vTaskDelete(reconnectTask);
            _reconnectTaskHandle = nullptr;
            _reconnectRunning = false;
            LOG_INFO("[MQTT] Background reconnect task stopped");
        }
    }
}

bool MQTTClient::ensureReconnectTask() {
    if (_reconnectTaskHandle) {
        return true;
    }

    const uint32_t reconnStackSize = (config.scheme == "mqtts")
        ? FastBee::MemoryBudget::MQTTS_RECONNECT_TASK_STACK
        : SIMPLE_TASK_STACK;
    BaseType_t ret = xTaskCreate(
        reconnectTaskEntry,
        "mqtt_reconn",
        reconnStackSize,
        this,
        1,
        &_reconnectTaskHandle
    );
    if (ret != pdPASS) {
        LOG_WARNING("[MQTT] Failed to create on-demand reconnect task");
        _reconnectTaskHandle = nullptr;
        return false;
    }

    LOG_INFO("[MQTT] On-demand reconnect task created");
    return true;
}

bool MQTTClient::connect() {
    // 最后一道防线：空 server 直接拒绝，避免空 host 反复 DNS 重连锁死堆内存
    if (config.server.length() == 0) {
        LOG_WARNING("MQTT: connect refused, server is empty");
        stopped = true;
        return false;
    }
    stopped = false;
    return reconnect();
}

void MQTTClient::disconnect() {
    _reconnectPending = false;
    if (mqttClient.connected()) {
        mqttClient.disconnect();
        // 确保 DISCONNECT 报文在关闭 socket 前已发送出去
        // PubSubClient::disconnect() 将报文写入 Client 缓冲区，但不保证立即发送
        // 如果不 flush，平台可能检测为异常断连而非正常离线
        if (_externalClient) {
            _externalClient->flush();
        } else {
            wifiClient.flush();
        }
        // 给 TCP 栈一点时间发送缓冲区数据
        delay(50);
    }
    // 确保底层 TCP socket 完全关闭释放
    wifiClient.stop();
    if (_externalClient) {
        _externalClient->stop();
    }
    releaseTlsTransport();
    isConnected = false;
    reconnectInterval = 5000;
    consecutiveTimeouts = 0;
    _lastMqttsTlsMemoryFailure = false;
    _mqttsMemoryBackoffActive = false;
    LOG_INFO("MQTT: Disconnected");
}

void MQTTClient::stop() {
    stopped = true;
    shutdown();
    disconnect();
    _notifyStatusChange();  // 通知状态变化：主动停止
    LOG_INFO("MQTT: Stopped (auto-reconnect disabled)");
}

// 重置错误计数器：网络恢复或切换后调用，立即触发重连
void MQTTClient::resetErrorCounters() {
    consecutiveTimeouts = 0;
    reconnectInterval = 5000;   // 重置为初始退避值
    lastReconnectAttempt = 0;   // 强制下次 handle() 立即尝试重连
    lastErrorCode = 0;
    _mqttsMemoryBackoffActive = false;
    // 清除可能卡死的重连调度标志，使网络恢复后能立即重新调度。
    // 注意：不在此处删除任务/清 _reconnectRunning，避免跨任务竞态；
    // 若 _reconnectRunning 卡死，由 handle() 看门狗兑底回收。
    _reconnectPending = false;
    _reconnectScheduledMs = 0;
    LOG_INFO("[MQTT] Error counters reset — immediate reconnect on next handle()");
}

// 自愈：强制重置卡死的重连标志与僵死任务。
// 触发场景：一次性重连任务异常终止（如栈溢出）后遗留 _reconnectPending/_reconnectRunning，
// 使 handle() 调度门控（!_reconnectPending && !_reconnectRunning）永远不满足、自动重连永久停摆。
void MQTTClient::recoverStuckReconnect(const char* reason) {
    LOG_WARNINGF("[MQTT] Reconnect stuck detected (%s), forcing reset (pending=%d running=%d)",
                 reason ? reason : "unknown", (int)_reconnectPending, (int)_reconnectRunning);
    _reconnectPending = false;
    _reconnectRunning = false;
    _reconnectScheduledMs = 0;
    TaskHandle_t stuck = _reconnectTaskHandle;
    _reconnectTaskHandle = nullptr;
    // 回收僵死任务（避免与本任务同一时删除）
    if (stuck && stuck != xTaskGetCurrentTaskHandle()) {
        vTaskDelete(stuck);
    }
    // 重置退避计数：卡死前的连续超时不应延续到恢复后，避免一恢复就陷入慢模式
    consecutiveTimeouts = 0;
}

bool MQTTClient::publish(const String& topic, const String& message) {
    RecursiveMutexGuard lock(_publishMutex);
    if (!isConnected) {
        return false;
    }

    // 2026-08-05 fix: 8BIT 低水位时拒发。重连后 publish 突发（info/ntp/
    // event/pending）会把 lwIP 发送缓冲塞满（broker ACK 慢，errno 11 反复
    // 重试），8BIT 被压到 1-3KB。拒发后 property 走 pending 队列重试，
    // 其余上报下周期补发，避免发送缓冲积压挤爆 DRAM。
    if (heap_caps_get_free_size(MALLOC_CAP_8BIT) < 5120U) {
        static unsigned long lastPubGateWarn = 0;
        if (millis() - lastPubGateWarn > 10000) {
            lastPubGateWarn = millis();
            ets_printf("[MQTT] Publish gated: 8BIT DRAM low (%lu), topic=%s\n",
                       (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                       topic.c_str());
        }
        return false;
    }

    // 查找对应的主题配置，使用其 QoS、Retain 和 autoPrefix 设置
    uint8_t qos = 0;
    bool retain = false;
    bool topicAutoPrefix = false;
    MqttTopicType topicType = MqttTopicType::DATA_REPORT;
    for (const auto& pt : config.publishTopics) {
        const String configuredTopic =
            buildFullTopicWithType(pt.topic, pt.autoPrefix, pt.topicType);
        if (pt.topic == topic || configuredTopic == topic) {
            if (pt.topicType == MqttTopicType::OTA_BINARY) {
                LOG_WARNING("MQTT: OTA_BINARY publish rejected because binary OTA is not supported");
                return false;
            }
            if (!pt.enabled) {
                LOG_WARNING("MQTT: Publish rejected because the configured topic is disabled");
                return false;
            }
            qos = pt.qos;
            retain = pt.retain;
            topicAutoPrefix = pt.autoPrefix;
            topicType = pt.topicType;
            break;
        }
    }
    
    String fullTopic = buildFullTopicWithType(topic, topicAutoPrefix, topicType);
    
    bool ok = mqttClient.publish(fullTopic.c_str(), message.c_str(), retain);

    if (!ok) {
        char buf[80];
        snprintf(buf, sizeof(buf), "MQTT: Publish failed topic=%s", fullTopic.c_str());
        LOG_WARNING(buf);
        // publish 失败通常意味着连接已断，立即标记以避免后续 write 对死 socket 阻塞
        if (!mqttClient.connected()) {
            isConnected = false;
        }
    }
    return ok;
}

bool MQTTClient::publishToTopic(size_t topicIndex, const String& message) {
    RecursiveMutexGuard lock(_publishMutex);
    if (!isConnected || topicIndex >= config.publishTopics.size()) {
        return false;
    }

    // Web 忙碌门控（实测 2026-08-05）：TLS publish 与在途 Web 响应竞争
    // lwIP/WiFi TX 通道，会把浏览器响应卡死 12s+（free8=20KB 内存充足
    // 也复现，非内存问题）。前台窗口/静态传输在途时拒绝 publish，
    // 调用方（上报队列/规则引擎）走失败回退路径暂缓重发。
    {
        FastBeeFramework* fwPub = FastBeeFramework::getInstance();
        WebConfigManager* webPub = fwPub ? fwPub->getWebConfigManager() : nullptr;
        if (webPub && webPub->isWebBusy()) {
            static unsigned long _pubGateWarn = 0;
            if (millis() - _pubGateWarn > 10000) {
                _pubGateWarn = millis();
                ets_printf("[MQTT] Publish deferred: web busy (foreground/static transfer active)\n");
            }
            return false;
        }
    }

    const MqttPublishTopic& pt = config.publishTopics[topicIndex];
    if (pt.topicType == MqttTopicType::OTA_BINARY) {
        LOG_WARNING("MQTT: OTA_BINARY publish rejected because binary OTA is not supported");
        return false;
    }
    if (!pt.enabled) {
        return false;
    }
    String fullTopic = buildFullTopicWithType(pt.topic, pt.autoPrefix, pt.topicType);
    const String fastBeeOtaReplyPath =
        "/" + config.deviceNum + "/http/upgrade/reply";
    // OTA acknowledgements are a fixed FastBee protocol contract and must not
    // be rewritten by user report scripts.
#if FASTBEE_ENABLE_RULE_SCRIPT
    String payload = (fullTopic == fastBeeOtaReplyPath ||
                      pt.topicType == MqttTopicType::OTA_UPGRADE ||
                      pt.topicType == MqttTopicType::OTA_BINARY)
        ? message
        : RuleScriptManager::getInstance().applyReportTransform(0, message);
#else
    String payload = message;
#endif
    bool ok = mqttClient.publish(fullTopic.c_str(), payload.c_str(), pt.retain);

    // 统一打印 MQTT 上报日志（Serial.printf 避免 LoggerSystem 文件 I/O 在发布路径中加剧栈压力）
    {
        String truncatedPayload = payload.length() > 120 ? payload.substring(0, 120) + "..." : payload;
        Serial.printf("[MQTT] TX ▲ topic=%s len=%u payload=%s result=%s\n",
                      fullTopic.c_str(), payload.length(), truncatedPayload.c_str(),
                      ok ? "OK" : "FAIL");
    }

    if (!ok) {
        char buf[80];
        snprintf(buf, sizeof(buf), "MQTT: Publish failed topic=%s", fullTopic.c_str());
        LOG_WARNING(buf);
        if (!mqttClient.connected()) {
            isConnected = false;
        }
    }
    return ok;
}

int MQTTClient::findEnabledPublishTopicIndex(MqttTopicType type) const {
    for (size_t i = 0; i < config.publishTopics.size(); i++) {
        if (config.publishTopics[i].topicType == type &&
            config.publishTopics[i].enabled) {
            return (int)i;
        }
    }
    return -1;
}

int MQTTClient::findFastBeeOtaReplyTopicIndex() const {
    if (config.deviceNum.isEmpty()) return -1;

    const String expected = "/" + config.deviceNum + "/http/upgrade/reply";
    int enabledIndex = -1;
    for (size_t i = 0; i < config.publishTopics.size(); ++i) {
        const MqttPublishTopic& topic = config.publishTopics[i];
        if (buildFullTopicWithType(topic.topic, topic.autoPrefix,
                                   topic.topicType) != expected) continue;
        // A disabled row for the exact protocol path is an explicit opt-out;
        // an enabled duplicate must not silently bypass it.
        if (!topic.enabled) return -1;
        if (enabledIndex < 0) enabledIndex = static_cast<int>(i);
    }
    return enabledIndex;
}

bool MQTTClient::hasUsableFastBeeOtaReplyTopic() const {
    return findFastBeeOtaReplyTopicIndex() >= 0;
}

bool MQTTClient::publishDeviceInfo() {
    if (!isConnected) {
        return false;
    }

    // 内存保护：该函数需要约 2KB 内部 DRAM 分配（JsonDocument + String payload）。
    if (!mqttHasInternalDram(30000, 4096, "publishDeviceInfo")) {
        return false;
    }

    // 查找 topicType==DEVICE_INFO 的发布主题
    int infoTopicIdx = findEnabledPublishTopicIndex(MqttTopicType::DEVICE_INFO);
    if (infoTopicIdx < 0) {
        LOG_DEBUG("MQTT: No enabled DEVICE_INFO publish topic found");
        return false;
    }

    // 构建设备信息 JSON
    JsonDocument doc;
    doc["rssi"] = WiFi.RSSI();
    doc["firmwareVersion"] = SystemInfo::VERSION;
    doc["status"] = 3;  // 在线

    // userId: 优先使用配置值，默认为 1
    if (!config.userId.isEmpty()) {
        long uid = config.userId.toInt();
        doc["userId"] = (uid > 0) ? uid : 1;
    } else {
        doc["userId"] = 1;
    }

    // 经纬度
    doc["longitude"] = config.longitude;
    doc["latitude"]  = config.latitude;

    // 可选字段：非空/非零时才包含
    if (!config.iccid.isEmpty()) {
        doc["iccid"] = config.iccid;
    }
    if (config.cardPlatformId != 0) {
        doc["cardPlatformId"] = config.cardPlatformId;
    }

    // summary: 解析配置的 JSON 字符串，为空时自动生成默认值
    if (!config.summary.isEmpty()) {
        JsonDocument summaryDoc;
        DeserializationError err = deserializeJson(summaryDoc, config.summary);
        if (!err) {
            doc["summary"] = summaryDoc.as<JsonObject>();
        }
    } else {
        // 自动生成默认 summary
        JsonObject summary = doc["summary"].to<JsonObject>();
        summary["name"] = "FastBee";
        summary["chip"] = ESP.getChipModel();
    }

    String payload;
    serializeJson(doc, payload);

    // 获取完整主题用于日志输出
    const MqttPublishTopic& pt = config.publishTopics[infoTopicIdx];
    String fullTopic = buildFullTopicWithType(pt.topic, pt.autoPrefix, pt.topicType);

    bool ok = publishToTopic((size_t)infoTopicIdx, payload);

    LOG_INFO(ok ? "MQTT: Published device info" : "MQTT: Failed to publish device info");

    return ok;
}

bool MQTTClient::publishMonitorData() {
    if (!isConnected) {
        return false;
    }
    // 内存保护：serializeJson 到 String 需要可用内部 DRAM。
    if (!mqttHasInternalDram(25000, 4096, "publishMonitorData")) {
        return false;
    }

    // 查找 topicType==REALTIME_MON 的发布主题
    int monTopicIdx = findEnabledPublishTopicIndex(MqttTopicType::REALTIME_MON);
    if (monTopicIdx < 0) {
        LOG_DEBUG("MQTT: No enabled REALTIME_MON publish topic found");
        return false;
    }

    String payload;

    // 优先使用数据提供者回调获取真实传感器数据
    if (_monitorDataProvider) {
        payload = _monitorDataProvider();
        if (payload.length() <= 2) {
            // 返回空或 "[]"，无可用数据
            LOG_DEBUG("MQTT: Monitor data provider returned empty data");
            return false;
        }
    } else {
        // 降级兜底：未注册数据提供者时使用模拟数据
        LOG_DEBUG("MQTT: Monitor using fallback random data");
        float temperature = 20.0 + (float)(esp_random() % 1500) / 100.0;
        float humidity    = 30.0 + (float)(esp_random() % 5000) / 100.0;

        String remark = "";
        struct tm timeinfo;
        if (getLocalTime(&timeinfo, 0) && timeinfo.tm_year >= 100) {
            char timeBuf[24];
            strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", &timeinfo);
            remark = timeBuf;
        }

        JsonDocument doc;
        JsonArray arr = doc.to<JsonArray>();

        JsonObject tempObj = arr.add<JsonObject>();
        tempObj["id"] = "temperature";
        char tempVal[8];
        snprintf(tempVal, sizeof(tempVal), "%.2f", temperature);
        tempObj["value"] = tempVal;
        tempObj["remark"] = remark;

        JsonObject humiObj = arr.add<JsonObject>();
        humiObj["id"] = "humidity";
        char humiVal[8];
        snprintf(humiVal, sizeof(humiVal), "%.2f", humidity);
        humiObj["value"] = humiVal;
        humiObj["remark"] = remark;

        serializeJson(doc, payload);
    }

    bool ok = publishToTopic((size_t)monTopicIdx, payload);

    LOG_INFO(ok ? "MQTT: Published monitor data" : "MQTT: Failed to publish monitor data");

    return ok;
}

bool MQTTClient::publishReportData(const String& payload) {
    if (!isConnected) {
        return false;
    }

    // 查找 topicType==DATA_REPORT 的发布主题
    int reportTopicIdx = findEnabledPublishTopicIndex(MqttTopicType::DATA_REPORT);
    if (reportTopicIdx < 0) {
        LOG_DEBUG("MQTT: No enabled DATA_REPORT publish topic found");
        return false;
    }

    const MqttPublishTopic& pt = config.publishTopics[reportTopicIdx];
    String fullTopic = buildFullTopicWithType(pt.topic, pt.autoPrefix, pt.topicType);

    // 透传HEX模式：当 Modbus transferType==1 且 payload 是纯 HEX ASCII 字符串时，
    // 将 payload 解码为二进制字节帧后发送（平台直接按 Modbus 帧解析）
    bool sentAsBinary = false;
    bool ok = false;
#if FASTBEE_ENABLE_MODBUS
    do {
        uint8_t transferType = 0;
        auto* fw = FastBeeFramework::getInstance();
        if (!fw) break;
        auto* pm = fw->getProtocolManager();
        if (!pm) break;
        auto modbusLease = pm->acquireModbusHandler(pdMS_TO_TICKS(100));
        ModbusHandler* mh = modbusLease.get();
        if (!mh) break;
        transferType = mh->getTransferType();
        if (transferType != 1) break;

        // Raw passthrough is shared with ordinary structured property reports.
        // Keep valid JSON objects/arrays on MQTT's text path; only non-JSON
        // payloads are eligible for strict HEX decoding below.
        const bool payloadIsStructuredJson = mqttPayloadIsStructuredJson(
            reinterpret_cast<const uint8_t*>(payload.c_str()), payload.length());
        if (payloadIsStructuredJson) break;

        // 校验是否为偶数长度且字符集全部为 [0-9a-fA-F]
        unsigned int hexLen = payload.length();
        if (hexLen < 8 || (hexLen % 2) != 0) {
            LOG_WARNINGF("MQTT: Modbus raw report must be even-length HEX (len=%u), publish rejected",
                         hexLen);
            return false;
        }
        bool allHex = true;
        for (unsigned int i = 0; i < hexLen; i++) {
            char c = payload.charAt(i);
            bool v = (c >= '0' && c <= '9') ||
                     (c >= 'a' && c <= 'f') ||
                     (c >= 'A' && c <= 'F');
            if (!v) { allHex = false; break; }
        }
        if (!allHex) {
            LOG_WARNING("MQTT: Modbus raw report contains non-HEX data, publish rejected");
            return false;
        }

        // HEX ASCII 转换为字节数组
        unsigned int byteLen = hexLen / 2;
        if (byteLen > Protocols::MODBUS_BUFFER_SIZE) {
            LOG_WARNINGF("MQTT: Modbus raw frame too large (%u > %u), publish rejected",
                         byteLen, (unsigned)Protocols::MODBUS_BUFFER_SIZE);
            return false;
        }
        uint8_t buf[Protocols::MODBUS_BUFFER_SIZE];
        auto hexVal = [](char c) -> uint8_t {
            if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
            if (c >= 'a' && c <= 'f') return (uint8_t)(c - 'a' + 10);
            if (c >= 'A' && c <= 'F') return (uint8_t)(c - 'A' + 10);
            return 0;
        };
        for (unsigned int i = 0; i < byteLen; i++) {
            buf[i] = (uint8_t)((hexVal(payload.charAt(i * 2)) << 4) |
                               hexVal(payload.charAt(i * 2 + 1)));
        }

        const uint16_t expectedCrc = mh->calculateCRC(buf, byteLen - 2U);
        if (buf[byteLen - 2U] != static_cast<uint8_t>(expectedCrc & 0xFFU) ||
            buf[byteLen - 1U] != static_cast<uint8_t>((expectedCrc >> 8) & 0xFFU)) {
            LOG_WARNING("MQTT: Modbus raw report CRC mismatch, publish rejected");
            return false;
        }

        // 以二进制方式发布（PubSubClient 接受 const uint8_t*+length）
        {
            RecursiveMutexGuard lock(_publishMutex);
            ok = mqttClient.publish(fullTopic.c_str(), buf, byteLen, pt.retain);
        }
        sentAsBinary = true;
        Serial.printf("[MQTT] TX ▲ topic=%s BIN len=%u hex=%s result=%s\n",
                      fullTopic.c_str(), byteLen, payload.c_str(), ok ? "OK" : "FAIL");
    } while (false);
#endif

    if (!sentAsBinary) {
        // 非透传模式或已验证的结构化 JSON：按文本发布。
        ok = publishToTopic((size_t)reportTopicIdx, payload);
    }

    char logBuf[128];
    snprintf(logBuf, sizeof(logBuf), "MQTT: %s DATA_REPORT topic=%s",
             ok ? "Published" : "Failed to publish", fullTopic.c_str());
    ok ? LOG_INFO(logBuf) : LOG_WARNING(logBuf);

    return ok;
}

bool MQTTClient::publishModbusRtuReport(const String& hexPayload) {
    if (!isConnected) return false;

    const int reportTopicIdx = findEnabledPublishTopicIndex(MqttTopicType::DATA_REPORT);
    if (reportTopicIdx < 0) {
        LOG_DEBUG("MQTT: No enabled DATA_REPORT publish topic found");
        return false;
    }

    uint8_t frame[Protocols::MODBUS_BUFFER_SIZE];
    size_t frameLength = 0;
    if (!decodeModbusRtuHex(hexPayload.c_str(), hexPayload.length(),
                            frame, sizeof(frame), frameLength)) {
        LOG_WARNINGF("MQTT: Explicit Modbus RTU report rejected (invalid HEX/CRC, len=%u)",
                     static_cast<unsigned>(hexPayload.length()));
        return false;
    }

    const MqttPublishTopic& topic = config.publishTopics[reportTopicIdx];
    const String fullTopic = buildFullTopicWithType(
        topic.topic, topic.autoPrefix, topic.topicType);
    bool ok = false;
    {
        RecursiveMutexGuard lock(_publishMutex);
        ok = mqttClient.publish(fullTopic.c_str(), frame, frameLength, topic.retain);
    }
    Serial.printf("[MQTT] TX ▲ topic=%s BIN len=%u result=%s\n",
                  fullTopic.c_str(), static_cast<unsigned>(frameLength),
                  ok ? "OK" : "FAIL");
    return ok;
}

bool MQTTClient::publishOtaReply(const String& payload) {
    if (!isConnected) return false;

    int otaTopicIdx = findFastBeeOtaReplyTopicIndex();
    if (otaTopicIdx < 0) {
        LOG_WARNING("MQTT: No enabled FastBee HTTPS OTA reply topic found");
        return false;
    }
    return publishToTopic(static_cast<size_t>(otaTopicIdx), payload);
}

bool MQTTClient::publishDeviceEvent(const String& eventId, const String& eventName, const String& eventData) {
    if (!isConnected) {
        return false;
    }
    if (eventId.isEmpty()) {
        return false;
    }

    // 构造与 DATA_REPORT 一致的数组格式：[{"id":"<eventId>","value":"<value>","remark":"event"}]
    // value 优先取 eventName，为空时退化为 eventData，仍为空时默认 "1"
    String value = eventName;
    if (value.isEmpty()) value = eventData;
    if (value.isEmpty()) value = "1";

    // JSON 转义：最小限度处理双引号和反斜杠
    auto escapeJson = [](const String& s) {
        String out;
        out.reserve(s.length() + 4);
        for (size_t i = 0; i < s.length(); ++i) {
            char c = s[i];
            if (c == '"' || c == '\\') {
                out += '\\';
                out += c;
            } else if (c == '\n') {
                out += "\\n";
            } else if (c == '\r') {
                out += "\\r";
            } else if (c == '\t') {
                out += "\\t";
            } else {
                out += c;
            }
        }
        return out;
    };

    String payload;
    payload.reserve(64 + eventId.length() + value.length());
    payload = "[{\"id\":\"";
    payload += escapeJson(eventId);
    payload += "\",\"value\":\"";
    payload += escapeJson(value);
    payload += "\",\"remark\":\"event\"}]";

    // 遍历所有启用的 DEVICE_EVENT 类型主题，广播事件
    bool anyPublished = false;
    for (size_t i = 0; i < config.publishTopics.size(); i++) {
        const MqttPublishTopic& pt = config.publishTopics[i];
        if (pt.topicType != MqttTopicType::DEVICE_EVENT) continue;
        if (!pt.enabled) continue;

        String fullTopic = buildFullTopicWithType(pt.topic, pt.autoPrefix, pt.topicType);
        bool ok = publishToTopic(i, payload);
        char evLogBuf[160];
        snprintf(evLogBuf, sizeof(evLogBuf), "MQTT: %s DEVICE_EVENT eventId=%s topic=%s",
                 ok ? "Published" : "Failed to publish", eventId.c_str(), fullTopic.c_str());
        ok ? LOG_INFO(evLogBuf) : LOG_WARNING(evLogBuf);
        if (ok) anyPublished = true;
    }

    return anyPublished;
}

bool MQTTClient::publishNtpSync() {
    if (!isConnected) {
        return false;
    }

    // 查找 topicType==NTP_SYNC 的发布主题
    int ntpTopicIdx = findEnabledPublishTopicIndex(MqttTopicType::NTP_SYNC);
    if (ntpTopicIdx < 0) {
        LOG_DEBUG("MQTT: No enabled NTP_SYNC publish topic found");
        return false;
    }

    // 记录设备发送时间（millis）
    ntpDeviceSendTime = (unsigned long long)millis();

    // 构建 NTP 请求 JSON: {"deviceSendTime": "1592361428000"}
    JsonDocument doc;
    doc["deviceSendTime"] = String(ntpDeviceSendTime);

    String payload;
    serializeJson(doc, payload);

    const MqttPublishTopic& pt = config.publishTopics[ntpTopicIdx];
    String fullTopic = buildFullTopicWithType(pt.topic, pt.autoPrefix, pt.topicType);

    bool ok = publishToTopic((size_t)ntpTopicIdx, payload);

    char logBuf[128];
    snprintf(logBuf, sizeof(logBuf), "MQTT: %s NTP_SYNC topic=%s",
             ok ? "Published" : "Failed to publish", fullTopic.c_str());
    ok ? LOG_INFO(logBuf) : LOG_WARNING(logBuf);

    return ok;
}

bool MQTTClient::subscribe(const String& topic) {
    if (!isConnected) {
        return false;
    }

    // 查找对应订阅主题配置的autoPrefix和topicType设置
    bool topicAutoPrefix = false;
    bool topicEnabled = true;
    MqttTopicType topicType = MqttTopicType::DATA_COMMAND;
    for (const auto& st : config.subscribeTopics) {
        const String configuredTopic =
            buildFullTopicWithType(st.topic, st.autoPrefix, st.topicType);
        if (st.topic == topic || configuredTopic == topic) {
            topicAutoPrefix = st.autoPrefix;
            topicEnabled = st.enabled;
            topicType = st.topicType;
            break;
        }
    }
    if (topicType == MqttTopicType::OTA_BINARY) {
        LOG_WARNING("MQTT: OTA_BINARY subscribe rejected because binary OTA is not supported");
        return false;
    }
    if (!topicEnabled) {
        LOG_WARNING("MQTT: Subscribe rejected because the configured topic is disabled");
        return false;
    }
    String fullTopic = buildFullTopicWithType(topic, topicAutoPrefix, topicType);
    bool ok = mqttClient.subscribe(fullTopic.c_str());

    char buf[80];
    snprintf(buf, sizeof(buf), "MQTT: %s topic=%s",
             ok ? "Subscribed" : "Subscribe failed", fullTopic.c_str());
    ok ? LOG_INFO(buf) : LOG_WARNING(buf);
    return ok;
}

bool MQTTClient::subscribeExact(const String& topic) {
    RecursiveMutexGuard lock(_publishMutex);
    if (!lock.isLocked() || !isConnected || topic.isEmpty() || topic.length()>160 ||
        topic.indexOf('#')>=0 || topic.indexOf('+')>=0) return false;
#if FASTBEE_ENABLE_REMOTE_RESOURCES
    for (const auto& subscription : config.subscribeTopics) {
        if (!subscription.enabled || subscription.topicType == MqttTopicType::OTA_BINARY) continue;
        const String reserved=buildFullTopicWithType(subscription.topic,subscription.autoPrefix,subscription.topicType);
        if (FastBee::RemoteResourceModel::topicMatches(reserved.c_str(),topic.c_str())) return false;
    }
#endif
    return mqttClient.subscribe(topic.c_str(),0);
}

bool MQTTClient::publishExact(const String& topic,const String& message) {
    RecursiveMutexGuard lock(_publishMutex);
    if (!lock.isLocked() || !isConnected || topic.isEmpty() || topic.length()>160 || message.length()>256 ||
        topic.indexOf('#')>=0 || topic.indexOf('+')>=0 ||
        heap_caps_get_free_size(MALLOC_CAP_8BIT)<12288U) return false;
    auto* fw=FastBeeFramework::getInstance();
    auto* web=fw ? fw->getWebConfigManager() : nullptr;
    if (web && web->isWebBusy()) return false;
    return mqttClient.publish(topic.c_str(),message.c_str(),false);
}

bool MQTTClient::unsubscribeExact(const String& topic) {
    RecursiveMutexGuard lock(_publishMutex);
    return lock.isLocked() && isConnected && mqttClient.unsubscribe(topic.c_str());
}

bool MQTTClient::subscribeAll() {
    if (!isConnected) {
        ets_printf("[MQTT] subscribeAll: not connected\n");
        return false;
    }
    
    bool allOk = true;
    ets_printf("[MQTT] subscribeAll: %d topics\n", (int)config.subscribeTopics.size());
    
    // 订阅所有配置的主题
    for (const auto& st : config.subscribeTopics) {
        if (st.topicType == MqttTopicType::OTA_BINARY) {
            ets_printf("[MQTT] skip OTA_BINARY topic='%s': binary OTA is not supported\n",
                       st.topic.c_str());
            continue;
        }
        if (!st.topic.isEmpty() && st.enabled) {
            String fullTopic = buildFullTopicWithType(st.topic, st.autoPrefix, st.topicType);
            bool ok = mqttClient.subscribe(fullTopic.c_str(), st.qos);
            ets_printf("[MQTT] %s topic=%s qos=%d\n",
                       ok ? "Subscribed" : "SUB_FAIL", fullTopic.c_str(), st.qos);
            
            char buf[96];
            snprintf(buf, sizeof(buf), "MQTT: %s topic=%s qos=%d",
                     ok ? "Subscribed" : "Subscribe failed", fullTopic.c_str(), st.qos);
            ok ? LOG_INFO(buf) : LOG_WARNING(buf);
            
            if (!ok) allOk = false;
        } else {
            ets_printf("[MQTT] skip topic='%s' enabled=%d\n", st.topic.c_str(), st.enabled);
        }
    }
    
    // 兼容旧配置：如果 subscribeTopics 为空但 subscribeTopic 不为空
    if (config.subscribeTopics.empty() && !config.subscribeTopic.isEmpty()) {
        allOk = subscribe(config.subscribeTopic);
    }

    return allOk;
}

void MQTTClient::yieldToWeb() {
    if (stopped || !isConnected) {
        return;
    }
    // Align foreground yielding with the Web guard recovery line. Waiting
    // until 12KB left a dead zone on classic ESP32: the Web guard already
    // rejected requests below 20KB while MQTT still retained socket buffers.
    // Releasing MQTT here lets the next bounded Web retry proceed.
    uint32_t const dram8Free = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    if (dram8Free >= FastBee::MemoryBudget::WEB_GUARD_RECOVERY_DRAM_FREE) {
        return;
    }
    ets_printf("[MQTT] yieldToWeb: foreground request arrived, free8=%lu < %lu, sync disconnect\n",
               (unsigned long)dram8Free,
               (unsigned long)FastBee::MemoryBudget::WEB_GUARD_RECOVERY_DRAM_FREE);
    isConnected = false;  // 先置位，避免 protocol_handle 并发继续 publish
    wifiClient.stop();
    if (_wifiClientSecure) _wifiClientSecure->stop();
    reconnectInterval = 30000;  // 30s 冷却，重连由前台推迟门控接管
    lastReconnectAttempt = millis();
    _notifyStatusChange();
}

void MQTTClient::handle() {
    if (stopped) {
        static unsigned long _hStopWarn = 0;
        if (millis() - _hStopWarn > 30000) { _hStopWarn = millis(); Serial.println("[MQTT-DBG] handle() skipped: stopped"); }
        return;  // 被显式停止时不做任何操作
    }

    bool activeNetworkIsWifi = (getActiveMqttNetworkType() == NetworkType::NET_WIFI);

    // 2026-08-05 fix: 8BIT DRAM 枯竭紧急断开。INTERNAL 口径（含 IRAM/RTC）
    // 仍显示 ~29KB 空闲时，可 malloc 的 8BIT DRAM 可能已 <1KB（lwIP 套接字
    // 发送缓冲滞留等），任何小分配都会 new OOM → abort 整机重启。
    // 此时强制断开 MQTT 释放套接字收发缓冲，走正常重连流程恢复。
    if (isConnected) {
        // 前台让位策略：Web 请求期间暂停 MQTT loop/publish，避免 TLS 或
        // lwIP 写操作占满 WiFi TX 通道。正常内存时保持 TCP 会话，避免
        // 每次打开菜单都断开并重连，导致状态接口长期显示
        // ``connecting=true``。只有前台期间跌破 WebGuard 恢复线，或没有前台
        // 请求且跌破 3KB 的紧急水位，才释放 socket 让 Web 先恢复。
        bool webForegroundActive = false;
        {
            FastBeeFramework* fwFg = FastBeeFramework::getInstance();
            WebConfigManager* webFg = fwFg ? fwFg->getWebConfigManager() : nullptr;
            webForegroundActive = webFg && webFg->isForegroundRequestActive();
        }
        uint32_t const dram8Free = heap_caps_get_free_size(MALLOC_CAP_8BIT);
        const bool foregroundMemoryPressure = webForegroundActive &&
            dram8Free < FastBee::MemoryBudget::WEB_GUARD_RECOVERY_DRAM_FREE;
        const bool emergencyMemoryPressure = !webForegroundActive && dram8Free < 3072U;
        if (foregroundMemoryPressure || emergencyMemoryPressure) {
            ets_printf("[MQTT] Yield to web (foreground=%d free8=%lu), force disconnect for memory/TX pressure\n",
                       webForegroundActive ? 1 : 0, (unsigned long)dram8Free);
            wifiClient.stop();
            if (_wifiClientSecure) _wifiClientSecure->stop();
            isConnected = false;
            reconnectInterval = 30000;  // 30s 冷却（15s 实测 churn）
            lastReconnectAttempt = millis();
            _notifyStatusChange();
            return;
        }
        if (webForegroundActive) {
            // The foreground hold is intentionally short (normally 3.5s).
            // Skipping loop() here prevents a blocking publish/read from
            // competing with the response, while the MQTT keepalive window
            // remains far longer than this pause. publishToTopic() has the
            // matching web-busy gate, so no queued report is lost.
            return;
        }
    }

    // 检查网络连接状态：若使用外部 Client 则跳过 WiFi 状态检查
    if (!_externalClient && activeNetworkIsWifi && WiFi.status() != WL_CONNECTED) {
        if (isConnected) {
            isConnected = false;
            // Preserve a concrete reason for the Web/API status endpoint.
            // Previously the session silently became offline while
            // lastErrorCode stayed at 0, so the UI could only say "未连接".
            lastErrorCode = -3;
            LOG_WARNING("MQTT: WiFi disconnected, marking MQTT offline");
            Serial.println("[MQTT-DBG] WiFi disconnected");
            _notifyStatusChange();
        }
        return;
    }

    if (config.scheme == "mqtts") {
        static unsigned long lastMqttsDramWarnMs = 0;
        uint32_t dramFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        uint32_t dramLargest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        if ((dramFree < 15000 || dramLargest < 8192) &&
            millis() - lastMqttsDramWarnMs > 30000) {
            lastMqttsDramWarnMs = millis();
            LOG_WARNINGF("[MQTT] MQTTS DRAM low (dram=%lu largest=%lu)",
                         (unsigned long)dramFree,
                         (unsigned long)dramLargest);
        }
    }

    if (!mqttClient.connected()) {
        if (isConnected) {
            isConnected = false;
            // The transport was healthy and then dropped between loop calls.
            // Report that transition explicitly; reconnect() will replace it
            // with the broker/DNS/auth code when a new attempt fails.
            lastErrorCode = -3;
            if (config.scheme == "mqtts" && _mqttsMemoryBackoffActive) {
                lastReconnectAttempt = millis();
            } else if (lastConnectedTime != 0 &&
                       millis() - lastConnectedTime < 60000UL) {
                // flapping 抑制：本次会话存活 <60s（连上即断）说明链路/broker 不稳，
                // 不重置为 5s 快速重连，而是在现有间隔上翻倍封顶 60s，
                // 避免 MQTTS 反复 TLS 建/毁加剧 DRAM 碎片
                reconnectInterval = min(max(reconnectInterval, (uint32_t)5000) * 2,
                                        (uint32_t)60000);
                consecutiveTimeouts = 0;
                ets_printf("[MQTT] Flapping detected (session<60s), backoff %lus\n",
                           (unsigned long)(reconnectInterval / 1000));
            } else {
                reconnectInterval = 5000;  // 刚断开时重置重连间隔
                consecutiveTimeouts = 0;
            }
            // 立即关闭底层 socket，避免后续 write() 对死 socket 阻塞 10s+
            wifiClient.stop();
            if (_wifiClientSecure) _wifiClientSecure->stop();
            LOG_WARNING("MQTT: Connection lost");
            Serial.println("[MQTT-DBG] Connection lost, socket stopped");
            _notifyStatusChange();  // 通知状态变化：连接断开
            // 触发MQTT断开连接系统事件（仅前3次触发，避免无限循环重启）
#if FASTBEE_ENABLE_PERIPH_EXEC
            if (reconnectCount <= 3) {
                PeriphExecManager::getInstance().triggerEvent(EventType::EVENT_MQTT_DISCONNECTED, "");
            } else {
                Serial.printf("[MQTT-DBG] Skip EVENT_MQTT_DISCONNECTED (reconnectCount=%lu > 3)\n",
                              (unsigned long)reconnectCount);
            }
#endif
        }

        if (!config.autoReconnect) {
            static unsigned long _arWarn = 0;
            if (millis() - _arWarn > 30000) { _arWarn = millis(); Serial.println("[MQTT-DBG] autoReconnect disabled"); }
            return;
        }

        unsigned long now = millis();

        // 8BIT 卡滞救援（实测结论）：MQTT 断开后套接字缓冲通常在 30-60s
        // 内回收；若 free8 <8KB 持续 45s 说明 lwIP 发送缓冲卡死在 WiFi
        // 层（wifiClient.stop() 无法释放），只有 WiFi 重连能回收
        // （2026-08-05 实测：zombie 重连后 free8 5KB→21KB 立即恢复）。
        // 不做前台门控（11:04 实测教训）：free8 卡滞 6.3KB 时 Web 请求
        // 已全部挂起，前台窗口又被重试请求持续续期，门控导致 64s 长挂
        // 直到 TCP PCB 超时才自愈；此时救援是唯一的快速恢复手段。
        // 5 分钟冷却防震荡。
        {
            uint32_t const rescue8Free = heap_caps_get_free_size(MALLOC_CAP_8BIT);
            if (rescue8Free < 8192U) {
                if (_low8SinceMs == 0) {
                    _low8SinceMs = now;
                }
            } else {
                _low8SinceMs = 0;
            }
            if (_low8SinceMs != 0 && now - _low8SinceMs > 45000UL &&
                (_lastWifiRescueMs == 0 || now - _lastWifiRescueMs > 300000UL)) {
                ets_printf("[MQTT] 8BIT stuck at %luB for 45s+, forcing WiFi reconnect to reclaim lwIP buffers\n",
                           (unsigned long)rescue8Free);
                _lastWifiRescueMs = now;
                _low8SinceMs = 0;
                WiFi.disconnect(false);  // NetworkManager update() 接管非阻塞重连
            }
        }

        // 自愈看门狗：一次性重连任务若异常终止（如栈溢出）可能遗留
        // _reconnectPending/_reconnectRunning 卡死，使调度门控永远不满足、自动重连永久停摆。
        // 若标志持续超过最大执行窗口（正常 doReconnect < 40s），强制重置并回收僵死任务。
        if ((_reconnectPending || _reconnectRunning) &&
            _reconnectScheduledMs != 0 &&
            (now - _reconnectScheduledMs > RECONNECT_WATCHDOG_TIMEOUT_MS)) {
            recoverStuckReconnect("watchdog_timeout");
        }

        if (now - lastReconnectAttempt >= reconnectInterval) {
            lastReconnectAttempt = now;
            // Web 前台窗口内推迟重连：重连成功后的突发 publish 会把 8BIT
            // 从 ~21KB 压到 1-3KB（lwIP 发送缓冲塞满），正好撞上浏览器
            // 加载登录页资源就会挂起。前台保护窗口结束后再重连。
            {
                FastBeeFramework* fwForWebGate = FastBeeFramework::getInstance();
                WebConfigManager* webGate = fwForWebGate ? fwForWebGate->getWebConfigManager() : nullptr;
                if (webGate && webGate->isForegroundRequestActive()) {
                    static unsigned long _wfWarn = 0;
                    if (now - _wfWarn > 10000) {
                        _wfWarn = now;
                        ets_printf("[MQTT] Reconnect deferred: web foreground request active\n");
                    }
                    return;
                }
            }
            // WiFi 未连接时跳过重连（仅当未使用外部 Client 时检查）
            if (!_externalClient && activeNetworkIsWifi && WiFi.status() != WL_CONNECTED) {
                Serial.printf("[MQTT-DBG] WiFi not connected (status=%d), skipping reconnect\n", WiFi.status());
                return;
            }
            // 调度后台任务执行重连（避免 reconnect() 阻塞 loopTask 7秒+）
            if (!_reconnectPending && !_reconnectRunning) {
                _reconnectPending = true;
                _reconnectScheduledMs = now;  // 记录调度时间戳（看门狗用）
                ets_printf("[MQTT] Reconnect scheduled (heap=%lu cnt=%lu interval=%lu)\n",
                    (unsigned long)ESP.getFreeHeap(), (unsigned long)reconnectCount, (unsigned long)reconnectInterval);
                if (!ensureReconnectTask()) {
                    _reconnectPending = false;
                    _reconnectScheduledMs = 0;
                    LOG_WARNING("[MQTT] Reconnect skipped: failed to create background task");
                } else {
                    LOG_INFO("[MQTT] Reconnect scheduled in background task");
                }
            } else if (_reconnectRunning) {
                static unsigned long _rrWarn = 0;
                if (millis() - _rrWarn > 10000) { _rrWarn = millis(); Serial.println("[MQTT-DBG] Background reconnect still running"); }
            }
        }
    } else {
        if (!isConnected) {
            isConnected = true;
            lastConnectedTime = millis();
            _low8SinceMs = 0;  // 连接态的短暂低水位由 5KB 强制断开处理，不计入卡滞
        }
        // reconnectCount 老化：连接稳定 5 分钟后清零，避免历史累计次数
        // 让下一次偶发断开直接落入 5 分钟慢模式（MAX_FAST_RETRIES 误判）
        if (reconnectCount > 0 && millis() - lastConnectedTime > 300000UL) {
            ets_printf("[MQTT] Connection stable 5min, reconnectCount reset (was %lu)\n",
                       (unsigned long)reconnectCount);
            reconnectCount = 0;
        }
        lastLoopTime = millis();
        mqttClient.loop();

        // 处理延迟的 DATA_COMMAND 和异步任务上报队列
        processQueuedCommands();
        processQueuedReports();
        
        // 实时监测：按间隔定时发布监测数据
        if (monitorActive && monitorRemaining > 0) {
            unsigned long now = millis();
            if (now - lastMonitorTime >= monitorInterval) {
                lastMonitorTime = now;
                publishMonitorData();
                monitorRemaining--;
                if (monitorRemaining <= 0) {
                    monitorActive = false;
                    LOG_INFO("MQTT: Monitor completed");
                }
            }
        }
    }
}

void MQTTClient::processQueuedCommands() {
    if (!_dataCommandQueue) return;
    // 内存保护：handleDataCommand 内部大量 JSON 序列化。
    if (!mqttHasInternalDram(30000, 4096, "processQueuedCommands")) return;
    String* cmd = nullptr;
    int processed = 0;
    while (processed < 2 && xQueueReceive(_dataCommandQueue, &cmd, 0) == pdTRUE) {
        if (cmd) {
            size_t cmdLen = cmd->length();
#if FASTBEE_ENABLE_PERIPH_EXEC
            PeriphExecManager& execMgr = PeriphExecManager::getInstance();
            String report = execMgr.handleDataCommand(*cmd);
            if (!report.isEmpty() && report != "[]") {
                LOG_INFOF("[PeriphExec] Reporting status (DataCommand response): %s", report.c_str());
                publishReportData(report);
            }
#endif
            delete cmd;
            // Task 4.3: 跟踪队列内存释放
            if (_cmdQueueTotalBytes >= cmdLen) {
                _cmdQueueTotalBytes -= cmdLen;
            } else {
                _cmdQueueTotalBytes = 0;
            }
        }
        processed++;
    }
}

void MQTTClient::processQueuedReports() {
    // 内存保护：publishReportData 构造 String + MQTT publish
    // 使用 DRAM 内部内存检测（排除 PSRAM），与 mqttHasInternalDram() 保持一致
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < 20000) return;

    // 前台让位（实测 2026-08-05）：publish 突发与页面资源竞争 WiFi TX
    // 通道（内存充足时也会阻塞 loopTask），前台窗口内推迟上报。
    {
        FastBeeFramework* fwFgR = FastBeeFramework::getInstance();
        WebConfigManager* webFgR = fwFgR ? fwFgR->getWebConfigManager() : nullptr;
        if (webFgR && webFgR->isForegroundRequestActive()) return;
    }

    // 限速（实测 2026-08-05）：重连后积压上报一次性 flush 会突发塞满
    // lwIP 发送缓冲，8BIT 数秒内跌 5-10KB → 强制断开 → churn。
    // 限制每秒最多 flush 一批，让 broker ACK 逐步释放发送缓冲。
    {
        static unsigned long _lastReportFlushMs = 0;
        unsigned long const nowFlush = millis();
        if (nowFlush - _lastReportFlushMs < 1000UL) return;
        _lastReportFlushMs = nowFlush;
    }

    // 背压机制：队列深度 >= HIGH_WATERMARK 时加速排空（处理更多批次）
    portENTER_CRITICAL(&_slotMux);
    uint8_t depthSnapshot = _slotCount;
    portEXIT_CRITICAL(&_slotMux);
    const uint8_t maxBatch = (depthSnapshot >= BACKPRESSURE_HIGH_WATERMARK)
        ? BACKPRESSURE_DRAIN_BATCH : 4;

    int processed = 0;
    while (processed < maxBatch) {
        // Move one ring entry into a consumer-owned retry slot while holding
        // the index lock. Network I/O remains outside the critical section.
        if (!_pendingReportSlot.occupied) {
            portENTER_CRITICAL(&_slotMux);
            while (_slotCount > 0 && !_pendingReportSlot.occupied) {
                MqttSlot& slot = _reportSlots[_slotReadIndex];
                if (slot.occupied) {
                    _pendingReportSlot.length = slot.length;
                    _pendingReportSlot.encoding = slot.encoding;
                    memcpy(_pendingReportSlot.payload, slot.payload,
                           slot.length + 1U);
                    _pendingReportSlot.occupied = true;
                }
                slot.clear();
                _slotReadIndex = (_slotReadIndex + 1) % MQTT_REPORT_SLOTS;
                _slotCount--;
            }
            portEXIT_CRITICAL(&_slotMux);
        }

        if (!_pendingReportSlot.occupied) break;
        const String pendingPayload(_pendingReportSlot.payload,
                                    _pendingReportSlot.length);
        const bool publishOk =
            _pendingReportSlot.encoding == MqttPayloadEncoding::MODBUS_RTU_HEX
                ? publishModbusRtuReport(pendingPayload)
                : publishReportData(pendingPayload);
        if (!publishOk) {
            // Keep the exact frame/JSON document for the next flush. Producers
            // can continue using the ring without racing the consumer slot.
            break;
        }
        _pendingReportSlot.clear();
        _queueStats.publishTotal++;
        processed++;
    }

#if FASTBEE_ENABLE_HEALTH_MONITOR
    if (processed > 0) {
        HealthMonitor* hm = FastBeeFramework::getInstance()->getHealthMonitor();
        if (hm) {
            portENTER_CRITICAL(&_slotMux);
            uint8_t depthNow = _slotCount;
            portEXIT_CRITICAL(&_slotMux);
            hm->setMqttQueueDepth(depthNow);
        }
    }
#endif
}

void MQTTClient::setMinReportInterval(uint32_t ms) {
    _minReportInterval = ms;
    if (ms > 0) {
        LOGGER.infof("[MQTT] Report interval limited to %lu ms", (unsigned long)ms);
    } else {
        LOGGER.info("[MQTT] Report interval limit removed");
    }
}

bool MQTTClient::queueReportData(const String& payload) {
    return queueReportData(payload.c_str(), (uint16_t)payload.length());
}

bool MQTTClient::queueReportData(const char* payload, uint16_t length) {
    return queueReportDataEncoded(payload, length, MqttPayloadEncoding::TEXT);
}

bool MQTTClient::queueModbusRtuReport(const String& hexPayload) {
    uint8_t validationFrame[Protocols::MODBUS_BUFFER_SIZE];
    size_t validationLength = 0;
    if (!decodeModbusRtuHex(hexPayload.c_str(), hexPayload.length(),
                            validationFrame, sizeof(validationFrame),
                            validationLength)) {
        LOG_WARNINGF("[MQTT] Invalid Modbus RTU frame rejected before enqueue (hexLen=%u)",
                     static_cast<unsigned>(hexPayload.length()));
        _queueStats.dropTotal++;
        return false;
    }
    return queueReportDataEncoded(
        hexPayload.c_str(), static_cast<uint16_t>(hexPayload.length()),
        MqttPayloadEncoding::MODBUS_RTU_HEX);
}

bool MQTTClient::queueReportDataEncoded(const char* payload, uint16_t length,
                                        MqttPayloadEncoding encoding) {
    if (!payload && length > 0) {
        _queueStats.dropTotal++;
        return false;
    }
    // 降采样检查：如果距上次上报不足 _minReportInterval 则跳过
    if (_minReportInterval > 0) {
        unsigned long now = millis();
        if (now - _lastReportQueueTime < _minReportInterval) {
            _queueStats.throttleDrops++;
            return false;  // 降采样丢弃
        }
        _lastReportQueueTime = now;
    }

    // 256-byte Modbus RTU frames occupy exactly 512 HEX characters. Preserve
    // that boundary and reject larger payloads atomically instead of emitting
    // a truncated frame/JSON document.
    if (length > MQTT_SLOT_MAX_PAYLOAD) {
        LOGGER.warningf("[MQTT] Payload too large: %u > %u bytes, enqueue rejected",
                        length, MQTT_SLOT_MAX_PAYLOAD);
        _queueStats.dropTotal++;
        return false;
    }

    // 入队与队满丢旧在同一临界区内完成，避免生产者(worker任务)与
    // 消费者(loopTask)并发修改 _slotReadIndex/_slotCount 导致索引错乱
    bool droppedOldest = false;
    portENTER_CRITICAL(&_slotMux);
    if (_slotCount >= MQTT_REPORT_SLOTS) {
        // 缓冲区满时丢弃最旧的
        _reportSlots[_slotReadIndex].clear();
        _slotReadIndex = (_slotReadIndex + 1) % MQTT_REPORT_SLOTS;
        _slotCount--;
        droppedOldest = true;
    }

    // 拷贝到下一个空闲 slot
    MqttSlot& slot = _reportSlots[_slotWriteIndex];
    memcpy(slot.payload, payload, length);
    slot.payload[length] = '\0';
    slot.length = length;
    slot.encoding = encoding;
    slot.occupied = true;

    _slotWriteIndex = (_slotWriteIndex + 1) % MQTT_REPORT_SLOTS;
    _slotCount++;
    uint8_t depthNow = _slotCount;
    portEXIT_CRITICAL(&_slotMux);

    // 日志/统计在锁外执行（LOGGER 可能阻塞，禁止在自旋锁临界区内调用）
    if (droppedOldest) {
        LOGGER.warning("[MQTT] Report queue full, dropping oldest");
        _queueStats.dropTotal++;
    }
    _queueStats.enqueueTotal++;

#if FASTBEE_ENABLE_HEALTH_MONITOR
    HealthMonitor* hm = FastBeeFramework::getInstance()->getHealthMonitor();
    if (hm) hm->setMqttQueueDepth(depthNow);
#endif
    
    return true;
}

String MQTTClient::getStatus() const {
    return isConnected ? F("Connected") : F("Disconnected");
}

void MQTTClient::setTransportClient(Client* client) {
    _externalClient = client;
    if (client) {
        mqttClient.setClient(*client);
        LOG_INFO("MQTT: External transport client set");
    } else {
        mqttClient.setClient(wifiClient);
        LOG_INFO("MQTT: Transport client cleared, using WiFiClient");
    }
}

void MQTTClient::setMessageCallback(std::function<void(const String&, const String&, MqttTopicType)> callback) {
    messageCallback = callback;
}

void MQTTClient::setStatusChangeCallback(StatusChangeCallback cb) {
    _statusChangeCallback = cb;
}

void MQTTClient::setMonitorDataProvider(std::function<String()> provider) {
    _monitorDataProvider = provider;
}

void MQTTClient::_notifyStatusChange() {
    if (!_statusChangeCallback) return;

    // 构建轻量级 JSON 状态字符串
    // 字段名必须与 MqttRouteHandler::handleGetMqttStatus 保持一致，
    // 前端 _updateMqttStatusUI 依赖 initialized/connected/reconnectCount 等字段
    JsonDocument doc;
    JsonObject data = doc.to<JsonObject>();

    data["initialized"] = true;  // SSE 推送时 MQTT 客户端已存在且配置已加载
    data["connected"] = isConnected;
    data["stopped"] = stopped;
    data["server"] = config.server;
    data["port"] = config.port;
    data["clientId"] = config.clientId;
    data["reconnectCount"] = reconnectCount;
    data["lastError"] = lastErrorCode;
    data["lastErrorCode"] = lastErrorCode;
    // Keep asynchronous SSE diagnostics aligned with the REST status route.
    // The transport pointer only says which Client object was injected; it
    // does not say that Ethernet/4G is currently registered on the network.
    // Conversely, Ethernet MQTTS intentionally uses the internal secure
    // client, so checking WiFi.status() alone would report a healthy Ethernet
    // link as offline.  Ask the active network manager for the authoritative
    // connection state and retain the old fallback during early boot, before
    // the framework has published its manager.
    bool networkConnected = false;
    FastBeeFramework* statusFw = FastBeeFramework::getInstance();
    FBNetworkManager* statusNet = statusFw
        ? static_cast<FBNetworkManager*>(statusFw->getNetworkManager())
        : nullptr;
    if (statusNet) {
        networkConnected = statusNet->isNetworkConnected();
    } else {
        networkConnected = _externalClient || WiFi.status() == WL_CONNECTED;
    }
    data["lastErrorName"] = mqttStatusErrorName(lastErrorCode);
    data["lastErrorClass"] = mqttStatusErrorClass(
        lastErrorCode, _lastMqttsTlsMemoryFailure);
    const char* errorMessage = mqttStatusErrorMessage(
        lastErrorCode, networkConnected, _lastMqttsTlsMemoryFailure);
    data["lastErrorMessage"] = errorMessage;
    // Keep the legacy alias used by older Web bundles and make the reason
    // available to SSE-only pages without waiting for a REST status poll.
    data["errorMessage"] = errorMessage;
    data["statusReason"] = errorMessage;
    data["networkConnected"] = networkConnected;
    data["autoReconnect"] = config.autoReconnect;
    data["connecting"] = !stopped && !isConnected && config.autoReconnect;
    data["tlsMemoryFailure"] = _lastMqttsTlsMemoryFailure;
    data["scheme"] = config.scheme;
    data["tlsAllocated"] = (_wifiClientSecure != nullptr);
    data["webPausedForMqtts"] = _webServerPaused;
    data["disabledByMemory"] = _disabledByMemory;
    data["stoppedByMemory"] = _stoppedByMemory;
    if (_disabledByMemory) {
        // Match the REST contract even when an SSE consumer receives the
        // stopped event before its next /api/mqtt/status poll.
        data["lastError"] = -13;
        data["lastErrorCode"] = -13;
        data["lastErrorName"] = "MQTT_DISABLED_BY_MEMORY";
        data["lastErrorClass"] = "resource";
        data["lastErrorMessage"] = "设备内存紧张，MQTT 已自动禁用，请恢复内存后重新启用";
        data["errorMessage"] = "设备内存紧张，MQTT 已自动禁用，请恢复内存后重新启用";
        data["statusReason"] = "设备内存紧张，MQTT 已自动禁用，请恢复内存后重新启用";
    } else if (_stoppedByMemory) {
        // Keep SSE diagnostics aligned with the REST status contract while a
        // temporary memory stop is waiting for the next recovery sample.
        data["connected"] = false;
        data["connecting"] = false;
        data["lastError"] = -14;
        data["lastErrorCode"] = -14;
        data["lastErrorName"] = "MQTT_STOPPED_BY_MEMORY";
        data["lastErrorClass"] = "resource";
        data["lastErrorMessage"] = "设备内存紧张，MQTT 已暂时停止，内存恢复后将自动重连";
        data["errorMessage"] = "设备内存紧张，MQTT 已暂时停止，内存恢复后将自动重连";
        data["statusReason"] = "设备内存紧张，MQTT 已暂时停止，内存恢复后将自动重连";
    }
    // 队列背压统计（消费者侧监控用）
    data["queueDepth"] = _slotCount;
    data["queueEnqueue"] = _queueStats.enqueueTotal;
    data["queueDrops"] = _queueStats.dropTotal;
    data["queuePublished"] = _queueStats.publishTotal;

    String json;
    serializeJson(doc, json);

    _statusChangeCallback(json);
}

MqttTopicType MQTTClient::getTopicTypeByPath(const String& topicPath, int8_t* outSubIndex) const {
    // 尝试匹配每个主题（包括占位符展开和autoPrefix生成的完整路径）
    // 先查订阅主题
    for (size_t i = 0; i < config.subscribeTopics.size(); i++) {
        const auto& st = config.subscribeTopics[i];
        if (!st.enabled) continue;
        String fullTopic = buildFullTopicWithType(st.topic, st.autoPrefix, st.topicType);
        if (fullTopic == topicPath) {
            if (outSubIndex) *outSubIndex = (int8_t)i;
            const String otaCommand =
                "/" + config.deviceNum + "/http/upgrade/set";
            return topicPath == otaCommand
                ? MqttTopicType::OTA_UPGRADE
                : st.topicType;
        }
    }
    // 再查发布主题
    for (const auto& pt : config.publishTopics) {
        if (!pt.enabled) continue;
        String fullTopic = buildFullTopicWithType(pt.topic, pt.autoPrefix, pt.topicType);
        if (fullTopic == topicPath) {
            return pt.topicType;
        }
    }
    return MqttTopicType::DATA_COMMAND; // 默认
}

String MQTTClient::getReportTopic() const {
    for (const auto& pt : config.publishTopics) {
        if (pt.topicType == MqttTopicType::DATA_REPORT && pt.enabled) {
            return buildFullTopicWithType(pt.topic, pt.autoPrefix, pt.topicType);
        }
    }
    return "(no_report_topic)";
}

String MQTTClient::buildFullTopic(const String& topic, bool autoPrefix) const {
    return buildFullTopicWithType(topic, autoPrefix, MqttTopicType::DATA_REPORT);
}

String MQTTClient::buildFullTopicWithType(const String& topic, bool autoPrefix, MqttTopicType topicType) const {
    String resolvedTopic = topic;
    resolvedTopic.replace("{deviceNum}", config.deviceNum);
    resolvedTopic.replace("{productId}", config.productId);

    if (!autoPrefix) {
        return resolvedTopic;
    }
    // autoPrefix启用时，根据topicType自动生成设备前缀
    // OTA类型(5,6): /{deviceNum} + topic
    // 其他类型: /{productId}/{deviceNum} + topic
    String prefix;
    if (topicType == MqttTopicType::OTA_UPGRADE || topicType == MqttTopicType::OTA_BINARY) {
        // A complete OTA template is already rooted at the device and must not
        // receive a second device prefix, even if an older UI left autoPrefix on.
        const String devicePrefix = "/" + config.deviceNum;
        if (!config.deviceNum.isEmpty() &&
            (resolvedTopic == devicePrefix || resolvedTopic.startsWith(devicePrefix + "/"))) {
            return resolvedTopic;
        }
        if (!config.deviceNum.isEmpty()) {
            prefix = devicePrefix;
        }
        // FastBee defines OTA topics at /{deviceNum}/... . The optional
        // application topicPrefix is intentionally not part of this contract.
        return prefix + (resolvedTopic.startsWith("/") ? resolvedTopic : "/" + resolvedTopic);
    } else {
        if (!config.productId.isEmpty() && !config.deviceNum.isEmpty()) {
            prefix = "/" + config.productId + "/" + config.deviceNum;
        } else if (!config.deviceNum.isEmpty()) {
            prefix = "/" + config.deviceNum;
        }
    }
    // 额外的topicPrefix（如果设置了的话，追加在设备前缀之前）
    if (!config.topicPrefix.isEmpty()) {
        return config.topicPrefix + prefix + resolvedTopic;
    }
    return prefix + resolvedTopic;
}

void MQTTClient::normalizeFastBeeOtaTopics() {
    const String replyPath = "/" + config.deviceNum + "/http/upgrade/reply";
    int replyWinner = -1;
    for (size_t i = 0; i < config.publishTopics.size(); ++i) {
        const auto& topic = config.publishTopics[i];
        if (buildFullTopicWithType(topic.topic, topic.autoPrefix,
                                   topic.topicType) != replyPath) continue;
        if (replyWinner < 0 ||
            (!topic.enabled && config.publishTopics[replyWinner].enabled)) {
            replyWinner = static_cast<int>(i);
        }
    }
    if (replyWinner < 0) {
        ensureFastBeeOtaPublishTopic(config.publishTopics);
        replyWinner = static_cast<int>(config.publishTopics.size()) - 1;
    }
    config.publishTopics[replyWinner].topic = FASTBEE_OTA_REPLY_TOPIC;
    config.publishTopics[replyWinner].autoPrefix = false;
    config.publishTopics[replyWinner].topicType = MqttTopicType::OTA_UPGRADE;
    for (size_t i = config.publishTopics.size(); i-- > 0;) {
        if (static_cast<int>(i) == replyWinner) continue;
        const auto& topic = config.publishTopics[i];
        if (buildFullTopicWithType(topic.topic, topic.autoPrefix,
                                   topic.topicType) == replyPath) {
            config.publishTopics.erase(config.publishTopics.begin() + i);
            if (static_cast<int>(i) < replyWinner) --replyWinner;
        }
    }

    const String commandPath = "/" + config.deviceNum + "/http/upgrade/set";
    int commandWinner = -1;
    for (size_t i = 0; i < config.subscribeTopics.size(); ++i) {
        const auto& topic = config.subscribeTopics[i];
        if (buildFullTopicWithType(topic.topic, topic.autoPrefix,
                                   topic.topicType) != commandPath) continue;
        if (commandWinner < 0 ||
            (!topic.enabled && config.subscribeTopics[commandWinner].enabled)) {
            commandWinner = static_cast<int>(i);
        }
    }
    if (commandWinner < 0) {
        ensureFastBeeOtaSubscribeTopic(config.subscribeTopics);
        commandWinner = static_cast<int>(config.subscribeTopics.size()) - 1;
    }
    config.subscribeTopics[commandWinner].topic = FASTBEE_OTA_COMMAND_TOPIC;
    config.subscribeTopics[commandWinner].autoPrefix = false;
    config.subscribeTopics[commandWinner].topicType = MqttTopicType::OTA_UPGRADE;
    for (size_t i = config.subscribeTopics.size(); i-- > 0;) {
        if (static_cast<int>(i) == commandWinner) continue;
        const auto& topic = config.subscribeTopics[i];
        if (buildFullTopicWithType(topic.topic, topic.autoPrefix,
                                   topic.topicType) == commandPath) {
            config.subscribeTopics.erase(config.subscribeTopics.begin() + i);
            if (static_cast<int>(i) < commandWinner) --commandWinner;
        }
    }
}

void MQTTClient::mqttCallback(char* topic, byte* payload, unsigned int length) {
    ets_printf("[MQTT] CALLBACK ENTERED topic=%s len=%u\n", topic, length);
    // DRAM 内存保护：低于阈值时丢弃入站消息，防止低内存时 JSON 解析/外设执行崩溃
    // 2026-08-05：改用 8BIT 口径（INTERNAL 虚高 ~28KB，旧守卫从不生效）
    uint32_t rxDramFree = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    if (rxDramFree < 6144U) {
        ets_printf("[MQTT] RX dropped: DRAM(8bit) too low (%lu < 6144) topic=%s\n",
                      (unsigned long)rxDramFree,
                      topic);
        return;
    }

    // 修复：原来逐字符 message += (char)payload[i]，每次迭代都可能触发 String 重分配
    // 改为一次性构造，O(1) 内存操作
    String message((const char*)payload, length);
    String topicStr(topic);

#if FASTBEE_ENABLE_REMOTE_RESOURCES
    // Handle dedicated state subscriptions before command/script decoding.
    bool reservedSubscription = false;
    for (const auto& st : config.subscribeTopics) {
        const String reserved = buildFullTopicWithType(st.topic, st.autoPrefix, st.topicType);
        if (FastBee::RemoteResourceModel::topicMatches(reserved.c_str(), topicStr.c_str())) {
            reservedSubscription = true;
            break;
        }
    }
    if (!reservedSubscription && FastBee::RemoteResourceManager::instance().consumeMqtt(topicStr,message)) return;
#endif

    // A disabled subscription may remain active briefly on an existing broker
    // connection after a runtime config reload. Drop it before topic-type
    // fallback can reinterpret it as a DATA_COMMAND.
    for (const auto& st : config.subscribeTopics) {
        if (st.enabled) continue;
        const String configuredTopic =
            buildFullTopicWithType(st.topic, st.autoPrefix, st.topicType);
        if (st.topic == topicStr || configuredTopic == topicStr) {
            if (st.topicType == MqttTopicType::OTA_BINARY) {
                LOG_WARNING("MQTT: OTA_BINARY message dropped because binary OTA is not supported");
            } else {
                LOG_WARNINGF("MQTT: Message dropped for disabled subscription topic=%s",
                             topicStr.c_str());
            }
            return;
        }
    }

    // 根据主题路径查找对应的主题类型。OTA_UPGRADE is a fixed FastBee
    // protocol route: a misconfigured row must never turn another topic into
    // an OTA command, or fall through to DATA_COMMAND processing.
    int8_t matchedSubIndex = -1;
    MqttTopicType tType = getTopicTypeByPath(topicStr, &matchedSubIndex);
    const String fastBeeOtaCommandTopic = buildFullTopicWithType(
        FASTBEE_OTA_COMMAND_TOPIC, false, MqttTopicType::OTA_UPGRADE);
    const bool matchedOtaSubscription =
        matchedSubIndex >= 0 &&
        static_cast<size_t>(matchedSubIndex) < config.subscribeTopics.size() &&
        config.subscribeTopics[static_cast<size_t>(matchedSubIndex)].topicType ==
            MqttTopicType::OTA_UPGRADE;
    if ((matchedOtaSubscription || tType == MqttTopicType::OTA_UPGRADE) &&
        topicStr != fastBeeOtaCommandTopic) {
        LOG_WARNINGF("MQTT: Rejected non-canonical FastBee OTA topic=%s expected=%s",
                     topicStr.c_str(), fastBeeOtaCommandTopic.c_str());
        return;
    }
    if (tType == MqttTopicType::OTA_BINARY) {
        LOG_WARNING("MQTT: OTA_BINARY message dropped because binary OTA is not supported");
        return;
    }
    const bool isOtaProtocol =
        tType == MqttTopicType::OTA_UPGRADE;
    const uint8_t modbusTransferType =
        tType == MqttTopicType::DATA_COMMAND ? currentModbusTransferType() : 0;
    const bool originalIsStructuredJson =
        tType == MqttTopicType::DATA_COMMAND &&
        mqttPayloadIsStructuredJson(payload, length);
    const bool isRawModbusCommand =
        tType == MqttTopicType::DATA_COMMAND &&
        modbusTransferType == 1 && !originalIsStructuredJson;

    // 无条件记录下行原始消息（诊断用）——无论 topic 是否匹配配置、tType 是什么都打印
    // 使用 Serial.printf 避免 LoggerSystem 文件 I/O 在 MQTT 回调中加剧栈压力
    {
        String truncatedRaw = message.length() > 120 ? message.substring(0, 120) + "..." : message;
        Serial.printf("[MQTT] RX ▼ topic=%s type=%d len=%u data=%s\n",
                      topic, (int)tType, length, truncatedRaw.c_str());
    }

    // FastBee OTA and raw Modbus RTU frames are fixed wire protocols. Passing
    // either through a user rule script can corrupt firmware metadata or bytes.
#if FASTBEE_ENABLE_RULE_SCRIPT
    if (!isOtaProtocol && !isRawModbusCommand) {
        String redirectTopic;
        message = RuleScriptManager::getInstance().applyReceiveTransform(
            0, message, topicStr, &redirectTopic);
        if (!redirectTopic.isEmpty()) {
            // 转换后重定向到目标主题，跳过正常处理流程
            {
                RecursiveMutexGuard lock(_publishMutex);
                mqttClient.publish(redirectTopic.c_str(), message.c_str(), false);
            }
            String truncated = message.length() > 120 ? message.substring(0, 120) + "..." : message;
            Serial.printf("[MQTT] Rule redirect: %s -> %s len=%u payload=%s\n",
                          topic, redirectTopic.c_str(), message.length(), truncated.c_str());
            return;
        }
    }
#endif

    char buf[96];
    snprintf(buf, sizeof(buf), "MQTT: Msg type=%d topic=%s len=%u",
             (int)tType, topic, length);
    LOG_DEBUG(buf);

    // 收到设备信息查询指令时，自动发布设备信息
    if (tType == MqttTopicType::DEVICE_INFO) {
        publishDeviceInfo();
    }

    // 收到实时监测指令时，解析 count/interval 并启动定时发布
    if (tType == MqttTopicType::REALTIME_MON) {
        JsonDocument monDoc;
        DeserializationError err = deserializeJson(monDoc, message);
        if (!err) {
            int count = monDoc["count"] | 0;
            unsigned long interval = monDoc["interval"] | 1000;
            // count=0 表示停止上报，立即终止已运行的实时监测，interval 参数被忽略
            if (count == 0) {
                if (monitorActive) {
                    monitorActive = false;
                    monitorRemaining = 0;
                    LOG_INFO("MQTT: Monitor STOPPED by count=0");
                } else {
                    LOG_DEBUG("MQTT: Monitor count=0 received (not running, ignored)");
                }
            } else if (count > 0 && interval >= 100) {
                monitorRemaining = count;
                monitorInterval = interval;
                monitorActive = true;
                lastMonitorTime = millis();
                char logBuf[80];
                snprintf(logBuf, sizeof(logBuf), "MQTT: Monitor started count=%d interval=%lums", count, interval);
                LOG_INFO(logBuf);
                // 立即发布第一次
                publishMonitorData();
                monitorRemaining--;
                if (monitorRemaining <= 0) {
                    monitorActive = false;
                    LOG_INFO("MQTT: Monitor completed (count=1, single-shot)");
                }
            } else {
                LOG_WARNING("MQTT: Invalid monitor params (count<0 or interval<100ms)");
            }
        }
    }

    // 收到数据下发指令时，入队延迟处理（避免在回调中阻塞 MQTT 连接）
    if (tType == MqttTopicType::DATA_COMMAND) {
        LOG_INFO("MQTT: Received DATA_COMMAND, queuing...");

        // 严格按 Modbus transferType 分流：
        //   transferType=0（JSON/结构体）：只接受 JSON 格式，非 JSON 直接告警丢弃
        //   transferType=1（透传HEX帧）  ：非 JSON 视为 HEX 帧（ASCII 或二进制）封装为 modbus_raw_send
        String enqueueMsg = message;
        bool shouldEnqueue = true;
        const size_t messageLength = message.length();
        const uint8_t* messageBytes =
            reinterpret_cast<const uint8_t*>(message.c_str());
        const bool messageIsStructuredJson =
            mqttPayloadIsStructuredJson(messageBytes, messageLength);

        if (!messageIsStructuredJson) {
            const bool eligibleRawFrame =
                modbusTransferType == 1 && isRawModbusCommand;
            if (!eligibleRawFrame) {
                LOG_WARNINGF(
                    "MQTT: transferType=%u requires a valid JSON object/array; payload dropped (len=%u)",
                    modbusTransferType, (unsigned)messageLength);
                shouldEnqueue = false;
            } else {
                uint8_t rawFrame[Protocols::MODBUS_BUFFER_SIZE];
                size_t rawFrameLength = 0;
                const MqttModbusPayloadEncoding encoding = decodeModbusRtuPayload(
                    messageBytes, messageLength, rawFrame, sizeof(rawFrame), rawFrameLength);
                if (encoding == MqttModbusPayloadEncoding::INVALID) {
                    LOG_WARNINGF(
                        "MQTT: Modbus raw command is neither a CRC-valid binary RTU frame nor CRC-valid HEX ASCII (len=%u), dropped",
                        (unsigned)messageLength);
                    shouldEnqueue = false;
                } else {
                    String hexStr;
                    hexStr.reserve(rawFrameLength * 2U);
                    for (size_t i = 0; i < rawFrameLength; ++i) {
                        char hexByte[3];
                        snprintf(hexByte, sizeof(hexByte), "%02X", rawFrame[i]);
                        hexStr += hexByte;
                    }
                    const char* encodingName =
                        encoding == MqttModbusPayloadEncoding::BINARY
                            ? "BIN->HEX"
                            : "HEX-ASCII";
                    Serial.printf("[MQTT] RX ▼ TRANSPARENT(%s) len=%u hex=%s\n",
                                  encodingName, (unsigned)rawFrameLength, hexStr.c_str());
                    enqueueMsg = "[{\"id\":\"modbus_raw_send\",\"value\":\"" + hexStr + "\"}]";
                }
            }
        }

        if (shouldEnqueue && _dataCommandQueue) {
            // Task 4.3: 队列内存限制检查
            size_t msgLen = enqueueMsg.length();
            if (_cmdQueueTotalBytes + msgLen > MQTT_CMD_QUEUE_MAX_BYTES) {
                LOG_WARNINGF("MQTT: DATA_COMMAND queue memory limit (%u > %u), dropping",
                             (unsigned)(_cmdQueueTotalBytes + msgLen),
                             (unsigned)MQTT_CMD_QUEUE_MAX_BYTES);
            } else {
                String* cmd = new (std::nothrow) String(enqueueMsg);
                if (cmd) {
                    if (xQueueSend(_dataCommandQueue, &cmd, 0) != pdTRUE) {
                        LOG_WARNING("MQTT: DATA_COMMAND queue full, dropping");
                        delete cmd;
                    } else {
                        _cmdQueueTotalBytes += msgLen;
                    }
                }
            }
        }
    }

    // 收到 NTP 时间同步响应时，计算校准时间并设置系统时钟
    if (tType == MqttTopicType::NTP_SYNC) {
        JsonDocument ntpDoc;
        DeserializationError err = deserializeJson(ntpDoc, message);
        if (!err && ntpDoc.containsKey("serverSendTime") && ntpDoc.containsKey("serverRecvTime")) {
            // 解析服务端时间戳（毫秒）
            double deviceSendTime = ntpDoc["deviceSendTime"].as<String>().toDouble();
            double serverRecvTime = ntpDoc["serverRecvTime"].as<String>().toDouble();
            double serverSendTime = ntpDoc["serverSendTime"].as<String>().toDouble();
            double deviceRecvTime = (double)millis();

            // 使用记录的本地发送时间替代服务端回传值（更精确）
            if (ntpDeviceSendTime > 0) {
                deviceSendTime = (double)ntpDeviceSendTime;
            }

            // NTP 算法：当前时间 = (serverRecvTime + serverSendTime + deviceRecvTime - deviceSendTime) / 2
            double currentTimeMs = (serverRecvTime + serverSendTime + deviceRecvTime - deviceSendTime) / 2.0;

            // 转换为 timeval 并设置系统时钟
            time_t epochSec = (time_t)(currentTimeMs / 1000.0);
            long microSec = (long)((currentTimeMs - (double)epochSec * 1000.0) * 1000.0);
            struct timeval tv;
            tv.tv_sec = epochSec;
            tv.tv_usec = microSec;
            settimeofday(&tv, nullptr);

            // 设置时区为 CST-8（中国标准时间）
            setenv("TZ", "CST-8", 1);
            tzset();

            // 重置发送时间
            ntpDeviceSendTime = 0;

            // 打印校准结果
            struct tm timeinfo;
            if (getLocalTime(&timeinfo, 0)) {
                char timeBuf[32];
                strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", &timeinfo);
                char logBuf[80];
                snprintf(logBuf, sizeof(logBuf), "MQTT: NTP synced, time: %s", timeBuf);
                LOG_INFO(logBuf);
                // 触发NTP同步完成系统事件
#if FASTBEE_ENABLE_PERIPH_EXEC
                PeriphExecManager::getInstance().triggerEvent(EventType::EVENT_NTP_SYNCED, timeBuf);
#endif
            }
        } else if (!err && !ntpDoc.containsKey("serverSendTime")) {
            // 收到的是 NTP 请求而非响应（仅含 deviceSendTime），忽略
            LOG_DEBUG("MQTT: NTP_SYNC request received (not response), ignoring");
        }
    }

    // DATA_COMMAND 已由 handleDataCommand 完整处理（含条件匹配+执行+响应），
    // 不再传递给 messageCallback 避免 handleMqttMessage 重复触发
    if (messageCallback && tType != MqttTopicType::DATA_COMMAND) {
        messageCallback(topicStr, message, tType);
    }
}

bool MQTTClient::reconnect() {
    const bool isMqttsReconnect = (config.scheme == "mqtts");
    _lastMqttsTlsMemoryFailure = false;
    // 运行时功能门控：mqtts 功能在功能配置页被禁用（用户关闭或开机预算不足）
    // 时拒绝建立 TLS 连接。明确提示策略：不静默降级明文，避免"以为加密实际
    // 明文"的安全语义风险；日志指引用户到功能配置页启用。
    if (isMqttsReconnect &&
        !FastBee::RuntimeFeatureManager::instance().shouldEnable("mqtts")) {
        lastErrorCode = -5;
        reconnectCount++;
        LOG_WARNING("[MQTT] MQTTS rejected: 'mqtts' feature disabled in feature settings (TLS needs ~42KB DRAM)");
        Serial.println("[MQTT] MQTTS rejected: enable 'mqtts' in Feature Settings first (TLS needs ~42KB DRAM)");
        _notifyStatusChange();
        return false;
    }
    if (isMqttsReconnect) {
        ensureMbedtlsAllocatorForMqtts();
    }
    // 显式关闭旧 TCP socket
    if (_externalClient) {
        _externalClient->stop();
    } else {
        wifiClient.stop();
        if (_wifiClientSecure) _wifiClientSecure->stop();
    }

    if (isMqttsReconnect && !_externalClient) {
        ensureTlsTransport();
        if (!_wifiClientSecure) {
            _lastMqttsTlsMemoryFailure = true;
            lastErrorCode = -4;
            reconnectCount++;
            LOG_WARNING("[MQTT] MQTTS reconnect aborted: WiFiClientSecure allocation failed");
            _notifyStatusChange();
            return false;
        }
    }

    LOG_INFO("MQTT: Connecting...");

    // 根据认证类型构建clientId和密码
    String connClientId = config.clientId;
    String connPassword;

    if (config.authType == MqttAuthType::ENCRYPTED &&
        !config.deviceNum.isEmpty() && !config.productId.isEmpty()) {
        // AES加密认证模式：自动构建clientId和加密密码
        connClientId = buildClientId();
        connPassword = buildEncryptedPassword();
        if (connPassword.isEmpty()) {
            LOG_WARNING("MQTT: AES password generation failed, falling back to simple auth");
            connClientId = "S&" + config.deviceNum + "&" + config.productId + "&" + config.userId;
            connPassword = buildSimplePassword();
        }
    } else {
        // 简单认证模式：使用配置的clientId，密码追加authCode
        connPassword = buildSimplePassword();
    }

    // 如果 clientId 为空，生成随机 ID
    if (connClientId.isEmpty()) {
        char id[20];
        snprintf(id, sizeof(id), "ESP32-%04X", (unsigned)esp_random() & 0xFFFF);
        connClientId = id;
    }

    char logBuf[128];
    snprintf(logBuf, sizeof(logBuf), "MQTT: Auth=%s ClientId=%s",
             config.authType == MqttAuthType::ENCRYPTED ? "AES" : "Simple",
             connClientId.c_str());
    LOG_INFO(logBuf);
    snprintf(logBuf, sizeof(logBuf), "MQTT: User=%s PwdLen=%d AuthCode=%s",
             config.username.c_str(), connPassword.length(),
             config.authCode.isEmpty() ? "(empty)" : config.authCode.c_str());
    LOG_INFO(logBuf);

    bool ok;
    ets_printf("[MQTT] Connecting to %s:%d clientId=%s\n",
                 config.server.c_str(), config.port, connClientId.c_str());

    // MQTTS: 连接前临时加大 socket timeout（TLS 握手在 C6 上需要 5~15s）
    // 连接成功后恢复 5s 短超时，避免 publish/write 对死 socket 长时间阻塞
    bool isMqtts = isMqttsReconnect;
    if (isMqtts) {
        mqttClient.setSocketTimeout(30);
    }
    uint32_t tlsBeforeDram = 0;
    uint32_t tlsBeforeLargest = 0;
    uint32_t tlsAfterDram = 0;
    uint32_t tlsAfterLargest = 0;
    if (isMqtts) {
        tlsBeforeDram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        tlsBeforeLargest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    }
    if (!config.willTopic.isEmpty()) {
        ok = mqttClient.connect(
            connClientId.c_str(),
            config.username.c_str(),
            connPassword.c_str(),
            config.willTopic.c_str(),
            config.willQos,
            config.willRetain,
            config.willPayload.c_str());
    } else {
        ok = mqttClient.connect(
            connClientId.c_str(),
            config.username.c_str(),
            connPassword.c_str());
    }
    if (isMqtts) {
        tlsAfterDram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        tlsAfterLargest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        ets_printf("[MQTT] TLS connect: DRAM before=%lu/%lu after=%lu/%lu ok=%u\n",
                   (unsigned long)tlsBeforeDram,
                   (unsigned long)tlsBeforeLargest,
                   (unsigned long)tlsAfterDram,
                   (unsigned long)tlsAfterLargest,
                   ok ? 1U : 0U);
    }

    if (ok) {
        // MQTTS 连接成功：从30s缩短到15s（仍足够处理TLS读，同时防止死socket阻塞）
        if (isMqtts) {
            mqttClient.setSocketTimeout(15);
        }
        isConnected = true;
        lastConnectedTime = millis();
        lastErrorCode = 0;
        ets_printf("[MQTT] *** MQTT CONNECTED! ***\n");
        LOG_INFO("MQTT: Connected");
        _notifyStatusChange();  // 通知状态变化：连接成功
        // 触发MQTT连接成功系统事件
#if FASTBEE_ENABLE_PERIPH_EXEC
        PeriphExecManager::getInstance().triggerEvent(EventType::EVENT_MQTT_CONNECTED, "");
#endif
        // 连接成功后订阅所有主题
        subscribeAll();
        mqttClient.loop();
        // 连接成功后发布一次设备信息
        publishDeviceInfo();
        mqttClient.loop();
        // 连接成功后发起 NTP 时间同步
        publishNtpSync();
    } else {
        // 连接失败：恢复短超时
        if (isMqtts) {
            mqttClient.setSocketTimeout(15);
        }
        // 连接失败时立即关闭 socket（仅默认 WiFiClient/WiFiClientSecure）
        if (!_externalClient) {
            wifiClient.stop();
            if (_wifiClientSecure) _wifiClientSecure->stop();
        }
        if (isMqtts && !_externalClient) {
            releaseTlsTransport();
        }
        lastErrorCode = mqttClient.state();
        if (isMqtts && !_externalClient &&
            (lastErrorCode == -2 || lastErrorCode == -4)) {
            // -2 (DNS/TCP) and -4 (timeout) are generic transport errors.
            // Mark them as memory-related only when the MQTTS admission floor
            // was actually missed before or after the handshake.  The former
            // READY/comfort thresholds are intentionally not used here: being
            // below the comfort zone triggers reclaim, but is not itself a
            // proof that the connection failed for lack of memory.
            const bool beforeMemoryPressure =
                !FastBee::MemoryBudget::canAttemptMqtts(
                    tlsBeforeDram, tlsBeforeLargest, mqttsTlsUsesPsram());
            const bool afterMemoryPressure =
                !FastBee::MemoryBudget::canAttemptMqtts(
                    tlsAfterDram, tlsAfterLargest, mqttsTlsUsesPsram());
            _lastMqttsTlsMemoryFailure = beforeMemoryPressure || afterMemoryPressure;
        }
        reconnectCount++;
        ets_printf("[MQTT] Connect FAILED rc=%d (cnt=%lu)\n",
                     lastErrorCode, (unsigned long)reconnectCount);
        char buf2[48];
        snprintf(buf2, sizeof(buf2), "MQTT: Connect failed rc=%d", lastErrorCode);
        LOG_WARNING(buf2);
        _notifyStatusChange();  // 通知状态变化：连接失败
        // 触发MQTT连接失败系统事件
#if FASTBEE_ENABLE_PERIPH_EXEC
        PeriphExecManager::getInstance().triggerEvent(EventType::EVENT_MQTT_CONN_FAILED, String(lastErrorCode));
#endif
    }
    return ok;
}


// ============ MQTTS TLS 内存管理 ============

void MQTTClient::ensureTlsTransport() {
    ensureMbedtlsAllocatorForMqtts();
    if (!_wifiClientSecure) {
        _wifiClientSecure = new (std::nothrow) WiFiClientSecure();
        if (!_wifiClientSecure) {
            LOG_WARNING("[MQTT] WiFiClientSecure allocation failed");
            return;
        }
        const size_t caBundleSize = static_cast<size_t>(x509_crt_bundle_end - x509_crt_bundle_start);
        if (caBundleSize == 0) {
            LOG_ERROR("[MQTT] Built-in CA certificate bundle is unavailable");
            delete _wifiClientSecure;
            _wifiClientSecure = nullptr;
            return;
        }
        _wifiClientSecure->setCACertBundle(x509_crt_bundle_start, caBundleSize);
        _wifiClientSecure->setHandshakeTimeout(30);
        if (!_externalClient) {
            mqttClient.setClient(*_wifiClientSecure);
        }
        ets_printf("[MQTT] WiFiClientSecure created (heap=%lu)\n",
                   (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    }
}

void MQTTClient::releaseTlsTransport() {
    if (_wifiClientSecure) {
        _wifiClientSecure->stop();
        delete _wifiClientSecure;
        _wifiClientSecure = nullptr;
        if (!_externalClient) {
            mqttClient.setClient(wifiClient);
        }
        ets_printf("[MQTT] WiFiClientSecure destroyed, TLS memory freed (dram=%lu)\n",
                   (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    }
}

bool MQTTClient::reclaimDramForMqtts(bool pauseWeb) {
    // Step 1: 释放 TLS 上下文
    releaseTlsTransport();
    delay(20);
    heap_caps_check_integrity_all(true);

    // Step 2: Keep Web online by default; Ethernet MQTTS may request a short
    // deep pause because W5500 + Web + TLS leaves too little contiguous DRAM.
    FastBeeFramework* fw = FastBeeFramework::getInstance();
    WebConfigManager* wcm = fw ? fw->getWebConfigManager() : nullptr;
    FBNetworkManager* netMgr = fw ? static_cast<FBNetworkManager*>(fw->getNetworkManager()) : nullptr;
    _webServerPaused = false;
    _mdnsPausedForMqtts = false;
    if (pauseWeb && wcm) {
        _webServerPaused = wcm->pauseForMqttsHandshake(
            FastBee::MemoryBudget::MQTTS_WEB_PAUSE_HOLD_MS, _mqttsForceWebPause);
        if (_webServerPaused) {
            delay(100);
        }
    }
    _mqttsForceWebPause = false;  // 一次性标志：用完即清，避免影响后续常规回收

    // Step 3: 关闭 SSE 客户端连接
    size_t closed = 0;
    SSERouteHandler* sse = wcm ? wcm->getSseRouteHandler() : nullptr;
    if (sse && sse->clientCount() > 0) {
        closed = sse->closeAllClients();
        if (closed > 0) delay(100);
    }

    if (_webServerPaused || closed > 0) {
        delay(FastBee::MemoryBudget::MQTTS_WEB_RESUME_DELAY_MS);
    }

    // Step 4: stop mDNS during a deep Web pause. mDNS is small, but it owns UDP
    // buffers and task state in the same scarce internal DRAM pool used by TLS.
    if (_webServerPaused && netMgr && netMgr->getConfig().enableMDNS) {
        netMgr->stopMDNS();
        _mdnsPausedForMqtts = true;
        delay(50);
    }

    // 强制 GC
    heap_caps_check_integrity_all(true);

    uint32_t dramFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    uint32_t dramLargest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    ets_printf("[MQTT] reclaimDram: web=%s mdns=%s sse=%u dram=%lu largest=%lu\n",
               _webServerPaused ? "paused" : "kept",
               _mdnsPausedForMqtts ? "paused" : "kept",
               (unsigned)closed, (unsigned long)dramFree, (unsigned long)dramLargest);

    // 预编译 mbedtls 需要 ~42KB DRAM（16KB in + 16KB out + ~10KB context）
    // 无 PSRAM 时走 MemoryBudget::canAttemptMqtts 的严格门槛重载
    return FastBee::MemoryBudget::canAttemptMqtts(dramFree, dramLargest, mqttsTlsUsesPsram());
}

void MQTTClient::resumeWebServices() {
    FastBeeFramework* fw = FastBeeFramework::getInstance();
    if (!fw) return;
    if (_webServerPaused) {
        WebConfigManager* wcm = fw->getWebConfigManager();
        if (wcm) {
            wcm->resumeFromMqttsHandshake();
            _webServerPaused = false;
        }
    }
    if (_mdnsPausedForMqtts) {
        FBNetworkManager* netMgr = static_cast<FBNetworkManager*>(fw->getNetworkManager());
        if (netMgr && netMgr->getConfig().enableMDNS) {
            netMgr->startMDNS();
        }
        _mdnsPausedForMqtts = false;
    }
}

// ============ 后台重连任务 ============

void MQTTClient::reconnectTaskEntry(void* param) {
    MQTTClient* client = (MQTTClient*)param;
    // 启动期延迟：让 Web 路由注册完成即可。
    LOG_INFOF("[MQTT] Reconnect task armed, holding %lums for web boot",
              (unsigned long)client->_taskStartupDelayMs);
    vTaskDelay(pdMS_TO_TICKS(client->_taskStartupDelayMs));
    LOG_INFO("[MQTT] Reconnect task active");

    if (client->_reconnectPending && !client->_reconnectRunning) {
        client->doReconnect();
    } else {
        // doReconnect 被跳过（如状态被并发重置）：主动清除 pending，
        // 避免 _reconnectPending 卡死为 true 导致 handle() 永不再调度。
        client->_reconnectPending = false;
    }

    // 加固：无论 doReconnect 是否执行/是否正常返回，任务退出前必须清除运行标志，
    // 防止异常路径下 _reconnectRunning 残留为 true 而永久阻塞重连调度。
    client->_reconnectRunning = false;
    client->_reconnectTaskHandle = nullptr;
    LOG_INFO("[MQTT] Reconnect task exiting");
    vTaskDelete(nullptr);
}

void MQTTClient::doReconnect() {
    _reconnectPending = false;
    _reconnectRunning = true;

    // The flag is specific to the current MQTTS attempt.  Do not carry a
    // previous TLS-memory diagnosis into a plain MQTT or feature-disabled
    // status response after the user changes the protocol.
    if (config.scheme != "mqtts") {
        _lastMqttsTlsMemoryFailure = false;
    }

    // 运行时功能门控（早于一切内存腾挪）：mqtts 功能被禁用时直接拒绝，
    // 避免为注定被拒的 TLS 握手执行 Web/mDNS 暂停等重操作。
    // reconnect() 入口有同款门控兜底同步 connect() 路径。
    if (config.scheme == "mqtts" &&
        !FastBee::RuntimeFeatureManager::instance().shouldEnable("mqtts")) {
        ets_printf("[MQTT] Reconnect skipped: mqtts feature disabled\n");
        LOG_WARNING("[MQTT] MQTTS reconnect skipped: 'mqtts' feature disabled in feature settings");
        _lastMqttsTlsMemoryFailure = false;
        lastErrorCode = -5;
        _notifyStatusChange();
        _reconnectRunning = false;
        return;
    }

    // 重连次数保护：连续失败超过 20 次后拉长间隔至 5 分钟，降低内存压力
    constexpr uint32_t MAX_FAST_RETRIES = 20;
    constexpr uint32_t SLOW_RETRY_INTERVAL = 300000;  // 5分钟
    // DNS/TCP 连续失败阈值：达到此次数后切换慢模式（5 分钟）
    // 阈值=3 在保护内存（避免反复 DNS+stop 碎片化）与容忍临时网络抖动之间取平衡
    // 阈值=1 过于激进：MQTT broker 偶发一次超时就等 5 分钟，用户感知为"永远连接中"
    constexpr uint8_t  DNS_FAIL_THRESHOLD = 3;
    
    bool slowMode = (reconnectCount > MAX_FAST_RETRIES) ||
                    (consecutiveTimeouts >= DNS_FAIL_THRESHOLD);
    if (slowMode) {
        reconnectInterval = SLOW_RETRY_INTERVAL;
        if (reconnectCount > MAX_FAST_RETRIES) {
            char buf[96];
            snprintf(buf, sizeof(buf), "[MQTT] Reconnect count=%lu exceeds %lu, slow mode (%lus)",
                     (unsigned long)reconnectCount, (unsigned long)MAX_FAST_RETRIES,
                     (unsigned long)(SLOW_RETRY_INTERVAL / 1000));
            LOG_WARNING(buf);
        }
    }

    // Plain-MQTT reconnect DRAM gate (aligned with web-reserve policy).
    // MQTT connect consumes ~19KB lwIP TCP buffers; during the 2026-08
    // reconnect storm largest dropped to 6KB and HTTP chunked responses broke
    // at 2872 bytes. Defer reconnect when DRAM is low to keep Web usable.
    {
        // 2026-08-05 fix: 增加 8BIT 口径门控。INTERNAL 含 IRAM/RTC 不可 malloc
        // 内存，实测虚高 ~28KB；exec 规则窗口内 INTERNAL 仍有 42KB 但 8BIT
        // 只剩 ~6KB，重连（lwIP 套接字缓冲 ~15KB）直接把 8BIT 打穿至 <1KB
        // 触发强制断开，形成 连接→挤出→断开→重连 死循环。
        // INTERNAL 含 IRAM 虚高 ~28KB，系统稳态 INTERNAL 仅 42-44KB；
        // 旧门限 36864 在现固件下永不满足导致 MQTT 永久离线，降至 28672。
        constexpr uint32_t MQTT_RECONNECT_MIN_FREE = 28672U;    // 28KB (INTERNAL 口径)
        constexpr uint32_t MQTT_RECONNECT_MIN_LARGEST = 12288U; // 12KB
        // 14KB：free8 断开稳态仅 15.3-16.5KB，20KB/18KB 门限实测永不满足
        // 导致 MQTT 永久离线；14KB 门限允许从稳态起步连接。连接耗
        // 12-15KB 会把 free8 压到 1-4KB，但前台让位策略（前台活跃时
        // 12KB 阈值主动断开）保证浏览器访问时 MQTT 让出内存。
        constexpr uint32_t MQTT_RECONNECT_MIN_8BIT = 14336U;
        // 采样窗口：free8 在忙/闲时段波动 ±6KB（exec 规则、WiFi TX），
        // 单次采样易撞上低谷导致重连永远饿死；每 250ms 采一次最多 4s，
        // 命中闲时窗口立即连接（重连任务内 delay 不阻塞 loopTask）。
        uint32_t gateFree = 0, gateLargest = 0, gate8Free = 0;
        bool gatePass = false;
        for (int gi = 0; gi < 16 && !gatePass; gi++) {
            // 等待期间出现 Web 前台请求则立即放弃：连接握手/突发 publish
            // 会与页面加载竞争 WiFi TX 通道（实测内存充足时也会挂起）
            {
                FastBeeFramework* fwGateW = FastBeeFramework::getInstance();
                WebConfigManager* webGateW = fwGateW ? fwGateW->getWebConfigManager() : nullptr;
                if (webGateW && webGateW->isForegroundRequestActive()) {
                    ets_printf("[MQTT] Reconnect aborted: web foreground active during gate wait\n");
                    _reconnectRunning = false;
                    return;
                }
            }
            gateFree    = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
            gateLargest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
            gate8Free   = heap_caps_get_free_size(MALLOC_CAP_8BIT);
            if (gateFree >= MQTT_RECONNECT_MIN_FREE &&
                gateLargest >= MQTT_RECONNECT_MIN_LARGEST &&
                gate8Free >= MQTT_RECONNECT_MIN_8BIT) {
                gatePass = true;
            } else {
                delay(250);
            }
        }
        if (!gatePass) {
            ets_printf("[MQTT] Reconnect deferred: DRAM low (free=%lu largest=%lu free8=%lu need=%lu/%lu/%lu)\n",
                       (unsigned long)gateFree, (unsigned long)gateLargest, (unsigned long)gate8Free,
                       (unsigned long)MQTT_RECONNECT_MIN_FREE, (unsigned long)MQTT_RECONNECT_MIN_LARGEST,
                       (unsigned long)MQTT_RECONNECT_MIN_8BIT);
            _reconnectRunning = false;
            return;
        }
    }

    // ===== MQTTS 专用内存保护 =====
    // ESP32-S3/C6 等芯片 MQTTS (SSL/TLS) 连接内存需求分析：
    //   - 内置 CA bundle 启用服务端身份校验（需预留证书解析内存）
    //   - SSL/TLS 握手缓冲区（mbedtls record layer）：~16KB
    //   - mbedtls 上下文（ssl_context + transform）：~8KB
    //   - MQTT 基础开销（PubSubClient + String 等）：~4KB
    //   - 握手前使用 MemoryBudget 的 TLS ready 门槛保留证书解析空间
    // 
    // 关键：WiFi/lwIP/mbedtls 均使用内部 DRAM（MALLOC_CAP_INTERNAL）
    //   - PSRAM 不可用于 SSL/TCP socket 内存（DMA 不可寻址）
    //   - 因此必须检测 DRAM 内部内存而非 MALLOC_CAP_DEFAULT
    //   - MALLOC_CAP_DEFAULT 在有 PSRAM 的板子上会包含 PSRAM（largest 显示 8MB+）
    //     导致检测通过但实际 DRAM 连续块不足，SSL 握手时再失败
    // 
    // 保护策略：
    // 1. 用 MALLOC_CAP_INTERNAL 检测真实 DRAM 最大连续块（排除 PSRAM）
    // 2. 使用分芯片 MQTTS 门槛，为 CA 链校验保留连续 DRAM
    // 3. 失败后延迟等待 FreeRTOS 回收内存再重试
    // 4. 连续失败后进入慢模式，避免反复尝试加剧碎片
    
    bool isMqtts = (config.scheme == "mqtts");
    bool usesInternalTls = isMqtts && !_externalClient;
    bool needsMqttsDramBudget = isMqtts;
    NetworkType activeNetworkType = getActiveMqttNetworkType();
    bool activeNetworkIsWifi = (activeNetworkType == NetworkType::NET_WIFI);
    if (isMqtts) {
        ensureMbedtlsAllocatorForMqtts();
    }
    auto enterMqttsMemoryBackoff = [&](const char* reason) {
        if (!isMqtts) {
            return;
        }
        // 保留给状态 API：下一次失败是内存准入拒绝，而不是 DNS/TCP 失败。
        _lastMqttsTlsMemoryFailure = true;
        consecutiveTimeouts = 0;
        reconnectInterval = SLOW_RETRY_INTERVAL;
        lastReconnectAttempt = millis();
        _mqttsMemoryBackoffActive = true;
        ets_printf("[MQTT] MQTTS memory backoff reason=%s next=%lus\n",
                   reason ? reason : "unknown",
                   (unsigned long)(SLOW_RETRY_INTERVAL / 1000));
    };
    auto enterMqttsMemoryRecoveryRetry = [&](const char* reason) {
        if (!isMqtts) {
            return;
        }
        consecutiveTimeouts = 0;
        reconnectInterval = FastBee::MemoryBudget::MQTTS_MEMORY_RECOVERY_RETRY_MS;
        lastReconnectAttempt = millis();
        _mqttsMemoryBackoffActive = true;
        ets_printf("[MQTT] MQTTS memory recovery retry reason=%s next=%lus\n",
                   reason ? reason : "unknown",
                   (unsigned long)(FastBee::MemoryBudget::MQTTS_MEMORY_RECOVERY_RETRY_MS / 1000));
    };
    if (usesInternalTls) {
        releaseTlsTransport();
        delay(20);
        heap_caps_check_integrity_all(true);
    }
    // MQTTS 阈值：预编译 libmbedtls.a 使用 CONFIG_MBEDTLS_SSL_MAX_CONTENT_LEN=16384
    //   需要 ~42KB DRAM（16KB in + 16KB out + ~10KB context/transform）
    //   minHeap:         DRAM 总空闲 >= 42KB
    //   minLargestBlock: DRAM 最大连续块 >= MemoryBudget::MQTTS_MIN_LARGEST_BLOCK；
    //                    large mbedTLS/RSA buffers prefer PSRAM, so internal DRAM keeps socket/control headroom.
    // MQTT 阈值：任意 8KB 总空闲 + 2KB 连续即可
    uint32_t minHeap = needsMqttsDramBudget
        ? (mqttsTlsUsesPsram()
               ? FastBee::MemoryBudget::MQTTS_MIN_DRAM_FREE
               : FastBee::MemoryBudget::MQTTS_NO_PSRAM_MIN_DRAM_FREE)
        : FastBee::MemoryBudget::MQTT_MIN_HEAP;
    uint32_t minLargestBlock = needsMqttsDramBudget
        ? (mqttsTlsUsesPsram()
               ? FastBee::MemoryBudget::MQTTS_MIN_LARGEST_BLOCK
               : FastBee::MemoryBudget::MQTTS_NO_PSRAM_MIN_LARGEST_BLOCK)
        : FastBee::MemoryBudget::MQTT_MIN_LARGEST_BLOCK;

    // 获取 DRAM 内部内存统计（排除 PSRAM）
    uint32_t reconnectFreeHeap     = needsMqttsDramBudget
        ? heap_caps_get_free_size(MALLOC_CAP_INTERNAL)
        : (uint32_t)ESP.getFreeHeap();
    uint32_t reconnectLargestBlock = needsMqttsDramBudget
        ? heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)
        : heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);

    // 补充输出 DRAM vs Total 对比，方便诊断 PSRAM 干扰问题
    if (needsMqttsDramBudget) {
        uint32_t totalFree    = (uint32_t)ESP.getFreeHeap();
        uint32_t totalLargest = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
        ets_printf("[MQTT] doReconnect: DRAM free=%lu largest=%lu | total free=%lu largest=%lu\n",
                   (unsigned long)reconnectFreeHeap, (unsigned long)reconnectLargestBlock,
                   (unsigned long)totalFree, (unsigned long)totalLargest);
    }

    bool mqttMemoryRecovered = false;
    if (reconnectFreeHeap < minHeap || reconnectLargestBlock < minLargestBlock) {
        ets_printf("[MQTT] doReconnect: heap too low (heap=%lu largest=%lu scheme=%s min=%lu/%lu), skipping\n",
                     (unsigned long)reconnectFreeHeap, (unsigned long)reconnectLargestBlock,
                     config.scheme.c_str(), (unsigned long)minHeap, (unsigned long)minLargestBlock);
        LOG_WARNINGF("[MQTT] Memory too low for %s reconnect, skipping (dram=%lu largest=%lu need=%lu/%lu)",
                     isMqtts ? "MQTTS" : "MQTT",
                     (unsigned long)reconnectFreeHeap,
                     (unsigned long)reconnectLargestBlock,
                     (unsigned long)minHeap,
                     (unsigned long)minLargestBlock);
        // MQTTS: 确保 Web 服务恢复（可能之前在 reclaim 中停止了）
        bool mqttsMemoryRecoverable = false;
        if (usesInternalTls) {
            MqttsWebPauseDecision recoveryPause = decideWebPauseForMqtts(
                reconnectFreeHeap,
                reconnectLargestBlock);
            // DEFER：前台活跃时维持“不暂停 Web”的回收（本路径只是探测可恢复性，
            // 不直接进入握手），但累积推迟计数，为后续 FORCE_PAUSE 铺垫
            bool pauseWebForRecovery =
                (recoveryPause == MqttsWebPauseDecision::PAUSE ||
                 recoveryPause == MqttsWebPauseDecision::FORCE_PAUSE);
            _mqttsForceWebPause = (recoveryPause == MqttsWebPauseDecision::FORCE_PAUSE);
            bool reclaimed = reclaimDramForMqtts(pauseWebForRecovery);
            reconnectFreeHeap     = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
            reconnectLargestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
            mqttMemoryRecovered =
                (reconnectFreeHeap >= minHeap && reconnectLargestBlock >= minLargestBlock);
            mqttsMemoryRecoverable = FastBee::MemoryBudget::canRetryMqttsMemoryRecovery(
                reconnectFreeHeap,
                reconnectLargestBlock,
                mqttsTlsUsesPsram());
            ets_printf("[MQTT] doReconnect: low-memory reclaim result reclaimed=%u dram=%lu largest=%lu ready=%u recoverable=%u\n",
                       reclaimed ? 1U : 0U,
                       (unsigned long)reconnectFreeHeap,
                       (unsigned long)reconnectLargestBlock,
                       mqttMemoryRecovered ? 1U : 0U,
                       mqttsMemoryRecoverable ? 1U : 0U);
            if (!mqttMemoryRecovered) {
                releaseTlsTransport();
                resumeWebServices();
            }
        }
        if (!mqttMemoryRecovered) {
            if (mqttsMemoryRecoverable) {
                enterMqttsMemoryRecoveryRetry("low_dram_recovering");
            } else {
                enterMqttsMemoryBackoff("low_dram");
            }
            _reconnectRunning = false;
            return;
        }
    }
    
    // MQTTS 额外保护：短暂延迟让 FreeRTOS idle 任务回收被 vTaskDelete 标记的内存
    if (usesInternalTls) {
        delay(50);  // idle 任务回收延迟释放内存，通常 10-50ms
        reconnectFreeHeap     = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        reconnectLargestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        
        // 回收舒适区：低于 RSA 握手目标时先尝试暂停 Web/SSE 释放 DRAM，
        // 但满足 MemoryBudget 最低线时不再被门限直接挡住，避免 MQTTS 无限跳过。
        uint32_t postReleaseDram = reconnectFreeHeap;
        uint32_t postReleaseLargest = reconnectLargestBlock;
        if (FastBee::MemoryBudget::shouldReclaimBeforeMqtts(postReleaseDram, postReleaseLargest)) {
            ets_printf("[MQTT] doReconnect: DRAM marginal (dram=%lu largest=%lu), trying reclaim\n",
                       (unsigned long)postReleaseDram, (unsigned long)postReleaseLargest);
            MqttsWebPauseDecision handshakePause = decideWebPauseForMqtts(
                postReleaseDram,
                postReleaseLargest);
            if (handshakePause == MqttsWebPauseDecision::DEFER) {
                // 历史 panic 现场：前台请求活跃时硬闯 TLS 握手会把 DRAM 打穿
                // （largest→308B → lwIP nothrow new 失败 → tcp_receive assert）。
                // 改为推迟握手，短退避后重查；连续推迟超限由 FORCE_PAUSE 兜底
                ets_printf("[MQTT] doReconnect: handshake deferred, foreground Web busy\n");
                enterMqttsMemoryRecoveryRetry("web_foreground_busy");
                releaseTlsTransport();
                _reconnectRunning = false;
                return;
            }
            bool pauseWebForHandshake =
                (handshakePause == MqttsWebPauseDecision::PAUSE ||
                 handshakePause == MqttsWebPauseDecision::FORCE_PAUSE);
            _mqttsForceWebPause = (handshakePause == MqttsWebPauseDecision::FORCE_PAUSE);
            if (reclaimDramForMqtts(pauseWebForHandshake)) {
                ets_printf("[MQTT] doReconnect: reclaimDramForMqtts SUCCESS\n");
            } else {
                ets_printf("[MQTT] doReconnect: reclaimDramForMqtts FAILED, skipping\n");
                enterMqttsMemoryBackoff("reclaim_failed");
                releaseTlsTransport();
                resumeWebServices();
                _reconnectRunning = false;
                return;
            }
        }
        ensureTlsTransport();
        if (!_wifiClientSecure) {
            _lastMqttsTlsMemoryFailure = true;
            LOG_WARNING("[MQTT] MQTTS reconnect skipped: TLS transport unavailable after reclaim");
            enterMqttsMemoryBackoff("tls_transport_unavailable");
            resumeWebServices();
            _reconnectRunning = false;
            return;
        }
        reconnectFreeHeap     = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        reconnectLargestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    }

    ets_printf("[MQTT] doReconnect: starting (heap=%lu largest=%lu scheme=%s)\n",
                 (unsigned long)reconnectFreeHeap, (unsigned long)reconnectLargestBlock,
                 isMqtts ? "mqtts" : "mqtt");
    LOG_INFOF("[MQTT] Background %s reconnect starting...", isMqtts ? "MQTTS" : "MQTT");

    bool ok = reconnect();

    if (ok) {
        reconnectInterval = 5000;  // 连接成功，重置间隔
        consecutiveTimeouts = 0;
        _lastMqttsTlsMemoryFailure = false;
        _mqttsMemoryBackoffActive = false;
        g_mqttsForegroundDeferCount = 0;  // 前台推迟计数随连接成功清零
        LOG_INFO("[MQTT] Background reconnect successful");
        // MQTTS TLS 连接成功：重启之前临时停止的 Web 服务器和 mDNS
        if (isMqtts) {
            resumeWebServices();
        }
    } else {
        uint32_t failDramFree = needsMqttsDramBudget ? heap_caps_get_free_size(MALLOC_CAP_INTERNAL) : 0;
        uint32_t failDramLargest = needsMqttsDramBudget ? heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) : 0;
        // A PubSubClient -4 is a generic connection timeout.  It must not be
        // presented as a TLS-memory failure unless the allocator/transport
        // path explicitly reported it or the post-failure DRAM is below the
        // MQTTS admission floor.  Otherwise a normal DNS/TCP/TLS timeout would
        // incorrectly tell the user to switch from mqtts to mqtt.
        const bool postHandshakeMemoryPressure =
            isMqtts && lastErrorCode == -4 &&
            !FastBee::MemoryBudget::canAttemptMqtts(
                failDramFree, failDramLargest, mqttsTlsUsesPsram());
        bool mqttsSslMemoryFailure =
            isMqtts &&
            (_lastMqttsTlsMemoryFailure ||
             postHandshakeMemoryPressure ||
             !FastBee::MemoryBudget::canAttemptMqtts(failDramFree, failDramLargest,
                                                     mqttsTlsUsesPsram()));

        // 检测连续 TCP/DNS 连接失败（rc=-2: MQTT_CONNECT_FAILED）
        // DNS 解析失败时 WiFiClient::connect() 返回 0，PubSubClient 报告 rc=-2
        // 连续 3 次说明网络层不通（如 WiFi 僵尸状态），立即进入慢模式
        if (mqttsSslMemoryFailure) {
            // MQTTS SSL 内存失败：释放 TLS 传输层，进入慢速重试
            enterMqttsMemoryBackoff("tls_ssl_memory_failure");
            releaseTlsTransport();
            resumeWebServices();
#if !defined(BOARD_HAS_PSRAM)
            // 无 PSRAM 设备：明确提示用户切换到 mqtt
            ets_printf("[MQTT] *** MQTTS connection FAILED: insufficient memory (no PSRAM). dram=%lu largest=%lu\n",
                       (unsigned long)failDramFree, (unsigned long)failDramLargest);
            ets_printf("[MQTT] *** PLEASE switch to mqtt:// in the web UI configuration.\n");
            LOG_ERROR("[MQTT] MQTTS failed: no PSRAM, insufficient DRAM for TLS. Please switch to mqtt://");
#else
            LOG_WARNINGF("[MQTT] MQTTS SSL memory failure, slow retry (dram=%lu largest=%lu next=%lus)",
                         (unsigned long)failDramFree,
                         (unsigned long)failDramLargest,
                         (unsigned long)(SLOW_RETRY_INTERVAL / 1000));
#endif
            ets_printf("[MQTT] MQTTS SSL memory failure dram=%lu largest=%lu, next retry %lus\n",
                       (unsigned long)failDramFree,
                       (unsigned long)failDramLargest,
                       (unsigned long)(SLOW_RETRY_INTERVAL / 1000));
        } else if (lastErrorCode == -2) {
            // rc=-2: TCP/DNS/TLS 连接失败（包括证书解析错误如 mbedtls -15104）
            // 无论失败原因，MQTTS 都必须恢复 Web 服务（之前可能被 reclaim 暂停）
            if (isMqtts) {
                releaseTlsTransport();
                resumeWebServices();
            }
            _mqttsMemoryBackoffActive = false;
            consecutiveTimeouts++;
            if (consecutiveTimeouts >= DNS_FAIL_THRESHOLD) {
                reconnectInterval = SLOW_RETRY_INTERVAL;
                LOG_WARNINGF("[MQTT] connect failure (rc=-2 DNS/network), slow mode (next retry %lus)",
                             (unsigned long)(SLOW_RETRY_INTERVAL / 1000));
            }
            // 加固：连续 6 次 connect 失败 → WiFi STA 可能处于僵尸状态（RSSI 正常但 socket 不通）
            // 主动重走 disconnect+reconnect 刷新 lwip TCP/IP 栈，避免累计内存劣化
            // 仅在第 6 、 12 、 18… 次触发（避免频繁刷 WiFi）
            constexpr uint8_t WIFI_RESET_THRESHOLD = 6;
            if (!_externalClient &&
                activeNetworkIsWifi &&
                consecutiveTimeouts >= WIFI_RESET_THRESHOLD &&
                (consecutiveTimeouts % WIFI_RESET_THRESHOLD) == 0) {
                wl_status_t st = WiFi.status();
                LOG_WARNINGF("[MQTT] %u consecutive failures, soft-resetting WiFi STA (status=%d, rssi=%d)",
                             (unsigned)consecutiveTimeouts, (int)st, (int)WiFi.RSSI());
                // 关闭 STA 但保留配置，然后重新 reconnect（不会丢弃 SSID/密码）
                WiFi.disconnect(false, false);
                delay(100);
                WiFi.reconnect();
            }
        } else {
            // 其他错误码（如 rc=4 认证失败等）
            if (isMqtts) {
                releaseTlsTransport();
                resumeWebServices();
            }
            _mqttsMemoryBackoffActive = false;
            consecutiveTimeouts = 0;
        }

        bool keepSlowRetry = slowMode || mqttsSslMemoryFailure;
        if (!keepSlowRetry && consecutiveTimeouts < DNS_FAIL_THRESHOLD) {
            // 快速重试阶段：指数退避 5s -> 10s -> 20s -> 30s（上限）
            reconnectInterval = min(reconnectInterval * 2, (uint32_t)30000);
        }
        // slowMode 时保持 SLOW_RETRY_INTERVAL，不再被覆盖
        char buf[80];
        snprintf(buf, sizeof(buf), "[MQTT] Background reconnect failed, next retry in %lus",
                 reconnectInterval / 1000);
        LOG_INFO(buf);
    }

    _reconnectRunning = false;
    _reconnectScheduledMs = 0;  // 本次重连已结束，清除看门狗时间戳
}

// ============ FastBee认证方法 ============

String MQTTClient::buildClientId() {
    // 客户端ID格式: 认证类型(E/S) & 设备编号 & 产品ID & 用户ID
    String prefix = (config.authType == MqttAuthType::ENCRYPTED) ? "E" : "S";
    return prefix + "&" + config.deviceNum + "&" + config.productId + "&" + config.userId;
}

String MQTTClient::buildSimplePassword() {
    // 简单认证密码格式: mqtt密码 或 mqtt密码&授权码
    if (config.authCode.isEmpty()) {
        return config.password;
    }
    return config.password + "&" + config.authCode;
}

String MQTTClient::buildEncryptedPassword() {
    // 加密认证密码生成流程:
    // 1. 获取NTP时间
    // 2. 计算过期时间 = 当前时间 + 1小时
    // 3. 构造密码明文: mqtt密码&过期时间 或 mqtt密码&过期时间&授权码
    // 4. AES-CBC-128加密, Base64编码

    if (config.mqttSecret.isEmpty() || config.mqttSecret.length() < 16) {
        LOG_WARNING("MQTT: mqttSecret too short for AES-128 (need 16 bytes)");
        return "";
    }

    unsigned long deviceSendTime = 0;
    String ntpJson = getNtpTime(deviceSendTime);
    if (ntpJson.isEmpty()) {
        LOG_WARNING("MQTT: Failed to get NTP time");
        return "";
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, ntpJson);
    if (err) {
        LOG_WARNING("MQTT: NTP response parse failed");
        return "";
    }

    double serverSendTime = jsonVariantToDoubleSafe(doc["serverSendTime"]);
    double serverRecvTime = jsonVariantToDoubleSafe(doc["serverRecvTime"]);
    
    // 兼容 FastBee 平台返回格式: {"data": {"serverTime": xxx}} 或 {"code":200,"data":{"serverTime":xxx}}
    if (serverSendTime <= 0.0 || serverRecvTime <= 0.0) {
        if (doc["data"].is<JsonObject>()) {
            double serverTime = jsonVariantToDoubleSafe(doc["data"]["serverTime"]);
            if (serverTime > 0.0) {
                // 如果是秒级时间戳（小于10000000000），转换为毫秒
                if (serverTime < 10000000000.0) {
                    serverTime *= 1000.0;
                }
                serverSendTime = serverTime;
                serverRecvTime = serverTime;
            }
        }
    }
    
    // 验证时间戳有效性（毫秒级时间戳应大于 1000000000000，即 2001年以后）
    if (serverSendTime < 1000000000000.0 || serverRecvTime < 1000000000000.0) {
        char buf[160];
        snprintf(buf, sizeof(buf), "MQTT: Invalid NTP timestamps: sendTime=%.0f, recvTime=%.0f", serverSendTime, serverRecvTime);
        LOG_WARNING(buf);
        return "";
    }
    double deviceRecvTime = (double)millis();
    double now = (serverSendTime + serverRecvTime + deviceRecvTime - (double)deviceSendTime) / 2.0;
    double expireTime = now + 3600000.0;
    // expireTime 为毫秒级时间戳（通常 > 2^32），必须使用 64 位整型避免溢出导致鉴权失败
    unsigned long long expireTimeMs = (unsigned long long)expireTime;
    char expireBuf[24];
    snprintf(expireBuf, sizeof(expireBuf), "%llu", expireTimeMs);

    String plainPassword;
    if (config.authCode.isEmpty()) {
        plainPassword = config.password + "&" + String(expireBuf);
    } else {
        plainPassword = config.password + "&" + String(expireBuf) + "&" + config.authCode;
    }

    LOG_INFO("MQTT: Generating AES encrypted password");

    String iv = "wumei-smart-open";
    String encrypted = aesEncrypt(plainPassword, config.mqttSecret, iv);

    if (encrypted.isEmpty()) {
        LOG_WARNING("MQTT: AES encryption failed");
        return "";
    }

    return encrypted;
}

String MQTTClient::getNtpTime(unsigned long& outDeviceSendTime) {
    if (config.ntpServer.isEmpty()) {
        LOG_WARNING("MQTT: NTP server not configured");
        return "";
    }

    String url = config.ntpServer;

    // NTP 时间同步无需加密，强制降级 HTTPS → HTTP 避免 SSL 内存分配失败
    if (url.startsWith("https://")) {
        url = "http://" + url.substring(8);
    }

    // FastBee平台需要 deviceSendTime 参数
    outDeviceSendTime = millis();
    
    if (url.indexOf('?') >= 0) {
        if (!url.endsWith("?") && !url.endsWith("&")) url += "&";
    } else {
        url += "?";
    }
    url += "deviceSendTime=" + String(outDeviceSendTime);

    char buf[160];
    snprintf(buf, sizeof(buf), "MQTT: Fetching NTP time from %s", url.c_str());
    LOG_DEBUG(buf);

    HTTPClient http;
    static WiFiClient plainClient;
    bool beginOk = http.begin(plainClient, url);

    if (!beginOk) {
        LOG_WARNING("MQTT: HTTP begin failed for NTP");
        return "";
    }

    http.setTimeout(5000);
    int httpCode = http.GET();
    String payload = "";

    if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_MOVED_PERMANENTLY) {
        payload = http.getString();
    } else {
        char buf2[64];
        snprintf(buf2, sizeof(buf2), "MQTT: NTP HTTP failed, code=%d", httpCode);
        LOG_WARNING(buf2);
    }

    http.end();
    return payload;
}

String MQTTClient::aesEncrypt(const String& plainData, const String& key, const String& iv) {
    int len = plainData.length();
    int nBlocks = len / 16 + 1;
    uint8_t nPadding = nBlocks * 16 - len;
    size_t paddedLen = nBlocks * 16;

    uint8_t* data = new(std::nothrow) uint8_t[paddedLen];
    if (!data) {
        LOG_ERROR("AES: Failed to allocate data buffer");
        return "";
    }
    memcpy(data, plainData.c_str(), len);
    for (size_t i = len; i < paddedLen; i++) {
        data[i] = nPadding;
    }

    uint8_t keyBuf[16];
    uint8_t ivBuf[16];
    memcpy(keyBuf, key.c_str(), 16);
    memcpy(ivBuf, iv.c_str(), 16);

    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    int ret = mbedtls_aes_setkey_enc(&aes, keyBuf, 128);
    if (ret != 0) {
        mbedtls_aes_free(&aes);
        delete[] data;
        LOG_WARNING("MQTT: AES setkey failed");
        return "";
    }

    ret = mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_ENCRYPT, paddedLen, ivBuf, data, data);
    mbedtls_aes_free(&aes);

    if (ret != 0) {
        delete[] data;
        LOG_WARNING("MQTT: AES encrypt failed");
        return "";
    }

    size_t b64Len = 0;
    mbedtls_base64_encode(nullptr, 0, &b64Len, data, paddedLen);
    uint8_t* b64Buf = new(std::nothrow) uint8_t[b64Len + 1];
    if (!b64Buf) {
        delete[] data;
        LOG_ERROR("AES: Failed to allocate base64 buffer");
        return "";
    }
    ret = mbedtls_base64_encode(b64Buf, b64Len + 1, &b64Len, data, paddedLen);
    delete[] data;

    if (ret != 0) {
        delete[] b64Buf;
        LOG_WARNING("MQTT: Base64 encode failed");
        return "";
    }

    String result = String((char*)b64Buf, b64Len);
    delete[] b64Buf;
    return result;
}

#endif // FASTBEE_ENABLE_MQTT
