/**
 * @file WiFiModeGuard.h
 * @brief WiFi 模式 RAII 守卫：作用域结束自动恢复进入时的模式 + 审计日志
 *
 * 背景：WiFi.scanNetworks() 内部会 enableSTA(true) 把纯 AP 切成 APSTA，
 * 调用方易忘恢复导致 STA 接口残留、持续抽干 DRAM（实测 22KB→16KB→7KB），
 * 跌破 AsyncTCP tcp_accept 8KB 门控后 AP 模式 Web 新连接全被 abort。
 * 用此守卫在作用域结束（含 return/异常）时自动恢复，并打印 from→to 审计日志，
 * 便于后续从串口直接定位任何模式切换的元凶。
 *
 * 用法：
 *   {
 *       WiFiModeGuard guard("AP-probe-scan");
 *       int n = WiFi.scanNetworks(false, true);
 *       // ... 处理结果 ...
 *   }  // 析构自动恢复纯 AP，无需手写 WiFi.mode(WIFI_MODE_AP)
 */
#pragma once

#include <WiFi.h>

class WiFiModeGuard {
public:
    explicit WiFiModeGuard(const char* caller = nullptr)
        : _caller(caller), _entryMode(WiFi.getMode()), _restored(false) {}

    ~WiFiModeGuard() {
        restore();
    }

    /// 离开作用域时把 WiFi 模式恢复到进入时的模式
    void restore() {
        if (_restored) return;
        _restored = true;
        WiFiMode_t current = WiFi.getMode();
        if (current != _entryMode) {
            WiFi.mode(_entryMode);
            if (_caller) {
                Serial.printf("[WiFi-Mode] restore %d->%d (%s)\n",
                              (int)current, (int)_entryMode, _caller);
            }
        }
    }

    /// 调用方主动接管恢复（如已显式切到目标模式且不希望守卫改回）
    void release() { _restored = true; }

    WiFiMode_t entryMode() const { return _entryMode; }

private:
    const char* _caller;
    WiFiMode_t _entryMode;
    bool _restored;
};
