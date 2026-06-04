#ifndef PERIPH_EXEC_MANAGER_H
#define PERIPH_EXEC_MANAGER_H

#include <Arduino.h>
#include <map>
#include <set>
#include <vector>
#include <memory>
#include <functional>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include "PeripheralExecution.h"
#include "AsyncExecTypes.h"
#include "ResourceProfile.h"
#include "utils/StaticPoolAllocator.h"

// 前向声明
class PeriphExecExecutor;
class PeriphExecScheduler;
class PeriphExecWorkerPool;

// ===== 协议层回调类型定义（解耦 core → protocols 依赖） =====

// MQTT 连接状态检查
using MqttIsConnectedCallback = std::function<bool()>;
// MQTT 队列上报数据
using MqttQueueReportCallback = std::function<bool(const String& reportData)>;
// Modbus sensorId 构建 (deviceIndex, channel) → sensorId string
using ModbusBuildSensorIdCallback = std::function<String(uint8_t deviceIndex, uint16_t channel)>;
// Modbus 未匹配 sensorId 直接控制 (sensorId, value, reportArr) → true=已处理
using ModbusDirectControlCallback = std::function<bool(const String& sensorId, const String& value, JsonArray& reportArr)>;
// Modbus 动态事件列表填充
using ModbusDynamicEventsCallback = std::function<void(JsonArray& arr)>;
// MQTT 设备事件上报回调：(eventId, eventName, eventData) → true=已发布
using MqttEventPublishCallback = std::function<bool(const String& eventId, const String& eventName, const String& eventData)>;

class PeriphExecManager {
public:
    static PeriphExecManager& getInstance();

    // 禁止拷贝
    PeriphExecManager(const PeriphExecManager&) = delete;
    PeriphExecManager& operator=(const PeriphExecManager&) = delete;

    // 初始化（加载配置、创建信号量）
    bool initialize();

    // ========== CRUD ==========
    bool addRule(const PeriphExecRule& rule);
    bool addRule(const PeriphExecRule& rule, String& errorMsg);
    bool updateRule(const String& id, const PeriphExecRule& rule);
    bool updateRule(const String& id, const PeriphExecRule& rule, String& errorMsg);
    bool removeRule(const String& id);
    PeriphExecRule* getRule(const String& id);
    std::vector<PeriphExecRule> getAllRules() const;
    bool enableRule(const String& id);
    bool disableRule(const String& id);
    size_t getRuleCount() const { return rules.size(); }

    // ========== 持久化 ==========
    bool saveConfiguration();
    bool loadConfiguration();

    // ========== 执行引擎（委托给 Executor） ==========

    // 获取执行引擎
    PeriphExecExecutor* getExecutor() { return _executor.get(); }

    // ========== 调度器（委托给 Scheduler） ==========

    // 获取调度器
    PeriphExecScheduler* getScheduler() { return _scheduler.get(); }

    // MQTT 消息处理入口（由 ProtocolManager messageCallback 调用）
    void handleMqttMessage(const String& topic, const String& message);

    // 轮询数据处理入口（由 Modbus/其他本地数据源回调调用）
    void handlePollData(const String& source, const String& data);

    // 数据下发命令处理：匹配 trigger.triggerPeriphId == item["id"]，执行规则并返回上报 JSON
    // 注意：此方法始终同步执行，立即返回结果
    String handleDataCommand(const String& message);

    // 定时器检查（由 TaskManager 定时任务调用）
    void checkTimers();

    // ========== 触发事件（委托给 Scheduler） ==========

    // 触发事件（由各系统模块调用）
    void triggerEvent(EventType eventType, const String& eventData = "");

    // 触发事件（通过事件ID）
    void triggerEventById(const String& eventId, const String& eventData = "");

    // 获取静态事件列表（用于前端配置）
    static String getStaticEventsJson();

    // 获取动态事件列表（包含外设执行规则事件）
    String getDynamicEventsJson();

    // Modbus 一次性读取回调（由 ProtocolManager 注册）
    void setModbusReadCallback(std::function<String(const String&)> callback);

    // Modbus 原始 HEX 帧透传回调（由 ProtocolManager 注册）
    void setModbusRawSendCallback(std::function<String(const String&)> callback);

    // ========== 协议层回调注入（解耦 core → protocols） ==========
    void setMqttIsConnectedCallback(MqttIsConnectedCallback cb);
    void setMqttQueueReportCallback(MqttQueueReportCallback cb);
    void setModbusBuildSensorIdCallback(ModbusBuildSensorIdCallback cb);
    void setModbusDirectControlCallback(ModbusDirectControlCallback cb);
    void setModbusDynamicEventsCallback(ModbusDynamicEventsCallback cb);
    void setMqttEventPublishCallback(MqttEventPublishCallback cb);

    // 通知 MQTT 层发布设备事件（由 Scheduler triggerEvent/triggerEventById 调用）
    bool notifyMqttEventPublish(const String& eventId, const String& eventName, const String& eventData);

    // SSE 传感器数据广播回调
    using SensorSSECallback = std::function<void(const String& data)>;
    void setSensorSSECallback(SensorSSECallback cb) { _sensorSSECb = cb; }
    void notifySensorDataSSE(const String& key, const String& label, const String& value, const String& unit);

    // ========== 异步执行 ==========

    // 记录异步执行结果（由异步任务完成时调用）
    void recordResult(const AsyncExecResult& result);

    // 获取最近的执行结果（最多保留 MAX_ASYNC_TASKS * 2 条）
    std::vector<AsyncExecResult> getRecentResults();

    // ========== 手动执行 ==========

    // 手动执行规则（用于"执行一次"按钮，可携带用户输入值）
    bool runOnce(const String& id, const String& receivedValue = "");

    // ========== 动作执行后数据上报（委托给 Scheduler） ==========

    // 尝试上报设备数据（检查网络和协议连接状态后上报）
    // 返回：true=上报成功或无需上报，false=上报失败
    bool tryReportDeviceData();

    // ========== 触发事件检测（委托给 Scheduler） ==========

    // 按键事件检测（由 TaskManager 定时任务调用，用于检测按键类型事件）
    void checkButtonEvents();

    // 查询是否存在启用中的按键事件规则（用于按键单击/双击检测优化）
    // periphId: 按键外设 ID；eventId: 事件 ID（如 "button_double_click"）
    bool hasButtonEventRule(const String& periphId, const String& eventId);

    // 查询是否存在启用中的数据源规则（用于 UART 等本地数据源按需轮询）
    bool hasDataSourceRule(const String& sourceId);

    // ========== 配置辅助方法（委托给 Scheduler） ==========

    // 获取有效触发类型列表（JSON数组格式）
    static String getValidTriggerTypes();

    // 获取触发事件分类列表（JSON数组格式，用于前端下拉选择）
    static String getEventCategoriesJson();

    // 获取外设支持的有效动作类型列表（JSON数组格式）
    // periphId: 外设ID，为空则返回所有动作类型
    static String getValidActionTypes(const String& periphId = "");

    // ========== 条件评估（公共静态方法，供 Manager/Scheduler/Executor 共用） ==========

    // 通用条件评估：根据操作符比较 value 和 compareValue
    // EQ/NEQ 自动检测数值/字符串类型，数值走浮点比较，非数值走字符串比较
    static bool evaluateCondition(const String& value, uint8_t op, const String& compareValue);

    // ========== 内部接口（供 Executor 和 Scheduler 使用） ==========

    // 检查是否已初始化
    bool isInitialized() const { return _rulesMutex != nullptr; }

    // 缓存待上报数据（MQTT 未连接时由 Executor 调用）
    void queuePendingReport(const String& reportData);

    // 重试待上报数据（在 checkTimers 中调用，MQTT 恢复后自动重试）
    void retryPendingReports();

    // 获取待上报缓存数量
    size_t getPendingReportCount() const;

    // 获取规则互斥量
    SemaphoreHandle_t getRulesMutex() const { return _rulesMutex; }

    // 获取运行中规则互斥量
    SemaphoreHandle_t getRunningRulesMutex() const { return _runningRulesMutex; }

    // 获取规则列表（需在持有 rulesMutex 时访问）
    // 使用 RuleMap typedef 指向封装了自定义 Allocator 的 map 类型
    using RuleMapAllocator = FastBee::PooledAllocator<std::pair<const String, PeriphExecRule>, 192, 32>;
    using RuleMap = std::map<String, PeriphExecRule, std::less<String>, RuleMapAllocator>;
    RuleMap& getRules() { return rules; }

    // 运行中规则ID集合 / 失败退避表（T2：同样走 SmallNodePool）
    using RunningRuleSet = std::set<String, std::less<String>,
                                    FastBee::SmallNodeAllocator<String>>;
    using FailureBackoffMap = std::map<String, unsigned long, std::less<String>,
                                        FastBee::SmallNodeAllocator<std::pair<const String, unsigned long>>>;

    // 获取运行中规则ID集合
    RunningRuleSet& getRunningRuleIds() { return _runningRuleIds; }

    // 获取失败退避记录
    FailureBackoffMap& getFailureBackoff() { return _failureBackoff; }

    // 获取任务槽信号量
    SemaphoreHandle_t getTaskSlotSemaphore() const { return _taskSlotSemaphore; }

    // 获取执行结果互斥量
    SemaphoreHandle_t getResultsMutex() const { return _resultsMutex; }

    // 获取执行结果列表
    std::vector<AsyncExecResult>& getExecutionResults() { return executionResults; }

    // 检查是否应该异步执行
    bool shouldRunAsync() const;

    // 异步分发规则执行
    void dispatchAsync(const PeriphExecRule& rule, const String& receivedValue);

    // 单任务执行（由 WorkerPool 在 worker 线程内调用）
    // 包含执行规则、清理 _runningRuleIds、归还信号量、归还对象池等完整生命周期
    void executeWorkerJob(AsyncExecContext* ctx);

    // 执行规则的所有动作（供异步任务调用）
    std::vector<ActionExecResult> executeAllActions(const PeriphExecRule& rule, const String& receivedValue, bool suppressReport = false);

    // 同步执行并触发完成事件（用于同步降级路径，确保链式执行正常工作）
    void executeSyncWithCompletion(const PeriphExecRule& rule, const String& receivedValue);

    // 定时触发检查（内部使用）
    void checkTimerTriggers(unsigned long now, bool modbusAvailable);

    // 事件匹配分发（内部使用）
    void dispatchEventMatchedRules(const String& eventId, const String& eventData);

    // 外设执行事件分发（内部使用）
    void dispatchPeriphExecEvent(const String& ruleId, const String& eventData);

    // 按键事件分发（内部使用）
    void dispatchButtonEventRules(const String& eventId, const String& periphId);

    // MQTT消息匹配处理（内部使用）
    void processMqttMessageMatch(JsonArray& arr);

    // 轮询数据匹配处理（内部使用）
    void processPollDataMatch(JsonArray& arr, const String& source);

    // 数据命令匹配处理（内部使用）
    String processDataCommandMatch(JsonArray& cmdArr, const std::vector<int>& processedIndices);

    // 获取动态事件列表（内部实现）
    String getDynamicEventsJsonInternal();

private:
    PeriphExecManager() = default;

    // ========== 子模块 ==========
    bool ruleNeedsModbus(const PeriphExecRule& rule) const;
    bool ruleHasPollCollectionAction(const PeriphExecRule& rule) const;
    bool shouldAvoidSyncFallback(const PeriphExecRule& rule) const;
    void sanitizeTriggerForSafety(ExecTrigger& trigger, bool hasPollCollectionAction, const String& ruleName) const;
    void sanitizeRuleForSafety(PeriphExecRule& rule) const;
    unsigned long getPollTriggerCooldownMs(const PeriphExecRule& rule, const String& source) const;
    bool shouldThrottlePollIngress(const String& source, unsigned long now);

    // 按规则ID安全分发（锁内拷贝规则，锁外调用dispatchAsync，避免死锁）
    void dispatchByRuleId(const String& ruleId, const String& receivedValue);

    std::unique_ptr<PeriphExecExecutor> _executor;
    std::unique_ptr<PeriphExecScheduler> _scheduler;

    // ========== 规则存储 ==========
    // 内存优化（T3）：使用静态池分配器消除 std::map 节点频繁分配造成的堆碎片
    // 参数选型：BlockSize=192 (PeriphExecRule 节点 ~128字节、留余量)，BlockCount=32 (典型规则数 5~30 条，2倍冗余)
    // 总 RAM 固定开销 = 192 * 32 = 6KB；池耗尽时自动 fallback malloc，不影响功能
    // 类型别名已在 public 区声明为 RuleMapAllocator / RuleMap
    RuleMap rules;

    // ========== FreeRTOS 同步原语 ==========
    SemaphoreHandle_t _rulesMutex = nullptr;       // 保护 rules map 的互斥量
    SemaphoreHandle_t _taskSlotSemaphore = nullptr; // 并发任务槽计数信号量
    SemaphoreHandle_t _resultsMutex = nullptr;      // 保护 executionResults 的互斥量

    // ========== 异步执行结果 ==========
    std::vector<AsyncExecResult> executionResults;

    // ========== Modbus 读取回调 ==========
    std::function<String(const String&)> _modbusReadCallback;

    // ========== Modbus 原始帧透传回调 ==========
    std::function<String(const String&)> _modbusRawSendCallback;

    // ========== 协议层回调（解耦注入） ==========
    MqttIsConnectedCallback _mqttIsConnectedCb;
    MqttQueueReportCallback _mqttQueueReportCb;
    ModbusBuildSensorIdCallback _modbusBuildSensorIdCb;
    ModbusDirectControlCallback _modbusDirectControlCb;
    ModbusDynamicEventsCallback _modbusDynamicEventsCb;
    MqttEventPublishCallback _mqttEventPublishCb;
    SensorSSECallback _sensorSSECb;

    // ========== 任务运行状态跟踪（T2：高频增删，走 SmallNodePool） ==========
    RunningRuleSet    _runningRuleIds;     // 正在运行的规则ID集合
    FailureBackoffMap _failureBackoff;     // 规则失败后的退避时间戳
    std::map<String, unsigned long, std::less<String>,
             FastBee::SmallNodeAllocator<std::pair<const String, unsigned long>>> _runningStartTime;  // 规则进入运行集合的时间戳（自愈用）
    unsigned long _lastBackoffCleanupTime = 0;  // 上次 _failureBackoff 过期清理时间
    unsigned long _lastDiagLogTime = 0;         // 上次诊断日志输出时间
    SemaphoreHandle_t _runningRulesMutex = nullptr;   // 保护 _runningRuleIds 的互斥量
    SemaphoreHandle_t _pollIngressMutex = nullptr;    // 保护轮询注入节流状态
    std::map<String, unsigned long, std::less<String>,
             FastBee::SmallNodeAllocator<std::pair<const String, unsigned long>>> _pollSourceLastAccepted;   // 最近一次接受的轮询数据时间
    std::map<String, unsigned long, std::less<String>,
             FastBee::SmallNodeAllocator<std::pair<const String, unsigned long>>> _pollSourceLastThrottleLog; // 最近一次节流日志时间

    // ========== 按键规则缓存（无锁，仅主循环线程读写）——T2：走 SmallNodePool ==========
    std::set<String, std::less<String>,
             FastBee::SmallNodeAllocator<String>> _buttonEventCache;  // 存储 "eventId" 或 "periphId:eventId"
    void rebuildButtonEventCache();      // 规则变更时重建缓存

    // ========== 数据源倒排索引（无锁，仅主循环线程读写） ==========
    std::set<String, std::less<String>,
             FastBee::SmallNodeAllocator<String>> _dataSourceCache;  // 启用中的数据源 ID 集合
    void rebuildDataSourceCache();       // 规则变更时重建索引

    // ========== 传感器读取值缓存（供 controls API 和 SSE 使用） ==========
public:
    struct SensorReadCache {
        String label;           // 显示标签
        String value;           // 格式化后的数值字符串
        String unit;            // 单位
        unsigned long timestamp; // 更新时间 millis()
    };
    static constexpr size_t SENSOR_CACHE_MAX_ENTRIES = FastBee::ResourceProfile::SENSOR_CACHE_MAX_ENTRIES;   // 最大缓存条目数
    static constexpr unsigned long SENSOR_CACHE_TTL_MS = 120000; // 缓存 TTL: 120s

    void updateSensorReadCache(const String& key, const String& label, const String& value, const String& unit);
    std::map<String, SensorReadCache> getSensorReadCacheCopy() const;  // 线程安全的拷贝访问
    void evictStaleSensorCache();  // 淘汰过期条目（由 checkTimers 定期调用）
private:
    std::map<String, SensorReadCache> _sensorReadCache;  // key = periphId + "_" + dataField
    mutable SemaphoreHandle_t _sensorCacheMutex = nullptr; // 保护传感器缓存的互斥量

    // ========== 异步执行上下文对象池 ==========
    AsyncExecContextPool _contextPool;          // 固定大小的上下文对象池

    // ========== 常驻 Worker 任务池（D 方案：消除运行期 xTaskCreate 碎片）==========
    std::unique_ptr<PeriphExecWorkerPool> _workerPool;

    // ========== 待上报数据缓存（MQTT 未连接时缓存，恢复后重试） ==========
    static const size_t MAX_PENDING_REPORTS = 5;           // 最大缓存条数
    static const unsigned long PENDING_REPORT_RETRY_MS = 5000;  // 重试间隔 5秒
    std::vector<String> _pendingReports;                    // 待上报数据缓存
    unsigned long _lastPendingReportRetry = 0;              // 上次重试时间



    // ID 生成
    String generateUniqueId();

    // 友元声明，允许 Executor 和 Scheduler 访问私有成员
    friend class PeriphExecExecutor;
    friend class PeriphExecScheduler;
};

#endif // PERIPH_EXEC_MANAGER_H
