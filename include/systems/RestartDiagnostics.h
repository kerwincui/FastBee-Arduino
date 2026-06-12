/**
 * @file RestartDiagnostics.h
 * @brief 设备异常重启诊断模块
 * 
 * 功能说明：
 * 1. 启动时检测重启原因（看门狗、栈溢出、低内存、异常等）
 * 2. 重启前保存系统状态快照到 RTC_NOINIT 内存（软件重启间保留）
 * 3. 启动后读取并输出上次重启前的完整诊断信息
 * 4. 提供足够的时间戳和上下文信息帮助追踪问题根源
 * 
 * RTC_NOINIT_ATTR 数据在软件重启（ESP.restart()）间保留，
 * 但在硬件上电重置时会丢失，这是预期行为。
 */

#ifndef RESTART_DIAGNOSTICS_H
#define RESTART_DIAGNOSTICS_H

#include <Arduino.h>
#include <esp_system.h>

// ========== 重启前状态快照结构 ==========
// 存储在 RTC_NOINIT 内存中，软件重启间保留
struct PreRestartSnapshot {
    uint32_t magic;              // 魔数验证：0xFB_DIAG (FastBee Diagnostics)
    uint32_t timestamp;          // 重启前 millis() 值（运行时长 ms）
    
    // 内存状态
    uint32_t freeHeap;           // 空闲堆
    uint32_t minFreeHeap;        // 历史最小空闲堆
    uint32_t largestFreeBlock;   // 最大连续可用块
    uint8_t  heapFragmentation;  // 碎片率 (0-100%)
    
    // MemGuard 状态
    uint8_t  memGuardLevel;      // 0=NORMAL, 1=WARN, 2=SEVERE, 3=CRITICAL
    uint8_t  consecutiveLowMem;  // 连续低内存计数
    
    // 任务栈水位（关键任务）
    uint16_t loopTaskWatermark;     // loopTask 剩余栈 (bytes)
    uint16_t asyncTcpWatermark;     // async_tcp 剩余栈 (bytes)
    uint16_t mqttReconnWatermark;   // mqtt_reconn 剩余栈 (bytes)
    
    // 系统状态
    uint8_t  wifiConnected;      // WiFi 是否连接
    int8_t   wifiRssi;           // WiFi 信号强度
    uint8_t  mqttQueueDepth;     // MQTT 队列深度
    uint8_t  sseClientCount;     // SSE 客户端数
    uint8_t  activeRuleCount;    // 活跃执行规则数
    
    // 重启原因分类
    uint8_t  restartReason;      // 自定义重启原因码（见 RestartReason 枚举）
    char     restartContext[48]; // 重启上下文描述（如触发模块名）
    
    // 校验
    uint32_t checksum;           // 简单 XOR 校验
};

// ========== 自定义重启原因码 ==========
enum class RestartReason : uint8_t {
    UNKNOWN             = 0,   // 未知（首次上电或 RTC 数据无效）
    CRITICAL_LOW_MEMORY = 1,   // 连续低内存触发
    FRAMEWORK_LOW_MEMORY = 2,  // FastBeeFramework 内存保护触发
    USER_COMMAND        = 3,   // 用户通过命令/Web 主动重启
    OTA_UPDATE          = 4,   // OTA 更新后重启
    UNCAUGHT_EXCEPTION  = 5,   // 未捕获异常
    WATCHDOG_TIMEOUT    = 6,   // 看门狗超时（ESP 系统级）
    STACK_OVERFLOW      = 7,   // 栈溢出检测
    MEMORY_COMPACTION   = 8,   // 碎片化严重无法恢复
    PERIPHERAL_FAULT    = 9,   // 外设异常
    CONFIG_CORRUPTION   = 10,  // 配置文件损坏
};

// ========== RestartDiagnostics 类 ==========
class RestartDiagnostics {
public:
    /**
     * @brief 启动诊断：检测 ESP 重启原因 + 读取上次保存的状态快照
     *        应在 setup() 中尽早调用（Serial 初始化后立即调用）
     */
    static void logBootDiagnostics();
    
    /**
     * @brief 重启前保存状态快照到 RTC 内存
     *        应在任何 ESP.restart() 调用之前调用
     * @param reason  自定义重启原因
     * @param context 上下文描述（最多 47 字符）
     */
    static void savePreRestartState(RestartReason reason, const char* context = nullptr);
    
    /**
     * @brief 获取 ESP 系统重启原因的可读字符串
     */
    static const char* getResetReasonString(esp_reset_reason_t reason);
    
    /**
     * @brief 获取自定义重启原因的可读字符串
     */
    static const char* getRestartReasonString(RestartReason reason);
    
    /**
     * @brief 检查上次重启是否为异常重启
     * @return true = 异常重启（WDT/panic/brownout等），false = 正常重启或首次上电
     */
    static bool wasAbnormalRestart();
    
    /**
     * @brief 获取上次保存的快照（如果有效）
     * @param out 输出快照结构
     * @return true = 快照有效，false = 无效（首次上电或数据损坏）
     */
    static bool getLastSnapshot(PreRestartSnapshot& out);

private:
    static uint32_t calculateChecksum(const PreRestartSnapshot& snap);
    static void collectCurrentState(PreRestartSnapshot& snap);
    static void printSnapshot(const PreRestartSnapshot& snap);
};

#endif // RESTART_DIAGNOSTICS_H
