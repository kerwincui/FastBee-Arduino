#pragma once
#include <cstring>

namespace FastBee { namespace Screen {

// Maps panel error codes to short Chinese phrases for on-screen display.
// Human-readable text (e.g. existing Chinese messages) passes through so
// richer diagnostics are never truncated to a generic label; unrecognized
// bare machine codes fall back to a readable label instead of leaking raw
// English identifiers from firmware/Web drift.
inline const char* panelErrorText(const char* error) {
    if (!error || !error[0]) return "";
    const auto is = [error](const char* code) { return !std::strcmp(error, code); };
    if (is("none")) return "";
    if (is("binding_unconfigured")) return "未绑定数据源";
    if (is("low_memory")) return "内存不足";
    if (is("deadline_exceeded")) return "超时";
    if (is("action_failed")) return "动作失败";
    if (is("incomplete_actions")) return "部分动作未完成";
    if (is("cancelled_by_system")) return "被系统取消";
    if (is("worker_queue_full")) return "队列已满";
    if (is("uart_source_unconfirmed")) return "UART 未确认";
    if (is("rule_revision_unconfirmed")) return "规则版本未确认";
    bool machineCode = true;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(error); *p; ++p) {
        const bool lower = *p >= 'a' && *p <= 'z';
        const bool upper = *p >= 'A' && *p <= 'Z';
        const bool digit = *p >= '0' && *p <= '9';
        if (!lower && !upper && !digit && *p != '_') {
            machineCode = false;
            break;
        }
    }
    return machineCode ? "未知错误" : error;
}

// Maps the system.network status value to a short Chinese phrase. Same
// pass-through/fallback policy as panelErrorText.
inline const char* panelNetworkStatusText(const char* status) {
    if (!status || !status[0]) return "";
    const auto is = [status](const char* code) { return !std::strcmp(status, code); };
    if (is("connected")) return "已连接";
    if (is("disconnected")) return "未连接";
    if (is("connecting")) return "连接中";
    if (is("ap_mode")) return "热点模式";
    if (is("failed")) return "连接失败";
    if (is("unknown")) return "未知状态";
    bool machineCode = true;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(status); *p; ++p) {
        const bool lower = *p >= 'a' && *p <= 'z';
        const bool upper = *p >= 'A' && *p <= 'Z';
        const bool digit = *p >= '0' && *p <= '9';
        if (!lower && !upper && !digit && *p != '_') {
            machineCode = false;
            break;
        }
    }
    return machineCode ? "未知状态" : status;
}
}}
