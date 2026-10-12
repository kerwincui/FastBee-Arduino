#include "protocols/MqttDiagnosticJson.h"

namespace FastBee {
namespace {
struct DiagnosticText {
    int code;
    const char* name;
    const char* category;
    const char* message;
    const char* reason;
};
constexpr DiagnosticText kDiagnostics[] = {
    {0, "OK", "none", "", nullptr},
    {-12, "MQTT_CONFIG_INVALID", "config",
     "MQTT 配置未加载或格式无效，请检查网络配置", nullptr},
    {-11, "MQTT_CLIENT_INIT_FAILED", "resource",
     "MQTT 客户端未初始化，可能是内存不足或启动失败", nullptr},
    {-11, "MQTT_CLIENT_INIT_FAILED", "resource",
     "MQTT 客户端启动失败，可能是内存不足或配置无效", nullptr},
    {0, "MQTT_LIFECYCLE_BUSY", "pending",
     "MQTT 实例切换中，运行状态暂未确认", nullptr},
    {-13, "MQTT_DISABLED_BY_MEMORY", "resource",
     "设备内存紧张，MQTT 已自动禁用，请恢复内存后重新启用", nullptr},
    {-14, "MQTT_STOPPED_BY_MEMORY", "resource",
     "设备内存紧张，MQTT 已暂时停止，内存恢复后将自动重连", nullptr},
    {-12, "MQTT_CONFIG_INVALID", "config",
     "MQTT 配置未加载或格式无效，请检查 Broker 地址和端口", nullptr},
    {-11, "MQTT_CLIENT_INIT_FAILED", "runtime",
     "MQTT 未完成重建，请检查配置、传输与资源状态", nullptr},
    {-8, "MQTT_AES_CLIENT_ID_INVALID", "config",
     "AES clientId requires E&deviceNum&productId&userId format",
     "加密认证客户端ID格式无效，应为 E&设备编号&产品编号&用户ID"},
    {-10, "MQTT_CONFIG_SAVE_FAILED", "config",
     "Failed to preserve or stage MQTT test configuration",
     "MQTT 测试配置保全或暂存失败；已有恢复记录保留"},
};
static_assert(sizeof(kDiagnostics) / sizeof(kDiagnostics[0]) ==
              static_cast<uint8_t>(MqttDiagnostic::Count), "Diagnostic table mismatch");
// Share the serializer's real instructions and unwind metadata across REST,
// reconnect/test replies and SSE; do not let LTO clone the key-writing sequence.
// The only two instantiations preserve the original JsonObject versus nested
// proxy allocation behavior; no per-diagnostic or per-call-site specialization.
template <typename Target>
#if defined(__clang__)
__attribute__((noinline))
#elif defined(__GNUC__)
__attribute__((noinline, noclone))
#endif
void writeDiagnostic(Target data, MqttDiagnostic diagnostic, uint8_t fields) {
    const uint8_t index = static_cast<uint8_t>(diagnostic);
    if (index >= static_cast<uint8_t>(MqttDiagnostic::Count)) return;
    const DiagnosticText& info = kDiagnostics[index];
    if (fields & MqttDiagnosticFields::LastError) data["lastError"] = info.code;
    if (fields & MqttDiagnosticFields::Error) data["error"] = info.code;
    data["lastErrorCode"] = info.code;
    data["lastErrorName"] = JsonString(info.name, true);
    data["lastErrorClass"] = JsonString(info.category, true);
    const bool staticMessage = (fields & MqttDiagnosticFields::CopyMessage) == 0;
    const JsonString message(info.message, staticMessage);
    if (fields & MqttDiagnosticFields::LastMessage) data["lastErrorMessage"] = message;
    data["errorMessage"] = message;
    if (fields & MqttDiagnosticFields::Reason)
        data["statusReason"] = JsonString(info.reason ? info.reason : info.message, staticMessage);
}
}

void writeMqttDiagnostic(JsonObject data, MqttDiagnostic diagnostic, uint8_t fields) {
    writeDiagnostic(data, diagnostic, fields);
}

void writeMqttDiagnosticData(JsonDocument& doc, MqttDiagnostic diagnostic, uint8_t fields) {
    writeDiagnostic(doc["data"], diagnostic, fields);
}
} // namespace FastBee
