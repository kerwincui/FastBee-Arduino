#pragma once
#include <cstring>

namespace FastBee { namespace Screen {
inline const char* weatherStatusText(const char* reason, bool cached, bool stale) {
    const auto is = [reason](const char* code) { return reason && !std::strcmp(reason, code); };
    if (is("time_unsynced")) return "等待设备校时";
    if (is("memory_unavailable")) return cached ? "内存不足，显示缓存" : "内存不足，等待恢复";
    if (is("network_offline")) return cached ? "网络未连，显示缓存" : "网络未连接";
    if (is("authentication")) return "请检查天气密钥";
    if (is("location_invalid")) return "请检查天气城市";
    if (is("rate_limited")) return "天气额度受限";
    if (is("ota_paused")) return "升级中，暂停更新";
    if (is("tls_configuration")) return "天气证书配置错误";
    if (stale) return "天气已过期，重试中";
    if (!reason || !reason[0] || is("none")) return "心知天气";
    return cached ? "更新失败，显示缓存" : "天气暂不可用";
}
}}
