/**
 * @file test_script_engine.cpp
 * @brief ScriptEngine 脚本引擎单元测试
 * 
 * 测试内容：
 * - 脚本行解析（GPIO/DELAY/PWM/DAC/LOG/PERIPH/MQTT）
 * - 多行脚本解析（空行/注释/混合）
 * - 脚本验证（延时限制/保留引脚/命令数限制）
 * - 脚本大小边界
 * - 未知命令与格式错误处理
 */

#include <unity.h>
#include <Arduino.h>

void test_script_engine_group();

// ========== 内联复现 ScriptEngine 核心逻辑 ==========

enum class ScriptCmdType : uint8_t {
    CMD_GPIO   = 0,
    CMD_DELAY  = 1,
    CMD_PWM    = 2,
    CMD_DAC    = 3,
    CMD_LOG    = 4,
    CMD_PERIPH = 5,
    CMD_MQTT   = 6
};

struct ScriptCommand {
    ScriptCmdType type;
    uint8_t pin;
    int32_t intParam;
    String strParam;
    String subAction;
    String extraParam;
    
    ScriptCommand() : type(ScriptCmdType::CMD_GPIO), pin(0), intParam(0) {}
};

static constexpr uint16_t MAX_SCRIPT_SIZE      = 1024;
static constexpr uint8_t  MAX_COMMANDS          = 50;
static constexpr uint32_t MAX_TOTAL_DELAY_MS    = 30000;
static constexpr uint32_t MAX_SINGLE_DELAY_MS   = 10000;

// 镜像 ScriptEngine::tokenize
static std::vector<String> tokenize(const String& line) {
    std::vector<String> tokens;
    int len = line.length();
    int i = 0;
    while (i < len) {
        while (i < len && (line[i] == ' ' || line[i] == '\t')) i++;
        if (i >= len) break;
        int start = i;
        while (i < len && line[i] != ' ' && line[i] != '\t') i++;
        tokens.push_back(line.substring(start, i));
    }
    return tokens;
}

// 镜像 ScriptEngine::parseLine
static bool parseLine(const String& line, ScriptCommand& cmd) {
    auto tokens = tokenize(line);
    if (tokens.empty()) return false;

    String cmdName = tokens[0];
    cmdName.toUpperCase();

    if (cmdName == "GPIO") {
        if (tokens.size() < 3) return false;
        cmd.type = ScriptCmdType::CMD_GPIO;
        cmd.pin = (uint8_t)tokens[1].toInt();
        String level = tokens[2];
        level.toUpperCase();
        if (level == "HIGH") { cmd.intParam = 1; }
        else if (level == "LOW") { cmd.intParam = 0; }
        else { return false; }
        return true;
    }

    if (cmdName == "DELAY") {
        if (tokens.size() < 2) return false;
        cmd.type = ScriptCmdType::CMD_DELAY;
        cmd.intParam = tokens[1].toInt();
        if (cmd.intParam < 0) cmd.intParam = 0;
        return true;
    }

    if (cmdName == "PWM") {
        if (tokens.size() < 3) return false;
        cmd.type = ScriptCmdType::CMD_PWM;
        cmd.pin = (uint8_t)tokens[1].toInt();
        cmd.intParam = tokens[2].toInt();
        return true;
    }

    if (cmdName == "DAC") {
        if (tokens.size() < 3) return false;
        cmd.type = ScriptCmdType::CMD_DAC;
        cmd.pin = (uint8_t)tokens[1].toInt();
        cmd.intParam = tokens[2].toInt();
        if (cmd.intParam < 0) cmd.intParam = 0;
        if (cmd.intParam > 255) cmd.intParam = 255;
        return true;
    }

    if (cmdName == "LOG") {
        if (tokens.size() < 2) return false;
        cmd.type = ScriptCmdType::CMD_LOG;
        String msg;
        for (size_t i = 1; i < tokens.size(); i++) {
            if (i > 1) msg += ' ';
            msg += tokens[i];
        }
        cmd.strParam = msg;
        return true;
    }

    if (cmdName == "PERIPH") {
        if (tokens.size() < 3) return false;
        cmd.type = ScriptCmdType::CMD_PERIPH;
        cmd.strParam = tokens[1];
        cmd.subAction = tokens[2];
        cmd.subAction.toUpperCase();
        if (cmd.subAction == "HIGH" || cmd.subAction == "LOW" || 
            cmd.subAction == "STOP" || cmd.subAction == "CLEAR" || cmd.subAction == "SHOW") {
            if (cmd.subAction == "SHOW" && tokens.size() >= 4) {
                cmd.extraParam = tokens[3];
            }
        } else if (cmd.subAction == "PWM" || cmd.subAction == "BLINK" || 
                   cmd.subAction == "BREATHE" || cmd.subAction == "BRIGHTNESS") {
            if (tokens.size() < 4) return false;
            cmd.intParam = tokens[3].toInt();
        } else if (cmd.subAction == "DISPLAY" || cmd.subAction == "TEXT") {
            if (tokens.size() < 4) return false;
            String val;
            for (size_t i = 3; i < tokens.size(); i++) {
                if (i > 3) val += ' ';
                val += tokens[i];
            }
            cmd.extraParam = val;
        } else {
            return false;
        }
        return true;
    }

    if (cmdName == "MQTT") {
        if (tokens.size() < 3) return false;
        cmd.type = ScriptCmdType::CMD_MQTT;
        cmd.intParam = tokens[1].toInt();
        String msg;
        for (size_t i = 2; i < tokens.size(); i++) {
            if (i > 2) msg += ' ';
            msg += tokens[i];
        }
        cmd.strParam = msg;
        return true;
    }

    return false;
}

// 镜像 ScriptEngine::parse
static std::vector<ScriptCommand> parse(const String& script) {
    std::vector<ScriptCommand> cmds;
    if (script.isEmpty() || script.length() > MAX_SCRIPT_SIZE) return cmds;

    int start = 0;
    int len = script.length();
    while (start < len) {
        int end = script.indexOf('\n', start);
        if (end < 0) end = len;
        String line = script.substring(start, end);
        line.trim();
        start = end + 1;
        if (line.isEmpty() || line[0] == '#') continue;
        ScriptCommand cmd;
        if (!parseLine(line, cmd)) return std::vector<ScriptCommand>();
        cmds.push_back(cmd);
    }
    return cmds;
}

// 镜像 ScriptEngine::validate
static bool validate(const std::vector<ScriptCommand>& cmds, String& errorMsg) {
    if (cmds.empty()) { errorMsg = "Empty script"; return false; }
    if (cmds.size() > MAX_COMMANDS) {
        errorMsg = "Too many commands (max " + String(MAX_COMMANDS) + ")";
        return false;
    }
    uint32_t totalDelay = 0;
    for (size_t i = 0; i < cmds.size(); i++) {
        const auto& cmd = cmds[i];
        switch (cmd.type) {
            case ScriptCmdType::CMD_DELAY:
                if ((uint32_t)cmd.intParam > MAX_SINGLE_DELAY_MS) {
                    errorMsg = "DELAY too long at line " + String(i + 1) +
                               " (max " + String(MAX_SINGLE_DELAY_MS) + "ms)";
                    return false;
                }
                totalDelay += cmd.intParam;
                break;
            case ScriptCmdType::CMD_GPIO:
            case ScriptCmdType::CMD_PWM:
            case ScriptCmdType::CMD_DAC:
                if (cmd.pin >= 6 && cmd.pin <= 11) {
                    errorMsg = "Pin " + String(cmd.pin) + " is reserved (SPI Flash) at line " + String(i + 1);
                    return false;
                }
                break;
            default: break;
        }
    }
    if (totalDelay > MAX_TOTAL_DELAY_MS) {
        errorMsg = "Total delay too long: " + String(totalDelay) + "ms (max " + String(MAX_TOTAL_DELAY_MS) + "ms)";
        return false;
    }
    return true;
}

// ========== Tokenize 测试 ==========

static void test_tokenize_basic() {
    auto tokens = tokenize("GPIO 5 HIGH");
    TEST_ASSERT_EQUAL(3, (int)tokens.size());
    TEST_ASSERT_EQUAL_STRING("GPIO", tokens[0].c_str());
    TEST_ASSERT_EQUAL_STRING("5", tokens[1].c_str());
    TEST_ASSERT_EQUAL_STRING("HIGH", tokens[2].c_str());
}

static void test_tokenize_tabs() {
    auto tokens = tokenize("GPIO\t5\tHIGH");
    TEST_ASSERT_EQUAL(3, (int)tokens.size());
}

static void test_tokenize_empty() {
    auto tokens = tokenize("");
    TEST_ASSERT_EQUAL(0, (int)tokens.size());
    
    auto tokens2 = tokenize("   ");
    TEST_ASSERT_EQUAL(0, (int)tokens2.size());
}

static void test_tokenize_leading_trailing_spaces() {
    auto tokens = tokenize("  GPIO 5 HIGH  ");
    TEST_ASSERT_EQUAL(3, (int)tokens.size());
}

// ========== ParseLine 单命令测试 ==========

static void test_parse_gpio_high() {
    ScriptCommand cmd;
    TEST_ASSERT_TRUE(parseLine("GPIO 5 HIGH", cmd));
    TEST_ASSERT_EQUAL((int)ScriptCmdType::CMD_GPIO, (int)cmd.type);
    TEST_ASSERT_EQUAL(5, cmd.pin);
    TEST_ASSERT_EQUAL(1, cmd.intParam);
}

static void test_parse_gpio_low() {
    ScriptCommand cmd;
    TEST_ASSERT_TRUE(parseLine("GPIO 12 LOW", cmd));
    TEST_ASSERT_EQUAL(12, cmd.pin);
    TEST_ASSERT_EQUAL(0, cmd.intParam);
}

static void test_parse_gpio_case_insensitive() {
    ScriptCommand cmd;
    TEST_ASSERT_TRUE(parseLine("gpio 5 high", cmd));
    TEST_ASSERT_EQUAL((int)ScriptCmdType::CMD_GPIO, (int)cmd.type);
}

static void test_parse_gpio_invalid_level() {
    ScriptCommand cmd;
    TEST_ASSERT_FALSE(parseLine("GPIO 5 MEDIUM", cmd));
}

static void test_parse_gpio_missing_args() {
    ScriptCommand cmd;
    TEST_ASSERT_FALSE(parseLine("GPIO", cmd));
    TEST_ASSERT_FALSE(parseLine("GPIO 5", cmd));
}

static void test_parse_delay() {
    ScriptCommand cmd;
    TEST_ASSERT_TRUE(parseLine("DELAY 1000", cmd));
    TEST_ASSERT_EQUAL((int)ScriptCmdType::CMD_DELAY, (int)cmd.type);
    TEST_ASSERT_EQUAL(1000, cmd.intParam);
}

static void test_parse_delay_negative_clamped() {
    ScriptCommand cmd;
    TEST_ASSERT_TRUE(parseLine("DELAY -100", cmd));
    TEST_ASSERT_EQUAL(0, cmd.intParam);  // 负数被钳位为 0
}

static void test_parse_pwm() {
    ScriptCommand cmd;
    TEST_ASSERT_TRUE(parseLine("PWM 4 128", cmd));
    TEST_ASSERT_EQUAL((int)ScriptCmdType::CMD_PWM, (int)cmd.type);
    TEST_ASSERT_EQUAL(4, cmd.pin);
    TEST_ASSERT_EQUAL(128, cmd.intParam);
}

static void test_parse_dac() {
    ScriptCommand cmd;
    TEST_ASSERT_TRUE(parseLine("DAC 25 128", cmd));
    TEST_ASSERT_EQUAL((int)ScriptCmdType::CMD_DAC, (int)cmd.type);
    TEST_ASSERT_EQUAL(128, cmd.intParam);
}

static void test_parse_dac_clamp_high() {
    ScriptCommand cmd;
    TEST_ASSERT_TRUE(parseLine("DAC 25 300", cmd));
    TEST_ASSERT_EQUAL(255, cmd.intParam);  // 超 255 被钳位
}

static void test_parse_dac_clamp_negative() {
    ScriptCommand cmd;
    TEST_ASSERT_TRUE(parseLine("DAC 25 -10", cmd));
    TEST_ASSERT_EQUAL(0, cmd.intParam);  // 负数被钳位为 0
}

static void test_parse_log() {
    ScriptCommand cmd;
    TEST_ASSERT_TRUE(parseLine("LOG Hello World", cmd));
    TEST_ASSERT_EQUAL((int)ScriptCmdType::CMD_LOG, (int)cmd.type);
    TEST_ASSERT_EQUAL_STRING("Hello World", cmd.strParam.c_str());
}

static void test_parse_periph_high() {
    ScriptCommand cmd;
    TEST_ASSERT_TRUE(parseLine("PERIPH relay1 HIGH", cmd));
    TEST_ASSERT_EQUAL((int)ScriptCmdType::CMD_PERIPH, (int)cmd.type);
    TEST_ASSERT_EQUAL_STRING("relay1", cmd.strParam.c_str());
    TEST_ASSERT_EQUAL_STRING("HIGH", cmd.subAction.c_str());
}

static void test_parse_periph_pwm_with_param() {
    ScriptCommand cmd;
    TEST_ASSERT_TRUE(parseLine("PERIPH led1 PWM 200", cmd));
    TEST_ASSERT_EQUAL_STRING("PWM", cmd.subAction.c_str());
    TEST_ASSERT_EQUAL(200, cmd.intParam);
}

static void test_parse_periph_display_template() {
    ScriptCommand cmd;
    TEST_ASSERT_TRUE(parseLine("PERIPH tm1637 DISPLAY ${sensor.temp}", cmd));
    TEST_ASSERT_EQUAL_STRING("DISPLAY", cmd.subAction.c_str());
    TEST_ASSERT_EQUAL_STRING("${sensor.temp}", cmd.extraParam.c_str());
}

static void test_parse_periph_missing_subaction() {
    ScriptCommand cmd;
    TEST_ASSERT_FALSE(parseLine("PERIPH relay1", cmd));
}

static void test_parse_periph_unknown_subaction() {
    ScriptCommand cmd;
    TEST_ASSERT_FALSE(parseLine("PERIPH relay1 JUMP", cmd));
}

static void test_parse_mqtt() {
    ScriptCommand cmd;
    TEST_ASSERT_TRUE(parseLine("MQTT 0 {\"temp\":25.5}", cmd));
    TEST_ASSERT_EQUAL((int)ScriptCmdType::CMD_MQTT, (int)cmd.type);
    TEST_ASSERT_EQUAL(0, cmd.intParam);
    TEST_ASSERT_EQUAL_STRING("{\"temp\":25.5}", cmd.strParam.c_str());
}

static void test_parse_mqtt_multi_word_message() {
    ScriptCommand cmd;
    TEST_ASSERT_TRUE(parseLine("MQTT 1 Hello World Message", cmd));
    TEST_ASSERT_EQUAL_STRING("Hello World Message", cmd.strParam.c_str());
}

static void test_parse_unknown_command() {
    ScriptCommand cmd;
    TEST_ASSERT_FALSE(parseLine("UNKNOWN_CMD 5 10", cmd));
}

static void test_parse_empty_line() {
    ScriptCommand cmd;
    TEST_ASSERT_FALSE(parseLine("", cmd));
}

// ========== Parse 多行脚本测试 ==========

static void test_parse_multiline_script() {
    String script = "GPIO 5 HIGH\nDELAY 1000\nGPIO 5 LOW\n";
    auto cmds = parse(script);
    TEST_ASSERT_EQUAL(3, (int)cmds.size());
    TEST_ASSERT_EQUAL((int)ScriptCmdType::CMD_GPIO, (int)cmds[0].type);
    TEST_ASSERT_EQUAL((int)ScriptCmdType::CMD_DELAY, (int)cmds[1].type);
    TEST_ASSERT_EQUAL((int)ScriptCmdType::CMD_GPIO, (int)cmds[2].type);
}

static void test_parse_skip_comments_and_blanks() {
    String script = "# This is a comment\n\nGPIO 5 HIGH\n# Another comment\nDELAY 500\n";
    auto cmds = parse(script);
    TEST_ASSERT_EQUAL(2, (int)cmds.size());
}

static void test_parse_empty_script() {
    auto cmds = parse("");
    TEST_ASSERT_EQUAL(0, (int)cmds.size());
}

static void test_parse_oversized_script() {
    // 超过 MAX_SCRIPT_SIZE 应返回空
    String bigScript;
    bigScript.reserve(MAX_SCRIPT_SIZE + 10);
    for (int i = 0; i < MAX_SCRIPT_SIZE + 10; i++) {
        bigScript += "X";
    }
    auto cmds = parse(bigScript);
    TEST_ASSERT_EQUAL(0, (int)cmds.size());
}

static void test_parse_error_returns_empty() {
    // 任意一行解析失败，整体返回空
    String script = "GPIO 5 HIGH\nINVALID_CMD\nDELAY 500\n";
    auto cmds = parse(script);
    TEST_ASSERT_EQUAL(0, (int)cmds.size());
}

// ========== Validate 验证测试 ==========

static void test_validate_empty_cmds() {
    std::vector<ScriptCommand> cmds;
    String errMsg;
    TEST_ASSERT_FALSE(validate(cmds, errMsg));
    TEST_ASSERT_TRUE(errMsg.indexOf("Empty") >= 0);
}

static void test_validate_too_many_commands() {
    std::vector<ScriptCommand> cmds;
    for (int i = 0; i <= MAX_COMMANDS; i++) {
        ScriptCommand cmd;
        cmd.type = ScriptCmdType::CMD_LOG;
        cmd.strParam = "x";
        cmds.push_back(cmd);
    }
    String errMsg;
    TEST_ASSERT_FALSE(validate(cmds, errMsg));
    TEST_ASSERT_TRUE(errMsg.indexOf("Too many") >= 0);
}

static void test_validate_single_delay_too_long() {
    ScriptCommand cmd;
    cmd.type = ScriptCmdType::CMD_DELAY;
    cmd.intParam = MAX_SINGLE_DELAY_MS + 1;
    std::vector<ScriptCommand> cmds = {cmd};
    String errMsg;
    TEST_ASSERT_FALSE(validate(cmds, errMsg));
    TEST_ASSERT_TRUE(errMsg.indexOf("too long") >= 0);
}

static void test_validate_total_delay_too_long() {
    // 4 条 DELAY，每条 8000ms，总计 32000ms > 30000ms
    std::vector<ScriptCommand> cmds;
    for (int i = 0; i < 4; i++) {
        ScriptCommand cmd;
        cmd.type = ScriptCmdType::CMD_DELAY;
        cmd.intParam = 8000;
        cmds.push_back(cmd);
    }
    String errMsg;
    TEST_ASSERT_FALSE(validate(cmds, errMsg));
    TEST_ASSERT_TRUE(errMsg.indexOf("Total delay") >= 0);
}

static void test_validate_reserved_spi_pins() {
    // GPIO 6-11 为 SPI Flash 保留引脚
    for (uint8_t pin = 6; pin <= 11; pin++) {
        ScriptCommand cmd;
        cmd.type = ScriptCmdType::CMD_GPIO;
        cmd.pin = pin;
        std::vector<ScriptCommand> cmds = {cmd};
        String errMsg;
        TEST_ASSERT_FALSE(validate(cmds, errMsg));
        TEST_ASSERT_TRUE(errMsg.indexOf("reserved") >= 0);
    }
}

static void test_validate_non_reserved_pins() {
    // GPIO 0-5 和 12+ 应通过
    for (uint8_t pin : {0, 1, 2, 3, 4, 5, 12, 13, 25, 26, 27, 32, 33}) {
        ScriptCommand cmd;
        cmd.type = ScriptCmdType::CMD_GPIO;
        cmd.pin = pin;
        cmd.intParam = 1;
        std::vector<ScriptCommand> cmds = {cmd};
        String errMsg;
        TEST_ASSERT_TRUE(validate(cmds, errMsg));
    }
}

static void test_validate_valid_script() {
    auto cmds = parse("GPIO 5 HIGH\nDELAY 1000\nLOG Hello\nGPIO 5 LOW\n");
    TEST_ASSERT_FALSE(cmds.empty());
    String errMsg;
    TEST_ASSERT_TRUE(validate(cmds, errMsg));
}

static void test_validate_reserved_pwm_pin() {
    ScriptCommand cmd;
    cmd.type = ScriptCmdType::CMD_PWM;
    cmd.pin = 8;  // SPI Flash 保留
    cmd.intParam = 128;
    std::vector<ScriptCommand> cmds = {cmd};
    String errMsg;
    TEST_ASSERT_FALSE(validate(cmds, errMsg));
}

static void test_validate_reserved_dac_pin() {
    ScriptCommand cmd;
    cmd.type = ScriptCmdType::CMD_DAC;
    cmd.pin = 10;  // SPI Flash 保留
    cmd.intParam = 100;
    std::vector<ScriptCommand> cmds = {cmd};
    String errMsg;
    TEST_ASSERT_FALSE(validate(cmds, errMsg));
}

static void test_validate_log_commands_skip_pin_check() {
    // LOG 命令没有 pin 字段，不受保留引脚限制
    ScriptCommand cmd;
    cmd.type = ScriptCmdType::CMD_LOG;
    cmd.strParam = "test message";
    std::vector<ScriptCommand> cmds = {cmd};
    String errMsg;
    TEST_ASSERT_TRUE(validate(cmds, errMsg));
}

// ========== 测试组入口 ==========

void test_script_engine_group() {
    // Tokenize
    RUN_TEST(test_tokenize_basic);
    RUN_TEST(test_tokenize_tabs);
    RUN_TEST(test_tokenize_empty);
    RUN_TEST(test_tokenize_leading_trailing_spaces);
    
    // ParseLine: GPIO
    RUN_TEST(test_parse_gpio_high);
    RUN_TEST(test_parse_gpio_low);
    RUN_TEST(test_parse_gpio_case_insensitive);
    RUN_TEST(test_parse_gpio_invalid_level);
    RUN_TEST(test_parse_gpio_missing_args);
    
    // ParseLine: DELAY
    RUN_TEST(test_parse_delay);
    RUN_TEST(test_parse_delay_negative_clamped);
    
    // ParseLine: PWM / DAC
    RUN_TEST(test_parse_pwm);
    RUN_TEST(test_parse_dac);
    RUN_TEST(test_parse_dac_clamp_high);
    RUN_TEST(test_parse_dac_clamp_negative);
    
    // ParseLine: LOG / PERIPH / MQTT
    RUN_TEST(test_parse_log);
    RUN_TEST(test_parse_periph_high);
    RUN_TEST(test_parse_periph_pwm_with_param);
    RUN_TEST(test_parse_periph_display_template);
    RUN_TEST(test_parse_periph_missing_subaction);
    RUN_TEST(test_parse_periph_unknown_subaction);
    RUN_TEST(test_parse_mqtt);
    RUN_TEST(test_parse_mqtt_multi_word_message);
    RUN_TEST(test_parse_unknown_command);
    RUN_TEST(test_parse_empty_line);
    
    // Parse 多行
    RUN_TEST(test_parse_multiline_script);
    RUN_TEST(test_parse_skip_comments_and_blanks);
    RUN_TEST(test_parse_empty_script);
    RUN_TEST(test_parse_oversized_script);
    RUN_TEST(test_parse_error_returns_empty);
    
    // Validate
    RUN_TEST(test_validate_empty_cmds);
    RUN_TEST(test_validate_too_many_commands);
    RUN_TEST(test_validate_single_delay_too_long);
    RUN_TEST(test_validate_total_delay_too_long);
    RUN_TEST(test_validate_reserved_spi_pins);
    RUN_TEST(test_validate_non_reserved_pins);
    RUN_TEST(test_validate_valid_script);
    RUN_TEST(test_validate_reserved_pwm_pin);
    RUN_TEST(test_validate_reserved_dac_pin);
    RUN_TEST(test_validate_log_commands_skip_pin_check);
}
