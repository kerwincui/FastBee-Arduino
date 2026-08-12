/**
 * @file test_ir_remote.cpp
 * @brief 红外遥控通用配置功能单元测试
 *
 * 测试内容（纯逻辑，镜像自 src，不依赖 IRremoteESP8266/LittleFS 硬件库）：
 * - parseIrCode        编码字符串解析（0x 前缀 / 大小写 / 边界 / 非法字符）
 * - isValidKeyName     按键名校验（字符集 / 长度边界）
 * - codeToHex          编码格式化（%08llX 补零）
 * - 按键映射 CRUD      upsert 去重更新 / delete / clear 彻底清除 / resolveKeyName 协议匹配 / MAX_IR_KEYS 上限
 * - 学习状态机         IDLE→LEARNING→CAPTURED / 超时 / 取消 / 超时钳位 / 剩余时间
 * - 事件数据构建        check() 中 eventData JSON 拼装（命中按键携带 key）
 * - 红外触发匹配        PeriphExecManager::dispatchEventMatchedRules 中 ir_code_received 的
 *                      compareValue 过滤（key:名称 / 协议:编码 / 纯编码 / 留空任意匹配）
 * - 接收器生命周期      stop() 资源释放契约：disable/清理恰好一次（回归双重 disableIRIn
 *                      导致定时器 NULL 崩溃的修复）
 * - 互斥锁并发契约      begin/stop 阻塞获锁、check 非阻塞跳过、所有分支释锁不泄漏、
 *                      互斥锁不可重入（回归 check/stop 竞态 use-after-free 防护）
 *
 * 说明：IRRemoteDriver.cpp / PeriphExecManager.cpp 依赖硬件解码库与文件系统，
 * native 环境无法直接编译，故本测试镜像其纯逻辑分支（与本仓库既有驱动测试一致的做法）。
 */

#include <unity.h>
#include <Arduino.h>
#include <ArduinoJson.h>
#include <vector>
#include <string>
#include <cstdio>
#include <cctype>

void test_ir_remote_group();

// ============================================================
// 常量（镜像自 include/peripherals/drivers/IRRemoteDriver.h）
// ============================================================
static constexpr size_t MIRROR_MAX_IR_KEYS = 40;
static constexpr unsigned long MIRROR_MIN_EVENT_INTERVAL_MS = 150;

enum class MirrorLearnState : uint8_t {
    IDLE = 0,
    LEARNING = 1,
    CAPTURED = 2,
    TIMEOUT = 3
};

// 大小写不敏感比较（mock String 未提供 equalsIgnoreCase）
static bool ciEquals(const String& a, const String& b) {
    if (a.length() != b.length()) return false;
    for (size_t i = 0; i < a.length(); ++i) {
        if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
    }
    return true;
}

// ============================================================
// 镜像 IRRemoteDriver.cpp 顶部静态工具函数
// ============================================================

// parseIrCode：将 "0x00FF6897" / "00FF6897" 解析为 uint64_t
static bool mirrorParseIrCode(const String& in, uint64_t& out) {
    String s = in;
    s.trim();
    if (s.length() == 0) return false;
    if (s.startsWith("0x") || s.startsWith("0X")) s = s.substring(2);
    if (s.length() == 0 || s.length() > 16) return false;
    uint64_t v = 0;
    for (size_t i = 0; i < s.length(); ++i) {
        char c = s[i];
        uint8_t d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return false;
        v = (v << 4) | d;
    }
    out = v;
    return true;
}

// isValidKeyName：非空、长度<=32、仅字母数字下划线短横线
static bool mirrorIsValidKeyName(const String& name) {
    if (name.length() == 0 || name.length() > 32) return false;
    for (size_t i = 0; i < name.length(); ++i) {
        char c = name[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-') continue;
        return false;
    }
    return true;
}

// codeToHex：uint64_t -> "0x%08llX"
static String mirrorCodeToHex(uint64_t value) {
    char hexBuf[20];
    snprintf(hexBuf, sizeof(hexBuf), "0x%08llX", (unsigned long long)value);
    return String(hexBuf);
}

// ============================================================
// 镜像按键映射存储（IRRemoteDriver 的 _keyMap + 持久化语义）
// ============================================================
struct MirrorKeyEntry {
    String name;
    uint64_t value;
    String protocol;
};

struct MirrorKeyStore {
    std::vector<MirrorKeyEntry> keyMap;
    bool irKeysInConfig = false;   // 模拟 periph_exec.json 中 irKeys 字段是否存在（旧版为独立 ir_keys.json）

    // 镜像 _persistKeyMap：空表则移除配置中的 irKeys 字段（避免残留），否则写入
    bool persist() {
        if (keyMap.empty()) {
            irKeysInConfig = false;
            return true;
        }
        irKeysInConfig = true;
        return true;
    }

    // 镜像 resolveKeyName：按 value 匹配，entry.protocol 非空时需协议一致
    String resolveKeyName(uint64_t value, const String& protocol) const {
        for (const auto& e : keyMap) {
            if (e.value != value) continue;
            if (e.protocol.length() > 0 && !ciEquals(e.protocol, protocol)) continue;
            return e.name;
        }
        return "";
    }

    // 镜像 upsertKey：同名更新（去重），否则新增；受 MAX_IR_KEYS 上限约束
    bool upsertKey(const String& name, const String& code, const String& protocol, String& err) {
        if (!mirrorIsValidKeyName(name)) { err = "invalid name"; return false; }
        uint64_t value = 0;
        if (!mirrorParseIrCode(code, value)) { err = "invalid code"; return false; }
        for (auto& e : keyMap) {
            if (e.name == name) {
                e.value = value;
                e.protocol = protocol;
                return persist();
            }
        }
        if (keyMap.size() >= MIRROR_MAX_IR_KEYS) { err = "limit"; return false; }
        keyMap.push_back(MirrorKeyEntry{name, value, protocol});
        return persist();
    }

    // 镜像 deleteKey
    bool deleteKey(const String& name) {
        bool removed = false;
        for (size_t i = 0; i < keyMap.size(); ++i) {
            if (keyMap[i].name == name) {
                keyMap.erase(keyMap.begin() + i);
                removed = true;
                break;
            }
        }
        if (!removed) return false;
        return persist();
    }

    // 镜像 clearKeys：清空内存表并移除配置中的 irKeys 字段（彻底清除，避免残留）
    bool clearKeys() {
        keyMap.clear();
        irKeysInConfig = false;
        return true;
    }
};

// ============================================================
// 镜像学习状态机（IRRemoteDriver startLearn/stopLearn/timeout/capture）
// 用可注入的 now 取代 millis()，以便确定性测试超时逻辑
// ============================================================
struct MirrorLearn {
    MirrorLearnState state = MirrorLearnState::IDLE;
    uint32_t deadline = 0;
    String code, protocol;

    void startLearn(uint32_t timeoutMs, uint32_t now) {
        if (timeoutMs < 1000) timeoutMs = 1000;
        if (timeoutMs > 60000) timeoutMs = 60000;
        state = MirrorLearnState::LEARNING;
        code = "";
        protocol = "";
        deadline = now + timeoutMs;
    }

    void stopLearn() {
        if (state == MirrorLearnState::LEARNING) state = MirrorLearnState::IDLE;
    }

    void updateTimeout(uint32_t now) {
        if (state == MirrorLearnState::LEARNING && (int32_t)(now - deadline) >= 0) {
            state = MirrorLearnState::TIMEOUT;
        }
    }

    // 镜像 check() 中学习模式下的捕获：仅 LEARNING 时接受
    void capture(const String& proto, const String& hex) {
        if (state == MirrorLearnState::LEARNING) {
            protocol = proto;
            code = hex;
            state = MirrorLearnState::CAPTURED;
        }
    }

    uint32_t remainingMs(uint32_t now) {
        updateTimeout(now);
        if (state != MirrorLearnState::LEARNING) return 0;
        int32_t rem = (int32_t)(deadline - now);
        return rem > 0 ? (uint32_t)rem : 0;
    }

    String stateName(uint32_t now) {
        updateTimeout(now);
        switch (state) {
            case MirrorLearnState::LEARNING: return "learning";
            case MirrorLearnState::CAPTURED: return "captured";
            case MirrorLearnState::TIMEOUT:  return "timeout";
            default:                         return "idle";
        }
    }
};

// ============================================================
// 镜像 check() 的 eventData JSON 构建
// ============================================================
static String mirrorBuildEventData(const String& protocol, const String& hex,
                                    int bits, const String& keyName) {
    String eventData = "{\"protocol\":\"" + protocol +
                       "\",\"code\":\"" + hex +
                       "\",\"bits\":" + String(bits);
    if (keyName.length() > 0) {
        eventData += ",\"key\":\"" + keyName + "\"";
    }
    eventData += "}";
    return eventData;
}

// ============================================================
// 镜像 PeriphExecManager::dispatchEventMatchedRules 中
// ir_code_received 的 compareValue 匹配分支
// 返回 true 表示该触发器命中（compareValue 为空视为任意匹配）
// ============================================================
static bool mirrorIrTriggerMatches(const String& compareValue, const String& eventData) {
    if (compareValue.length() == 0) return true;  // 空 compareValue -> 任意红外编码均触发

    bool codeMatched = false;
    JsonDocument evDoc;
    if (!deserializeJson(evDoc, eventData.c_str()) && evDoc.is<JsonObject>()) {
        String want = compareValue;
        want.trim();
        int keyColon = want.indexOf(':');
        String wantPrefix = keyColon > 0 ? want.substring(0, keyColon) : String("");
        if (ciEquals(wantPrefix, String("key"))) {
            String wantName = want.substring(keyColon + 1);
            wantName.trim();
            String evKey = evDoc["key"] | "";
            codeMatched = (wantName.length() > 0) && ciEquals(wantName, evKey);
        } else {
            String evCode = evDoc["code"] | "";
            String evProto = evDoc["protocol"] | "";
            if (keyColon > 0) {
                if (!ciEquals(wantPrefix, evProto)) want = "";
                else want = want.substring(keyColon + 1);
            }
            if (want.length() > 0) {
                if (!want.startsWith("0x") && !want.startsWith("0X")) want = "0x" + want;
                codeMatched = ciEquals(want, evCode);
            }
        }
    }
    return codeMatched;
}

// ============================================================
// TEST GROUP 1: parseIrCode 编码解析
// ============================================================

static void test_parse_code_with_0x_prefix() {
    uint64_t v = 0;
    TEST_ASSERT_TRUE(mirrorParseIrCode("0x00FF6897", v));
    TEST_ASSERT_EQUAL_UINT32(0x00FF6897UL, (uint32_t)v);
}

static void test_parse_code_without_prefix() {
    uint64_t v = 0;
    TEST_ASSERT_TRUE(mirrorParseIrCode("00FF6897", v));
    TEST_ASSERT_EQUAL_UINT32(0x00FF6897UL, (uint32_t)v);
}

static void test_parse_code_lowercase_and_uppercase_prefix() {
    uint64_t v1 = 0, v2 = 0;
    TEST_ASSERT_TRUE(mirrorParseIrCode("0xff", v1));
    TEST_ASSERT_TRUE(mirrorParseIrCode("0XFF", v2));
    TEST_ASSERT_EQUAL_UINT32(0xFFUL, (uint32_t)v1);
    TEST_ASSERT_EQUAL_UINT32(0xFFUL, (uint32_t)v2);
}

static void test_parse_code_trims_whitespace() {
    uint64_t v = 0;
    TEST_ASSERT_TRUE(mirrorParseIrCode("  0x1A  ", v));
    TEST_ASSERT_EQUAL_UINT32(0x1AUL, (uint32_t)v);
}

static void test_parse_code_rejects_empty() {
    uint64_t v = 0;
    TEST_ASSERT_FALSE(mirrorParseIrCode("", v));
    TEST_ASSERT_FALSE(mirrorParseIrCode("   ", v));
}

static void test_parse_code_rejects_bare_prefix() {
    uint64_t v = 0;
    TEST_ASSERT_FALSE(mirrorParseIrCode("0x", v));
}

static void test_parse_code_rejects_invalid_chars() {
    uint64_t v = 0;
    TEST_ASSERT_FALSE(mirrorParseIrCode("0xZZ", v));
    TEST_ASSERT_FALSE(mirrorParseIrCode("12G4", v));
}

static void test_parse_code_rejects_overlong() {
    uint64_t v = 0;
    // 去掉 0x 后 17 位十六进制超上限（>16）
    TEST_ASSERT_FALSE(mirrorParseIrCode("0x12345678901234567", v));
}

static void test_parse_code_full_64bit() {
    uint64_t v = 0;
    TEST_ASSERT_TRUE(mirrorParseIrCode("0xFFFFFFFFFFFFFFFF", v));
    TEST_ASSERT_TRUE(v == 0xFFFFFFFFFFFFFFFFULL);
}

// ============================================================
// TEST GROUP 2: isValidKeyName 按键名校验
// ============================================================

static void test_keyname_valid_examples() {
    TEST_ASSERT_TRUE(mirrorIsValidKeyName("power"));
    TEST_ASSERT_TRUE(mirrorIsValidKeyName("vol_up"));
    TEST_ASSERT_TRUE(mirrorIsValidKeyName("key-1"));
    TEST_ASSERT_TRUE(mirrorIsValidKeyName("A1_b2-C3"));
}

static void test_keyname_rejects_empty() {
    TEST_ASSERT_FALSE(mirrorIsValidKeyName(""));
}

static void test_keyname_rejects_space_and_special() {
    TEST_ASSERT_FALSE(mirrorIsValidKeyName("vol up"));
    TEST_ASSERT_FALSE(mirrorIsValidKeyName("key.1"));
    TEST_ASSERT_FALSE(mirrorIsValidKeyName("power!"));
    TEST_ASSERT_FALSE(mirrorIsValidKeyName("a/b"));
}

static void test_keyname_length_boundaries() {
    String name32 = "abcdefghijklmnopqrstuvwxyz012345";  // 恰好 32
    String name33 = name32 + "x";                        // 33，超限
    TEST_ASSERT_EQUAL_UINT(32, (unsigned)name32.length());
    TEST_ASSERT_TRUE(mirrorIsValidKeyName(name32));
    TEST_ASSERT_FALSE(mirrorIsValidKeyName(name33));
}

// ============================================================
// TEST GROUP 3: codeToHex 格式化
// ============================================================

static void test_code_to_hex_pads_to_8() {
    TEST_ASSERT_EQUAL_STRING("0x000000FF", mirrorCodeToHex(0xFF).c_str());
    TEST_ASSERT_EQUAL_STRING("0x00FF6897", mirrorCodeToHex(0x00FF6897ULL).c_str());
}

static void test_code_to_hex_beyond_32bit() {
    // 超 8 位十六进制时不截断
    TEST_ASSERT_EQUAL_STRING("0x1234567890", mirrorCodeToHex(0x1234567890ULL).c_str());
}

static void test_code_to_hex_roundtrip_with_parse() {
    uint64_t v = 0;
    String hex = mirrorCodeToHex(0x20DF10EFULL);
    TEST_ASSERT_TRUE(mirrorParseIrCode(hex, v));
    TEST_ASSERT_EQUAL_UINT32(0x20DF10EFUL, (uint32_t)v);
}

// ============================================================
// TEST GROUP 4: 按键映射 CRUD
// ============================================================

static void test_keymap_upsert_add_new() {
    MirrorKeyStore s;
    String err;
    TEST_ASSERT_TRUE(s.upsertKey("power", "0x00FF6897", "NEC", err));
    TEST_ASSERT_EQUAL_UINT(1, (unsigned)s.keyMap.size());
    TEST_ASSERT_TRUE(s.irKeysInConfig);
}

static void test_keymap_upsert_updates_existing_no_dup() {
    MirrorKeyStore s;
    String err;
    TEST_ASSERT_TRUE(s.upsertKey("power", "0x00FF6897", "NEC", err));
    // 同名再次录入：更新编码与协议，不新增
    TEST_ASSERT_TRUE(s.upsertKey("power", "0x11112222", "SONY", err));
    TEST_ASSERT_EQUAL_UINT(1, (unsigned)s.keyMap.size());
    TEST_ASSERT_EQUAL_UINT32(0x11112222UL, (uint32_t)s.keyMap[0].value);
    TEST_ASSERT_EQUAL_STRING("SONY", s.keyMap[0].protocol.c_str());
}

static void test_keymap_upsert_rejects_invalid_name_and_code() {
    MirrorKeyStore s;
    String err;
    TEST_ASSERT_FALSE(s.upsertKey("bad name", "0x00FF6897", "NEC", err));
    TEST_ASSERT_FALSE(s.upsertKey("ok", "0xZZ", "NEC", err));
    TEST_ASSERT_EQUAL_UINT(0, (unsigned)s.keyMap.size());
}

static void test_keymap_delete() {
    MirrorKeyStore s;
    String err;
    s.upsertKey("power", "0x00FF6897", "NEC", err);
    s.upsertKey("vol_up", "0x00FF629D", "NEC", err);
    TEST_ASSERT_TRUE(s.deleteKey("power"));
    TEST_ASSERT_EQUAL_UINT(1, (unsigned)s.keyMap.size());
    TEST_ASSERT_EQUAL_STRING("vol_up", s.keyMap[0].name.c_str());
    // 删除不存在的按键返回 false
    TEST_ASSERT_FALSE(s.deleteKey("nonexistent"));
}

static void test_keymap_clear_removes_config_field_no_residual() {
    MirrorKeyStore s;
    String err;
    s.upsertKey("power", "0x00FF6897", "NEC", err);
    s.upsertKey("vol_up", "0x00FF629D", "NEC", err);
    TEST_ASSERT_TRUE(s.irKeysInConfig);
    // 清码：彻底清除
    TEST_ASSERT_TRUE(s.clearKeys());
    TEST_ASSERT_EQUAL_UINT(0, (unsigned)s.keyMap.size());
    TEST_ASSERT_FALSE(s.irKeysInConfig);  // 配置中 irKeys 字段已移除，无残留
    // 清码后 resolveKeyName 不再命中旧编码
    TEST_ASSERT_EQUAL_STRING("", s.resolveKeyName(0x00FF6897ULL, "NEC").c_str());
}

static void test_keymap_delete_last_removes_config_field() {
    MirrorKeyStore s;
    String err;
    s.upsertKey("power", "0x00FF6897", "NEC", err);
    TEST_ASSERT_TRUE(s.irKeysInConfig);
    // 删除最后一个按键后表为空 -> persist 移除配置中的 irKeys 字段
    TEST_ASSERT_TRUE(s.deleteKey("power"));
    TEST_ASSERT_FALSE(s.irKeysInConfig);
}

static void test_keymap_enforces_max_limit() {
    MirrorKeyStore s;
    String err;
    for (size_t i = 0; i < MIRROR_MAX_IR_KEYS; ++i) {
        String name = "k" + String((int)i);
        TEST_ASSERT_TRUE(s.upsertKey(name, mirrorCodeToHex(0x1000 + i), "NEC", err));
    }
    TEST_ASSERT_EQUAL_UINT((unsigned)MIRROR_MAX_IR_KEYS, (unsigned)s.keyMap.size());
    // 第 41 个新按键被拒绝
    TEST_ASSERT_FALSE(s.upsertKey("overflow", "0x9999", "NEC", err));
    TEST_ASSERT_EQUAL_UINT((unsigned)MIRROR_MAX_IR_KEYS, (unsigned)s.keyMap.size());
    // 但对已存在按键的更新不受上限影响
    TEST_ASSERT_TRUE(s.upsertKey("k0", "0xABCD", "NEC", err));
}

// ============================================================
// TEST GROUP 5: resolveKeyName 协议匹配语义
// ============================================================

static void test_resolve_matches_by_value() {
    MirrorKeyStore s;
    String err;
    s.upsertKey("power", "0x00FF6897", "NEC", err);
    TEST_ASSERT_EQUAL_STRING("power", s.resolveKeyName(0x00FF6897ULL, "NEC").c_str());
}

static void test_resolve_protocol_mismatch_returns_empty() {
    MirrorKeyStore s;
    String err;
    s.upsertKey("power", "0x00FF6897", "NEC", err);
    // 编码一致但协议不符 -> 不命中
    TEST_ASSERT_EQUAL_STRING("", s.resolveKeyName(0x00FF6897ULL, "SONY").c_str());
}

static void test_resolve_empty_protocol_matches_any() {
    MirrorKeyStore s;
    String err;
    // entry.protocol 为空 -> 任意协议均可命中该编码
    s.upsertKey("power", "0x00FF6897", "", err);
    TEST_ASSERT_EQUAL_STRING("power", s.resolveKeyName(0x00FF6897ULL, "NEC").c_str());
    TEST_ASSERT_EQUAL_STRING("power", s.resolveKeyName(0x00FF6897ULL, "SONY").c_str());
}

static void test_resolve_protocol_case_insensitive() {
    MirrorKeyStore s;
    String err;
    s.upsertKey("power", "0x00FF6897", "NEC", err);
    TEST_ASSERT_EQUAL_STRING("power", s.resolveKeyName(0x00FF6897ULL, "nec").c_str());
}

static void test_resolve_unknown_value_returns_empty() {
    MirrorKeyStore s;
    String err;
    s.upsertKey("power", "0x00FF6897", "NEC", err);
    TEST_ASSERT_EQUAL_STRING("", s.resolveKeyName(0xDEADBEEFULL, "NEC").c_str());
}

// ============================================================
// TEST GROUP 6: 学习状态机
// ============================================================

static void test_learn_start_enters_learning() {
    MirrorLearn lm;
    lm.startLearn(10000, 1000);
    TEST_ASSERT_TRUE(lm.state == MirrorLearnState::LEARNING);
    TEST_ASSERT_EQUAL_STRING("learning", lm.stateName(1000).c_str());
}

static void test_learn_capture_transitions_to_captured() {
    MirrorLearn lm;
    lm.startLearn(10000, 1000);
    lm.capture("NEC", "0x00FF6897");
    TEST_ASSERT_TRUE(lm.state == MirrorLearnState::CAPTURED);
    TEST_ASSERT_EQUAL_STRING("captured", lm.stateName(1000).c_str());
    TEST_ASSERT_EQUAL_STRING("0x00FF6897", lm.code.c_str());
    TEST_ASSERT_EQUAL_STRING("NEC", lm.protocol.c_str());
}

static void test_learn_capture_ignored_when_not_learning() {
    MirrorLearn lm;  // IDLE
    lm.capture("NEC", "0x00FF6897");
    TEST_ASSERT_TRUE(lm.state == MirrorLearnState::IDLE);
    TEST_ASSERT_EQUAL_STRING("", lm.code.c_str());
}

static void test_learn_timeout_transition() {
    MirrorLearn lm;
    lm.startLearn(10000, 1000);       // deadline = 11000
    lm.updateTimeout(5000);           // 未到期
    TEST_ASSERT_TRUE(lm.state == MirrorLearnState::LEARNING);
    lm.updateTimeout(11000);          // 恰好到期
    TEST_ASSERT_TRUE(lm.state == MirrorLearnState::TIMEOUT);
    TEST_ASSERT_EQUAL_STRING("timeout", lm.stateName(11000).c_str());
}

static void test_learn_stop_cancels() {
    MirrorLearn lm;
    lm.startLearn(10000, 1000);
    lm.stopLearn();
    TEST_ASSERT_TRUE(lm.state == MirrorLearnState::IDLE);
    // 非学习态调用 stopLearn 不改变状态
    lm.state = MirrorLearnState::CAPTURED;
    lm.stopLearn();
    TEST_ASSERT_TRUE(lm.state == MirrorLearnState::CAPTURED);
}

static void test_learn_timeout_clamped_min() {
    MirrorLearn lm;
    lm.startLearn(100, 1000);   // < 1000 钳到 1000
    TEST_ASSERT_EQUAL_UINT32(1000UL, lm.deadline - 1000);
}

static void test_learn_timeout_clamped_max() {
    MirrorLearn lm;
    lm.startLearn(999999, 1000);  // > 60000 钳到 60000
    TEST_ASSERT_EQUAL_UINT32(60000UL, lm.deadline - 1000);
}

static void test_learn_remaining_ms() {
    MirrorLearn lm;
    lm.startLearn(10000, 1000);      // deadline = 11000
    TEST_ASSERT_EQUAL_UINT32(6000UL, lm.remainingMs(5000));
    TEST_ASSERT_EQUAL_UINT32(0UL, lm.remainingMs(11000));
    // 超时后剩余为 0
    TEST_ASSERT_EQUAL_UINT32(0UL, lm.remainingMs(20000));
}

// ============================================================
// TEST GROUP 7: eventData JSON 构建
// ============================================================

static void test_eventdata_without_key() {
    String ev = mirrorBuildEventData("NEC", "0x00FF6897", 32, "");
    JsonDocument doc;
    TEST_ASSERT_FALSE(deserializeJson(doc, ev.c_str()));
    TEST_ASSERT_EQUAL_STRING("NEC", (const char*)(doc["protocol"] | ""));
    TEST_ASSERT_EQUAL_STRING("0x00FF6897", (const char*)(doc["code"] | ""));
    TEST_ASSERT_EQUAL_INT(32, (int)(doc["bits"] | 0));
    TEST_ASSERT_FALSE(doc["key"].is<const char*>());  // 无 key 字段
}

static void test_eventdata_with_key() {
    String ev = mirrorBuildEventData("NEC", "0x00FF6897", 32, "power");
    JsonDocument doc;
    TEST_ASSERT_FALSE(deserializeJson(doc, ev.c_str()));
    TEST_ASSERT_EQUAL_STRING("power", (const char*)(doc["key"] | ""));
}

// ============================================================
// TEST GROUP 8: 红外触发匹配（compareValue 过滤）
// ============================================================

static void test_match_empty_comparevalue_matches_any() {
    String ev = mirrorBuildEventData("NEC", "0x00FF6897", 32, "power");
    // 留空 -> 任意红外编码均触发
    TEST_ASSERT_TRUE(mirrorIrTriggerMatches("", ev));
}

static void test_match_by_key_name() {
    String ev = mirrorBuildEventData("NEC", "0x00FF6897", 32, "power");
    TEST_ASSERT_TRUE(mirrorIrTriggerMatches("key:power", ev));
}

static void test_match_by_key_name_case_insensitive() {
    String ev = mirrorBuildEventData("NEC", "0x00FF6897", 32, "power");
    TEST_ASSERT_TRUE(mirrorIrTriggerMatches("key:POWER", ev));
    TEST_ASSERT_TRUE(mirrorIrTriggerMatches("KEY:power", ev));
}

static void test_match_by_key_fails_without_key_field() {
    // eventData 未携带 key（未学习该编码）-> key:power 不匹配
    String ev = mirrorBuildEventData("NEC", "0x00FF6897", 32, "");
    TEST_ASSERT_FALSE(mirrorIrTriggerMatches("key:power", ev));
}

static void test_match_by_key_fails_on_different_key() {
    String ev = mirrorBuildEventData("NEC", "0x00FF6897", 32, "vol_up");
    TEST_ASSERT_FALSE(mirrorIrTriggerMatches("key:power", ev));
}

static void test_match_by_protocol_and_code() {
    String ev = mirrorBuildEventData("NEC", "0x00FF6897", 32, "");
    TEST_ASSERT_TRUE(mirrorIrTriggerMatches("NEC:0x00FF6897", ev));
}

static void test_match_protocol_mismatch_fails() {
    String ev = mirrorBuildEventData("SONY", "0x00FF6897", 32, "");
    TEST_ASSERT_FALSE(mirrorIrTriggerMatches("NEC:0x00FF6897", ev));
}

static void test_match_by_code_any_protocol() {
    String evNec = mirrorBuildEventData("NEC", "0x00FF6897", 32, "");
    String evSony = mirrorBuildEventData("SONY", "0x00FF6897", 32, "");
    // 纯编码（无协议前缀）匹配任意协议
    TEST_ASSERT_TRUE(mirrorIrTriggerMatches("0x00FF6897", evNec));
    TEST_ASSERT_TRUE(mirrorIrTriggerMatches("0x00FF6897", evSony));
}

static void test_match_by_code_without_0x_prefix() {
    String ev = mirrorBuildEventData("NEC", "0x00FF6897", 32, "");
    // 用户填写不带 0x 前缀，逻辑自动补齐
    TEST_ASSERT_TRUE(mirrorIrTriggerMatches("00FF6897", ev));
}

static void test_match_by_code_case_insensitive() {
    String ev = mirrorBuildEventData("NEC", "0x00FF6897", 32, "");
    TEST_ASSERT_TRUE(mirrorIrTriggerMatches("0x00ff6897", ev));
}

static void test_match_code_mismatch_fails() {
    String ev = mirrorBuildEventData("NEC", "0x00FF6897", 32, "");
    TEST_ASSERT_FALSE(mirrorIrTriggerMatches("0xDEADBEEF", ev));
}

static void test_match_invalid_eventdata_fails() {
    // 非法 JSON -> 不匹配（除非 compareValue 为空）
    TEST_ASSERT_FALSE(mirrorIrTriggerMatches("key:power", "not-a-json"));
}

// ============================================================
// 镜像 getKeysJson：输出按键映射的 JSON 数组字符串
// 格式: [{"name":"power","code":"0x00FF6897","protocol":"NEC"}]
// ============================================================
static String mirrorGetKeysJson(const MirrorKeyStore& store) {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (const auto& e : store.keyMap) {
        JsonObject obj = arr.add<JsonObject>();
        obj["name"] = e.name;
        obj["code"] = mirrorCodeToHex(e.value);
        obj["protocol"] = e.protocol;
    }
    String out;
    serializeJson(doc, out);
    return out;
}

// ============================================================
// 镜像防重复触发间隔逻辑（MIN_EVENT_INTERVAL_MS）
// check() 中：if (now - _lastEventTime < MIN_EVENT_INTERVAL_MS) return;
// ============================================================
struct MirrorDebounce {
    unsigned long lastEventTime = 0;

    // 返回 true 表示允许派发事件，false 表示被防抖过滤
    bool shouldDispatch(unsigned long now) {
        if (now - lastEventTime < MIRROR_MIN_EVENT_INTERVAL_MS) return false;
        lastEventTime = now;
        return true;
    }
};

// ============================================================
// 镜像 IRrecv 接收器生命周期（enableIRIn/disableIRIn/析构）与
// IRRemoteDriver::stop() 的资源释放契约。
//
// 回归背景（双重 disableIRIn 导致定时器 NULL 崩溃）：
//   IRrecv::disableIRIn() 内部执行 timerEnd() 释放硬件定时器；
//   ~IRrecv() 析构函数会自动再调用一次 disableIRIn()。
//   旧版 stop() 先手动 recv->disableIRIn() 再 delete recv，析构时
//   第二次 disableIRIn() 对已释放的定时器调 timerWrite() → LoadProhibited panic。
//   正确契约：接收器整个生命周期内 disable/清理恰好执行一次，
//   由 delete 触发的析构函数统一负责，stop() 不得手动提前调用。
// ============================================================
struct MirrorIrLifecycleStats {
    int disableCalls = 0;        // disableIRIn() 总调用次数
    int disableOnDeadTimer = 0;  // 对已释放定时器的调用次数（>0 即真实硬件的崩溃路径）
};

struct MirrorIrReceiver {
    bool timerAlive = false;     // 定时器资源是否有效（timerBegin 后为 true）
    MirrorIrLifecycleStats* stats;

    explicit MirrorIrReceiver(MirrorIrLifecycleStats* s) : stats(s) {}

    // 镜像 enableIRIn：timerBegin 创建定时器
    void enableIRIn() { timerAlive = true; }

    // 镜像 disableIRIn：timerEnd 释放定时器；对已释放定时器再次调用即崩溃点
    void disableIRIn() {
        stats->disableCalls++;
        if (!timerAlive) {
            stats->disableOnDeadTimer++;  // 真实硬件：timerWrite(NULL) → panic
            return;
        }
        timerAlive = false;  // timerEnd() 释放定时器
    }

    // 镜像 ~IRrecv：析构自动调用 disableIRIn()
    ~MirrorIrReceiver() { disableIRIn(); }
};

// ============================================================
// 镜像驱动并发互斥锁契约（IRRemoteDriver / RFLearnDriver 共用的
// FreeRTOS 互斥锁保护模式，回归：HTTP/AsyncTCP 任务与 loop 任务
// 并发访问驱动导致的 use-after-free 防护）：
//   - begin()/stop() 阻塞获锁（portMAX_DELAY）
//   - check() 非阻塞获锁（0）：锁被占用（stop/begin 进行中）则跳过本轮，
//     绝不触碰可能已释放的接收器对象
//   - 所有 return 分支都必须释放锁（不泄漏）
//   - FreeRTOS 互斥锁不可重入：持锁期间再次获取会同任务死锁
//     （RF begin() 引脚变更修复正是为了避免在 begin 内调 stop 造成重入）
// ============================================================
struct MirrorMutex {
    bool held = false;
    int reentryAttempts = 0;   // 持锁期间再次获取的次数（真实硬件=死锁）

    // blocking=true 镜像 portMAX_DELAY；blocking=false 镜像超时 0（非阻塞）
    bool take(bool blocking) {
        if (held) {
            reentryAttempts++;   // 不可重入：同任务再获取即死锁根源
            return false;        // 测试中不真死锁，记录并返回获取失败
        }
        held = true;
        return true;
    }
    void give() { held = false; }
};

// 镜像带锁保护的驱动生命周期（begin/check/stop 的获锁/释锁骨架）
struct MirrorLockedIrDriver {
    MirrorMutex mutex;
    MirrorIrReceiver* recv = nullptr;   // 接收器对象（stop 后为 null）
    MirrorIrLifecycleStats* stats;
    int checkBodyRuns = 0;              // check() 真正进入解码体的次数

    explicit MirrorLockedIrDriver(MirrorIrLifecycleStats* s) : stats(s) {}

    // 镜像 begin()：阻塞获锁
    bool begin() {
        if (!mutex.take(true)) return false;
        if (!recv) {
            recv = new MirrorIrReceiver(stats);
            recv->enableIRIn();
        }
        mutex.give();
        return true;
    }

    // 镜像 check()：非阻塞获锁，锁被占则跳过；所有 return 分支都释锁
    void check() {
        if (!mutex.take(false)) return;          // 锁被占（stop/begin 进行中）→ 跳过本轮
        if (!recv) { mutex.give(); return; }     // 未初始化 → 释锁返回
        checkBodyRuns++;                          // 真正触碰接收器（解码）
        mutex.give();
    }

    // 镜像 stop()：阻塞获锁，delete 由析构统一 disable
    void stop() {
        if (!mutex.take(true)) return;
        if (recv) { delete recv; recv = nullptr; }
        mutex.give();
    }
};

// ============================================================
// TEST GROUP 9: getKeysJson 输出格式
// ============================================================

static void test_keys_json_empty() {
    MirrorKeyStore s;
    String json = mirrorGetKeysJson(s);
    TEST_ASSERT_EQUAL_STRING("[]", json.c_str());
}

static void test_keys_json_single_entry() {
    MirrorKeyStore s;
    String err;
    s.upsertKey("power", "0x00FF6897", "NEC", err);
    String json = mirrorGetKeysJson(s);
    JsonDocument doc;
    TEST_ASSERT_FALSE(deserializeJson(doc, json.c_str()));
    TEST_ASSERT_TRUE(doc.is<JsonArray>());
    TEST_ASSERT_EQUAL_UINT(1, (unsigned)doc.size());
    TEST_ASSERT_EQUAL_STRING("power", (const char*)(doc[0]["name"] | ""));
    TEST_ASSERT_EQUAL_STRING("0x00FF6897", (const char*)(doc[0]["code"] | ""));
    TEST_ASSERT_EQUAL_STRING("NEC", (const char*)(doc[0]["protocol"] | ""));
}

static void test_keys_json_multiple_entries() {
    MirrorKeyStore s;
    String err;
    s.upsertKey("power", "0x00FF6897", "NEC", err);
    s.upsertKey("vol_up", "0x00FF629D", "NEC", err);
    s.upsertKey("menu", "0x20DF10EF", "LG", err);
    String json = mirrorGetKeysJson(s);
    JsonDocument doc;
    TEST_ASSERT_FALSE(deserializeJson(doc, json.c_str()));
    TEST_ASSERT_EQUAL_UINT(3, (unsigned)doc.size());
    // 验证顺序保持插入顺序
    TEST_ASSERT_EQUAL_STRING("power", (const char*)(doc[0]["name"] | ""));
    TEST_ASSERT_EQUAL_STRING("vol_up", (const char*)(doc[1]["name"] | ""));
    TEST_ASSERT_EQUAL_STRING("menu", (const char*)(doc[2]["name"] | ""));
    TEST_ASSERT_EQUAL_STRING("LG", (const char*)(doc[2]["protocol"] | ""));
}

static void test_keys_json_code_format_padded() {
    MirrorKeyStore s;
    String err;
    // 短编码应被补零到 8 位
    s.upsertKey("k1", "0xFF", "NEC", err);
    String json = mirrorGetKeysJson(s);
    JsonDocument doc;
    deserializeJson(doc, json.c_str());
    TEST_ASSERT_EQUAL_STRING("0x000000FF", (const char*)(doc[0]["code"] | ""));
}

static void test_keys_json_after_delete_and_clear() {
    MirrorKeyStore s;
    String err;
    s.upsertKey("power", "0x00FF6897", "NEC", err);
    s.upsertKey("vol_up", "0x00FF629D", "NEC", err);
    s.deleteKey("power");
    String json1 = mirrorGetKeysJson(s);
    JsonDocument doc1;
    deserializeJson(doc1, json1.c_str());
    TEST_ASSERT_EQUAL_UINT(1, (unsigned)doc1.size());
    TEST_ASSERT_EQUAL_STRING("vol_up", (const char*)(doc1[0]["name"] | ""));
    // 清码后为空数组
    s.clearKeys();
    String json2 = mirrorGetKeysJson(s);
    TEST_ASSERT_EQUAL_STRING("[]", json2.c_str());
}

// ============================================================
// TEST GROUP 10: 防重复触发间隔（MIN_EVENT_INTERVAL_MS）
// ============================================================

static void test_debounce_first_event_passes() {
    MirrorDebounce db;
    // 首次事件（lastEventTime=0），间隔足够
    TEST_ASSERT_TRUE(db.shouldDispatch(200));
}

static void test_debounce_rapid_events_filtered() {
    MirrorDebounce db;
    TEST_ASSERT_TRUE(db.shouldDispatch(1000));
    // 100ms 后（< 150ms 间隔）应被过滤
    TEST_ASSERT_FALSE(db.shouldDispatch(1100));
    TEST_ASSERT_FALSE(db.shouldDispatch(1149));
}

static void test_debounce_after_interval_passes() {
    MirrorDebounce db;
    TEST_ASSERT_TRUE(db.shouldDispatch(1000));
    // 恰好 150ms 后应通过
    TEST_ASSERT_TRUE(db.shouldDispatch(1150));
}

static void test_debounce_long_gap_passes() {
    MirrorDebounce db;
    TEST_ASSERT_TRUE(db.shouldDispatch(1000));
    // 长时间间隔后肯定通过
    TEST_ASSERT_TRUE(db.shouldDispatch(5000));
}

static void test_debounce_repeated_burst() {
    MirrorDebounce db;
    // 模拟红外遥控器长按重复码（~110ms 间隔）
    // 从 t=1000 开始，避免与初始 lastEventTime=0 的边界混淆（0-0=0<150 会被误滤）
    TEST_ASSERT_TRUE(db.shouldDispatch(1000));
    int passed = 0;
    for (unsigned long t = 1110; t <= 2100; t += 110) {
        if (db.shouldDispatch(t)) passed++;
    }
    // 10 次重复码中，只有间隔 >= 150ms 的才通过
    // 实际：last=1000, 1110-1000=110 X, 1220-1000=220 ✓ last=1220, 1330-1220=110 X,
    //       1440-1220=220 ✓ last=1440, 1550 X, 1660 ✓, 1770 X, 1880 ✓, 1990 X, 2100-1880=220 ✓
    TEST_ASSERT_EQUAL_INT(5, passed);
}

// ============================================================
// TEST GROUP 11: 端到端 —— 学习后按名称联动
// ============================================================

static void test_e2e_learn_then_match_by_name() {
    // 1) 用户主动学习：进入学习态，捕获编码
    MirrorLearn lm;
    lm.startLearn(10000, 1000);
    lm.capture("NEC", mirrorCodeToHex(0x00FF6897ULL));
    TEST_ASSERT_TRUE(lm.state == MirrorLearnState::CAPTURED);

    // 2) 保存为按键 "power"
    MirrorKeyStore store;
    String err;
    TEST_ASSERT_TRUE(store.upsertKey("power", lm.code, lm.protocol, err));

    // 3) 后续收到同一编码：resolveKeyName 得到 "power"，eventData 携带 key
    uint64_t rxValue = 0;
    mirrorParseIrCode(lm.code, rxValue);
    String keyName = store.resolveKeyName(rxValue, "NEC");
    TEST_ASSERT_EQUAL_STRING("power", keyName.c_str());
    String ev = mirrorBuildEventData("NEC", mirrorCodeToHex(rxValue), 32, keyName);

    // 4) 规则 "按键1打开继电器" 用 key:power 匹配 -> 命中
    TEST_ASSERT_TRUE(mirrorIrTriggerMatches("key:power", ev));

    // 5) 清码后重新收到同一编码：不再携带 key，按名称规则不再命中
    store.clearKeys();
    String keyName2 = store.resolveKeyName(rxValue, "NEC");
    TEST_ASSERT_EQUAL_STRING("", keyName2.c_str());
    String ev2 = mirrorBuildEventData("NEC", mirrorCodeToHex(rxValue), 32, keyName2);
    TEST_ASSERT_FALSE(mirrorIrTriggerMatches("key:power", ev2));
    // 但按编码匹配仍可命中（编码本身不因清码而改变）
    TEST_ASSERT_TRUE(mirrorIrTriggerMatches("NEC:0x00FF6897", ev2));
}

// ============================================================
// TEST GROUP 12: 接收器生命周期 —— stop() 资源释放契约
// （回归：双重 disableIRIn 导致定时器 NULL 崩溃）
// ============================================================

// 正确契约：stop() 仅 delete，析构统一 disable → 恰好一次，无死定时器调用
static void test_stop_disables_exactly_once() {
    MirrorIrLifecycleStats stats;
    MirrorIrReceiver* recv = new MirrorIrReceiver(&stats);
    recv->enableIRIn();
    // 修复后的 stop()：不手动调 disableIRIn()，直接 delete（析构接管）
    delete recv;
    TEST_ASSERT_EQUAL_INT(1, stats.disableCalls);
    TEST_ASSERT_EQUAL_INT(0, stats.disableOnDeadTimer);  // 无崩溃路径
}

// 反例：复现旧版 bug —— 手动 disable 再 delete → 二次调用命中死定时器（即崩溃）
static void test_old_buggy_stop_double_disables() {
    MirrorIrLifecycleStats stats;
    MirrorIrReceiver* recv = new MirrorIrReceiver(&stats);
    recv->enableIRIn();
    recv->disableIRIn();  // 旧版 stop() 的手动调用
    delete recv;          // 析构再次调用
    TEST_ASSERT_EQUAL_INT(2, stats.disableCalls);
    // 第二次落在已释放的定时器上 —— 真实硬件即在此 panic
    TEST_ASSERT_EQUAL_INT(1, stats.disableOnDeadTimer);
}

// 未初始化（未 enableIRIn）的接收器被 stop：析构 disable 一次，但定时器本就不存在
// 说明：真实驱动中 stop() 有 _irRecv 空指针守护，不会 delete 空对象；
//       这里验证即使 disable 被调，也不会对“有效定时器”造成重复释放。
static void test_stop_without_enable_no_double_free() {
    MirrorIrLifecycleStats stats;
    MirrorIrReceiver* recv = new MirrorIrReceiver(&stats);
    // 未调 enableIRIn（timerAlive=false）
    delete recv;  // 析构调 disableIRIn，落在 dead timer 计数但非“重复释放”
    TEST_ASSERT_EQUAL_INT(1, stats.disableCalls);
    // 定时器从未分配，不存在“释放后再释放”的双重释放
    TEST_ASSERT_EQUAL_INT(1, stats.disableOnDeadTimer);
}

// 重启周期：begin→stop→begin→stop，每个周期 disable 恰好一次、无死定时器调用
static void test_restart_cycle_disables_once_per_cycle() {
    MirrorIrLifecycleStats stats;
    for (int cycle = 0; cycle < 3; ++cycle) {
        MirrorIrReceiver* recv = new MirrorIrReceiver(&stats);
        recv->enableIRIn();
        delete recv;  // 每个周期析构 disable 一次
    }
    TEST_ASSERT_EQUAL_INT(3, stats.disableCalls);
    TEST_ASSERT_EQUAL_INT(0, stats.disableOnDeadTimer);
}

// ============================================================
// TEST GROUP 13: 互斥锁并发契约（回归：check/stop 竞态 use-after-free 防护）
// ============================================================

// 契约 1：锁被占用（stop/begin 进行中）时 check() 跳过本轮，不触碰接收器
static void test_lock_check_skips_when_held() {
    MirrorIrLifecycleStats stats;
    MirrorLockedIrDriver drv(&stats);
    drv.begin();
    drv.mutex.take(false);              // 模拟另一任务（stop/begin）持锁
    int before = drv.checkBodyRuns;
    drv.check();                        // 应被跳过
    TEST_ASSERT_EQUAL_INT(before, drv.checkBodyRuns);  // 未进入解码体
    drv.mutex.give();
    drv.stop();
}

// 契约 2：check() 所有 return 分支都释放锁（不泄漏）
static void test_lock_check_releases_on_all_paths() {
    MirrorIrLifecycleStats stats;
    MirrorLockedIrDriver drv(&stats);
    drv.begin();
    drv.check();                        // 正常路径
    TEST_ASSERT_FALSE(drv.mutex.held);  // 锁已释放
    drv.stop();
    drv.check();                        // 未初始化路径（recv=null）
    TEST_ASSERT_FALSE(drv.mutex.held);  // 提前返回也释锁
}

// 契约 3：begin/stop 阻塞获锁并释放；整个生命周期接收器恰好清理一次
static void test_lock_begin_stop_balanced_single_disable() {
    MirrorIrLifecycleStats stats;
    MirrorLockedIrDriver drv(&stats);
    drv.begin();
    TEST_ASSERT_FALSE(drv.mutex.held);  // begin 后锁已释放
    drv.stop();
    TEST_ASSERT_FALSE(drv.mutex.held);  // stop 后锁已释放
    TEST_ASSERT_EQUAL_INT(1, stats.disableCalls);        // 析构 disable 恰好一次
    TEST_ASSERT_EQUAL_INT(0, stats.disableOnDeadTimer);  // 无死定时器调用
}

// 契约 4：FreeRTOS 互斥锁不可重入 —— 持锁期间再获取即死锁（RF begin 修复动机）
static void test_lock_nonrecursive_reentry_is_deadlock() {
    MirrorMutex m;
    TEST_ASSERT_TRUE(m.take(true));     // 首次获取成功
    TEST_ASSERT_FALSE(m.take(true));    // 持锁再获取：真实硬件将死锁
    TEST_ASSERT_EQUAL_INT(1, m.reentryAttempts);
    m.give();
    TEST_ASSERT_TRUE(m.take(true));     // 释放后可再获取
    m.give();
    TEST_ASSERT_EQUAL_INT(1, m.reentryAttempts);  // 未新增重入
}

// ============================================================
// 测试组注册
// ============================================================
void test_ir_remote_group() {
    // Group 1: parseIrCode
    RUN_TEST(test_parse_code_with_0x_prefix);
    RUN_TEST(test_parse_code_without_prefix);
    RUN_TEST(test_parse_code_lowercase_and_uppercase_prefix);
    RUN_TEST(test_parse_code_trims_whitespace);
    RUN_TEST(test_parse_code_rejects_empty);
    RUN_TEST(test_parse_code_rejects_bare_prefix);
    RUN_TEST(test_parse_code_rejects_invalid_chars);
    RUN_TEST(test_parse_code_rejects_overlong);
    RUN_TEST(test_parse_code_full_64bit);

    // Group 2: isValidKeyName
    RUN_TEST(test_keyname_valid_examples);
    RUN_TEST(test_keyname_rejects_empty);
    RUN_TEST(test_keyname_rejects_space_and_special);
    RUN_TEST(test_keyname_length_boundaries);

    // Group 3: codeToHex
    RUN_TEST(test_code_to_hex_pads_to_8);
    RUN_TEST(test_code_to_hex_beyond_32bit);
    RUN_TEST(test_code_to_hex_roundtrip_with_parse);

    // Group 4: 按键映射 CRUD
    RUN_TEST(test_keymap_upsert_add_new);
    RUN_TEST(test_keymap_upsert_updates_existing_no_dup);
    RUN_TEST(test_keymap_upsert_rejects_invalid_name_and_code);
    RUN_TEST(test_keymap_delete);
    RUN_TEST(test_keymap_clear_removes_config_field_no_residual);
    RUN_TEST(test_keymap_delete_last_removes_config_field);
    RUN_TEST(test_keymap_enforces_max_limit);

    // Group 5: resolveKeyName
    RUN_TEST(test_resolve_matches_by_value);
    RUN_TEST(test_resolve_protocol_mismatch_returns_empty);
    RUN_TEST(test_resolve_empty_protocol_matches_any);
    RUN_TEST(test_resolve_protocol_case_insensitive);
    RUN_TEST(test_resolve_unknown_value_returns_empty);

    // Group 6: 学习状态机
    RUN_TEST(test_learn_start_enters_learning);
    RUN_TEST(test_learn_capture_transitions_to_captured);
    RUN_TEST(test_learn_capture_ignored_when_not_learning);
    RUN_TEST(test_learn_timeout_transition);
    RUN_TEST(test_learn_stop_cancels);
    RUN_TEST(test_learn_timeout_clamped_min);
    RUN_TEST(test_learn_timeout_clamped_max);
    RUN_TEST(test_learn_remaining_ms);

    // Group 7: eventData 构建
    RUN_TEST(test_eventdata_without_key);
    RUN_TEST(test_eventdata_with_key);

    // Group 8: 触发匹配
    RUN_TEST(test_match_empty_comparevalue_matches_any);
    RUN_TEST(test_match_by_key_name);
    RUN_TEST(test_match_by_key_name_case_insensitive);
    RUN_TEST(test_match_by_key_fails_without_key_field);
    RUN_TEST(test_match_by_key_fails_on_different_key);
    RUN_TEST(test_match_by_protocol_and_code);
    RUN_TEST(test_match_protocol_mismatch_fails);
    RUN_TEST(test_match_by_code_any_protocol);
    RUN_TEST(test_match_by_code_without_0x_prefix);
    RUN_TEST(test_match_by_code_case_insensitive);
    RUN_TEST(test_match_code_mismatch_fails);
    RUN_TEST(test_match_invalid_eventdata_fails);

    // Group 9: getKeysJson 输出格式
    RUN_TEST(test_keys_json_empty);
    RUN_TEST(test_keys_json_single_entry);
    RUN_TEST(test_keys_json_multiple_entries);
    RUN_TEST(test_keys_json_code_format_padded);
    RUN_TEST(test_keys_json_after_delete_and_clear);

    // Group 10: 防重复触发间隔
    RUN_TEST(test_debounce_first_event_passes);
    RUN_TEST(test_debounce_rapid_events_filtered);
    RUN_TEST(test_debounce_after_interval_passes);
    RUN_TEST(test_debounce_long_gap_passes);
    RUN_TEST(test_debounce_repeated_burst);

    // Group 11: 端到端
    RUN_TEST(test_e2e_learn_then_match_by_name);

    // Group 12: 接收器生命周期（stop 资源释放契约 / 双重 disableIRIn 回归）
    RUN_TEST(test_stop_disables_exactly_once);
    RUN_TEST(test_old_buggy_stop_double_disables);
    RUN_TEST(test_stop_without_enable_no_double_free);
    RUN_TEST(test_restart_cycle_disables_once_per_cycle);

    // Group 13: 互斥锁并发契约（check/stop 竞态 use-after-free 防护）
    RUN_TEST(test_lock_check_skips_when_held);
    RUN_TEST(test_lock_check_releases_on_all_paths);
    RUN_TEST(test_lock_begin_stop_balanced_single_disable);
    RUN_TEST(test_lock_nonrecursive_reentry_is_deadlock);
}
