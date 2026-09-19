#include "core/FeatureFlags.h"
#if FASTBEE_ENABLE_PERIPH_EXEC

#include "./network/handlers/PeriphExecRouteHandler.h"
#include "core/MemoryBudget.h"
#include "./network/handlers/HandlerUtils.h"
#include "./network/WebHandlerContext.h"
#include "core/PeriphExecManager.h"
#include "core/PeriphExecExecutor.h"  // guessSensorDataField（数据源字段推断与执行器同规则）
#include "core/PeripheralManager.h"
#include "core/ChipConfig.h"
#include "core/ResourceProfile.h"
#include "core/RuntimeFeatureManager.h"
#include "core/AsyncExecTypes.h"  // MutexGuard
#if FASTBEE_ENABLE_MQTT
#include "protocols/MQTTClient.h"
#endif
#include "protocols/ProtocolManager.h"
#include <ArduinoJson.h>
#include <AsyncJson.h>
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <map>
#include <memory>
#include <utility>
#include <vector>

namespace {
constexpr uint8_t CONTROL_GROUP_COUNT = 6;
constexpr size_t CONTROL_ITEM_MAX_BYTES = 6144;
constexpr uint32_t PERIPH_EXEC_TIMER_INTERVAL_MIN_SEC = 1;
constexpr uint32_t PERIPH_EXEC_POLL_INTERVAL_MIN_SEC = 5;
constexpr uint32_t PERIPH_EXEC_INTERVAL_MAX_SEC = 86400UL;

uint32_t intervalSecondsFromInput(int value) {
    // Preserve an invalid negative input as zero so Manager applies the
    // trigger-specific lower bound instead of uint32_t wraparound becoming 24h.
    return value < 0 ? 0U : static_cast<uint32_t>(value);
}

const char* const CONTROL_GROUP_HEADERS[CONTROL_GROUP_COUNT] = {
    "\"gpio\":[",
    "\"modbus\":[",
    "\"system\":[",
    "\"script\":[",
    "\"sensor\":[",
    "\"other\":["
};

enum class ControlGroup : uint8_t {
    GPIO = 0,
    MODBUS,
    SYSTEM,
    SCRIPT,
    SENSOR,
    OTHER
};

ControlGroup controlGroupForAction(int actionType) {
    if ((actionType >= 0 && actionType <= 5) || actionType == 13 || actionType == 14) {
        return ControlGroup::GPIO;
    }
    if (actionType >= 16 && actionType <= 18) return ControlGroup::MODBUS;
    if (actionType >= 6 && actionType <= 12) return ControlGroup::SYSTEM;
    if (actionType == 15) return ControlGroup::SCRIPT;
    if (actionType == 19) return ControlGroup::SENSOR;
    return ControlGroup::OTHER;
}

// A controls item is intentionally bounded below the endpoint's 8 KB largest-
// block admission threshold. concat() reports allocation failure, so a partial
// JSON item is never sent when memory becomes fragmented during construction.
class ControlJsonWriter {
public:
    explicit ControlJsonWriter(size_t initialCapacity) {
        if (initialCapacity > CONTROL_ITEM_MAX_BYTES) {
            initialCapacity = CONTROL_ITEM_MAX_BYTES;
        }
        ok_ = value_.reserve(static_cast<unsigned int>(initialCapacity));
    }

    void append(const char* value) {
        if (!ok_ || !value) {
            ok_ = false;
            return;
        }
        const size_t length = strlen(value);
        if (!hasRoom(length) || !value_.concat(value, static_cast<unsigned int>(length))) {
            ok_ = false;
        }
    }

    void append(char value) {
        if (!ok_ || !hasRoom(1) || !value_.concat(value)) ok_ = false;
    }

    void append(int value) {
        char buffer[16];
        const int written = snprintf(buffer, sizeof(buffer), "%d", value);
        if (written <= 0 || static_cast<size_t>(written) >= sizeof(buffer)) {
            ok_ = false;
            return;
        }
        append(buffer);
    }

    void append(bool value) {
        append(value ? "true" : "false");
    }

    void appendQuoted(const String& value) {
        if (!ok_) return;

        size_t encodedLength = 2;  // surrounding quotes
        for (size_t i = 0; i < value.length(); ++i) {
            const uint8_t c = static_cast<uint8_t>(value[i]);
            const bool shortEscape = c == '"' || c == '\\' || c == '\b' || c == '\f' ||
                                     c == '\n' || c == '\r' || c == '\t';
            const size_t increment = shortEscape ? 2 : (c < 0x20 ? 6 : 1);
            if (encodedLength > CONTROL_ITEM_MAX_BYTES - increment) {
                ok_ = false;
                return;
            }
            encodedLength += increment;
        }
        if (!hasRoom(encodedLength)) {
            ok_ = false;
            return;
        }

        append('"');
        for (size_t i = 0; ok_ && i < value.length(); ++i) {
            const uint8_t c = static_cast<uint8_t>(value[i]);
            switch (c) {
                case '"': append("\\\""); break;
                case '\\': append("\\\\"); break;
                case '\b': append("\\b"); break;
                case '\f': append("\\f"); break;
                case '\n': append("\\n"); break;
                case '\r': append("\\r"); break;
                case '\t': append("\\t"); break;
                default:
                    if (c < 0x20) {
                        char escaped[7];
                        snprintf(escaped, sizeof(escaped), "\\u%04x", c);
                        append(escaped);
                    } else {
                        append(static_cast<char>(c));
                    }
                    break;
            }
        }
        append('"');
    }

    bool ok() const { return ok_; }
    String take() { return std::move(value_); }

private:
    bool hasRoom(size_t additional) const {
        return additional <= CONTROL_ITEM_MAX_BYTES - value_.length();
    }

    String value_;
    bool ok_ = true;
};

struct GroupedControlItem {
    uint8_t group = 0;
    String json;
};

bool sendControlsChunked(AsyncWebServerRequest* request,
                         std::vector<GroupedControlItem>&& items) {
    struct State {
        enum Phase : uint8_t {
            PREFIX,
            GROUP_HEADER,
            ITEM_SEPARATOR,
            ITEM_BODY,
            GROUP_FOOTER,
            SUFFIX,
            DONE
        };

        std::vector<GroupedControlItem> items;
        size_t itemIndex = 0;
        size_t itemPosition = 0;
        size_t tokenPosition = 0;
        uint8_t group = 0;
        Phase phase = PREFIX;
        bool firstInGroup = true;
    };

    auto state = std::make_shared<State>();
    state->items = std::move(items);

    AsyncWebServerResponse* response = request->beginChunkedResponse(
        "application/json",
        [state](uint8_t* buffer, size_t maxLen, size_t /*index*/) -> size_t {
            if (!buffer || maxLen == 0) return RESPONSE_TRY_AGAIN;
            size_t written = 0;

            auto copyToken = [&](const char* token, size_t length) -> bool {
                const size_t remaining = length - state->tokenPosition;
                const size_t available = maxLen - written;
                const size_t count = remaining < available ? remaining : available;
                if (count > 0) {
                    memcpy(buffer + written, token + state->tokenPosition, count);
                    state->tokenPosition += count;
                    written += count;
                }
                return state->tokenPosition >= length;
            };

            while (written < maxLen && state->phase != State::DONE) {
                if (state->phase == State::PREFIX) {
                    static const char prefix[] = "{\"success\":true,\"data\":{";
                    if (copyToken(prefix, sizeof(prefix) - 1)) {
                        state->tokenPosition = 0;
                        state->phase = State::GROUP_HEADER;
                    }
                    continue;
                }

                if (state->phase == State::GROUP_HEADER) {
                    const char* header = CONTROL_GROUP_HEADERS[state->group];
                    if (copyToken(header, strlen(header))) {
                        state->tokenPosition = 0;
                        state->firstInGroup = true;
                        state->phase = State::ITEM_SEPARATOR;
                    }
                    continue;
                }

                if (state->phase == State::ITEM_SEPARATOR) {
                    if (state->itemIndex >= state->items.size() ||
                        state->items[state->itemIndex].group != state->group) {
                        state->phase = State::GROUP_FOOTER;
                        state->tokenPosition = 0;
                        continue;
                    }
                    if (state->firstInGroup) {
                        state->phase = State::ITEM_BODY;
                        continue;
                    }
                    static const char comma[] = ",";
                    if (copyToken(comma, 1)) {
                        state->tokenPosition = 0;
                        state->phase = State::ITEM_BODY;
                    }
                    continue;
                }

                if (state->phase == State::ITEM_BODY) {
                    String& item = state->items[state->itemIndex].json;
                    const size_t remaining = item.length() - state->itemPosition;
                    const size_t available = maxLen - written;
                    const size_t count = remaining < available ? remaining : available;
                    if (count > 0) {
                        memcpy(buffer + written, item.c_str() + state->itemPosition, count);
                        state->itemPosition += count;
                        written += count;
                    }
                    if (state->itemPosition >= item.length()) {
                        item = String();
                        ++state->itemIndex;
                        state->itemPosition = 0;
                        state->firstInGroup = false;
                        state->phase = State::ITEM_SEPARATOR;
                    }
                    continue;
                }

                if (state->phase == State::GROUP_FOOTER) {
                    const char* footer = state->group + 1 < CONTROL_GROUP_COUNT ? "]," : "]";
                    const size_t footerLength = state->group + 1 < CONTROL_GROUP_COUNT ? 2 : 1;
                    if (copyToken(footer, footerLength)) {
                        state->tokenPosition = 0;
                        ++state->group;
                        state->phase = state->group < CONTROL_GROUP_COUNT
                            ? State::GROUP_HEADER : State::SUFFIX;
                    }
                    continue;
                }

                if (state->phase == State::SUFFIX) {
                    static const char suffix[] = "}}";
                    if (copyToken(suffix, sizeof(suffix) - 1)) {
                        state->items.clear();
                        state->items.shrink_to_fit();
                        state->phase = State::DONE;
                    }
                }
            }
            return written;
        });
    if (!response) return false;
    response->addHeader("Cache-Control", "no-store");
    response->addHeader("Connection", "close");
    request->send(response);
    return true;
}

String escapePeriphExecJsonString(const String& value) {
    String out;
    out.reserve(value.length() + 8);
    for (size_t i = 0; i < value.length(); ++i) {
        char c = value[i];
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<uint8_t>(c) < 0x20) {
                    char buf[7];
                    snprintf(buf, sizeof(buf), "\\u%04x", static_cast<uint8_t>(c));
                    out += buf;
                } else {
                    out += c;
                }
                break;
        }
    }
    return out;
}

const char* asyncExecStatusName(AsyncExecStatus status) {
    switch (status) {
        case AsyncExecStatus::PENDING: return "queued";
        case AsyncExecStatus::RUNNING: return "running";
        case AsyncExecStatus::COMPLETED: return "success";
        case AsyncExecStatus::FAILED: return "failed";
        case AsyncExecStatus::CANCELLED: return "cancelled";
        case AsyncExecStatus::REJECTED: return "rejected";
        case AsyncExecStatus::TIMED_OUT: return "timed_out";
    }
    return "unknown";
}

const char* asyncExecLegacyStatusName(AsyncExecStatus status) {
    switch (status) {
        case AsyncExecStatus::PENDING: return "pending";
        case AsyncExecStatus::RUNNING: return "running";
        case AsyncExecStatus::COMPLETED: return "completed";
        case AsyncExecStatus::FAILED: return "failed";
        case AsyncExecStatus::CANCELLED: return "cancelled";
        case AsyncExecStatus::REJECTED: return "rejected";
        case AsyncExecStatus::TIMED_OUT: return "timed_out";
    }
    return "unknown";
}

const char* asyncExecErrorName(AsyncExecError error) {
    switch (error) {
        case AsyncExecError::NONE: return "none";
        case AsyncExecError::LOW_MEMORY: return "low_memory";
        case AsyncExecError::DEADLINE_EXCEEDED: return "deadline_exceeded";
        case AsyncExecError::ACTION_FAILED: return "action_failed";
        case AsyncExecError::INCOMPLETE_ACTIONS: return "incomplete_actions";
        case AsyncExecError::CANCELLED_BY_SYSTEM: return "cancelled_by_system";
        case AsyncExecError::WORKER_QUEUE_FULL: return "worker_queue_full";
    }
    return "unknown";
}

bool asyncExecStatusIsTerminal(AsyncExecStatus status) {
    return status == AsyncExecStatus::COMPLETED ||
           status == AsyncExecStatus::FAILED ||
           status == AsyncExecStatus::CANCELLED ||
           status == AsyncExecStatus::REJECTED ||
           status == AsyncExecStatus::TIMED_OUT;
}

int runOnceRejectionStatus(const String& reason) {
    if (reason == "rule_not_found" || reason == "rule_not_found_or_disabled") return 404;
    if (reason == "already_running") return 409;
    if (reason == "action_not_automatable" || reason == "action_order_invalid") return 400;
    if (reason == "no_task_slot" || reason == "context_pool_exhausted" ||
        reason == "worker_queue_full") return 429;
    return 503;
}

bool shouldDegradeControlsResponse() {
    return FastBee::MemoryBudget::dram8Free() < 16384 ||
           FastBee::MemoryBudget::dram8Largest() < 8192;
}

// 生成 Flash 保存失败的详细错误消息（包含文件系统与堆状态，便于定位内存不足/碎片问题）
String buildFlashSaveErrorMessage(const char* operation) {
    size_t fsTotal = LittleFS.totalBytes();
    size_t fsUsed  = LittleFS.usedBytes();
    size_t fsFree  = fsTotal > fsUsed ? fsTotal - fsUsed : 0;
    char buf[240];
    snprintf(buf, sizeof(buf),
             "%s failed (flash free=%uKB/%uKB, heap free=%uKB maxBlock=%uKB, check serial log)",
             operation,
             (unsigned)(fsFree / 1024),
             (unsigned)(fsTotal / 1024),
             (unsigned)(ESP.getFreeHeap() / 1024),
             (unsigned)(ESP.getMaxAllocHeap() / 1024));
    return String(buf);
}

bool isRulePersistenceFailure(const String& errorMsg) {
    return errorMsg.startsWith("PERSISTENCE_ROLLBACK_OK") ||
           errorMsg.startsWith("ROLLBACK_FAILED");
}

int ruleMutationErrorStatus(const String& errorMsg, int fallbackStatus) {
    if (isRulePersistenceFailure(errorMsg)) return 500;
    if (errorMsg.startsWith("RULE_SERVICE_BUSY")) return 503;
    if (errorMsg == "Rule not found") return 404;
    return fallbackStatus;
}

String ruleMutationErrorMessage(const String& errorMsg,
                                const char* fallbackMessage) {
    String message = errorMsg.isEmpty() ? String(fallbackMessage) : errorMsg;
    if (isRulePersistenceFailure(errorMsg)) {
        message += "; ";
        message += buildFlashSaveErrorMessage("Rule persistence");
    }
    return message;
}

void sendDegradedControlsResponse(AsyncWebServerRequest* request) {
    AsyncWebServerResponse* response = request->beginResponse(
        200, "application/json",
        F("{\"success\":true,\"degraded\":true,\"message\":\"Low memory - controls temporarily reduced\","
          "\"data\":{\"gpio\":[],\"modbus\":[],\"system\":[],\"script\":[],\"sensor\":[],\"other\":[]}}"));
    HandlerUtils::sendWithClose(request, response);
}

bool buildControlItemJson(
    const PeriphExecRule& rule,
    PeripheralManager& peripheralManager,
    const std::map<String, PeriphExecManager::SensorReadCache>& sensorCache,
    String& itemJson) {
    if (rule.actions.empty()) return false;

    const ExecAction& firstAction = rule.actions[0];
    const int actionType = firstAction.actionType;
    String peripheralName;
    if (!firstAction.targetPeriphId.isEmpty()) {
        PeripheralConfig peripheral;
        if (peripheralManager.getPeripheralSnapshot(firstAction.targetPeriphId, peripheral)) {
            peripheralName = peripheral.name;
        }
    }

    size_t initialCapacity = 192 + rule.id.length() + rule.name.length() +
                             firstAction.targetPeriphId.length() + peripheralName.length();
    if (actionType == static_cast<int>(ExecActionType::ACTION_SENSOR_READ)) {
        initialCapacity += rule.actions.size() * 96;
    }
    ControlJsonWriter writer(initialCapacity);
    writer.append("{\"id\":");
    writer.appendQuoted(rule.id);
    writer.append(",\"name\":");
    writer.appendQuoted(rule.name);
    writer.append(",\"actionType\":");
    writer.append(actionType);

    bool hasSetMode = false;
    for (const auto& trigger : rule.triggers) {
        if (trigger.triggerType == 0 && trigger.operatorType == 1) {
            hasSetMode = true;
            break;
        }
    }
    writer.append(",\"hasSetMode\":");
    writer.append(hasSetMode);

    if (!firstAction.targetPeriphId.isEmpty()) {
        writer.append(",\"targetPeriphId\":");
        writer.appendQuoted(firstAction.targetPeriphId);
        if (!peripheralName.isEmpty()) {
            writer.append(",\"targetPeriphName\":");
            writer.appendQuoted(peripheralName);
        }
    }

    if (actionType == static_cast<int>(ExecActionType::ACTION_MODBUS_POLL) &&
        !firstAction.actionValue.isEmpty()) {
        JsonDocument pollDocument;
        const DeserializationError error = deserializeJson(pollDocument, firstAction.actionValue);
        if (!error && pollDocument["poll"].is<JsonArray>()) {
            writer.append(",\"pollTaskIndex\":[");
            bool firstPoll = true;
            for (JsonVariant value : pollDocument["poll"].as<JsonArray>()) {
                if (!firstPoll) writer.append(',');
                firstPoll = false;
                writer.append(value.as<int>());
            }
            writer.append(']');
        }
    }

    if (actionType == static_cast<int>(ExecActionType::ACTION_SENSOR_READ)) {
        writer.append(",\"sensors\":[");
        bool firstSensor = true;
        for (const auto& action : rule.actions) {
            if (action.actionType != static_cast<uint8_t>(ExecActionType::ACTION_SENSOR_READ) ||
                action.actionValue.isEmpty()) {
                continue;
            }

            JsonDocument sensorDocument;
            if (deserializeJson(sensorDocument, action.actionValue)) continue;

            String sensorPeriphId = sensorDocument["periphId"] | "";
            if (sensorPeriphId.isEmpty()) sensorPeriphId = action.targetPeriphId;
            const String reportField = sensorDocument["dataField"] | "";
            const String channelField = sensorDocument["channelField"] | "";
            const String cacheField = channelField.isEmpty()
                ? PeriphExecExecutor::resolveChannelField(
                      sensorDocument["sensorCategory"] | "analog",
                      sensorPeriphId,
                      reportField)
                : channelField;
            const String label = sensorDocument["sensorLabel"] | "";
            const String unit = sensorDocument["unit"] | "";
            String cacheKey = sensorPeriphId + "_" + cacheField;

            if (!firstSensor) writer.append(',');
            firstSensor = false;
            writer.append("{\"key\":");
            writer.appendQuoted(cacheKey);
            writer.append(",\"label\":");
            writer.appendQuoted(label);
            writer.append(",\"unit\":");
            writer.appendQuoted(unit);

            const auto cached = sensorCache.find(cacheKey);
            if (cached != sensorCache.end()) {
                writer.append(",\"value\":");
                writer.appendQuoted(cached->second.value);
            }
            writer.append('}');
        }
        writer.append(']');
    }

    writer.append('}');
    if (!writer.ok()) return false;
    itemJson = writer.take();
    return !itemJson.isEmpty();
}
}

PeriphExecRouteHandler::PeriphExecRouteHandler(WebHandlerContext* ctx)
    : ctx(ctx) {
}

void PeriphExecRouteHandler::setupRoutes(AsyncWebServer* server) {
    // 注意：更具体的路由必须先注册，否则会被 /api/periph-exec 拦截

    // 设备控制API（按动作类型分组）
    server->on("/api/periph-exec/controls", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleGetControls(request); });

    // 触发事件相关API
    server->on("/api/periph-exec/events/static", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleGetStaticEvents(request); });
    
    server->on("/api/periph-exec/events/dynamic", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleGetDynamicEvents(request); });
    
    server->on("/api/periph-exec/events/categories", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleGetEventCategories(request); });
    
    server->on("/api/periph-exec/trigger-types", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleGetTriggerTypes(request); });

    server->on("/api/periph-exec/results", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleGetRecentResults(request); });

    // 向后兼容：旧版系统事件API重定向到静态事件
    server->on("/api/periph-exec/system-events", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleGetStaticEvents(request); });

    // JSON body handlers for add/update（支持 triggers[]/actions[] 数组）
    // 注意: update 必须先于 add 注册，因为 AsyncCallbackJsonWebHandler 使用前缀匹配
    auto* preflightJsonHandler = new AsyncCallbackJsonWebHandler("/api/periph-exec/preflight",
        [this](AsyncWebServerRequest* request, JsonVariant& json) {
            handlePreflightRuleJson(request, json);
        });
    preflightJsonHandler->setMethod(HTTP_POST);
    preflightJsonHandler->setMaxContentLength(12288);
    server->addHandler(preflightJsonHandler);

    auto* updateJsonHandler = new AsyncCallbackJsonWebHandler("/api/periph-exec/update",
        [this](AsyncWebServerRequest* request, JsonVariant& json) {
            handleUpdateRuleJson(request, json);
        });
    updateJsonHandler->setMethod(HTTP_POST);
    server->addHandler(updateJsonHandler);

    auto* addJsonHandler = new AsyncCallbackJsonWebHandler("/api/periph-exec",
        [this](AsyncWebServerRequest* request, JsonVariant& json) {
            handleAddRuleJson(request, json);
        });
    addJsonHandler->setMethod(HTTP_POST);
    server->addHandler(addJsonHandler);

    // 旧版 form-encoded fallback（向后兼容）
    server->on("/api/periph-exec/update", HTTP_POST,
              [this](AsyncWebServerRequest* request) { handleUpdateRule(request); });

    server->on("/api/periph-exec/enable", HTTP_POST,
              [this](AsyncWebServerRequest* request) { handleEnableRule(request); });

    server->on("/api/periph-exec/disable", HTTP_POST,
              [this](AsyncWebServerRequest* request) { handleDisableRule(request); });

    server->on("/api/periph-exec/run", HTTP_POST,
              [this](AsyncWebServerRequest* request) { handleRunOnce(request); });

    server->on("/api/periph-exec/", HTTP_DELETE,
              [this](AsyncWebServerRequest* request) { handleDeleteRule(request); });

    server->on("/api/periph-exec", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleGetRules(request); });

    // 旧版 form-encoded add fallback
    server->on("/api/periph-exec", HTTP_POST,
              [this](AsyncWebServerRequest* request) { handleAddRule(request); });
}

// ========== 获取所有规则 ==========

void PeriphExecRouteHandler::handleGetRules(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;

    if (HandlerUtils::rejectHeavyRequestOnPressure(request, "Rule list", MemoryGuardLevel::CRITICAL, 5)) {
        return;
    }

    PeriphExecManager& mgr = PeriphExecManager::getInstance();
    PeripheralManager& pm = PeripheralManager::getInstance();

    // ============ 单条查询：在单一锁持有期内复制规则 ============
    if (request->hasParam("id")) {
        String ruleId = request->getParam("id")->value();
        // getRule() 要求调用方已持有 _rulesMutex；不要调用 getRuleSnapshot()
        // here because it acquires the same non-recursive mutex again.
        // 超时 2000ms：规则执行长时间占锁时快速返回 503，避免永久阻塞 async_tcp 任务
        // （处理器运行在 async_tcp 任务上，无限等待会冻结所有 HTTP 请求）
        PeriphExecRule rule;
        {
            MutexGuard lock(mgr.getRulesMutex(), pdMS_TO_TICKS(2000));
            if (!lock.isLocked()) {
                HandlerUtils::sendJsonError(request, 503, "Service busy (rules locked), retry in 2s", 2);
                return;
            }

            const PeriphExecRule* storedRule = mgr.getRule(ruleId);
            if (!storedRule) {
                request->send(404, "application/json", "{\"success\":false,\"message\":\"Rule not found\"}");
                return;
            }
            // Copy while protected; serialization and response allocation happen
            // after releasing the manager mutex so execution is not stalled.
            rule = *storedRule;
        }

        String response = serializeRuleFull(rule);
        HandlerUtils::sendJsonSuccess(request, response);
        return;
    }

    // ============ 列表查询：仅指针排序 + 当前页序列化，避免全量深拷贝 ============
    // 解析分页参数
    int page = 1;
    int pageSize = 10;
    if (request->hasParam("page")) {
        page = request->getParam("page")->value().toInt();
        if (page < 1) page = 1;
    }
    if (request->hasParam("pageSize")) {
        pageSize = request->getParam("pageSize")->value().toInt();
        if (pageSize < 1) pageSize = 1;
        if (pageSize > 50) pageSize = 50;
    }

    // 检查堆内存是否充足（chunked流式发送内存占用很小，只需保证最低运行空间）
    if (HandlerUtils::checkLowMemory(request, 3072)) return;

    // 分页步长始终与前端请求保持一致，避免数据间隙。
    // 内存不足时由 compact 模式（摘要字段）来节省空间，而非减少条数。
    int effectivePageSize = pageSize;

    // 持锁短临界区：构建指针 vector + 排序 + 序列化当前页到 items
    // 超时 2000ms：避免规则执行占用锁时无限等待导致分页请求卡死
    int total = 0;
    int startIdx = 0;
    int endIdx = 0;
    std::vector<String> items;
    std::vector<String> sensorSourceIds;
    String sensorSources;
    {
        MutexGuard lock(mgr.getRulesMutex(), pdMS_TO_TICKS(2000));
        if (!lock.isLocked()) {
            HandlerUtils::sendJsonError(request, 503, "Service busy (rules locked), retry in 2s", 2);
            return;
        }
        auto& ruleMap = mgr.getRules();
        // 仅指针 vector，每项 4-8 字节，46 条规则也只有 ~370 字节
        std::vector<const PeriphExecRule*> ptrs;
        ptrs.reserve(ruleMap.size());
        for (const auto& pair : ruleMap) {
            ptrs.push_back(&pair.second);
        }

        auto addSensorSource = [&](const ExecAction& action) {
            if (action.actionType != static_cast<uint8_t>(ExecActionType::ACTION_SENSOR_READ)) return;
            if (action.actionValue.isEmpty()) return;

            JsonDocument sDoc;
            DeserializationError sErr = deserializeJson(sDoc, action.actionValue);
            if (sErr) return;

            String periphId = sDoc["periphId"].as<String>();
            if (periphId.isEmpty()) periphId = action.targetPeriphId;
            if (periphId.isEmpty()) return;

            // 内部通道字段与执行器缓存键同规则（显式 dataField 优先；多字段类别推断；
            // 单值传感器用外设配置 id），保证 sourceId=${periphId.field} 模板变量能命中传感器缓存
            String explicitChannelField = sDoc["channelField"] | "";
            String dataField = explicitChannelField.isEmpty()
                ? PeriphExecExecutor::resolveChannelField(
                      sDoc["sensorCategory"] | "analog", periphId, sDoc["dataField"] | "")
                : explicitChannelField;
            String sourceId = periphId + "_" + dataField;
            for (const auto& existingId : sensorSourceIds) {
                if (existingId == sourceId) return;
            }
            sensorSourceIds.push_back(sourceId);

            String sensorLabel = sDoc["sensorLabel"].as<String>();
            if (sensorLabel.isEmpty()) {
                if (dataField == "humidity") sensorLabel = "湿度";
                else if (dataField == "voltage") sensorLabel = "电压";
                else if (dataField == "value") sensorLabel = "数值";
                else sensorLabel = "温度";
            }
            String unit = sDoc["unit"].as<String>();

            String periphName = periphId;
            PeripheralConfig periph;
            if (pm.getPeripheralSnapshot(periphId, periph) && !periph.name.isEmpty()) periphName = periph.name;

            String displayLabel = periphName + " - " + sensorLabel;
            if (!sensorSources.isEmpty()) sensorSources += ",";
            sensorSources += "{\"id\":\"" + escapePeriphExecJsonString(sourceId) + "\"";
            sensorSources += ",\"label\":\"" + escapePeriphExecJsonString(displayLabel) + "\"";
            sensorSources += ",\"periphId\":\"" + escapePeriphExecJsonString(periphId) + "\"";
            sensorSources += ",\"field\":\"" + escapePeriphExecJsonString(dataField) + "\"";
            if (!unit.isEmpty()) sensorSources += ",\"unit\":\"" + escapePeriphExecJsonString(unit) + "\"";
            sensorSources += "}";
        };

        for (const auto* rule : ptrs) {
            if (!rule) continue;
            for (const auto& action : rule->actions) {
                addSensorSource(action);
            }
        }

        // 排序：启用的排前面，然后按名称排序
        std::sort(ptrs.begin(), ptrs.end(), [](const PeriphExecRule* a, const PeriphExecRule* b) {
            if (a->enabled != b->enabled) return a->enabled > b->enabled;
            return strcmp(a->name.c_str(), b->name.c_str()) < 0;
        });

        total = static_cast<int>(ptrs.size());
        startIdx = (page - 1) * effectivePageSize;
        endIdx = (startIdx < total) ? min(startIdx + effectivePageSize, total) : startIdx;

        items.reserve(endIdx - startIdx);
        for (int i = startIdx; i < endIdx; i++) {
            const auto& rule = *ptrs[i];

            // 列表视图只返回摘要字段
            int triggerCount = rule.triggers.size();
            int actionCount = rule.actions.size();
            int triggerSummary = triggerCount > 0 ? rule.triggers[0].triggerType : -1;
            int actionSummary = actionCount > 0 ? rule.actions[0].actionType : -1;

            bool hasSetMode = false;
            for (const auto& trigger : rule.triggers) {
                if (trigger.triggerType == 0 && trigger.operatorType == 1) {
                    hasSetMode = true;
                    break;
                }
            }

            String targetPeriphName;
            if (actionCount > 0 && !rule.actions[0].targetPeriphId.isEmpty()) {
                PeripheralConfig periph;
                if (pm.getPeripheralSnapshot(rule.actions[0].targetPeriphId, periph)) targetPeriphName = periph.name;
            }

            // 构建流程摘要: 触发 → 动作 → 上报
            String flowSummary;
            flowSummary.reserve(80);
            if (triggerCount > 0) {
                const auto& t = rule.triggers[0];
                switch (t.triggerType) {
                    case 0: flowSummary = (t.operatorType == 1) ? F("MQTT设值") : F("MQTT指令"); break;
                    case 1:
                        if (t.timerMode == 1 && !t.timePoint.isEmpty()) {
                            flowSummary = F("每日"); flowSummary += t.timePoint;
                        } else {
                            flowSummary = F("每"); flowSummary += String(t.intervalSec); flowSummary += F("秒");
                        }
                        break;
                    case 4: flowSummary = F("事件:"); flowSummary += t.eventId; break;
                    case 5: flowSummary = F("轮询采集"); break;
                    case 6: flowSummary = F("规则链"); break;
                    default: flowSummary = F("触发"); break;
                }
                if (triggerCount > 1) { flowSummary += F("…"); }
            }
            flowSummary += F(" → ");
            if (actionCount > 0) {
                const auto& a = rule.actions[0];
                switch (a.actionType) {
                    case 0: flowSummary += F("设高电平"); break;
                    case 1: flowSummary += F("设低电平"); break;
                    case 2: flowSummary += F("闪烁"); break;
                    case 3: flowSummary += F("呼吸灯"); break;
                    case 4: flowSummary += F("PWM"); break;
                    case 5: flowSummary += F("DAC"); break;
                    case 6: flowSummary += F("重启"); break;
                    case 10: flowSummary += F("调用"); break;
                    case 13: flowSummary += F("高反转"); break;
                    case 14: flowSummary += F("低反转"); break;
                    case 15: flowSummary += F("脚本"); break;
                    case 16: flowSummary += F("写线圈"); break;
                    case 17: flowSummary += F("写寄存器"); break;
                    case 18: flowSummary += F("Modbus采集"); break;
                    case 19: flowSummary += F("读传感器"); break;
                    case 21: flowSummary += F("发事件"); break;
                    case 22: flowSummary += F("启用规则"); break;
                    case 23: flowSummary += F("禁用规则"); break;
                    case 24: flowSummary += F("显数字"); break;
                    case 25: flowSummary += F("显文本"); break;
                    case 26: flowSummary += F("清数码管"); break;
                    case 27: flowSummary += F("OLED显示"); break;
                    case 33: flowSummary += F("远程控制"); break;
                    case 34: flowSummary += F("远程读取"); break;
                    default: flowSummary += F("动作"); break;
                }
                if (!targetPeriphName.isEmpty()) { flowSummary += F(" → "); flowSummary += targetPeriphName; }
                if (actionCount > 1) { flowSummary += F("…"); }
            }
            if (rule.reportAfterExec) { flowSummary += F(" → 上报"); }

            // 条件分支（平台触发有比较时）
            if (triggerCount > 0 && rule.triggers[0].triggerType == 0 && rule.triggers[0].operatorType != 1 && !rule.triggers[0].compareValue.isEmpty()) {
                // 在触发和动作之间插入条件
                String condPart = F("若>"); condPart += rule.triggers[0].compareValue;
                // 替换第一个 " → " 为 " → 若X → "
                int arrowPos = flowSummary.indexOf(F(" → "));
                if (arrowPos >= 0) {
                    flowSummary = flowSummary.substring(0, arrowPos + 4) + condPart + F(" → ") + flowSummary.substring(arrowPos + 4);
                }
            }

            String item;
            item.reserve(200 + rule.id.length() + rule.name.length() + targetPeriphName.length() + flowSummary.length());
            item = F("{\"id\":\"");
            item += escapePeriphExecJsonString(rule.id);
            item += F("\",\"name\":\"");
            item += escapePeriphExecJsonString(rule.name);
            item += F("\",\"enabled\":");
            item += rule.enabled ? F("true") : F("false");
            item += F(",\"execMode\":");
            item += String(rule.execMode);
            item += F(",\"reportAfterExec\":");
            item += rule.reportAfterExec ? F("true") : F("false");
            item += F(",\"allowPanelControl\":");
            item += rule.allowPanelControl ? F("true") : F("false");
            item += F(",\"recordHistory\":");
            item += rule.recordHistory ? F("true") : F("false");
            item += F(",\"triggerCount\":");
            item += String(triggerCount);
            item += F(",\"actionCount\":");
            item += String(actionCount);
            item += F(",\"triggerSummary\":");
            item += String(triggerSummary);
            item += F(",\"actionSummary\":");
            item += String(actionSummary);
            item += F(",\"hasSetMode\":");
            item += hasSetMode ? F("true") : F("false");
            item += F(",\"targetPeriphName\":\"");
            item += escapePeriphExecJsonString(targetPeriphName);
            item += F("\",\"flowSummary\":\"");
            item += escapePeriphExecJsonString(flowSummary);
            item += F("\"}");
            items.emplace_back(std::move(item));
        }
    }  // 释放 _rulesMutex

    // 处理空页边界（避免 items 为空时调用 sendJsonListChunked）
    // 依然需要返回有效的分页响应结构

    // 复用 HandlerUtils::sendJsonListChunked（与 handleGetPeripherals 同模式）
    String header;
    const int profileMax = static_cast<int>(FastBee::ResourceProfile::MAX_PERIPH_EXEC_RULES);
    // 执行规则是软限制（“推荐值，超限只警告”），remaining至少为1，不锁定新增按钮
    const int profileRemaining = std::max(1, profileMax - total);
    const bool runtimeDegraded = HandlerUtils::memoryDegradationActive();

    header.reserve(460 + sensorSources.length());
    header = F("{\"success\":true,\"total\":");
    header += String(total);
    header += F(",\"page\":");
    header += String(page);
    header += F(",\"pageSize\":");
    header += String(effectivePageSize);
    // 保留 compact 标志，前端可据此判断是否缺少详细字段
    const bool compact = (FastBee::MemoryBudget::dramFree() < 16384) || (FastBee::MemoryBudget::dramLargest() < 8192);
    if (compact) {
        header += F(",\"compact\":true");
    }
    if (compact || runtimeDegraded) {
        header += F(",\"degraded\":true");
    }
    HandlerUtils::appendMemoryDegradationStatus(header);
    header += F(",\"profile\":{\"name\":\"");
    header += FastBee::ResourceProfile::NAME;
    header += F("\",\"max\":");
    header += String(profileMax);
    header += F(",\"used\":");
    header += String(total);
    header += F(",\"remaining\":");
    header += String(profileRemaining);
    header += F("}");
    header += F(",\"sensorSources\":[");
    header += sensorSources;
    header += F("]");
    header += F(",\"data\":[");

    if (!HandlerUtils::sendJsonListChunked(request, header, std::move(items))) {
        HandlerUtils::sendJsonError(request, 503, "Failed to create streaming response");
    }
}

// ========== 新增规则（form-encoded 向后兼容） ==========

void PeriphExecRouteHandler::handleAddRule(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;
    if (!ctx->requireDeveloperMode(request)) return;

    PeriphExecRule rule;
    rule.id = ctx->getParamValue(request, "id", "");
    rule.name = ctx->getParamValue(request, "name", "");
    rule.enabled = ctx->getParamBool(request, "enabled", true);
    rule.execMode = ctx->getParamInt(request, "execMode", 0);
    rule.protocolType = ctx->getParamInt(request, "protocolType", 0);
    rule.scriptContent = ctx->getParamValue(request, "scriptContent", "");
    rule.reportAfterExec = ctx->getParamBool(request, "reportAfterExec", true);
    rule.allowPanelControl = ctx->getParamBool(request, "allowPanelControl", false);
    rule.recordHistory = ctx->getParamBool(request, "recordHistory", false);

    // 旧版 form-encoded: 将平铺字段封装为单元素 triggers[]/actions[]
    ExecTrigger t;
    t.triggerType = ctx->getParamInt(request, "triggerType", 0);
    t.triggerPeriphId = ctx->getParamValue(request, "triggerPeriphId", "");
    t.operatorType = ctx->getParamInt(request, "operatorType", 0);
    t.compareValue = ctx->getParamValue(request, "compareValue", "");
    t.timerMode = ctx->getParamInt(request, "timerMode", 0);
    t.intervalSec = intervalSecondsFromInput(
        ctx->getParamInt(request, "intervalSec", 60));
    t.timePoint = ctx->getParamValue(request, "timePoint", "");
    t.eventId = ctx->getParamValue(request, "eventId", "");
    t.pollResponseTimeout = ctx->getParamInt(request, "pollResponseTimeout", 1000);
    t.pollMaxRetries = ctx->getParamInt(request, "pollMaxRetries", 2);
    t.pollInterPollDelay = ctx->getParamInt(request, "pollInterPollDelay", 100);
    rule.triggers.push_back(t);

    ExecAction a;
    a.targetPeriphId = ctx->getParamValue(request, "targetPeriphId", "");
    a.actionType = ctx->getParamInt(request, "actionType", 0);
    a.actionValue = ctx->getParamValue(request, "actionValue", "");
    a.useReceivedValue = ctx->getParamBool(request, "useReceivedValue", false);
    a.syncDelayMs = ctx->getParamInt(request, "syncDelayMs", 0);
    a.execMode = rule.execMode;  // form-encoded 模式：从规则级别继承
    rule.actions.push_back(a);

    if (rule.name.isEmpty()) {
        ctx->sendError(request, 400, "Rule name is required");
        return;
    }

    PeriphExecManager& mgr = PeriphExecManager::getInstance();
    String errorMsg;
    if (mgr.addRuleAndSave(rule, errorMsg)) {
        ctx->sendSuccess(request, "Rule added");
    } else {
        ctx->sendError(request, ruleMutationErrorStatus(errorMsg, 400),
                       ruleMutationErrorMessage(errorMsg, "Failed to add rule"));
    }
}

// ========== 更新规则 ==========

void PeriphExecRouteHandler::handleUpdateRule(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;
    if (!ctx->requireDeveloperMode(request)) return;

    String id = ctx->getParamValue(request, "id", "");
    if (id.isEmpty()) {
        ctx->sendError(request, 400, "Rule ID is required");
        return;
    }

    PeriphExecManager& mgr = PeriphExecManager::getInstance();
    PeriphExecRule existing;
    if (!mgr.getRuleSnapshot(id, existing)) {
        ctx->sendError(request, 404, "Rule not found");
        return;
    }

    PeriphExecRule rule;
    rule.id = id;
    rule.name = ctx->getParamValue(request, "name", existing.name);
    rule.enabled = ctx->getParamBool(request, "enabled", existing.enabled);
    rule.execMode = ctx->getParamInt(request, "execMode", existing.execMode);
    rule.protocolType = ctx->getParamInt(request, "protocolType", existing.protocolType);
    rule.scriptContent = ctx->getParamValue(request, "scriptContent", existing.scriptContent);
    rule.reportAfterExec = ctx->getParamBool(request, "reportAfterExec", existing.reportAfterExec);
    rule.allowPanelControl = ctx->getParamBool(request, "allowPanelControl", existing.allowPanelControl);
    rule.recordHistory = ctx->getParamBool(request, "recordHistory", existing.recordHistory);

    // 旧版 form-encoded: 使用已有 trigger/action 作为默认值
    ExecTrigger t;
    if (!existing.triggers.empty()) {
        t = existing.triggers[0]; // 复制现有 trigger 作为默认
    }
    t.triggerType = ctx->getParamInt(request, "triggerType", t.triggerType);
    t.triggerPeriphId = ctx->getParamValue(request, "triggerPeriphId", t.triggerPeriphId);
    t.operatorType = ctx->getParamInt(request, "operatorType", t.operatorType);
    t.compareValue = ctx->getParamValue(request, "compareValue", t.compareValue);
    t.timerMode = ctx->getParamInt(request, "timerMode", t.timerMode);
    t.intervalSec = intervalSecondsFromInput(
        ctx->getParamInt(request, "intervalSec", static_cast<int>(t.intervalSec)));
    t.timePoint = ctx->getParamValue(request, "timePoint", t.timePoint);
    t.eventId = ctx->getParamValue(request, "eventId", t.eventId);
    t.pollResponseTimeout = ctx->getParamInt(request, "pollResponseTimeout", t.pollResponseTimeout);
    t.pollMaxRetries = ctx->getParamInt(request, "pollMaxRetries", t.pollMaxRetries);
    t.pollInterPollDelay = ctx->getParamInt(request, "pollInterPollDelay", t.pollInterPollDelay);
    rule.triggers.push_back(t);

    ExecAction a;
    if (!existing.actions.empty()) {
        a = existing.actions[0]; // 复制现有 action 作为默认
    }
    a.targetPeriphId = ctx->getParamValue(request, "targetPeriphId", a.targetPeriphId);
    a.actionType = ctx->getParamInt(request, "actionType", a.actionType);
    a.actionValue = ctx->getParamValue(request, "actionValue", a.actionValue);
    a.useReceivedValue = ctx->getParamBool(request, "useReceivedValue", a.useReceivedValue);
    a.syncDelayMs = ctx->getParamInt(request, "syncDelayMs", a.syncDelayMs);
    a.execMode = rule.execMode;  // form-encoded 模式：从规则级别继承
    rule.actions.push_back(a);

    String errorMsg;
    if (mgr.updateRuleAndSave(id, rule, errorMsg)) {
        ctx->sendSuccess(request, "Rule updated");
    } else {
        ctx->sendError(request, ruleMutationErrorStatus(errorMsg, 400),
                       ruleMutationErrorMessage(errorMsg, "Failed to update rule"));
    }
}

// ========== 删除规则 ==========

void PeriphExecRouteHandler::handleDeleteRule(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;
    if (!ctx->requireDeveloperMode(request)) return;

    String id = ctx->getParamValue(request, "id", "");
    if (id.isEmpty()) {
        ctx->sendError(request, 400, "Rule ID is required");
        return;
    }

    PeriphExecManager& mgr = PeriphExecManager::getInstance();
    String errorMsg;
    if (mgr.removeRuleAndSave(id, errorMsg)) {
        ctx->sendSuccess(request, "Rule deleted");
    } else {
        ctx->sendError(request, ruleMutationErrorStatus(errorMsg, 404),
                       ruleMutationErrorMessage(errorMsg, "Rule not found"));
    }
}

// ========== 启用规则 ==========

void PeriphExecRouteHandler::handleEnableRule(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;
    if (!ctx->requireDeveloperMode(request)) return;

    String id = ctx->getParamValue(request, "id", "");
    if (id.isEmpty()) {
        ctx->sendError(request, 400, "Rule ID is required");
        return;
    }

    PeriphExecManager& mgr = PeriphExecManager::getInstance();
    String errorMsg;
    if (mgr.enableRuleAndSave(id, errorMsg)) {
        ctx->sendSuccess(request, "Rule enabled");
    } else {
        ctx->sendError(request, ruleMutationErrorStatus(errorMsg, 404),
                       ruleMutationErrorMessage(errorMsg, "Rule not found"));
    }
}

// ========== 禁用规则 ==========

void PeriphExecRouteHandler::handleDisableRule(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;
    if (!ctx->requireDeveloperMode(request)) return;

    String id = ctx->getParamValue(request, "id", "");
    if (id.isEmpty()) {
        ctx->sendError(request, 400, "Rule ID is required");
        return;
    }

    PeriphExecManager& mgr = PeriphExecManager::getInstance();
    String errorMsg;
    if (mgr.disableRuleAndSave(id, errorMsg)) {
        ctx->sendSuccess(request, "Rule disabled");
    } else {
        ctx->sendError(request, ruleMutationErrorStatus(errorMsg, 404),
                       ruleMutationErrorMessage(errorMsg, "Rule not found"));
    }
}

// ========== 执行一次规则 ==========

void PeriphExecRouteHandler::handleRunOnce(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;
    if (!ctx->requireDeveloperMode(request)) return;

    String id = ctx->getParamValue(request, "id", "");
    if (id.isEmpty()) {
        ctx->sendError(request, 400, "Rule ID is required");
        return;
    }

    // 可选的用户输入值（设置模式下从前端弹窗传入）
    String value = ctx->getParamValue(request, "value", "");

    PeriphExecManager& mgr = PeriphExecManager::getInstance();
    
    // 异步提交执行（不阻塞 Web 请求线程）
    String rejectionReason;
    AsyncExecResult operation;
    bool submitted = mgr.runOnce(id, value, &rejectionReason, &operation);

    JsonDocument doc;
    if (submitted) {
        doc["success"] = true;
        doc["message"] = operation.asyncExecution
            ? "Rule execution accepted" : "Rule execution finished";
        doc["operationId"] = operation.operationId;
        doc["state"] = asyncExecStatusName(operation.status);
        doc["queuedAt"] = operation.queuedAt;
        doc["deadlineAt"] = operation.deadlineAt;
        doc["queuePosition"] = operation.queuePosition;
    } else {
        doc["success"] = false;
        doc["reason"] = rejectionReason;
        if (operation.operationId != 0) {
            doc["operationId"] = operation.operationId;
            doc["state"] = asyncExecStatusName(operation.status);
        }
        if (rejectionReason == "rule_not_found") {
            doc["message"] = "Rule not found";
        } else if (rejectionReason == "already_running") {
            doc["message"] = "Rule is already running";
        } else {
            doc["message"] = "Rule execution was not admitted; retry later";
        }
    }
    String responseBody;
    responseBody.reserve(measureJson(doc) + 1);
    serializeJson(doc, responseBody);
    const int responseStatus = submitted
        ? (operation.asyncExecution && !asyncExecStatusIsTerminal(operation.status) ? 202 : 200)
        : runOnceRejectionStatus(rejectionReason);
    AsyncWebServerResponse* response = request->beginResponse(
        responseStatus, "application/json", responseBody);
    if (!submitted && responseStatus >= 429) response->addHeader("Retry-After", "2");
    HandlerUtils::sendWithClose(request, response);
}

void PeriphExecRouteHandler::handleGetRecentResults(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;

    int limit = 10;
    if (request->hasParam("limit")) {
        limit = request->getParam("limit")->value().toInt();
        if (limit < 1) limit = 1;
        if (limit > 20) limit = 20;
    }

    uint32_t operationFilter = 0;
    if (request->hasParam("operationId")) {
        operationFilter = static_cast<uint32_t>(
            strtoul(request->getParam("operationId")->value().c_str(), nullptr, 10));
    }
    const bool includeActions = operationFilter != 0 ||
        (request->hasParam("includeActions") &&
         request->getParam("includeActions")->value() == "true");

    auto results = PeriphExecManager::getInstance().getRecentResults();
    int total = static_cast<int>(results.size());
    int emitted = 0;
    String items;
    const size_t expectedItems = static_cast<size_t>(
        limit < total ? limit : total);
    items.reserve(expectedItems * (includeActions ? 768 : 320));

    for (int i = total - 1; i >= 0 && emitted < limit; --i) {
        const auto& result = results[static_cast<size_t>(i)];
        if (operationFilter != 0 && result.operationId != operationFilter) continue;
        if (emitted > 0) items += ",";

        unsigned long duration = 0;
        if (result.startTime != 0 && result.endTime != 0) {
            duration = result.endTime - result.startTime;
        }

        items += F("{\"operationId\":");
        items += String(result.operationId);
        items += F(",\"ruleId\":\"");
        items += escapePeriphExecJsonString(result.ruleId);
        items += F("\",\"ruleName\":\"");
        items += escapePeriphExecJsonString(result.ruleName);
        items += F("\",\"status\":");
        items += String(static_cast<int>(result.status));
        items += F(",\"statusName\":\"");
        items += asyncExecLegacyStatusName(result.status);
        items += F("\",\"state\":\"");
        items += asyncExecStatusName(result.status);
        items += F("\",\"terminal\":");
        items += asyncExecStatusIsTerminal(result.status) ? F("true") : F("false");
        items += F(",\"errorCode\":\"");
        items += asyncExecErrorName(result.errorCode);
        items += F("\",\"queuedAt\":");
        items += String(result.queuedAt);
        items += F(",\"startedAt\":");
        items += String(result.startTime);
        items += F(",\"endedAt\":");
        items += String(result.endTime);
        items += F(",\"deadlineAt\":");
        items += String(result.deadlineAt);
        items += F(",\"queuePosition\":");
        items += String(static_cast<unsigned int>(result.queuePosition));
        items += F(",\"async\":");
        items += result.asyncExecution ? F("true") : F("false");
        // Legacy timestamps retained for existing device tests and clients.
        items += F(",\"startTime\":");
        items += String(result.startTime);
        items += F(",\"endTime\":");
        items += String(result.endTime);
        items += F(",\"durationMs\":");
        items += String(duration);
        items += F(",\"reportCount\":");
        items += String(static_cast<unsigned int>(result.reportCount));
        items += F(",\"actionCount\":");
        items += String(static_cast<unsigned int>(result.actionCount));
        items += F(",\"actionTraceTruncated\":");
        items += result.actionTraceTruncated ? F("true") : F("false");
        if (includeActions) {
            items += F(",\"actions\":[");
            for (uint8_t actionIndex = 0;
                 actionIndex < result.actionTraceCount &&
                 actionIndex < ASYNC_EXEC_ACTION_TRACE_LIMIT;
                 ++actionIndex) {
                if (actionIndex > 0) items += ',';
                const auto& action = result.actionTraces[actionIndex];
                items += F("{\"index\":");
                items += String(static_cast<unsigned int>(action.index));
                items += F(",\"actionType\":");
                items += String(static_cast<unsigned int>(action.actionType));
                items += F(",\"targetPeriphId\":\"");
                items += escapePeriphExecJsonString(String(action.targetPeriphId));
                items += F("\",\"value\":\"");
                items += escapePeriphExecJsonString(String(action.valueSummary));
                items += F("\",\"success\":");
                items += action.success ? F("true") : F("false");
                items += F(",\"durationMs\":");
                items += String(action.durationMs);
                items += F(",\"detailCount\":");
                items += String(static_cast<unsigned int>(action.detailCount));
                items += F(",\"remark\":\"");
                items += escapePeriphExecJsonString(String(action.remark));
                items += F("\"}");
            }
            items += F("]");
        }
        items += F("}");
        emitted++;
    }

    String response;
    response.reserve(80 + items.length());
    response += F("{\"success\":true,\"total\":");
    response += String(total);
    response += F(",\"data\":[");
    response += items;
    response += F("]}");
    request->send(200, "application/json", response);
}

// ========== 获取静态事件列表 ==========

void PeriphExecRouteHandler::handleGetStaticEvents(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;

    String eventsJson = PeriphExecManager::getStaticEventsJson();
    HandlerUtils::sendJsonSuccess(request, eventsJson);
}

// ========== 获取动态事件列表（包含外设执行规则事件） ==========

void PeriphExecRouteHandler::handleGetDynamicEvents(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;

    if (HandlerUtils::rejectHeavyRequestOnPressure(request, "Dynamic event list", MemoryGuardLevel::CRITICAL, 5)) {
        return;
    }
    if (HandlerUtils::checkLowMemory(request, 6144)) return;

    PeriphExecManager& mgr = PeriphExecManager::getInstance();
    String eventsJson = mgr.getDynamicEventsJson();
    if (eventsJson.isEmpty()) {
        AsyncWebServerResponse* response = request->beginResponse(200, "application/json",
            F("{\"success\":true,\"degraded\":true,\"message\":\"Low memory - dynamic events temporarily reduced\",\"data\":[]}"));
        HandlerUtils::sendWithClose(request, response);
        return;
    }
    HandlerUtils::sendJsonSuccess(request, eventsJson);
}

// ========== 获取事件分类列表 ==========

void PeriphExecRouteHandler::handleGetEventCategories(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;

    String categoriesJson = PeriphExecManager::getEventCategoriesJson();
    HandlerUtils::sendJsonSuccess(request, categoriesJson);
}

// ========== 获取触发类型列表 ==========

void PeriphExecRouteHandler::handleGetTriggerTypes(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;

    String triggerTypesJson = PeriphExecManager::getValidTriggerTypes();
    HandlerUtils::sendJsonSuccess(request, triggerTypesJson);
}

// ========== JSON → PeriphExecRule 解析辅助 ==========

// 从 JsonVariant 读取整数，兼容 JSON 数值和字符串两种格式
static int jsonInt(JsonVariant v, int def) {
    if (v.is<int>()) return v.as<int>();
    if (v.is<const char*>()) return String(v.as<const char*>()).toInt();
    return def;
}

void PeriphExecRouteHandler::parseRuleFromJson(JsonObject& obj, PeriphExecRule& rule) {
    // 注意：as<String>() 对缺失字段返回 "null" 字符串（serializeJson 序列化结果），
    // 必须用 | "" 或 is<>() 保护所有字符串提取
    if (obj["name"].is<const char*>()) rule.name = obj["name"] | "";
    if (obj["enabled"].is<bool>()) rule.enabled = obj["enabled"].as<bool>();
    else if (obj["enabled"].is<const char*>()) rule.enabled = String(obj["enabled"].as<const char*>()) == "1";
    rule.execMode = jsonInt(obj["execMode"], rule.execMode);

    // 数据转换管道
    rule.protocolType = jsonInt(obj["protocolType"], rule.protocolType);
    if (obj["scriptContent"].is<const char*>()) {
        rule.scriptContent = obj["scriptContent"] | "";
        // 兼容处理：旧版序列化可能将 null 存储为字符串 "null"
        if (rule.scriptContent == "null") rule.scriptContent = "";
    }
    if (obj["reportAfterExec"].is<bool>()) rule.reportAfterExec = obj["reportAfterExec"].as<bool>();
    if (obj["allowPanelControl"].is<bool>()) rule.allowPanelControl = obj["allowPanelControl"].as<bool>();
    if (obj["recordHistory"].is<bool>()) rule.recordHistory = obj["recordHistory"].as<bool>();

    // 解析 triggers 数组
    if (obj["triggers"].is<JsonArray>()) {
        rule.triggers.clear();
        JsonArray trigArr = obj["triggers"].as<JsonArray>();
        for (JsonObject tObj : trigArr) {
            ExecTrigger t;
            t.triggerType = jsonInt(tObj["triggerType"], 0);
            t.triggerPeriphId = tObj["triggerPeriphId"] | "";
            t.operatorType = jsonInt(tObj["operatorType"], 0);
            t.compareValue = tObj["compareValue"] | "";
            t.timerMode = jsonInt(tObj["timerMode"], 0);
            t.intervalSec = intervalSecondsFromInput(
                jsonInt(tObj["intervalSec"], 60));
            t.timePoint = tObj["timePoint"] | "";
            t.eventId = tObj["eventId"] | "";
            // 轮询触发通信参数
            t.pollResponseTimeout = jsonInt(tObj["pollResponseTimeout"], 1000);
            t.pollMaxRetries = jsonInt(tObj["pollMaxRetries"], 2);
            t.pollInterPollDelay = jsonInt(tObj["pollInterPollDelay"], 100);
            t.lastTriggerTime = 0;
            t.triggerCount = 0;
            rule.triggers.push_back(t);
        }
    }

    // 解析 actions 数组
    if (obj["actions"].is<JsonArray>()) {
        rule.actions.clear();
        JsonArray actArr = obj["actions"].as<JsonArray>();
        for (JsonObject aObj : actArr) {
            ExecAction a;
            a.targetPeriphId = aObj["targetPeriphId"] | "";
            a.actionType = jsonInt(aObj["actionType"], 0);
            a.actionValue = aObj["actionValue"] | "";
            a.useReceivedValue = aObj["useReceivedValue"] | false;
            a.syncDelayMs = jsonInt(aObj["syncDelayMs"], 0);
            a.execMode = jsonInt(aObj["execMode"], 0);
            rule.actions.push_back(a);
        }
    }
}

namespace {
bool parseStrictLong(const String& text, long& value) {
    if (text.isEmpty()) return false;
    char* end = nullptr;
    value = strtol(text.c_str(), &end, 10);
    return end && end != text.c_str() && *end == '\0';
}

bool parseClockTime(const String& value) {
    if (value.length() != 5 || value[2] != ':') return false;
    if (!isDigit(value[0]) || !isDigit(value[1]) ||
        !isDigit(value[3]) || !isDigit(value[4])) return false;
    const int hour = value.substring(0, 2).toInt();
    const int minute = value.substring(3).toInt();
    return hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59;
}

bool isOutputPeripheral(PeripheralType type) {
    return type == PeripheralType::GPIO_DIGITAL_OUTPUT ||
           type == PeripheralType::GPIO_PWM_OUTPUT ||
           type == PeripheralType::GPIO_ANALOG_OUTPUT;
}

bool isSensorPeripheral(PeripheralType type) {
    return type == PeripheralType::GPIO_DIGITAL_INPUT ||
           type == PeripheralType::GPIO_DIGITAL_INPUT_PULLUP ||
           type == PeripheralType::GPIO_DIGITAL_INPUT_PULLDOWN ||
           type == PeripheralType::GPIO_ANALOG_INPUT ||
           type == PeripheralType::ADC ||
           type == PeripheralType::SENSOR_GENERIC ||
           type == PeripheralType::ONE_WIRE ||
           type == PeripheralType::RADAR_SENSOR ||
           type == PeripheralType::RF_MODULE;
}

bool actionNeedsPeripheral(uint8_t actionType) {
    return !(actionType == 6 || actionType == 7 || actionType == 8 ||
             actionType == 9 || actionType == 15 || actionType == 18 ||
             actionType == 21 || actionType == 22 || actionType == 23 ||
             actionType == 31 || actionType == 32);
}

bool actionTargetMatches(uint8_t actionType, const PeripheralConfig& config) {
    if (actionType <= 3 || actionType == 13 || actionType == 14 ||
        actionType == 30) return isOutputPeripheral(config.type);
    if (actionType == 4) {
        return config.type == PeripheralType::GPIO_PWM_OUTPUT ||
               config.type == PeripheralType::GPIO_DIGITAL_OUTPUT;
    }
    if (actionType == 5) {
        return config.type == PeripheralType::DAC ||
               config.type == PeripheralType::GPIO_ANALOG_OUTPUT;
    }
    if (actionType == 11) return config.type == PeripheralType::NEO_PIXEL;
    if (actionType == 12) return config.type == PeripheralType::STEPPER_MOTOR;
    if (actionType == 16 || actionType == 17) return config.isModbusPeripheral();
    if (actionType == 19) return isSensorPeripheral(config.type);
    if (actionType >= 24 && actionType <= 26) {
        return config.type == PeripheralType::SEVEN_SEGMENT_TM1637;
    }
    if (actionType == 27) return config.type == PeripheralType::LCD;
    if (actionType == 28) return config.type == PeripheralType::RF_MODULE;
    if (actionType == 29) return config.type == PeripheralType::UART;
    return true;
}

void writeRuleCandidate(JsonObject target, const PeriphExecRule& rule) {
    target["id"] = rule.id;
    target["name"] = rule.name;
    target["enabled"] = rule.enabled;
    target["execMode"] = rule.execMode;
    target["protocolType"] = rule.protocolType;
    target["scriptContent"] = rule.scriptContent;
    target["reportAfterExec"] = rule.reportAfterExec;
    target["allowPanelControl"] = rule.allowPanelControl;
    target["recordHistory"] = rule.recordHistory;

    JsonArray triggers = target["triggers"].to<JsonArray>();
    for (const ExecTrigger& trigger : rule.triggers) {
        JsonObject item = triggers.add<JsonObject>();
        item["triggerType"] = trigger.triggerType;
        item["triggerPeriphId"] = trigger.triggerPeriphId;
        item["operatorType"] = trigger.operatorType;
        item["compareValue"] = trigger.compareValue;
        item["timerMode"] = trigger.timerMode;
        item["intervalSec"] = trigger.intervalSec;
        item["timePoint"] = trigger.timePoint;
        item["eventId"] = trigger.eventId;
        item["pollResponseTimeout"] = trigger.pollResponseTimeout;
        item["pollMaxRetries"] = trigger.pollMaxRetries;
        item["pollInterPollDelay"] = trigger.pollInterPollDelay;
    }

    JsonArray actions = target["actions"].to<JsonArray>();
    for (const ExecAction& action : rule.actions) {
        JsonObject item = actions.add<JsonObject>();
        item["targetPeriphId"] = action.targetPeriphId;
        item["actionType"] = action.actionType;
        item["actionValue"] = action.actionValue;
        item["useReceivedValue"] = action.useReceivedValue;
        item["syncDelayMs"] = action.syncDelayMs;
        item["execMode"] = action.execMode;
    }
}
}  // namespace

void PeriphExecRouteHandler::handlePreflightRuleJson(AsyncWebServerRequest* request,
                                                     JsonVariant& json) {
    if (!ctx->requireAuth(request)) return;
    if (!ctx->requireDeveloperMode(request)) return;

    JsonObject payload = json.as<JsonObject>();
    if (payload.isNull()) {
        ctx->sendBadRequest(request, "Rule preflight body must be a JSON object");
        return;
    }

    String operation = payload["operation"] | "add";
    operation.toLowerCase();
    if (operation != "add" && operation != "update") {
        ctx->sendBadRequest(request, "Invalid operation; expected add or update");
        return;
    }

    JsonObject ruleObject = payload["rule"].is<JsonObject>()
        ? payload["rule"].as<JsonObject>() : payload;
    PeriphExecManager& manager = PeriphExecManager::getInstance();
    PeriphExecRule current;
    PeriphExecRule candidate;
    String id = ruleObject["id"] | "";

    if (operation == "update") {
        if (id.isEmpty()) {
            ctx->sendBadRequest(request, "Rule ID is required for update preflight");
            return;
        }
        if (!manager.getRuleSnapshot(id, current)) {
            ctx->sendError(request, 404, "Rule not found");
            return;
        }
        candidate = current;
    } else if (!id.isEmpty() && manager.getRuleSnapshot(id, current)) {
        ctx->sendError(request, 409, "Rule ID already exists");
        return;
    }

    parseRuleFromJson(ruleObject, candidate);
    if (!id.isEmpty()) candidate.id = id;

    JsonDocument response;
    response["success"] = true;
    JsonObject data = response["data"].to<JsonObject>();
    data["operation"] = operation;
    JsonArray issues = data["issues"].to<JsonArray>();
    size_t errorCount = 0;
    size_t warningCount = 0;
    uint8_t risk = 0;  // 0=low, 1=medium, 2=high
    auto addIssue = [&](const char* code, const char* severity,
                        const String& path, const String& message,
                        uint8_t issueRisk = 0) {
        JsonObject issue = issues.add<JsonObject>();
        issue["code"] = code;
        issue["severity"] = severity;
        issue["path"] = path;
        issue["message"] = message;
        if (strcmp(severity, "error") == 0) ++errorCount;
        else ++warningCount;
        if (issueRisk > risk) risk = issueRisk;
    };

    if (operation == "add" &&
        manager.getRuleCount() >= FastBee::ResourceProfile::MAX_PERIPH_EXEC_RULES) {
        addIssue("RULE_LIMIT_REACHED", "error", "rule",
                 String("Rule limit reached for ") + FastBee::ResourceProfile::NAME);
    }
    if (candidate.id.length() > FastBee::ResourceProfile::MAX_RULE_ID_LEN) {
        addIssue("RULE_ID_TOO_LONG", "error", "rule.id", "Rule ID exceeds profile limit");
    }
    if (candidate.name.isEmpty()) {
        addIssue("RULE_NAME_REQUIRED", "error", "rule.name", "Rule name is required");
    } else if (candidate.name.length() > FastBee::ResourceProfile::MAX_RULE_NAME_LEN) {
        addIssue("RULE_NAME_TOO_LONG", "error", "rule.name", "Rule name exceeds profile limit");
    }
    if (candidate.execMode > 1) {
        addIssue("INVALID_EXEC_MODE", "error", "rule.execMode", "execMode must be 0 or 1");
    }
    if (candidate.protocolType != 0 && candidate.protocolType != 1 &&
        candidate.protocolType != 3 && candidate.protocolType != 4 &&
        candidate.protocolType != 5) {
        addIssue("INVALID_PROTOCOL_TYPE", "error", "rule.protocolType",
                 "Unsupported protocolType");
    }
    if (candidate.scriptContent.length() > FastBee::ResourceProfile::MAX_SCRIPT_CONTENT_LEN) {
        addIssue("SCRIPT_TOO_LONG", "error", "rule.scriptContent",
                 "Rule scriptContent exceeds profile limit");
    }

    if (candidate.triggers.empty()) {
        addIssue("TRIGGER_REQUIRED", "error", "rule.triggers",
                 "At least one trigger is required");
    } else if (candidate.triggers.size() > MAX_TRIGGERS_PER_RULE) {
        addIssue("TOO_MANY_TRIGGERS", "error", "rule.triggers",
                 String("Maximum triggers per rule: ") + MAX_TRIGGERS_PER_RULE);
    }

    PeripheralManager& peripherals = PeripheralManager::getInstance();
    for (size_t index = 0; index < candidate.triggers.size(); ++index) {
        const ExecTrigger& trigger = candidate.triggers[index];
        const String path = String("rule.triggers[") + index + "]";
        const bool supportedType = trigger.triggerType == 0 || trigger.triggerType == 1 ||
                                   trigger.triggerType == 4 || trigger.triggerType == 5;
        if (!supportedType) {
            addIssue("INVALID_TRIGGER_TYPE", "error", path + ".triggerType",
                     trigger.triggerType == 6
                         ? "Rule-chain trigger type 6 is not implemented by the runtime"
                         : "Supported trigger types are 0, 1, 4 and 5");
            continue;
        }
        if ((trigger.triggerType == 0 || trigger.triggerType == 5) &&
            trigger.operatorType > static_cast<uint8_t>(ExecOperator::NOT_CONTAIN)) {
            addIssue("INVALID_OPERATOR", "error", path + ".operatorType",
                     "operatorType must be between 0 and 9");
        }
        if (trigger.triggerType == 0 && trigger.triggerPeriphId.isEmpty()) {
            addIssue("TRIGGER_SOURCE_REQUIRED", "error", path + ".triggerPeriphId",
                     "Platform trigger requires a source identifier");
        }
        if (trigger.triggerType == 1) {
            if (trigger.timerMode > 1) {
                addIssue("INVALID_TIMER_MODE", "error", path + ".timerMode",
                         "timerMode must be interval(0) or daily(1)");
            } else if (trigger.timerMode == 0 &&
                       (trigger.intervalSec < PERIPH_EXEC_TIMER_INTERVAL_MIN_SEC ||
                        trigger.intervalSec > PERIPH_EXEC_INTERVAL_MAX_SEC)) {
                addIssue("INVALID_TIMER_INTERVAL", "error", path + ".intervalSec",
                         "Timer interval must be between 1 and 86400 seconds");
            } else if (trigger.timerMode == 1 && !parseClockTime(trigger.timePoint)) {
                addIssue("INVALID_DAILY_TIME", "error", path + ".timePoint",
                         "Daily time must use HH:MM (00:00-23:59)");
            }
        }
        if (trigger.triggerType == 4) {
            if (trigger.eventId.isEmpty()) {
                addIssue("EVENT_ID_REQUIRED", "error", path + ".eventId",
                         "Event trigger requires eventId");
            } else if (!findStaticEvent(trigger.eventId.c_str())) {
                PeripheralConfig eventSource;
                if (!peripherals.getPeripheralSnapshot(trigger.eventId, eventSource) ||
                    eventSource.type != PeripheralType::DEVICE_EVENT) {
                    addIssue("UNVERIFIED_EVENT_ID", "warning", path + ".eventId",
                             "Event is not a built-in or configured DEVICE_EVENT source", 1);
                }
            }
        }
        if (trigger.triggerType == 5) {
            if (trigger.intervalSec < PERIPH_EXEC_POLL_INTERVAL_MIN_SEC ||
                trigger.intervalSec > PERIPH_EXEC_INTERVAL_MAX_SEC) {
                addIssue("INVALID_POLL_INTERVAL", "error", path + ".intervalSec",
                         "Poll interval must be between 5 and 86400 seconds");
            }
            if (trigger.pollResponseTimeout < 100 || trigger.pollResponseTimeout > 5000) {
                addIssue("INVALID_POLL_TIMEOUT", "error", path + ".pollResponseTimeout",
                         "Poll timeout must be between 100 and 5000 ms");
            }
            if (trigger.pollMaxRetries > 3) {
                addIssue("INVALID_POLL_RETRIES", "error", path + ".pollMaxRetries",
                         "Poll retries must be between 0 and 3");
            }
            if (trigger.pollInterPollDelay < 20 || trigger.pollInterPollDelay > 1000) {
                addIssue("INVALID_POLL_DELAY", "error", path + ".pollInterPollDelay",
                         "Inter-poll delay must be between 20 and 1000 ms");
            }
        }
    }

    if (candidate.actions.empty()) {
        addIssue("ACTION_REQUIRED", "error", "rule.actions",
                 "At least one action is required");
    } else if (candidate.actions.size() > MAX_ACTIONS_PER_RULE) {
        addIssue("TOO_MANY_ACTIONS", "error", "rule.actions",
                 String("Maximum actions per rule: ") + MAX_ACTIONS_PER_RULE);
    }

    for (size_t index = 0; index < candidate.actions.size(); ++index) {
        const ExecAction& action = candidate.actions[index];
        const String path = String("rule.actions[") + index + "]";
        if (action.actionType > 34 || action.actionType == 20) {
            addIssue("INVALID_ACTION_TYPE", "error", path + ".actionType",
                     "actionType must be 0-34 and cannot use reserved slot 20");
            continue;
        }
        if (action.execMode > 1) {
            addIssue("INVALID_ACTION_EXEC_MODE", "error", path + ".execMode",
                     "Action execMode must be 0 or 1");
        }
        if (action.syncDelayMs > 10000) {
            addIssue("ACTION_DELAY_TOO_LONG", "error", path + ".syncDelayMs",
                     "Action delay cannot exceed 10000 ms");
        }
        if (action.actionValue.length() > FastBee::ResourceProfile::MAX_ACTION_VALUE_LEN) {
            addIssue("ACTION_VALUE_TOO_LONG", "error", path + ".actionValue",
                     "Action value exceeds profile limit");
        }

        if (action.actionType == 6 || action.actionType == 7 || action.actionType == 9) {
            addIssue("DANGEROUS_SYSTEM_ACTION", "warning", path,
                     action.actionType == 7 ? "Factory reset requires explicit confirmation"
                                            : "Restart or OTA requires explicit confirmation",
                     2);
        }
#if !FASTBEE_ENABLE_OTA
        if (action.actionType == 9) {
            addIssue("OTA_NOT_COMPILED", "error", path + ".actionType",
                     "OTA action is not compiled into this firmware");
        }
#endif
        if (action.actionType == 15 &&
            !FastBee::RuntimeFeatureManager::instance().shouldEnable("commandScript")) {
            addIssue("SCRIPT_FEATURE_DISABLED", "error", path + ".actionType",
                     "Command script feature is disabled");
        }
        if ((action.actionType == 16 || action.actionType == 17 || action.actionType == 18) &&
            !FastBee::RuntimeFeatureManager::instance().shouldEnable("modbus")) {
            addIssue("MODBUS_FEATURE_DISABLED", "error", path + ".actionType",
                     "Modbus feature is disabled");
        }
#if !FASTBEE_ENABLE_REMOTE_RESOURCES
        if (action.actionType == 33 || action.actionType == 34) {
            addIssue("REMOTE_RESOURCES_NOT_COMPILED", "error", path + ".actionType",
                     "Remote resources are not compiled into this firmware");
        }
#endif
        if (action.actionType == 33) {
            if (action.targetPeriphId.isEmpty()) {
                addIssue("REMOTE_TARGET_REQUIRED", "error", path + ".targetPeriphId",
                         "Remote channel ID is required");
            }
            if ((!action.useReceivedValue && action.actionValue != "on" &&
                 action.actionValue != "off" && action.actionValue != "trigger") ||
                (action.useReceivedValue && !action.actionValue.isEmpty())) {
                addIssue("INVALID_REMOTE_VALUE", "error", path + ".actionValue",
                         "Remote channel value must be on, off or trigger");
            }
        }
        if (action.actionType == 34 &&
            (action.targetPeriphId.isEmpty() || !action.actionValue.isEmpty() ||
             action.useReceivedValue)) {
            addIssue("INVALID_REMOTE_READ", "error", path,
                     "Remote read requires a source ID and no action value");
        }

        if (actionNeedsPeripheral(action.actionType)) {
            if (action.targetPeriphId.isEmpty()) {
                addIssue("ACTION_TARGET_REQUIRED", "error", path + ".targetPeriphId",
                         "This action requires a target peripheral");
            } else {
                PeripheralConfig target;
                if (!peripherals.getPeripheralSnapshot(action.targetPeriphId, target)) {
                    addIssue("ACTION_TARGET_NOT_FOUND", "error", path + ".targetPeriphId",
                             "Target peripheral does not exist");
                } else {
                    if (!actionTargetMatches(action.actionType, target)) {
                        addIssue("ACTION_TARGET_TYPE_MISMATCH", "error", path + ".targetPeriphId",
                                 String("Action is incompatible with ") + getPeripheralTypeName(target.type));
                    }
                    if (!target.enabled) {
                        addIssue("ACTION_TARGET_DISABLED", "warning", path + ".targetPeriphId",
                                 "Target peripheral is currently disabled", 1);
                    }
                }
            }
        }

        if ((action.actionType == 22 || action.actionType == 23)) {
            PeriphExecRule targetRule;
            if (action.targetPeriphId.isEmpty() ||
                !manager.getRuleSnapshot(action.targetPeriphId, targetRule)) {
                addIssue("TARGET_RULE_NOT_FOUND", "error", path + ".targetPeriphId",
                         "Rule-control action requires an existing target rule");
            }
        }
        if (action.actionType == 21 && action.targetPeriphId.isEmpty()) {
            addIssue("EVENT_TARGET_REQUIRED", "error", path + ".targetPeriphId",
                     "Trigger-event action requires an event ID");
        }
        if (action.actionType == 31 || action.actionType == 32) {
            if (action.actionValue != "webServer" && action.actionValue != "mdns") {
                addIssue("FEATURE_NOT_ALLOWED", "error", path + ".actionValue",
                         "Feature actions may only target webServer or mdns");
            }
            if (risk < 1) risk = 1;
        }

        long numeric = 0;
        if (!action.useReceivedValue && (action.actionType == 2 || action.actionType == 3)) {
            if (!parseStrictLong(action.actionValue, numeric) || numeric < 50 || numeric > 60000) {
                addIssue("INVALID_EFFECT_INTERVAL", "error", path + ".actionValue",
                         "Blink/breathe interval must be 50-60000 ms");
            }
        }
        if (!action.useReceivedValue && action.actionType == 4) {
            if (!parseStrictLong(action.actionValue, numeric) || numeric < 0 || numeric > 65535) {
                addIssue("INVALID_PWM_VALUE", "error", path + ".actionValue",
                         "PWM value must be between 0 and 65535");
            }
        }
        if (!action.useReceivedValue && action.actionType == 5) {
            if (!parseStrictLong(action.actionValue, numeric) || numeric < 0 || numeric > 255) {
                addIssue("INVALID_DAC_VALUE", "error", path + ".actionValue",
                         "DAC value must be between 0 and 255");
            }
        }
        if (action.actionType == 15 && action.actionValue.isEmpty()) {
            addIssue("SCRIPT_ACTION_EMPTY", "error", path + ".actionValue",
                     "Script action requires command content");
        }
        if (action.actionType == 18) {
            JsonDocument poll;
            if (action.actionValue.isEmpty() || deserializeJson(poll, action.actionValue) ||
                (!poll["poll"].is<JsonArray>() && !poll["ctrl"].is<JsonArray>())) {
                addIssue("INVALID_MODBUS_POLL", "error", path + ".actionValue",
                         "Modbus poll value must be JSON with poll or ctrl array");
            }
        }
        if (action.actionType == 19) {
            JsonDocument sensor;
            if (action.actionValue.isEmpty() || deserializeJson(sensor, action.actionValue)) {
                addIssue("INVALID_SENSOR_CONFIG", "error", path + ".actionValue",
                         "Sensor read actionValue must be valid JSON");
            }
        }
    }

    const bool allowed = errorCount == 0;
    data["allowed"] = allowed;
    data["requiresConfirmation"] = allowed && (warningCount > 0 || risk > 0);
    data["riskLevel"] = risk == 2 ? "high" : (risk == 1 ? "medium" : "low");
    data["errorCount"] = errorCount;
    data["warningCount"] = warningCount;
    JsonObject limits = data["limits"].to<JsonObject>();
    limits["maxRules"] = FastBee::ResourceProfile::MAX_PERIPH_EXEC_RULES;
    limits["maxTriggersPerRule"] = MAX_TRIGGERS_PER_RULE;
    limits["maxActionsPerRule"] = MAX_ACTIONS_PER_RULE;
    if (operation == "update") writeRuleCandidate(data["current"].to<JsonObject>(), current);
    writeRuleCandidate(data["candidate"].to<JsonObject>(), candidate);
    response["allowed"] = allowed;
    HandlerUtils::sendJsonStream(request, response);
}

// ========== JSON body: 新增规则 ==========

void PeriphExecRouteHandler::handleAddRuleJson(AsyncWebServerRequest* request, JsonVariant& json) {
    if (!ctx->requireAuth(request)) return;
    if (!ctx->requireDeveloperMode(request)) return;

    JsonObject obj = json.as<JsonObject>();
    PeriphExecRule rule;
    // as<String>() 对缺失字段返回 "null" 字符串，用 | 运算符安全提取
    const char* idStr = obj["id"] | (const char*)nullptr;
    if (idStr && idStr[0] != '\0') rule.id = idStr;
    parseRuleFromJson(obj, rule);

    if (rule.name.isEmpty()) {
        ctx->sendError(request, 400, "Rule name is required");
        return;
    }

    PeriphExecManager& mgr = PeriphExecManager::getInstance();
    String errorMsg;
    if (mgr.addRuleAndSave(rule, errorMsg)) {
        ctx->sendSuccess(request, "Rule added");
    } else {
        ctx->sendError(request, ruleMutationErrorStatus(errorMsg, 400),
                       ruleMutationErrorMessage(errorMsg, "Failed to add rule"));
    }
}

// ========== 获取设备控制列表（按动作类型分组） ==========

void PeriphExecRouteHandler::handleGetControls(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;

    // This is the device-control dashboard's aggregate read model. Keep the
    // execution-rule CRUD and run endpoints available when the optional page
    // is disabled, but do not spend RAM building its dedicated payload.
    if (!FastBee::RuntimeFeatureManager::instance().shouldEnable("deviceControl")) {
        ctx->sendError(request, 503, "Device control disabled by runtime feature config");
        return;
    }

    if (shouldDegradeControlsResponse()) {
        sendDegradedControlsResponse(request);
        return;
    }

    PeriphExecManager& mgr = PeriphExecManager::getInstance();
    PeripheralManager& pm = PeripheralManager::getInstance();

    // Avoid copying the sensor cache when the current rules cannot produce a
    // sensor control. A rule mutation between this probe and the response can
    // only omit a transient cached value; the control definition stays valid.
    bool needsSensorCache = false;
    {
        MutexGuard probeLock(mgr.getRulesMutex(), pdMS_TO_TICKS(50));
        if (!probeLock.isLocked()) {
            sendDegradedControlsResponse(request);
            return;
        }
        for (const auto& pair : mgr.getRules()) {
            const auto& rule = pair.second;
            if (rule.enabled && !rule.actions.empty() &&
                rule.actions[0].actionType ==
                    static_cast<uint8_t>(ExecActionType::ACTION_SENSOR_READ)) {
                needsSensorCache = true;
                break;
            }
        }
    }

    std::map<String, PeriphExecManager::SensorReadCache> sensorCache;
    if (needsSensorCache) {
        sensorCache = mgr.getSensorReadCacheCopy();
        if (shouldDegradeControlsResponse()) {
            sendDegradedControlsResponse(request);
            return;
        }
    }

    std::vector<GroupedControlItem> items;
    bool buildFailed = false;
    const bool commandScriptEnabled =
        FastBee::RuntimeFeatureManager::instance().shouldEnable("commandScript");
    {
        MutexGuard lock(mgr.getRulesMutex(), pdMS_TO_TICKS(50));
        if (!lock.isLocked()) {
            sendDegradedControlsResponse(request);
            return;
        }
        auto& ruleMap = mgr.getRules();
        items.reserve(ruleMap.size());

        // Iterating by group preserves the existing response order without
        // accumulating six group Strings and then copying all six into a
        // seventh, contiguous response buffer.
        for (uint8_t group = 0; group < CONTROL_GROUP_COUNT && !buildFailed; ++group) {
            for (const auto& pair : ruleMap) {
                const auto& rule = pair.second;
                if (!rule.enabled || rule.actions.empty()) continue;

                const int actionType = rule.actions[0].actionType;
#if !FASTBEE_ENABLE_OTA
                if (actionType == static_cast<int>(ExecActionType::ACTION_SYS_OTA)) continue;
#endif
                if (actionType == static_cast<int>(ExecActionType::ACTION_SCRIPT) &&
                    !commandScriptEnabled) {
                    continue;
                }
                if (static_cast<uint8_t>(controlGroupForAction(actionType)) != group) continue;

                GroupedControlItem item;
                item.group = group;
                if (!buildControlItemJson(rule, pm, sensorCache, item.json)) {
                    buildFailed = true;
                    break;
                }
                items.emplace_back(std::move(item));

                if (shouldDegradeControlsResponse()) {
                    buildFailed = true;
                    break;
                }
            }
        }
    }

    if (buildFailed) {
        items.clear();
        items.shrink_to_fit();
        sendDegradedControlsResponse(request);
        return;
    }
    if (!sendControlsChunked(request, std::move(items))) {
        HandlerUtils::sendJsonError(
            request, 503, "Device controls temporarily unavailable (stream allocation failed)", 5);
    }
}

// ========== JSON body: 更新规则 ==========

void PeriphExecRouteHandler::handleUpdateRuleJson(AsyncWebServerRequest* request, JsonVariant& json) {
    if (!ctx->requireAuth(request)) return;
    if (!ctx->requireDeveloperMode(request)) return;

    JsonObject obj = json.as<JsonObject>();
    // as<String>() 对缺失字段返回 "null" 字符串，用 | 运算符安全提取
    const char* idStr = obj["id"] | "";
    String id = idStr;
    if (id.isEmpty()) {
        ctx->sendError(request, 400, "Rule ID is required");
        return;
    }

    PeriphExecManager& mgr = PeriphExecManager::getInstance();
    PeriphExecRule existing;
    if (!mgr.getRuleSnapshot(id, existing)) {
        ctx->sendError(request, 404, "Rule not found");
        return;
    }

    // 基于现有规则更新
    PeriphExecRule rule = existing;
    parseRuleFromJson(obj, rule);
    rule.id = id;

    String errorMsg;
    if (mgr.updateRuleAndSave(id, rule, errorMsg)) {
        ctx->sendSuccess(request, "Rule updated");
    } else {
        ctx->sendError(request, ruleMutationErrorStatus(errorMsg, 400),
                       ruleMutationErrorMessage(errorMsg, "Failed to update rule"));
    }
}

// ========== 完整序列化单条规则 ==========

String PeriphExecRouteHandler::serializeRuleFull(const PeriphExecRule& rule) {
    PeripheralManager& pm = PeripheralManager::getInstance();

    JsonDocument ruleDoc;
    ruleDoc["id"] = rule.id;
    ruleDoc["name"] = rule.name;
    ruleDoc["enabled"] = rule.enabled;
    ruleDoc["execMode"] = rule.execMode;

    // 触发器数组
    JsonArray trigArr = ruleDoc["triggers"].to<JsonArray>();
    for (const auto& t : rule.triggers) {
        JsonObject tObj = trigArr.add<JsonObject>();
        tObj["triggerType"] = t.triggerType;
        tObj["triggerPeriphId"] = t.triggerPeriphId;
        tObj["operatorType"] = t.operatorType;
        tObj["compareValue"] = t.compareValue;
        tObj["timerMode"] = t.timerMode;
        tObj["intervalSec"] = t.intervalSec;
        tObj["timePoint"] = t.timePoint;
        tObj["eventId"] = t.eventId;
        // 轮询触发通信参数
        tObj["pollResponseTimeout"] = t.pollResponseTimeout;
        tObj["pollMaxRetries"] = t.pollMaxRetries;
        tObj["pollInterPollDelay"] = t.pollInterPollDelay;
        // 运行时状态
        tObj["lastTriggerTime"] = t.lastTriggerTime;
        tObj["triggerCount"] = t.triggerCount;

        // 关联触发外设名称
        if (!t.triggerPeriphId.isEmpty()) {
            PeripheralConfig periph;
            if (pm.getPeripheralSnapshot(t.triggerPeriphId, periph)) {
                tObj["triggerPeriphName"] = periph.name;
            }
        }
    }

    // 动作数组
    JsonArray actArr = ruleDoc["actions"].to<JsonArray>();
    for (const auto& a : rule.actions) {
        JsonObject aObj = actArr.add<JsonObject>();
        aObj["targetPeriphId"] = a.targetPeriphId;
        aObj["actionType"] = a.actionType;
        aObj["actionValue"] = a.actionValue;
        aObj["useReceivedValue"] = a.useReceivedValue;
        aObj["syncDelayMs"] = a.syncDelayMs;
        aObj["execMode"] = a.execMode;
        if (!a.targetPeriphId.isEmpty()) {
            PeripheralConfig periph;
            if (pm.getPeripheralSnapshot(a.targetPeriphId, periph)) {
                aObj["targetPeriphName"] = periph.name;
                aObj["targetPeriphType"] = static_cast<int>(periph.type);
            }
        }
    }

    // 数据转换管道
    ruleDoc["protocolType"] = rule.protocolType;
    ruleDoc["scriptContent"] = rule.scriptContent;

    // 数据上报控制
    ruleDoc["reportAfterExec"] = rule.reportAfterExec;
    ruleDoc["allowPanelControl"] = rule.allowPanelControl;
    ruleDoc["recordHistory"] = rule.recordHistory;

    String output;
    serializeJson(ruleDoc, output);
    return output;
}

#endif // FASTBEE_ENABLE_PERIPH_EXEC
