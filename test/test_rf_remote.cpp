/**
 * @file test_rf_remote.cpp
 * @brief 433MHz RF 遥控学习驱动单元测试
 *
 * 测试内容（纯逻辑，镜像自 src，不依赖 GPIO 中断/LittleFS 硬件）：
 * - EV1527 解码          脉冲时序 → 编码（3:1=1 / 1:3=0 / 噪声跳过 / 最短脉宽基准）
 * - 脉宽合法性           minPulse ∈ [80, 2000] 否则丢弃
 * - 学习状态机           IDLE→LEARNING→CAPTURED / 超时 / 取消 / 超时钳位
 * - 防重复触发           相同编码+位长在 300ms 内不重复处理（不同编码放行）
 * - 按键映射 CRUD        upsert 去重更新 / delete / clear / resolve(code+bits) /
 *                        名称字符集（含空格）/ 位长 12-32 校验 / MAX_RF_KEYS 上限
 * - begin 引脚变更修复   引脚变更时内联 detach（不调 stop()），避免持锁期间重入死锁
 * - 互斥锁并发契约       begin/stop 阻塞获锁、check 非阻塞跳过、所有分支释锁不泄漏
 *
 * 说明：RFLearnDriver.cpp 依赖 GPIO 中断与文件系统，native 环境无法直接编译，
 * 故本测试镜像其纯逻辑分支（与本仓库既有驱动测试一致的做法）。
 */

#include <unity.h>
#include <Arduino.h>
#include <ArduinoJson.h>
#include <vector>
#include <string>
#include <cstdio>
#include <cctype>
#include <cstdlib>

void test_rf_remote_group();

// ============================================================
// 常量（镜像自 include/peripherals/drivers/RFLearnDriver.h）
// ============================================================
static constexpr size_t MIRROR_RF_MAX_KEYS = 40;
static constexpr unsigned long MIRROR_RF_MIN_EVENT_INTERVAL_MS = 300;
static constexpr uint8_t MIRROR_RF_MIN_DATA_BITS = 12;
static constexpr uint8_t MIRROR_RF_MAX_DATA_BITS = 32;

enum class MirrorRfLearnState : uint8_t {
    IDLE = 0,
    LEARNING = 1,
    CAPTURED = 2,
    TIMEOUT = 3
};

// ============================================================
// 镜像 FreeRTOS 互斥锁（不可重入；持锁再获取=死锁，测试中计数不真死锁）
// ============================================================
struct MirrorRfMutex {
    bool held = false;
    int reentryAttempts = 0;

    bool take(bool blocking) {
        if (held) {
            reentryAttempts++;
            return false;
        }
        held = true;
        return true;
    }
    void give() { held = false; }
};

// ============================================================
// 镜像 EV1527 解码（RFLearnDriver::check() 中的解码段）
// 输入脉冲缓冲（交替 HIGH/LOW，每 2 个脉冲一位），输出编码/位长/平均脉宽
// ============================================================
struct MirrorRfDecodeResult {
    uint32_t code = 0;
    uint8_t bits = 0;
    uint16_t avgPulse = 0;
    bool valid = false;
};

static MirrorRfDecodeResult mirrorRfDecode(const std::vector<uint16_t>& buf) {
    MirrorRfDecodeResult r;
    uint8_t count = (uint8_t)buf.size();
    if (count < MIRROR_RF_MIN_DATA_BITS * 2) return r;  // 数据不足

    // 取最短脉冲作为 1T 基准
    uint16_t minPulse = 65535;
    for (uint8_t i = 0; i < count; i++) {
        if (buf[i] < minPulse) minPulse = buf[i];
    }
    if (minPulse < 80 || minPulse > 2000) return r;  // 不合理脉宽

    uint16_t T = minPulse;
    uint32_t code = 0;
    uint8_t bits = 0;
    uint16_t pulseSum = 0;
    uint8_t pulseSamples = 0;

    for (uint8_t i = 0; i + 1 < count && bits < MIRROR_RF_MAX_DATA_BITS; i += 2) {
        uint16_t high = buf[i];
        uint16_t low = buf[i + 1];
        if (high > T * 6 || low > T * 6) continue;  // 跳过明显异常脉冲

        uint8_t highRatio = (high + T / 2) / T;
        uint8_t lowRatio = (low + T / 2) / T;

        if (highRatio >= 2 && lowRatio <= 2) {        // 3:1 -> bit 1
            code = (code << 1) | 1;
            bits++;
            pulseSum += (high + low) / 4;
            pulseSamples++;
        } else if (highRatio <= 2 && lowRatio >= 2) { // 1:3 -> bit 0
            code = (code << 1) | 0;
            bits++;
            pulseSum += (high + low) / 4;
            pulseSamples++;
        }
        // 其他比例：噪声，跳过
    }

    if (bits < MIRROR_RF_MIN_DATA_BITS) return r;
    r.code = code;
    r.bits = bits;
    r.avgPulse = pulseSamples > 0 ? pulseSum / pulseSamples : T;
    r.valid = true;
    return r;
}

// 构造一帧 EV1527 脉冲序列：bit=1 → (3T, 1T)；bit=0 → (1T, 3T)
static std::vector<uint16_t> buildEv1527Frame(uint32_t code, uint8_t bits, uint16_t T) {
    std::vector<uint16_t> buf;
    for (int i = (int)bits - 1; i >= 0; i--) {
        bool bit = (code >> i) & 1U;
        if (bit) {
            buf.push_back((uint16_t)(T * 3));
            buf.push_back(T);
        } else {
            buf.push_back(T);
            buf.push_back((uint16_t)(T * 3));
        }
    }
    return buf;
}

// ============================================================
// 镜像学习状态机（startLearn/stopLearn/timeout/capture）
// 用可注入的 now 取代 millis()，以便确定性测试超时逻辑
// ============================================================
struct MirrorRfLearn {
    MirrorRfLearnState state = MirrorRfLearnState::IDLE;
    unsigned long deadline = 0;
    String code;
    uint8_t bits = 0;
    uint16_t pulse = 0;

    void startLearn(uint32_t timeoutMs, unsigned long now) {
        if (timeoutMs < 1000) timeoutMs = 1000;
        if (timeoutMs > 60000) timeoutMs = 60000;
        state = MirrorRfLearnState::LEARNING;
        deadline = now + timeoutMs;
        code = "";
        bits = 0;
        pulse = 0;
    }

    void stopLearn() {
        state = MirrorRfLearnState::IDLE;
        code = "";
    }

    void updateTimeout(unsigned long now) {
        if (state == MirrorRfLearnState::LEARNING && now >= deadline) {
            state = MirrorRfLearnState::TIMEOUT;
        }
    }

    // 镜像 check() 学习模式下的捕获：仅 LEARNING 时接受
    void capture(const String& c, uint8_t b, uint16_t p) {
        if (state == MirrorRfLearnState::LEARNING) {
            code = c;
            bits = b;
            pulse = p;
            state = MirrorRfLearnState::CAPTURED;
        }
    }

    String stateName(unsigned long now) {
        updateTimeout(now);
        switch (state) {
            case MirrorRfLearnState::LEARNING: return "learning";
            case MirrorRfLearnState::CAPTURED: return "captured";
            case MirrorRfLearnState::TIMEOUT:  return "timeout";
            default:                           return "idle";
        }
    }
};

// ============================================================
// 镜像防重复触发（check() 中：相同编码+位长在最小间隔内不重复处理）
// 与 IR 不同：RF 仅当 code 与 bits 都相同时才防抖，不同编码直接放行
// ============================================================
struct MirrorRfDebounce {
    unsigned long lastEventTime = 0;
    uint32_t lastValue = 0;
    uint8_t lastBitLength = 0;

    // 返回 true 表示应处理，false 表示被防抖过滤
    bool shouldProcess(uint32_t code, uint8_t bits, unsigned long now) {
        if (code == lastValue && bits == lastBitLength &&
            (now - lastEventTime) < MIRROR_RF_MIN_EVENT_INTERVAL_MS) {
            return false;
        }
        lastValue = code;
        lastBitLength = bits;
        lastEventTime = now;
        return true;
    }
};

// ============================================================
// 镜像按键映射存储（RFLearnDriver 的 _keyMap + 持久化语义）
// ============================================================

// 名称校验：1-32 字符，仅字母数字下划线短横线空格（RF 允许空格，与 IR 不同）
static bool mirrorRfValidName(const String& name) {
    if (name.length() == 0 || name.length() > 32) return false;
    for (size_t i = 0; i < name.length(); i++) {
        char c = name[i];
        if (!isalnum((unsigned char)c) && c != '_' && c != '-' && c != ' ') return false;
    }
    return true;
}

// 编码解析：strtoul 十六进制；结果为 0 且非 "0x0*"/"0" 视为非法
static bool mirrorRfParseCode(const String& code, uint32_t& out) {
    uint32_t v = (uint32_t)strtoul(code.c_str(), nullptr, 16);
    if (v == 0 && !code.startsWith("0x0") && !code.equals("0")) return false;
    out = v;
    return true;
}

struct MirrorRfKeyEntry {
    String name;
    uint32_t code;
    uint8_t bitLength;
    uint16_t pulseWidth;
};

struct MirrorRfKeyStore {
    std::vector<MirrorRfKeyEntry> keyMap;
    bool fileExists = false;   // 模拟 /config/rf_keys.json 是否存在

    // 镜像 _persistKeyMap：RF 总是写文件（空表也写空数组），不删除文件
    bool persist() {
        fileExists = true;
        return true;
    }

    // 镜像 resolveKeyName：按 code+bitLength 精确匹配
    String resolveKeyName(uint32_t code, uint8_t bitLength) const {
        for (const auto& e : keyMap) {
            if (e.code == code && e.bitLength == bitLength) return e.name;
        }
        return "";
    }

    // 镜像 upsertKey：同名更新（去重），否则新增；受 MAX_RF_KEYS 上限约束（更新不受限）
    bool upsertKey(const String& name, const String& code, uint8_t bitLength,
                   uint16_t pulseWidth, String& err) {
        if (!mirrorRfValidName(name)) { err = "invalid name"; return false; }
        uint32_t codeVal = 0;
        if (!mirrorRfParseCode(code, codeVal)) { err = "invalid code"; return false; }
        if (bitLength < MIRROR_RF_MIN_DATA_BITS || bitLength > MIRROR_RF_MAX_DATA_BITS) {
            err = "invalid bitLength";
            return false;
        }
        if (keyMap.size() >= MIRROR_RF_MAX_KEYS) {
            bool found = false;
            for (auto& e : keyMap) {
                if (e.name == name) { found = true; break; }
            }
            if (!found) { err = "limit"; return false; }
        }
        for (auto& e : keyMap) {
            if (e.name == name) {
                e.code = codeVal;
                e.bitLength = bitLength;
                e.pulseWidth = pulseWidth;
                return persist();
            }
        }
        keyMap.push_back(MirrorRfKeyEntry{name, codeVal, bitLength, pulseWidth});
        return persist();
    }

    // 镜像 deleteKey
    bool deleteKey(const String& name) {
        for (auto it = keyMap.begin(); it != keyMap.end(); ++it) {
            if (it->name == name) {
                keyMap.erase(it);
                return persist();
            }
        }
        return false;
    }

    // 镜像 clearKeys：清空并持久化空数组
    bool clearKeys() {
        keyMap.clear();
        return persist();
    }
};

// ============================================================
// 镜像 RFLearnDriver begin()/stop()/check() 的锁契约与引脚变更处理
// （回归修复 #3 互斥锁并发保护 / #4 begin 引脚变更不调 stop 避免重入死锁）
// ============================================================
struct MirrorRfDriver {
    MirrorRfMutex mutex;
    bool initialized = false;
    uint8_t rxPin = 255;
    int attachCount = 0;    // attachInterrupt 次数
    int detachCount = 0;    // detachInterrupt 次数
    int checkBodyRuns = 0;  // check() 真正进入解码体的次数

    // 修复版 begin()：引脚变更时内联 detach（不调 stop()），避免锁重入
    bool beginFixed(uint8_t pin) {
        if (!mutex.take(true)) return false;          // 阻塞获锁
        if (initialized) {
            if (rxPin == pin) { mutex.give(); return true; }
            // 引脚变更：内联 detachInterrupt（而非调 stop()）→ 不重入锁
            detachCount++;
            initialized = false;
        }
        rxPin = pin;
        attachCount++;                                 // attachInterrupt(...)
        initialized = true;
        mutex.give();
        return true;
    }

    // 旧版（有缺陷）begin()：引脚变更时调 stop() → stop 重获同一锁 → 死锁
    bool beginBuggy(uint8_t pin) {
        if (!mutex.take(true)) return false;
        if (initialized && rxPin != pin) {
            stopLocked();                              // 旧代码：持锁调 stop() → 重入
        }
        rxPin = pin;
        attachCount++;
        initialized = true;
        mutex.give();
        return true;
    }

    // 镜像 stop()：阻塞获锁（begin 持锁时再获=死锁）
    void stopLocked() {
        if (!mutex.take(true)) return;                 // 重入点：持锁时获取失败
        if (initialized) {
            detachCount++;
            initialized = false;
        }
        mutex.give();
    }

    // 镜像 check()：非阻塞获锁，锁被占则跳过；所有 return 分支都释锁
    void check(bool frameReady) {
        if (!mutex.take(false)) return;                // 锁被占 → 跳过本轮
        if (!initialized) { mutex.give(); return; }
        if (!frameReady) { mutex.give(); return; }     // 无帧就绪 → 释锁返回
        checkBodyRuns++;                               // 真正进入解码
        mutex.give();
    }
};

// ============================================================
// TEST GROUP 1: EV1527 解码
// ============================================================

static void test_rf_decode_basic_24bit() {
    auto buf = buildEv1527Frame(0xAABBCC, 24, 350);
    MirrorRfDecodeResult r = mirrorRfDecode(buf);
    TEST_ASSERT_TRUE(r.valid);
    TEST_ASSERT_EQUAL_UINT32(0xAABBCCUL, r.code);
    TEST_ASSERT_EQUAL_UINT8(24, r.bits);
    TEST_ASSERT_EQUAL_UINT16(350, r.avgPulse);
}

static void test_rf_decode_all_ones_and_zeros() {
    auto ones = buildEv1527Frame(0xFFFFFF, 24, 350);
    MirrorRfDecodeResult r1 = mirrorRfDecode(ones);
    TEST_ASSERT_TRUE(r1.valid);
    TEST_ASSERT_EQUAL_UINT32(0xFFFFFFUL, r1.code);

    auto zeros = buildEv1527Frame(0x000000, 24, 350);
    MirrorRfDecodeResult r0 = mirrorRfDecode(zeros);
    TEST_ASSERT_TRUE(r0.valid);
    TEST_ASSERT_EQUAL_UINT32(0x000000UL, r0.code);
    TEST_ASSERT_EQUAL_UINT8(24, r0.bits);
}

static void test_rf_decode_12bit_minimum() {
    auto buf = buildEv1527Frame(0xABC, 12, 300);
    MirrorRfDecodeResult r = mirrorRfDecode(buf);
    TEST_ASSERT_TRUE(r.valid);
    TEST_ASSERT_EQUAL_UINT32(0xABCUL, r.code);
    TEST_ASSERT_EQUAL_UINT8(12, r.bits);
}

static void test_rf_decode_rejects_too_few_bits() {
    // 仅 8 位（< MIN_DATA_BITS=12）→ 无效
    auto buf = buildEv1527Frame(0xAB, 8, 350);
    MirrorRfDecodeResult r = mirrorRfDecode(buf);
    TEST_ASSERT_FALSE(r.valid);
}

static void test_rf_decode_rejects_pulse_too_short() {
    // minPulse=50 < 80 → 不合理脉宽
    auto buf = buildEv1527Frame(0xAABBCC, 24, 50);
    MirrorRfDecodeResult r = mirrorRfDecode(buf);
    TEST_ASSERT_FALSE(r.valid);
}

static void test_rf_decode_rejects_pulse_too_long() {
    // minPulse=2500 > 2000 → 不合理脉宽
    auto buf = buildEv1527Frame(0xAABBCC, 24, 2500);
    MirrorRfDecodeResult r = mirrorRfDecode(buf);
    TEST_ASSERT_FALSE(r.valid);
}

static void test_rf_decode_skips_abnormal_pulses() {
    // 在有效帧中插入一个超长异常脉冲对（> 6T），应被跳过而不破坏解码
    auto buf = buildEv1527Frame(0xAABB, 16, 350);
    // 在中间插入异常脉冲对 (9000, 9000) —— 远超 6T=2100
    buf.insert(buf.begin() + 8, {(uint16_t)9000, (uint16_t)9000});
    MirrorRfDecodeResult r = mirrorRfDecode(buf);
    TEST_ASSERT_TRUE(r.valid);
    TEST_ASSERT_EQUAL_UINT32(0xAABBUL, r.code);  // 异常对被跳过，编码不变
    TEST_ASSERT_EQUAL_UINT8(16, r.bits);
}

// ============================================================
// TEST GROUP 2: 学习状态机
// ============================================================

static void test_rf_learn_start_enters_learning() {
    MirrorRfLearn lm;
    lm.startLearn(10000, 1000);
    TEST_ASSERT_TRUE(lm.state == MirrorRfLearnState::LEARNING);
    TEST_ASSERT_EQUAL_STRING("learning", lm.stateName(1000).c_str());
}

static void test_rf_learn_capture_transitions() {
    MirrorRfLearn lm;
    lm.startLearn(10000, 1000);
    lm.capture("0xAABBCC", 24, 350);
    TEST_ASSERT_TRUE(lm.state == MirrorRfLearnState::CAPTURED);
    TEST_ASSERT_EQUAL_STRING("captured", lm.stateName(1000).c_str());
    TEST_ASSERT_EQUAL_STRING("0xAABBCC", lm.code.c_str());
    TEST_ASSERT_EQUAL_UINT8(24, lm.bits);
    TEST_ASSERT_EQUAL_UINT16(350, lm.pulse);
}

static void test_rf_learn_capture_ignored_when_not_learning() {
    MirrorRfLearn lm;  // IDLE
    lm.capture("0xAABBCC", 24, 350);
    TEST_ASSERT_TRUE(lm.state == MirrorRfLearnState::IDLE);
    TEST_ASSERT_EQUAL_STRING("", lm.code.c_str());
}

static void test_rf_learn_timeout_transition() {
    MirrorRfLearn lm;
    lm.startLearn(10000, 1000);   // deadline = 11000
    lm.updateTimeout(5000);
    TEST_ASSERT_TRUE(lm.state == MirrorRfLearnState::LEARNING);
    lm.updateTimeout(11000);      // 恰好到期
    TEST_ASSERT_TRUE(lm.state == MirrorRfLearnState::TIMEOUT);
    TEST_ASSERT_EQUAL_STRING("timeout", lm.stateName(11000).c_str());
}

static void test_rf_learn_stop_cancels() {
    MirrorRfLearn lm;
    lm.startLearn(10000, 1000);
    lm.stopLearn();
    TEST_ASSERT_TRUE(lm.state == MirrorRfLearnState::IDLE);
    TEST_ASSERT_EQUAL_STRING("", lm.code.c_str());
}

static void test_rf_learn_timeout_clamped() {
    MirrorRfLearn lo, hi;
    lo.startLearn(100, 1000);     // < 1000 钳到 1000
    TEST_ASSERT_EQUAL_UINT32(1000UL, (uint32_t)(lo.deadline - 1000));
    hi.startLearn(999999, 1000);  // > 60000 钳到 60000
    TEST_ASSERT_EQUAL_UINT32(60000UL, (uint32_t)(hi.deadline - 1000));
}

// ============================================================
// TEST GROUP 3: 防重复触发（300ms，code+bits 双条件）
// ============================================================

static void test_rf_debounce_same_code_filtered() {
    MirrorRfDebounce db;
    TEST_ASSERT_TRUE(db.shouldProcess(0xAABBCC, 24, 1000));
    // 相同 code+bits，100ms 后（< 300ms）被过滤
    TEST_ASSERT_FALSE(db.shouldProcess(0xAABBCC, 24, 1100));
    TEST_ASSERT_FALSE(db.shouldProcess(0xAABBCC, 24, 1299));
}

static void test_rf_debounce_after_interval_passes() {
    MirrorRfDebounce db;
    TEST_ASSERT_TRUE(db.shouldProcess(0xAABBCC, 24, 1000));
    // 恰好 300ms 后通过
    TEST_ASSERT_TRUE(db.shouldProcess(0xAABBCC, 24, 1300));
}

static void test_rf_debounce_different_code_passes_immediately() {
    MirrorRfDebounce db;
    TEST_ASSERT_TRUE(db.shouldProcess(0xAABBCC, 24, 1000));
    // 不同编码即使间隔很短也放行（与 IR 的关键差异）
    TEST_ASSERT_TRUE(db.shouldProcess(0x112233, 24, 1050));
}

static void test_rf_debounce_same_code_different_bits_passes() {
    MirrorRfDebounce db;
    TEST_ASSERT_TRUE(db.shouldProcess(0xAABBCC, 24, 1000));
    // code 相同但位长不同 → 不视为重复
    TEST_ASSERT_TRUE(db.shouldProcess(0xAABBCC, 20, 1050));
}

// ============================================================
// TEST GROUP 4: 按键映射 CRUD
// ============================================================

static void test_rf_keymap_upsert_add_and_update() {
    MirrorRfKeyStore s;
    String err;
    TEST_ASSERT_TRUE(s.upsertKey("door", "0xAABBCC", 24, 350, err));
    TEST_ASSERT_EQUAL_UINT(1, (unsigned)s.keyMap.size());
    // 同名更新：去重，不新增
    TEST_ASSERT_TRUE(s.upsertKey("door", "0x112233", 24, 300, err));
    TEST_ASSERT_EQUAL_UINT(1, (unsigned)s.keyMap.size());
    TEST_ASSERT_EQUAL_UINT32(0x112233UL, s.keyMap[0].code);
    TEST_ASSERT_EQUAL_UINT16(300, s.keyMap[0].pulseWidth);
}

static void test_rf_keymap_name_allows_space() {
    MirrorRfKeyStore s;
    String err;
    // RF 名称允许空格（与 IR 不同）
    TEST_ASSERT_TRUE(s.upsertKey("door bell", "0xAABBCC", 24, 350, err));
    TEST_ASSERT_EQUAL_UINT(1, (unsigned)s.keyMap.size());
}

static void test_rf_keymap_rejects_invalid_name() {
    MirrorRfKeyStore s;
    String err;
    TEST_ASSERT_FALSE(s.upsertKey("", "0xAABBCC", 24, 350, err));
    TEST_ASSERT_FALSE(s.upsertKey("bad.name", "0xAABBCC", 24, 350, err));
    TEST_ASSERT_EQUAL_UINT(0, (unsigned)s.keyMap.size());
}

static void test_rf_keymap_rejects_invalid_bitlength() {
    MirrorRfKeyStore s;
    String err;
    TEST_ASSERT_FALSE(s.upsertKey("k", "0xAABBCC", 8, 350, err));   // < 12
    TEST_ASSERT_FALSE(s.upsertKey("k", "0xAABBCC", 40, 350, err));   // > 32
    TEST_ASSERT_TRUE(s.upsertKey("k", "0xAABBCC", 12, 350, err));    // 边界 12 合法
    TEST_ASSERT_TRUE(s.upsertKey("k2", "0xAABBCC", 32, 350, err));   // 边界 32 合法
}

static void test_rf_keymap_rejects_invalid_code() {
    MirrorRfKeyStore s;
    String err;
    // "ZZ" strtoul=0 且非 0x0*/"0" → 非法
    TEST_ASSERT_FALSE(s.upsertKey("k", "ZZ", 24, 350, err));
    // 显式 0 合法
    TEST_ASSERT_TRUE(s.upsertKey("zero", "0", 24, 350, err));
}

static void test_rf_keymap_delete_and_clear() {
    MirrorRfKeyStore s;
    String err;
    s.upsertKey("a", "0x1", 24, 350, err);
    s.upsertKey("b", "0x2", 24, 350, err);
    TEST_ASSERT_TRUE(s.deleteKey("a"));
    TEST_ASSERT_EQUAL_UINT(1, (unsigned)s.keyMap.size());
    TEST_ASSERT_FALSE(s.deleteKey("nonexistent"));
    TEST_ASSERT_TRUE(s.clearKeys());
    TEST_ASSERT_EQUAL_UINT(0, (unsigned)s.keyMap.size());
    TEST_ASSERT_TRUE(s.fileExists);  // RF 清码仍写空数组文件（与 IR 删除文件不同）
}

static void test_rf_keymap_resolve_by_code_and_bits() {
    MirrorRfKeyStore s;
    String err;
    s.upsertKey("door", "0xAABBCC", 24, 350, err);
    TEST_ASSERT_EQUAL_STRING("door", s.resolveKeyName(0xAABBCC, 24).c_str());
    // code 对但位长不符 → 不命中
    TEST_ASSERT_EQUAL_STRING("", s.resolveKeyName(0xAABBCC, 20).c_str());
    // 位长对但 code 不符 → 不命中
    TEST_ASSERT_EQUAL_STRING("", s.resolveKeyName(0xDEADBEEF, 24).c_str());
}

static void test_rf_keymap_enforces_max_limit() {
    MirrorRfKeyStore s;
    String err;
    for (size_t i = 0; i < MIRROR_RF_MAX_KEYS; ++i) {
        String name = "k" + String((int)i);
        char hex[12];
        snprintf(hex, sizeof(hex), "0x%X", (unsigned)(0x1000 + i));
        TEST_ASSERT_TRUE(s.upsertKey(name, hex, 24, 350, err));
    }
    TEST_ASSERT_EQUAL_UINT((unsigned)MIRROR_RF_MAX_KEYS, (unsigned)s.keyMap.size());
    // 第 41 个新按键被拒绝
    TEST_ASSERT_FALSE(s.upsertKey("overflow", "0x9999", 24, 350, err));
    // 但已存在按键的更新不受上限影响
    TEST_ASSERT_TRUE(s.upsertKey("k0", "0xABCD", 24, 350, err));
}

// ============================================================
// TEST GROUP 5: begin 引脚变更修复（修复 #4：不调 stop() 避免锁重入死锁）
// ============================================================

// 修复版：引脚变更内联 detach，全程无锁重入，旧引脚正确 detach
static void test_rf_begin_pin_change_no_reentry() {
    MirrorRfDriver d;
    TEST_ASSERT_TRUE(d.beginFixed(4));
    TEST_ASSERT_TRUE(d.beginFixed(5));   // 引脚变更 4 → 5
    TEST_ASSERT_EQUAL_INT(0, d.mutex.reentryAttempts);  // 无重入（避免死锁）
    TEST_ASSERT_EQUAL_UINT8(5, d.rxPin);
    TEST_ASSERT_TRUE(d.initialized);
    TEST_ASSERT_EQUAL_INT(1, d.detachCount);   // 旧引脚 detach 一次
    TEST_ASSERT_EQUAL_INT(2, d.attachCount);   // 引脚 4 和 5 各 attach 一次
}

// 同引脚重复 begin：幂等，不重复 attach/detach
static void test_rf_begin_same_pin_idempotent() {
    MirrorRfDriver d;
    TEST_ASSERT_TRUE(d.beginFixed(4));
    TEST_ASSERT_TRUE(d.beginFixed(4));   // 同引脚
    TEST_ASSERT_EQUAL_INT(1, d.attachCount);   // 仅 attach 一次
    TEST_ASSERT_EQUAL_INT(0, d.detachCount);
    TEST_ASSERT_EQUAL_INT(0, d.mutex.reentryAttempts);
}

// 反例：旧版引脚变更调 stop() → 持锁期间重获同一锁 → 死锁（检测到重入）
static void test_rf_old_begin_pin_change_would_reenter() {
    MirrorRfDriver d;
    d.beginBuggy(4);
    d.beginBuggy(5);   // 引脚变更 → 内部调 stop() → 重入
    TEST_ASSERT_GREATER_THAN(0, d.mutex.reentryAttempts);  // 证明死锁路径
}

// ============================================================
// TEST GROUP 6: 互斥锁并发契约（修复 #3：check/stop 竞态防护）
// ============================================================

// 契约 1：锁被占用时 check() 跳过本轮，不进入解码体
static void test_rf_lock_check_skips_when_held() {
    MirrorRfDriver d;
    d.beginFixed(4);
    d.mutex.take(false);              // 模拟另一任务（stop/begin）持锁
    int before = d.checkBodyRuns;
    d.check(true);                    // 有帧就绪，但锁被占 → 仍应跳过
    TEST_ASSERT_EQUAL_INT(before, d.checkBodyRuns);
    d.mutex.give();
}

// 契约 2：check() 所有 return 分支都释放锁（不泄漏）
static void test_rf_lock_check_releases_on_all_paths() {
    MirrorRfDriver d;
    d.beginFixed(4);
    d.check(false);                   // 无帧就绪路径
    TEST_ASSERT_FALSE(d.mutex.held);
    d.check(true);                    // 正常解码路径
    TEST_ASSERT_FALSE(d.mutex.held);
    d.stopLocked();
    d.check(true);                    // 未初始化路径
    TEST_ASSERT_FALSE(d.mutex.held);
}

// 契约 3：begin/stop 阻塞获锁并释放，无重入
static void test_rf_lock_begin_stop_balanced() {
    MirrorRfDriver d;
    d.beginFixed(4);
    TEST_ASSERT_FALSE(d.mutex.held);
    d.stopLocked();
    TEST_ASSERT_FALSE(d.mutex.held);
    TEST_ASSERT_FALSE(d.initialized);
    TEST_ASSERT_EQUAL_INT(0, d.mutex.reentryAttempts);
}

// ============================================================
// 测试组注册
// ============================================================
void test_rf_remote_group() {
    // Group 1: EV1527 解码
    RUN_TEST(test_rf_decode_basic_24bit);
    RUN_TEST(test_rf_decode_all_ones_and_zeros);
    RUN_TEST(test_rf_decode_12bit_minimum);
    RUN_TEST(test_rf_decode_rejects_too_few_bits);
    RUN_TEST(test_rf_decode_rejects_pulse_too_short);
    RUN_TEST(test_rf_decode_rejects_pulse_too_long);
    RUN_TEST(test_rf_decode_skips_abnormal_pulses);

    // Group 2: 学习状态机
    RUN_TEST(test_rf_learn_start_enters_learning);
    RUN_TEST(test_rf_learn_capture_transitions);
    RUN_TEST(test_rf_learn_capture_ignored_when_not_learning);
    RUN_TEST(test_rf_learn_timeout_transition);
    RUN_TEST(test_rf_learn_stop_cancels);
    RUN_TEST(test_rf_learn_timeout_clamped);

    // Group 3: 防重复触发
    RUN_TEST(test_rf_debounce_same_code_filtered);
    RUN_TEST(test_rf_debounce_after_interval_passes);
    RUN_TEST(test_rf_debounce_different_code_passes_immediately);
    RUN_TEST(test_rf_debounce_same_code_different_bits_passes);

    // Group 4: 按键映射 CRUD
    RUN_TEST(test_rf_keymap_upsert_add_and_update);
    RUN_TEST(test_rf_keymap_name_allows_space);
    RUN_TEST(test_rf_keymap_rejects_invalid_name);
    RUN_TEST(test_rf_keymap_rejects_invalid_bitlength);
    RUN_TEST(test_rf_keymap_rejects_invalid_code);
    RUN_TEST(test_rf_keymap_delete_and_clear);
    RUN_TEST(test_rf_keymap_resolve_by_code_and_bits);
    RUN_TEST(test_rf_keymap_enforces_max_limit);

    // Group 5: begin 引脚变更修复（修复 #4）
    RUN_TEST(test_rf_begin_pin_change_no_reentry);
    RUN_TEST(test_rf_begin_same_pin_idempotent);
    RUN_TEST(test_rf_old_begin_pin_change_would_reenter);

    // Group 6: 互斥锁并发契约（修复 #3）
    RUN_TEST(test_rf_lock_check_skips_when_held);
    RUN_TEST(test_rf_lock_check_releases_on_all_paths);
    RUN_TEST(test_rf_lock_begin_stop_balanced);
}
