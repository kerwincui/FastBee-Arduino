#ifndef RULE_SCRIPT_H
#define RULE_SCRIPT_H

#include "core/FeatureFlags.h"
#if FASTBEE_ENABLE_RULE_SCRIPT

#include <Arduino.h>

// 规则脚本触发类型
enum class RuleScriptTrigger : uint8_t {
    DATA_RECEIVE = 0,   // 数据接收触发（协议数据到达时应用模板转换）
    DATA_REPORT  = 1    // 数据上报触发（协议数据发送前应用模板转换）
};

// 规则脚本协议类型（仅保留已实现的协议）
enum class RuleScriptProtocol : uint8_t {
    MQTT       = 0,
    MODBUS_RTU = 1
};

// 规则脚本数据结构
struct RuleScript {
    String id;                     // 唯一标识 (rs_<millis>)
    String name;                   // 显示名称
    bool enabled = true;           // 启用状态
    uint8_t triggerType = 0;       // RuleScriptTrigger: 0=接收, 1=上报
    uint8_t protocolType = 0;      // RuleScriptProtocol: 0=MQTT, 1=ModbusRTU
    String scriptContent;          // 纯文本模板 (${key} 占位符)
    String sourceTopic;            // 源主题过滤（空=匹配所有 MQTT 消息，支持 +/# 通配符）
    String targetTopic;            // 目标主题（空=不重定向，非空则转换后发布到此主题）

    // 运行时字段 (不持久化)
    unsigned long lastTriggerTime = 0;
    uint32_t triggerCount = 0;
};

// 配置文件路径
#define RULE_SCRIPT_CONFIG_FILE "/config/rule_scripts.json"

#endif // FASTBEE_ENABLE_RULE_SCRIPT

#endif // RULE_SCRIPT_H
