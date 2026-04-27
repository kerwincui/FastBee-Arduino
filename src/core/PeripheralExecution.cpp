#include "core/PeripheralExecution.h"
#include <cstring>

const EventDef STATIC_EVENTS[] = {
    // WiFi 事件
    {EventType::EVENT_WIFI_CONNECTED, "wifi_connected", "WiFi连接成功", "WiFi"},
    {EventType::EVENT_WIFI_DISCONNECTED, "wifi_disconnected", "WiFi断开连接", "WiFi"},
    {EventType::EVENT_WIFI_CONN_FAILED, "wifi_conn_failed", "WiFi连接失败", "WiFi"},
    // MQTT 事件
    {EventType::EVENT_MQTT_CONNECTED, "mqtt_connected", "MQTT连接成功", "MQTT"},
    {EventType::EVENT_MQTT_DISCONNECTED, "mqtt_disconnected", "MQTT断开连接", "MQTT"},
    {EventType::EVENT_MQTT_CONN_FAILED, "mqtt_conn_failed", "MQTT连接失败", "MQTT"},
    {EventType::EVENT_MQTT_ENABLED, "mqtt_enabled", "MQTT协议启用", "MQTT"},
    // 网络模式事件
    {EventType::EVENT_NET_MODE_AP, "net_mode_ap", "网络模式切换为AP", "网络"},
    {EventType::EVENT_NET_MODE_STA, "net_mode_sta", "网络模式切换为STA", "网络"},
    // 协议事件
    {EventType::EVENT_MODBUS_RTU_ENABLED, "modbus_rtu_enabled", "Modbus RTU启用", "协议"},
    {EventType::EVENT_MODBUS_TCP_ENABLED, "modbus_tcp_enabled", "Modbus TCP启用", "协议"},
    {EventType::EVENT_TCP_ENABLED, "tcp_enabled", "TCP协议启用", "协议"},
    {EventType::EVENT_HTTP_ENABLED, "http_enabled", "HTTP协议启用", "协议"},
    {EventType::EVENT_COAP_ENABLED, "coap_enabled", "CoAP协议启用", "协议"},
    // 系统服务事件
    {EventType::EVENT_NTP_SYNCED, "ntp_synced", "NTP时间同步完成", "系统"},
    {EventType::EVENT_OTA_START, "ota_start", "OTA升级开始", "系统"},
    {EventType::EVENT_OTA_SUCCESS, "ota_success", "OTA升级成功", "系统"},
    {EventType::EVENT_OTA_FAILED, "ota_failed", "OTA升级失败", "系统"},
    // 规则引擎事件
    {EventType::EVENT_RULE_EXEC_TIME, "rule_exec_time", "规则脚本执行时间", "规则"},
    {EventType::EVENT_RULE_EXEC_ERROR, "rule_exec_error", "规则脚本执行错误", "规则"},
    // 系统状态事件
    {EventType::EVENT_SYSTEM_BOOT, "system_boot", "系统启动", "系统"},
    {EventType::EVENT_SYSTEM_READY, "system_ready", "系统就绪", "系统"},
    {EventType::EVENT_SYSTEM_ERROR, "system_error", "系统错误", "系统"},
    {EventType::EVENT_FACTORY_RESET, "factory_reset", "恢复出厂设置", "系统"},
    // 按键事件
    {EventType::EVENT_BUTTON_CLICK, "button_click", "按键单击", "按键"},
    {EventType::EVENT_BUTTON_DOUBLE_CLICK, "button_double_click", "按键双击", "按键"},
    {EventType::EVENT_BUTTON_LONG_PRESS_2S, "button_long_press_2s", "按键长按2秒", "按键"},
    {EventType::EVENT_BUTTON_LONG_PRESS_5S, "button_long_press_5s", "按键长按5秒", "按键"},
    {EventType::EVENT_BUTTON_LONG_PRESS_10S, "button_long_press_10s", "按键长按10秒", "按键"},
    {EventType::EVENT_BUTTON_PRESS, "button_press", "按键按下", "按键"},
    {EventType::EVENT_BUTTON_RELEASE, "button_release", "按键释放", "按键"},
    // 外设执行事件
    {EventType::EVENT_PERIPH_EXEC_COMPLETED, "periph_exec_completed", "外设执行完成", "外设执行"},
    // 数据事件
    {EventType::EVENT_DATA_RECEIVE, "data_receive", "数据接收", "数据"},
    {EventType::EVENT_DATA_REPORT, "data_report", "数据上报", "数据"},
    // 终止标记
    {EventType::NONE, nullptr, nullptr, nullptr}
};

const size_t STATIC_EVENTS_COUNT = sizeof(STATIC_EVENTS) / sizeof(STATIC_EVENTS[0]) - 1; // 不含终止标记

size_t getStaticEventCount() {
    return STATIC_EVENTS_COUNT;
}

const EventDef* findStaticEvent(const char* id) {
    for (size_t i = 0; STATIC_EVENTS[i].id != nullptr; i++) {
        if (strcmp(STATIC_EVENTS[i].id, id) == 0) {
            return &STATIC_EVENTS[i];
        }
    }
    return nullptr;
}
