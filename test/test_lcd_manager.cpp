/**
 * @file test_lcd_manager.cpp
 * @brief LCD/OLED 显示屏管理器单元测试
 * 
 * 测试内容（纯逻辑，不依赖硬件）：
 * - 分辨率 → 最大行数计算（128x64, 128x32, 72x40, 64x128）
 * - LCD 配置参数 JSON 序列化/反序列化（含 contrast 对比度）
 * - 自定义文本行数截断逻辑（不同分辨率）
 * - OLED 显示动作类型配置（外设执行规则）
 * - ESP32-C3 引脚校验逻辑
 * - 开机重申（performMaintenance）时间窗口与限流逻辑
 * - 源码级回归守卫：禁止在周期性重申里使用 begin()（会清空画面缓冲区）
 * - 出厂配置文件守卫：oled 参数完整性、无临时诊断规则残留
 */

#include <unity.h>
#include <Arduino.h>
#include <ArduinoJson.h>
#include <fstream>
#include <sstream>
#include <string>
#include "mocks/MockPeripheral.h"
#include "helpers/TestConfig.h"
#include "helpers/TestAssertions.h"

void test_lcd_manager_group();

// 辅助：读取项目源文件（用于源码级回归守卫）
static std::string readLcdSrcFile(const char* relativePath) {
    const char* roots[] = { ".", "..", "../..", "../../..", "../../../.." };
    for (const char* root : roots) {
        std::string fullPath = std::string(root) + "/" + relativePath;
        std::ifstream file(fullPath);
        if (file.is_open()) {
            std::ostringstream ss;
            ss << file.rdbuf();
            return ss.str();
        }
    }
    return "";
}

// 辅助：截取某个函数定义的函数体（首个行首右花括号为止）
static std::string extractFunctionBody(const std::string& content, const char* signature) {
    size_t start = content.find(signature);
    if (start == std::string::npos) return "";
    size_t end = content.find("\n}", start);
    if (end == std::string::npos) return content.substr(start);
    return content.substr(start, end - start);
}

static bool srcContains(const std::string& haystack, const char* needle) {
    return haystack.find(needle) != std::string::npos;
}

// 辅助：剔除行注释，使“禁止调用某 API”类守卫不会误伤说明文字
static std::string stripLineComments(const std::string& src) {
    std::string out;
    out.reserve(src.size());
    size_t i = 0;
    while (i < src.size()) {
        if (src[i] == '/' && i + 1 < src.size() && src[i + 1] == '/') {
            while (i < src.size() && src[i] != '\n') i++;
        } else {
            out += src[i++];
        }
    }
    return out;
}

// ========== 镜像 LCDManager 行计算逻辑 ==========

/**
 * @brief 镜像 LCDManager::getFontHeight()
 * 
 * 字体索引 0 = 8px, 1 = 13px(WQY12), 2 = 16px(WQY15)
 */
static uint8_t mirrorGetFontHeight(uint8_t fontIndex) {
    switch (fontIndex) {
        case 0: return 8;
        case 1: return 13;
        case 2: return 16;
        default: return 13;
    }
}

/**
 * @brief 镜像 LCDManager::getMaxLines()
 */
static uint8_t mirrorGetMaxLines(uint8_t screenHeight, uint8_t fontIndex) {
    uint8_t fontH = mirrorGetFontHeight(fontIndex);
    if (fontH == 0) return 0;
    return screenHeight / fontH;
}

/**
 * @brief 镜像 showCustomText 行数截断逻辑
 * 
 * 返回实际可显示行数（受屏高和硬上限 MAX_CUSTOM_LINES=6 约束）
 */
static uint8_t mirrorGetMaxCustomLines(uint8_t screenHeight, uint8_t fontIndex) {
    static constexpr uint8_t MAX_CUSTOM_LINES = 6;
    uint8_t maxPixelLines = mirrorGetMaxLines(screenHeight, fontIndex);
    if (maxPixelLines == 0) maxPixelLines = 4;
    return (maxPixelLines < MAX_CUSTOM_LINES) ? maxPixelLines : MAX_CUSTOM_LINES;
}

/**
 * @brief 镜像 createDisplay 的分辨率匹配逻辑
 * 
 * 返回匹配结果：0=128x64 SSD1306, 1=128x32 SSD1306, 2=72x40 SSD1306,
 *                3=64x128 SH1106, 99=默认回退(128x64)
 */
static int mirrorResolutionMatch(uint8_t width, uint8_t height) {
    if (width == 128 && height == 64) return 0;
    if (width == 128 && height == 32) return 1;
    if (width == 72  && height == 40) return 2;
    if (width == 64  && height == 128) return 3;
    return 99; // fallback to 128x64
}

// ========== ESP32-C3 引脚校验镜像 ==========

static bool isValidGpioC3(int pin) {
    return (pin >= 0 && pin <= 21);
}

static bool validateI2CPinsC3(int sda, int scl) {
    return isValidGpioC3(sda) && isValidGpioC3(scl);
}

// ========== 镜像 contrast（对比度）解析逻辑 ==========

/**
 * @brief 镜像 PeripheralManager::loadConfiguration 中 contrast 的解析与截断
 */
static uint8_t mirrorParseContrast(JsonObject params) {
    int contrast = params["contrast"] | 255;  // 默认拉满
    return (uint8_t)(contrast > 255 ? 255 : (contrast < 0 ? 0 : contrast));
}

/**
 * @brief 镜像 LCDManager::initialize 中的生效值换算（0 视为未配置 → 255）
 */
static uint8_t mirrorEffectiveContrast(uint8_t configured) {
    return (configured == 0) ? 255 : configured;
}

// ========== 镜像 performMaintenance 重申限流逻辑 ==========

static constexpr unsigned long REASSERT_WINDOW_MS = 30000;
static constexpr unsigned long REASSERT_INTERVAL_MS = 3000;

/**
 * @brief 镜像 LCDManager::performMaintenance 的时间闸门
 *
 * 仅在初始化后 30 秒窗口内、且距上次重申满 3 秒时才重申一次。
 */
static bool mirrorShouldReassert(unsigned long now, unsigned long initTime, unsigned long lastReassert) {
    if (now - initTime > REASSERT_WINDOW_MS) return false;
    if (now - lastReassert < REASSERT_INTERVAL_MS) return false;
    return true;
}

/**
 * @brief 模拟维护任务按固定节拍运行，统计实际重申次数
 */
static int simulateReassertCount(unsigned long initTime, unsigned long tickMs, unsigned long totalMs) {
    unsigned long lastReassert = initTime;
    int count = 0;
    for (unsigned long elapsed = 0; elapsed <= totalMs; elapsed += tickMs) {
        unsigned long now = initTime + elapsed;
        if (mirrorShouldReassert(now, initTime, lastReassert)) {
            count++;
            lastReassert = now;
        }
    }
    return count;
}

// ============================================================
//  TEST GROUP 1: 分辨率 → 最大行数计算
// ============================================================

static void test_lcd_max_lines_128x64_default_font() {
    // 128x64 + WQY12(font=1, h=13): 64/13 = 4 行
    uint8_t lines = mirrorGetMaxLines(64, 1);
    TEST_ASSERT_EQUAL(4, lines);
}

static void test_lcd_max_lines_128x32_default_font() {
    // 128x32 + WQY12(font=1, h=13): 32/13 = 2 行
    uint8_t lines = mirrorGetMaxLines(32, 1);
    TEST_ASSERT_EQUAL(2, lines);
}

static void test_lcd_max_lines_72x40_default_font() {
    // 72x40 + WQY12(font=1, h=13): 40/13 = 3 行
    uint8_t lines = mirrorGetMaxLines(40, 1);
    TEST_ASSERT_EQUAL(3, lines);
}

static void test_lcd_max_lines_64x128_default_font() {
    // 64x128 (SH1106 竖屏) + WQY12(font=1, h=13): 128/13 = 9 行
    uint8_t lines = mirrorGetMaxLines(128, 1);
    TEST_ASSERT_EQUAL(9, lines);
}

static void test_lcd_max_lines_small_font() {
    // 128x64 + small(font=0, h=8): 64/8 = 8 行
    uint8_t lines = mirrorGetMaxLines(64, 0);
    TEST_ASSERT_EQUAL(8, lines);
}

static void test_lcd_max_lines_large_font_72x40() {
    // 72x40 + large(font=2, h=16): 40/16 = 2 行
    uint8_t lines = mirrorGetMaxLines(40, 2);
    TEST_ASSERT_EQUAL(2, lines);
}

// ============================================================
//  TEST GROUP 2: 分辨率匹配逻辑
// ============================================================

static void test_lcd_match_128x64_ssd1306() {
    TEST_ASSERT_EQUAL(0, mirrorResolutionMatch(128, 64));
}

static void test_lcd_match_128x32_ssd1306() {
    TEST_ASSERT_EQUAL(1, mirrorResolutionMatch(128, 32));
}

static void test_lcd_match_72x40_ssd1306_c3() {
    // ESP32-C3 OLED 开发板 72x40
    TEST_ASSERT_EQUAL(2, mirrorResolutionMatch(72, 40));
}

static void test_lcd_match_64x128_sh1106() {
    TEST_ASSERT_EQUAL(3, mirrorResolutionMatch(64, 128));
}

static void test_lcd_match_unknown_fallback() {
    // 未知分辨率应回退到默认 128x64
    TEST_ASSERT_EQUAL(99, mirrorResolutionMatch(100, 50));
    TEST_ASSERT_EQUAL(99, mirrorResolutionMatch(0, 0));
}

// ============================================================
//  TEST GROUP 3: 自定义文本行数截断
// ============================================================

static void test_custom_lines_128x64_max_4_plus_title() {
    // 128x64 默认字体最多 4 行，硬上限 6，实际取 min(4,6)=4
    uint8_t maxLines = mirrorGetMaxCustomLines(64, 1);
    TEST_ASSERT_EQUAL(4, maxLines);
}

static void test_custom_lines_128x32_max_2() {
    // 128x32 默认字体最多 2 行
    uint8_t maxLines = mirrorGetMaxCustomLines(32, 1);
    TEST_ASSERT_EQUAL(2, maxLines);
}

static void test_custom_lines_72x40_max_3() {
    // 72x40 默认字体最多 3 行
    uint8_t maxLines = mirrorGetMaxCustomLines(40, 1);
    TEST_ASSERT_EQUAL(3, maxLines);
}

static void test_custom_lines_64x128_capped_at_6() {
    // 64x128 默认字体最多 9 行，但硬上限 6，取 min(9,6)=6
    uint8_t maxLines = mirrorGetMaxCustomLines(128, 1);
    TEST_ASSERT_EQUAL(6, maxLines);
}

static void test_custom_lines_72x40_large_font_max_2() {
    // 72x40 大字体(h=16) 最多 2 行
    uint8_t maxLines = mirrorGetMaxCustomLines(40, 2);
    TEST_ASSERT_EQUAL(2, maxLines);
}

// ============================================================
//  TEST GROUP 4: LCD 配置参数 JSON 序列化/反序列化
// ============================================================

static void test_lcd_config_json_serialize_128x64() {
    JsonDocument doc;
    doc["width"] = 128;
    doc["height"] = 64;
    doc["interface"] = 2; // I2C
    
    int w = doc["width"] | 128;
    int h = doc["height"] | 64;
    int iface = doc["interface"] | 2;
    
    TEST_ASSERT_EQUAL(128, w);
    TEST_ASSERT_EQUAL(64, h);
    TEST_ASSERT_EQUAL(2, iface);
}

static void test_lcd_config_json_serialize_72x40() {
    JsonDocument doc;
    doc["width"] = 72;
    doc["height"] = 40;
    doc["interface"] = 2;
    
    int w = doc["width"] | 128;
    int h = doc["height"] | 64;
    int iface = doc["interface"] | 2;
    
    TEST_ASSERT_EQUAL(72, w);
    TEST_ASSERT_EQUAL(40, h);
    TEST_ASSERT_EQUAL(2, iface);
}

static void test_lcd_config_json_defaults() {
    // 空 JSON 应使用默认值 128x64 I2C
    JsonDocument doc;
    
    int w = doc["width"] | 128;
    int h = doc["height"] | 64;
    int iface = doc["interface"] | 2;
    
    TEST_ASSERT_EQUAL(128, w);
    TEST_ASSERT_EQUAL(64, h);
    TEST_ASSERT_EQUAL(2, iface);
}

static void test_lcd_config_json_clamp_overflow() {
    // 镜像 PeripheralManager 中的截断逻辑
    JsonDocument doc;
    doc["width"] = 300;   // > 255
    doc["height"] = -10;  // < 0
    
    int w = doc["width"] | 128;
    int h = doc["height"] | 64;
    
    uint8_t clampedW = (uint8_t)(w > 255 ? 255 : (w < 0 ? 0 : w));
    uint8_t clampedH = (uint8_t)(h > 255 ? 255 : (h < 0 ? 0 : h));
    
    TEST_ASSERT_EQUAL(255, clampedW);
    TEST_ASSERT_EQUAL(0, clampedH);
}

static void test_lcd_config_json_spi_interface() {
    JsonDocument doc;
    doc["width"] = 128;
    doc["height"] = 64;
    doc["interface"] = 1; // SPI
    
    int iface = doc["interface"] | 2;
    TEST_ASSERT_EQUAL(1, iface);
}

// ============================================================
//  TEST GROUP 5: ESP32-C3 I2C 引脚校验
// ============================================================

static void test_c3_i2c_pins_valid_5_6() {
    // ESP32-C3 OLED 标准接线：SDA=5, SCL=6
    TEST_ASSERT_TRUE(validateI2CPinsC3(5, 6));
}

static void test_c3_i2c_pins_valid_boundary() {
    // GPIO 0 和 21 均合法
    TEST_ASSERT_TRUE(validateI2CPinsC3(0, 21));
    TEST_ASSERT_TRUE(validateI2CPinsC3(21, 0));
}

static void test_c3_i2c_pins_invalid_22() {
    // GPIO 22 在 C3 上不合法（有效范围 0-21）
    TEST_ASSERT_FALSE(validateI2CPinsC3(5, 22));
    TEST_ASSERT_FALSE(validateI2CPinsC3(22, 6));
}

static void test_c3_i2c_pins_invalid_negative() {
    TEST_ASSERT_FALSE(validateI2CPinsC3(-1, 6));
    TEST_ASSERT_FALSE(validateI2CPinsC3(5, -1));
}

// ============================================================
//  TEST GROUP 6: OLED 外设执行动作配置
// ============================================================

static void test_oled_exec_action_type_exists() {
    // 验证 ActionType 包含 OLED_DISPLAY（镜像自定义显示动作）
    // ActionType::OLED_DISPLAY 在真实代码中的值
    // 这里通过 MockPeriphExecManager 验证规则可添加
    MockPeriphExecManager& mgr = MockPeriphExecManager::getInstance();
    mgr.initialize();
    
    PeriphExecRule rule;
    rule.id = "oled_display_rule";
    rule.name = "OLED Show Temp";
    rule.enabled = true;
    rule.triggerType = TriggerType::TIMER_SCHEDULE;
    rule.actionType = ActionType::OLED_DISPLAY;  // OLED 自定义显示
    rule.actionValue = "# Env\nTemp:${dht_01.temperature}";
    rule.targetPeriphId = "oled_display";
    
    TEST_ASSERT_TRUE(mgr.addRule(rule));
    
    PeriphExecRule* fetched = mgr.getRule("oled_display_rule");
    TEST_ASSERT_NOT_NULL(fetched);
    TEST_ASSERT_EQUAL(ActionType::OLED_DISPLAY, fetched->actionType);
    TEST_ASSERT_TRUE(fetched->actionValue.indexOf("temperature") >= 0);
}

static void test_oled_exec_multiline_content() {
    // 验证多行 OLED 内容解析
    String content = "# 环境监测\n温度:25.5°C\n湿度:60%";
    
    // 统计 \n 分隔的行数
    int lineCount = 1;
    for (int i = 0; i < (int)content.length(); i++) {
        if (content[i] == '\n') lineCount++;
    }
    
    TEST_ASSERT_EQUAL(3, lineCount);
    
    // 首行以 # 开头 → 标题行
    int firstNewline = content.indexOf('\n');
    String firstLine = content.substring(0, firstNewline);
    TEST_ASSERT_TRUE(firstLine.startsWith("#"));
}

static void test_oled_exec_content_truncation_72x40() {
    // 72x40 屏最多 3 行（默认字体），超出部分应被截断
    String content = "# Title\nLine1\nLine2\nLine3\nLine4";
    uint8_t maxLines = mirrorGetMaxCustomLines(40, 1); // = 3
    
    // 统计实际行数
    int totalLines = 1;
    for (int i = 0; i < (int)content.length(); i++) {
        if (content[i] == '\n') totalLines++;
    }
    TEST_ASSERT_EQUAL(5, totalLines);  // 5 行内容
    
    // 截断后应只保留 maxLines 行
    TEST_ASSERT_EQUAL(3, maxLines);
    TEST_ASSERT_TRUE(totalLines > maxLines);  // 需要截断
}

// ============================================================
//  TEST GROUP 7: 传感器分页显示计算
// ============================================================

static void test_sensor_page_count_128x64() {
    // 128x64: maxLines=4, 每页显示 3 条（预留标题行）
    uint8_t linesPerPage = mirrorGetMaxLines(64, 1) - 1;  // = 3
    
    // 5 条传感器数据 → 2 页
    uint8_t entryCount = 5;
    uint8_t pages = (entryCount + linesPerPage - 1) / linesPerPage;
    TEST_ASSERT_EQUAL(2, pages);
}

static void test_sensor_page_count_72x40() {
    // 72x40: maxLines=3, 每页显示 2 条（预留标题行）
    uint8_t linesPerPage = mirrorGetMaxLines(40, 1) - 1;  // = 2
    
    // 5 条传感器数据 → 3 页
    uint8_t entryCount = 5;
    uint8_t pages = (entryCount + linesPerPage - 1) / linesPerPage;
    TEST_ASSERT_EQUAL(3, pages);
}

static void test_sensor_page_count_128x32() {
    // 128x32: maxLines=2, 每页显示 1 条
    uint8_t linesPerPage = mirrorGetMaxLines(32, 1) - 1;  // = 1
    
    // 3 条传感器数据 → 3 页
    uint8_t entryCount = 3;
    uint8_t pages = (entryCount + linesPerPage - 1) / linesPerPage;
    TEST_ASSERT_EQUAL(3, pages);
}

static void test_sensor_page_count_single_entry() {
    // 只有 1 条数据时，任何屏幕都只需 1 页
    uint8_t linesPerPage64 = mirrorGetMaxLines(64, 1) - 1;
    uint8_t linesPerPage40 = mirrorGetMaxLines(40, 1) - 1;
    
    TEST_ASSERT_EQUAL(1, (1 + linesPerPage64 - 1) / linesPerPage64);
    TEST_ASSERT_EQUAL(1, (1 + linesPerPage40 - 1) / linesPerPage40);
}

// ============================================================
//  TEST GROUP 8: contrast（对比度）配置贯通
// ============================================================

static void test_contrast_default_when_absent() {
    // 旧配置文件没有 contrast 字段时，应默认拉满 255（保持升级前行为）
    JsonDocument doc;
    JsonObject params = doc.to<JsonObject>();
    params["width"] = 128;
    params["height"] = 64;

    TEST_ASSERT_EQUAL_UINT8(255, mirrorParseContrast(params));
}

static void test_contrast_explicit_value_preserved() {
    // 显式配置的中间值必须原样保留，不能被默认值覆盖
    JsonDocument doc;
    JsonObject params = doc.to<JsonObject>();
    params["contrast"] = 200;

    TEST_ASSERT_EQUAL_UINT8(200, mirrorParseContrast(params));
    TEST_ASSERT_EQUAL_UINT8(200, mirrorEffectiveContrast(200));
}

static void test_contrast_zero_treated_as_unconfigured() {
    // contrast=0 会让屏幕完全看不见，视为未配置 → 按最大亮度处理
    JsonDocument doc;
    JsonObject params = doc.to<JsonObject>();
    params["contrast"] = 0;

    TEST_ASSERT_EQUAL_UINT8(0, mirrorParseContrast(params));
    TEST_ASSERT_EQUAL_UINT8(255, mirrorEffectiveContrast(0));
}

static void test_contrast_clamp_overflow() {
    // 超出 uint8_t 范围的值必须被截断，不能回绕成小值
    JsonDocument doc;
    JsonObject params = doc.to<JsonObject>();
    params["contrast"] = 300;
    TEST_ASSERT_EQUAL_UINT8(255, mirrorParseContrast(params));

    params["contrast"] = 256;
    TEST_ASSERT_EQUAL_UINT8(255, mirrorParseContrast(params));
}

static void test_contrast_clamp_negative() {
    // 负值截断为 0，再经生效值换算回到 255（不会出现黑屏）
    JsonDocument doc;
    JsonObject params = doc.to<JsonObject>();
    params["contrast"] = -10;

    uint8_t stored = mirrorParseContrast(params);
    TEST_ASSERT_EQUAL_UINT8(0, stored);
    TEST_ASSERT_EQUAL_UINT8(255, mirrorEffectiveContrast(stored));
}

static void test_contrast_roundtrip_through_peripheral_config() {
    // 配置结构体必须能携带 contrast 存取（不丢字段）
    MockPeripheralManager& pm = MockPeripheralManager::getInstance();
    pm.initialize();

    PeripheralConfig config;
    config.id = "oled_contrast";
    config.name = "OLED显示屏";
    config.type = PeripheralType::LCD;
    config.lcd.width = 128;
    config.lcd.height = 32;
    config.lcd.interface = 2;
    config.lcd.contrast = 200;

    TEST_ASSERT_TRUE(pm.addPeripheral(config));

    PeripheralConfig* fetched = pm.getPeripheral("oled_contrast");
    TEST_ASSERT_NOT_NULL(fetched);
    TEST_ASSERT_EQUAL_UINT8(200, fetched->lcd.contrast);
    TEST_ASSERT_EQUAL_UINT8(32, fetched->lcd.height);
}

static void test_contrast_default_in_config_struct() {
    // 未赋值时结构体默认值应为 255，避免新增外设一上电就是黑屏
    PeripheralConfig config;
    TEST_ASSERT_EQUAL_UINT8(255, config.lcd.contrast);
}

// ============================================================
//  TEST GROUP 9: 开机重申（performMaintenance）限流逻辑
// ============================================================

static void test_reassert_not_before_interval() {
    // 初始化后不到 3 秒不应重申，避免开机瞬间刷 I2C
    unsigned long t0 = 100000;
    TEST_ASSERT_FALSE(mirrorShouldReassert(t0, t0, t0));
    TEST_ASSERT_FALSE(mirrorShouldReassert(t0 + 2999, t0, t0));
    TEST_ASSERT_TRUE(mirrorShouldReassert(t0 + 3000, t0, t0));
}

static void test_reassert_stops_after_window() {
    // 超过 30 秒稳定窗口后必须停止重申，不能长期占用 I2C 带宽
    unsigned long t0 = 100000;
    TEST_ASSERT_TRUE(mirrorShouldReassert(t0 + 30000, t0, t0));
    TEST_ASSERT_FALSE(mirrorShouldReassert(t0 + 30001, t0, t0));
    TEST_ASSERT_FALSE(mirrorShouldReassert(t0 + 60000, t0, t0));
}

static void test_reassert_count_with_1s_task_tick() {
    // 维护任务注册为 1 秒节拍，30 秒窗口内应重申 10 次（3s,6s,...,30s）
    int count = simulateReassertCount(100000, 1000, 40000);
    TEST_ASSERT_EQUAL(10, count);
}

static void test_reassert_count_not_amplified_by_fast_tick() {
    // 即使节拍变快（100ms），限流也应把次数锁在 10 次
    int count = simulateReassertCount(100000, 100, 40000);
    TEST_ASSERT_EQUAL(10, count);
}

static void test_reassert_survives_millis_overflow() {
    // millis() 回绕前后无符号减法仍然成立，不能因溢出而永久停止或狂刷
    unsigned long t0 = 0xFFFFF000UL;  // 距回绕仅 4096ms
    TEST_ASSERT_FALSE(mirrorShouldReassert(t0 + 2000, t0, t0));
    TEST_ASSERT_TRUE(mirrorShouldReassert(t0 + 5000, t0, t0));   // 已跨过回绕点
    TEST_ASSERT_FALSE(mirrorShouldReassert(t0 + 31000, t0, t0)); // 超出窗口

    TEST_ASSERT_EQUAL(10, simulateReassertCount(t0, 1000, 40000));
}

// ============================================================
//  TEST GROUP 10: 源码级回归守卫
//
//  背景：U8G2::begin() = initDisplay() + clearDisplay() + setPowerSave(0)，
//  而 u8g2_ClearDisplay() 内部通过 FirstPage/NextPage 分页循环实现，
//  FirstPage 会调 u8g2_ClearBuffer() 清零内存画面缓冲区。
//  因此周期性重申里一旦误用 begin()，随后 sendBuffer() 就会把空白
//  推上屏，表现为“亮一下就黑屏”。这组守卫防止该回归重现。
// ============================================================

static void test_source_maintenance_uses_init_display_not_begin() {
    std::string content = readLcdSrcFile("src/peripherals/LCDManager.cpp");
    if (content.empty()) {
        TEST_IGNORE_MESSAGE("LCDManager.cpp not readable, skipping source check");
        return;
    }

    std::string body = extractFunctionBody(content, "void LCDManager::performMaintenance()");
    TEST_ASSERT_TRUE_MESSAGE(!body.empty(), "performMaintenance() definition not found");
    std::string code = stripLineComments(body);  // 注释里会提到 begin()，需剔除

    // 必须用 initDisplay()（只发命令、不动缓冲区）
    TEST_ASSERT_TRUE_MESSAGE(srcContains(code, "->initDisplay()"),
                             "performMaintenance must re-send init via initDisplay()");
    // 禁止出现 begin()：它会清空画面缓冲区导致黑屏
    TEST_ASSERT_FALSE_MESSAGE(srcContains(code, "->begin()"),
                              "performMaintenance must NOT call begin() (it clears the frame buffer)");
    // 禁止直接清缓冲/清屏
    TEST_ASSERT_FALSE_MESSAGE(srcContains(code, "clearBuffer"),
                              "performMaintenance must NOT clear the frame buffer");
    TEST_ASSERT_FALSE_MESSAGE(srcContains(code, "clearDisplay"),
                              "performMaintenance must NOT call clearDisplay()");
}

static void test_source_maintenance_reasserts_power_and_contrast() {
    std::string content = readLcdSrcFile("src/peripherals/LCDManager.cpp");
    if (content.empty()) {
        TEST_IGNORE_MESSAGE("LCDManager.cpp not readable, skipping source check");
        return;
    }

    std::string body = extractFunctionBody(content, "void LCDManager::performMaintenance()");
    TEST_ASSERT_TRUE_MESSAGE(!body.empty(), "performMaintenance() definition not found");

    // 重发初始化后必须退出省电、重施对比度、并把原有画面推回屏幕
    TEST_ASSERT_TRUE_MESSAGE(srcContains(body, "setPowerSave(0)"),
                             "performMaintenance must exit power save");
    TEST_ASSERT_TRUE_MESSAGE(srcContains(body, "setContrast(_contrast)"),
                             "performMaintenance must re-apply configured contrast");
    TEST_ASSERT_TRUE_MESSAGE(srcContains(body, "sendBuffer()"),
                             "performMaintenance must push the existing frame buffer back");
    // 限流常量必须存在，不能无限制刷屏
    TEST_ASSERT_TRUE_MESSAGE(srcContains(body, "REASSERT_WINDOW_MS"),
                             "performMaintenance must keep the stability window guard");
    TEST_ASSERT_TRUE_MESSAGE(srcContains(body, "REASSERT_INTERVAL_MS"),
                             "performMaintenance must keep the interval throttle");
}

static void test_source_maintenance_has_no_temp_warning_log() {
    std::string content = readLcdSrcFile("src/peripherals/LCDManager.cpp");
    if (content.empty()) {
        TEST_IGNORE_MESSAGE("LCDManager.cpp not readable, skipping source check");
        return;
    }

    std::string body = extractFunctionBody(content, "void LCDManager::performMaintenance()");
    TEST_ASSERT_TRUE_MESSAGE(!body.empty(), "performMaintenance() definition not found");
    std::string code = stripLineComments(body);

    // 排查时临时提升过的 WARNING 级诊断日志不得残留（生产版不裁剪 WARNING，会刷屏）
    TEST_ASSERT_FALSE_MESSAGE(srcContains(code, "LOG_WARNING"),
                              "temporary WARNING diagnostics must not stay in performMaintenance");
}

static void test_source_lcd_serializes_i2c_operations_and_reports_bus_failure() {
    std::string source = readLcdSrcFile("src/peripherals/LCDManager.cpp");
    std::string header = readLcdSrcFile("include/peripherals/LCDManager.h");
    TEST_ASSERT_FALSE_MESSAGE(source.empty(), "LCDManager.cpp not readable");
    TEST_ASSERT_FALSE_MESSAGE(header.empty(), "LCDManager.h not readable");

    TEST_ASSERT_TRUE_MESSAGE(srcContains(header, "_displayMutex"),
                             "LCD manager must own a display-operation mutex");
    TEST_ASSERT_TRUE_MESSAGE(srcContains(source, "xSemaphoreCreateRecursiveMutex()"),
                             "compound LCD operations require a recursive mutex");

    std::string custom = extractFunctionBody(source, "bool LCDManager::showCustomText(");
    std::string maintenance = extractFunctionBody(source, "void LCDManager::performMaintenance()");
    std::string refresh = extractFunctionBody(source, "bool LCDManager::refresh()");
    std::string initialize = extractFunctionBody(source, "bool LCDManager::initialize(");
    std::string probe = extractFunctionBody(source, "bool LCDManager::probeI2cDisplay(");
    TEST_ASSERT_TRUE_MESSAGE(srcContains(custom, "LCD_GUARD_OR_RETURN(false)"),
                             "rule-driven OLED output must serialize access");
    TEST_ASSERT_TRUE_MESSAGE(srcContains(maintenance, "RecursiveMutexGuard lcdGuard(_displayMutex, 0)"),
                             "maintenance must yield instead of blocking behind OLED output");
    TEST_ASSERT_TRUE_MESSAGE(srcContains(initialize, "probeI2cDisplay(true)"),
                             "initialization must probe before U8G2 sends its setup sequence");
    TEST_ASSERT_TRUE_MESSAGE(srcContains(maintenance, "probeI2cDisplay(true)"),
                             "maintenance must probe before reasserting the display");
    TEST_ASSERT_TRUE_MESSAGE(srcContains(refresh, "probeI2cDisplay(true)"),
                             "I2C refresh must probe the configured display address");
    TEST_ASSERT_TRUE_MESSAGE(srcContains(probe, "Wire.endTransmission()"),
                             "the display probe must complete an I2C address transaction");
    TEST_ASSERT_TRUE_MESSAGE(srcContains(refresh, "return false"),
                             "an unavailable I2C display must not be reported as successful");
}

static void test_source_initialize_applies_contrast_and_power_save() {
    std::string content = readLcdSrcFile("src/peripherals/LCDManager.cpp");
    if (content.empty()) {
        TEST_IGNORE_MESSAGE("LCDManager.cpp not readable, skipping source check");
        return;
    }

    // 初始化必须读配置对比度、0 视为未配置、且显式退出省电模式
    TEST_ASSERT_TRUE_MESSAGE(srcContains(content, "params.lcd.contrast"),
                             "initialize must read contrast from config");
    TEST_ASSERT_TRUE_MESSAGE(srcContains(content, "contrast = 255"),
                             "initialize must treat contrast=0 as unconfigured (fallback 255)");
    TEST_ASSERT_TRUE_MESSAGE(srcContains(content, "setPowerSave(0)"),
                             "initialize must explicitly exit power save");
}

static void test_source_lcd_header_declares_maintenance_state() {
    std::string content = readLcdSrcFile("include/peripherals/LCDManager.h");
    if (content.empty()) {
        TEST_IGNORE_MESSAGE("LCDManager.h not readable, skipping source check");
        return;
    }

    TEST_ASSERT_TRUE_MESSAGE(srcContains(content, "performMaintenance"),
                             "LCDManager.h must declare performMaintenance()");
    TEST_ASSERT_TRUE_MESSAGE(srcContains(content, "_contrast"),
                             "LCDManager.h must keep _contrast state");
    TEST_ASSERT_TRUE_MESSAGE(srcContains(content, "_initTime"),
                             "LCDManager.h must keep _initTime state");
    TEST_ASSERT_TRUE_MESSAGE(srcContains(content, "_lastReassert"),
                             "LCDManager.h must keep _lastReassert state");
}

static void test_source_framework_registers_lcd_maintenance_task() {
    std::string content = readLcdSrcFile("src/core/FastBeeFramework.cpp");
    if (content.empty()) {
        TEST_IGNORE_MESSAGE("FastBeeFramework.cpp not readable, skipping source check");
        return;
    }

    // 维护任务必须被注册，否则黑屏自恢复能力彻底失效
    TEST_ASSERT_TRUE_MESSAGE(srcContains(content, "lcd_maintenance"),
                             "framework must register the lcd_maintenance task");
    TEST_ASSERT_TRUE_MESSAGE(srcContains(content, "performMaintenance"),
                             "lcd_maintenance task must invoke LCDManager::performMaintenance");
    // 必须被编译开关保护，否则禁用 LCD 的固件会链接失败
    TEST_ASSERT_TRUE_MESSAGE(srcContains(content, "FASTBEE_ENABLE_LCD"),
                             "lcd_maintenance registration must be guarded by FASTBEE_ENABLE_LCD");
}

static void test_source_config_struct_has_contrast_field() {
    std::string content = readLcdSrcFile("include/core/PeripheralConfig.h");
    if (content.empty()) {
        TEST_IGNORE_MESSAGE("PeripheralConfig.h not readable, skipping source check");
        return;
    }

    size_t lcdStruct = content.find("} lcd;");
    TEST_ASSERT_TRUE_MESSAGE(lcdStruct != std::string::npos, "lcd params struct not found");

    // contrast 必须在 lcd 参数结构体内（往前回看 400 字符足够覆盖该结构）
    size_t from = lcdStruct > 400 ? lcdStruct - 400 : 0;
    std::string block = content.substr(from, lcdStruct - from);
    TEST_ASSERT_TRUE_MESSAGE(srcContains(block, "contrast"),
                             "PeripheralConfig lcd struct must contain contrast");
}

static void test_source_peripheral_manager_persists_contrast() {
    std::string content = readLcdSrcFile("src/core/PeripheralManager.cpp");
    if (content.empty()) {
        TEST_IGNORE_MESSAGE("PeripheralManager.cpp not readable, skipping source check");
        return;
    }

    // 保存时写出、加载时读回（默认 255），两边都不能漏
    TEST_ASSERT_TRUE_MESSAGE(srcContains(content, "params[\"contrast\"]  = config.params.lcd.contrast"),
                             "saveConfiguration must persist lcd contrast");
    TEST_ASSERT_TRUE_MESSAGE(srcContains(content, "params[\"contrast\"] | 255"),
                             "loadConfiguration must read lcd contrast with default 255");
}

static void test_source_route_handler_parses_contrast_on_add_and_update() {
    std::string content = readLcdSrcFile("src/network/handlers/PeripheralRouteHandler.cpp");
    if (content.empty()) {
        TEST_IGNORE_MESSAGE("PeripheralRouteHandler.cpp not readable, skipping source check");
        return;
    }

    // 新增与更新两条路由都必须解析 contrast，否则 Web 端保存后参数丢失
    int occurrences = 0;
    size_t pos = 0;
    const std::string needle = "config.params.lcd.contrast";
    while ((pos = content.find(needle, pos)) != std::string::npos) {
        occurrences++;
        pos += needle.length();
    }
    TEST_ASSERT_TRUE_MESSAGE(occurrences >= 2,
                             "both add and update routes must assign lcd contrast");
    TEST_ASSERT_TRUE_MESSAGE(srcContains(content, "getParamInt(request, \"contrast\""),
                             "route handler must read the contrast request param");
}

// ============================================================
//  TEST GROUP 11: 出厂配置文件守卫
// ============================================================

static void test_config_oled_params_complete_and_supported() {
    std::string content = readLcdSrcFile("data/config/peripherals.json");
    if (content.empty()) {
        TEST_IGNORE_MESSAGE("peripherals.json not readable, skipping config check");
        return;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, content);
    TEST_ASSERT_TRUE_MESSAGE(!err, "peripherals.json must be valid JSON");

    bool foundLcd = false;
    for (JsonObject p : doc["peripherals"].as<JsonArray>()) {
        if ((p["type"] | 0) != 36) continue;  // 36 = PeripheralType::LCD
        foundLcd = true;

        JsonObject params = p["params"].as<JsonObject>();
        int w = params["width"] | 0;
        int h = params["height"] | 0;

        // 分辨率必须是已适配型号，否则会默默回退到 128x64 造成花屏
        TEST_ASSERT_TRUE_MESSAGE(mirrorResolutionMatch((uint8_t)w, (uint8_t)h) != 99,
                                 "oled resolution in peripherals.json must be a supported one");
        // I2C 接口 + 两根信号线
        TEST_ASSERT_EQUAL_MESSAGE(2, params["interface"] | -1, "oled must use I2C interface");
        TEST_ASSERT_EQUAL_MESSAGE(2, p["pinCount"] | 0, "oled must declare 2 pins (SCL/SDA)");

        // contrast 必须存在且在可见范围内（0 会黑屏）
        TEST_ASSERT_TRUE_MESSAGE(params["contrast"].is<int>(),
                                 "oled params must declare contrast");
        int contrast = params["contrast"] | 0;
        TEST_ASSERT_TRUE_MESSAGE(contrast > 0 && contrast <= 255,
                                 "oled contrast must be within 1..255");
    }

    // Unified factory images deliberately ship without pin-bound peripherals.
    // An empty list is the safe baseline for boards whose GPIO map is unknown;
    // users can add a validated LCD/OLED profile after selecting the board.
    if (!foundLcd) {
        TEST_ASSERT_EQUAL_MESSAGE(0, doc["peripherals"].as<JsonArray>().size(),
                                  "without LCD/OLED, factory peripheral config must be empty");
    }
}

static void test_config_no_temp_diagnostic_rules_left() {
    std::string content = readLcdSrcFile("data/config/periph_exec.json");
    if (content.empty()) {
        TEST_IGNORE_MESSAGE("periph_exec.json not readable, skipping config check");
        return;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, content);
    TEST_ASSERT_TRUE_MESSAGE(!err, "periph_exec.json must be valid JSON");

    // 排查 OLED 黑屏时临时注入的诊断规则（exec_099 等 09x 段）不得随固件发布
    TEST_ASSERT_FALSE_MESSAGE(srcContains(content, "exec_099"),
                              "temporary diagnostic rule exec_099 must not be shipped");
    TEST_ASSERT_FALSE_MESSAGE(srcContains(content, "oled test"),
                              "temporary diagnostic content 'oled test' must not be shipped");
}

// ============================================================
//  测试入口
// ============================================================

void test_lcd_manager_group() {
    // Group 1: 分辨率 → 最大行数
    RUN_TEST(test_lcd_max_lines_128x64_default_font);
    RUN_TEST(test_lcd_max_lines_128x32_default_font);
    RUN_TEST(test_lcd_max_lines_72x40_default_font);
    RUN_TEST(test_lcd_max_lines_64x128_default_font);
    RUN_TEST(test_lcd_max_lines_small_font);
    RUN_TEST(test_lcd_max_lines_large_font_72x40);
    
    // Group 2: 分辨率匹配
    RUN_TEST(test_lcd_match_128x64_ssd1306);
    RUN_TEST(test_lcd_match_128x32_ssd1306);
    RUN_TEST(test_lcd_match_72x40_ssd1306_c3);
    RUN_TEST(test_lcd_match_64x128_sh1106);
    RUN_TEST(test_lcd_match_unknown_fallback);
    
    // Group 3: 自定义文本行数截断
    RUN_TEST(test_custom_lines_128x64_max_4_plus_title);
    RUN_TEST(test_custom_lines_128x32_max_2);
    RUN_TEST(test_custom_lines_72x40_max_3);
    RUN_TEST(test_custom_lines_64x128_capped_at_6);
    RUN_TEST(test_custom_lines_72x40_large_font_max_2);
    
    // Group 4: 配置参数 JSON 序列化/反序列化
    RUN_TEST(test_lcd_config_json_serialize_128x64);
    RUN_TEST(test_lcd_config_json_serialize_72x40);
    RUN_TEST(test_lcd_config_json_defaults);
    RUN_TEST(test_lcd_config_json_clamp_overflow);
    RUN_TEST(test_lcd_config_json_spi_interface);
    
    // Group 5: ESP32-C3 引脚校验
    RUN_TEST(test_c3_i2c_pins_valid_5_6);
    RUN_TEST(test_c3_i2c_pins_valid_boundary);
    RUN_TEST(test_c3_i2c_pins_invalid_22);
    RUN_TEST(test_c3_i2c_pins_invalid_negative);
    
    // Group 6: OLED 外设执行动作
    RUN_TEST(test_oled_exec_action_type_exists);
    RUN_TEST(test_oled_exec_multiline_content);
    RUN_TEST(test_oled_exec_content_truncation_72x40);
    
    // Group 7: 传感器分页显示
    RUN_TEST(test_sensor_page_count_128x64);
    RUN_TEST(test_sensor_page_count_72x40);
    RUN_TEST(test_sensor_page_count_128x32);
    RUN_TEST(test_sensor_page_count_single_entry);

    // Group 8: contrast 对比度配置贯通
    RUN_TEST(test_contrast_default_when_absent);
    RUN_TEST(test_contrast_explicit_value_preserved);
    RUN_TEST(test_contrast_zero_treated_as_unconfigured);
    RUN_TEST(test_contrast_clamp_overflow);
    RUN_TEST(test_contrast_clamp_negative);
    RUN_TEST(test_contrast_roundtrip_through_peripheral_config);
    RUN_TEST(test_contrast_default_in_config_struct);

    // Group 9: 开机重申限流逻辑
    RUN_TEST(test_reassert_not_before_interval);
    RUN_TEST(test_reassert_stops_after_window);
    RUN_TEST(test_reassert_count_with_1s_task_tick);
    RUN_TEST(test_reassert_count_not_amplified_by_fast_tick);
    RUN_TEST(test_reassert_survives_millis_overflow);

    // Group 10: 源码级回归守卫（禁止 begin() 清缓冲区回归）
    RUN_TEST(test_source_maintenance_uses_init_display_not_begin);
    RUN_TEST(test_source_maintenance_reasserts_power_and_contrast);
    RUN_TEST(test_source_maintenance_has_no_temp_warning_log);
    RUN_TEST(test_source_lcd_serializes_i2c_operations_and_reports_bus_failure);
    RUN_TEST(test_source_initialize_applies_contrast_and_power_save);
    RUN_TEST(test_source_lcd_header_declares_maintenance_state);
    RUN_TEST(test_source_framework_registers_lcd_maintenance_task);
    RUN_TEST(test_source_config_struct_has_contrast_field);
    RUN_TEST(test_source_peripheral_manager_persists_contrast);
    RUN_TEST(test_source_route_handler_parses_contrast_on_add_and_update);

    // Group 11: 出厂配置文件守卫
    RUN_TEST(test_config_oled_params_complete_and_supported);
    RUN_TEST(test_config_no_temp_diagnostic_rules_left);
}
