#include "core/FeatureFlags.h"
#if FASTBEE_ENABLE_RULE_SCRIPT

#include "core/ScriptEngine.h"
#include "core/PeripheralManager.h"
#include "core/ChipConfig.h"
#if FASTBEE_ENABLE_MQTT
#include "protocols/MQTTClient.h"
#endif
#include "systems/LoggerSystem.h"
#include <esp_random.h>

// ========== 解析 ==========

std::vector<ScriptCommand> ScriptEngine::parse(const String& script) {
    std::vector<ScriptCommand> cmds;

    if (script.isEmpty() || script.length() > MAX_SCRIPT_SIZE) {
        return cmds;
    }

    int start = 0;
    int len = script.length();

    while (start < len) {
        int end = script.indexOf('\n', start);
        if (end < 0) end = len;

        String line = script.substring(start, end);
        line.trim();
        start = end + 1;

        // 跳过空行和注释
        if (line.isEmpty() || line.charAt(0) == '#') {
            continue;
        }

        ScriptCommand cmd;
        if (!parseLine(line, cmd)) {
            LOGGER.warningf("[Script] Parse error: %s", line.c_str());
            return std::vector<ScriptCommand>(); // 解析失败返回空
        }
        cmds.push_back(cmd);
    }

    return cmds;
}

bool ScriptEngine::parseLine(const String& line, ScriptCommand& cmd) {
    auto tokens = tokenize(line);
    if (tokens.empty()) return false;

    String cmdName = tokens[0];
    cmdName.toUpperCase();

    if (cmdName == "GPIO") {
        // GPIO <pin> HIGH|LOW
        if (tokens.size() < 3) return false;
        cmd.type = ScriptCmdType::CMD_GPIO;
        cmd.pin = (uint8_t)tokens[1].toInt();
        String level = tokens[2];
        level.toUpperCase();
        if (level == "HIGH") {
            cmd.intParam = 1;
        } else if (level == "LOW") {
            cmd.intParam = 0;
        } else {
            return false;
        }
        return true;
    }

    if (cmdName == "DELAY") {
        // DELAY <ms>
        if (tokens.size() < 2) return false;
        cmd.type = ScriptCmdType::CMD_DELAY;
        cmd.intParam = tokens[1].toInt();
        if (cmd.intParam < 0) cmd.intParam = 0;
        return true;
    }

    if (cmdName == "PWM") {
        // PWM <pin> <duty>
        if (tokens.size() < 3) return false;
        cmd.type = ScriptCmdType::CMD_PWM;
        cmd.pin = (uint8_t)tokens[1].toInt();
        cmd.intParam = tokens[2].toInt();
        return true;
    }

    if (cmdName == "DAC") {
        // DAC <pin> <value>
        if (tokens.size() < 3) return false;
        cmd.type = ScriptCmdType::CMD_DAC;
        cmd.pin = (uint8_t)tokens[1].toInt();
        cmd.intParam = tokens[2].toInt();
        if (cmd.intParam < 0) cmd.intParam = 0;
        if (cmd.intParam > 255) cmd.intParam = 255;
        return true;
    }

    if (cmdName == "LOG") {
        // LOG <message...>
        if (tokens.size() < 2) return false;
        cmd.type = ScriptCmdType::CMD_LOG;
        // 合并剩余 token 为消息
        String msg;
        for (size_t i = 1; i < tokens.size(); i++) {
            if (i > 1) msg += ' ';
            msg += tokens[i];
        }
        cmd.strParam = msg;
        return true;
    }

    if (cmdName == "PERIPH") {
        // PERIPH <id> <subAction> [param]
        if (tokens.size() < 3) return false;
        cmd.type = ScriptCmdType::CMD_PERIPH;
        cmd.strParam = tokens[1]; // 外设ID
        cmd.subAction = tokens[2];
        cmd.subAction.toUpperCase();

        if (cmd.subAction == "HIGH" || cmd.subAction == "LOW" || cmd.subAction == "STOP") {
            // 无需额外参数
        } else if (cmd.subAction == "PWM" || cmd.subAction == "BLINK" || cmd.subAction == "BREATHE") {
            if (tokens.size() < 4) return false;
            cmd.intParam = tokens[3].toInt();
        } else {
            return false;
        }
        return true;
    }

    if (cmdName == "MQTT") {
        // MQTT <topicIndex> <message...>
        if (tokens.size() < 3) return false;
        cmd.type = ScriptCmdType::CMD_MQTT;
        cmd.intParam = tokens[1].toInt(); // 发布主题索引
        // 合并剩余 token 为消息模板 (支持 RANDOM/RANDOMF 表达式)
        String msg;
        for (size_t i = 2; i < tokens.size(); i++) {
            if (i > 2) msg += ' ';
            msg += tokens[i];
        }
        cmd.strParam = msg;
        return true;
    }

    // 未知命令
    return false;
}

std::vector<String> ScriptEngine::tokenize(const String& line) {
    std::vector<String> tokens;
    int len = line.length();
    int i = 0;

    while (i < len) {
        // 跳过空白
        while (i < len && (line.charAt(i) == ' ' || line.charAt(i) == '\t')) {
            i++;
        }
        if (i >= len) break;

        // 提取 token
        int start = i;
        while (i < len && line.charAt(i) != ' ' && line.charAt(i) != '\t') {
            i++;
        }
        tokens.push_back(line.substring(start, i));
    }

    return tokens;
}

// ========== 验证 ==========

bool ScriptEngine::validate(const std::vector<ScriptCommand>& cmds, String& errorMsg) {
    if (cmds.empty()) {
        errorMsg = "Empty script";
        return false;
    }

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
                // 检查 SPI Flash 引脚 (6-11)
                if (cmd.pin >= 6 && cmd.pin <= 11) {
                    errorMsg = "Pin " + String(cmd.pin) + " is reserved (SPI Flash) at line " + String(i + 1);
                    return false;
                }
                break;

            default:
                break;
        }
    }

    if (totalDelay > MAX_TOTAL_DELAY_MS) {
        errorMsg = "Total delay too long: " + String(totalDelay) +
                   "ms (max " + String(MAX_TOTAL_DELAY_MS) + "ms)";
        return false;
    }

    return true;
}

// ========== 执行 ==========

bool ScriptEngine::execute(const std::vector<ScriptCommand>& cmds, MQTTClient* mqtt) {
    unsigned long startTime = millis();
    PeripheralManager& pm = PeripheralManager::getInstance();

    for (size_t i = 0; i < cmds.size(); i++) {
        // 总执行超时检测
        if (millis() - startTime > MAX_EXECUTION_TIME_MS) {
            LOGGER.warning("[Script] Execution timeout, aborted");
            return false;
        }

        const auto& cmd = cmds[i];

        switch (cmd.type) {
            case ScriptCmdType::CMD_GPIO: {
                pinMode(cmd.pin, OUTPUT);
                digitalWrite(cmd.pin, cmd.intParam ? HIGH : LOW);
                LOGGER.infof("[Script] GPIO %d = %s", cmd.pin, cmd.intParam ? "HIGH" : "LOW");
                break;
            }

            case ScriptCmdType::CMD_DELAY: {
                if (!scriptDelay((uint32_t)cmd.intParam, startTime)) {
                    LOGGER.warning("[Script] Timeout during delay, aborted");
                    return false;
                }
                break;
            }

            case ScriptCmdType::CMD_PWM: {
                // 按引脚分配通道 (8-15), 避免与 PeripheralManager 管理的通道 0-7 冲突
                // 同引脚在不同脚本中映射到相同通道，不同引脚映射到不同通道
                uint8_t scriptChannel = 8 + (cmd.pin % 8);
                ledcSetup(scriptChannel, 5000, 8);
                ledcAttachPin(cmd.pin, scriptChannel);
                ledcWrite(scriptChannel, (uint32_t)cmd.intParam);
                LOGGER.infof("[Script] PWM pin %d = %d (ch%d)", cmd.pin, cmd.intParam, scriptChannel);
                break;
            }

            case ScriptCmdType::CMD_DAC: {
#if CHIP_HAS_DAC
                dacWrite(cmd.pin, (uint8_t)cmd.intParam);
                LOGGER.infof("[Script] DAC pin %d = %d", cmd.pin, cmd.intParam);
#else
                LOGGER.warning("[Script] DAC not supported on this chip");
#endif
                break;
            }

            case ScriptCmdType::CMD_LOG: {
                String logMsg = processRandomExpressions(cmd.strParam);
                LOGGER.infof("[Script] %s", logMsg.c_str());
                break;
            }

            case ScriptCmdType::CMD_PERIPH: {
                if (!pm.hasPeripheral(cmd.strParam)) {
                    LOGGER.warningf("[Script] Peripheral not found: %s", cmd.strParam.c_str());
                    break; // 宽容策略: 跳过继续执行
                }

                if (cmd.subAction == "HIGH") {
                    pm.stopActionTicker(cmd.strParam);
                    pm.writePin(cmd.strParam, GPIOState::STATE_HIGH);
                } else if (cmd.subAction == "LOW") {
                    pm.stopActionTicker(cmd.strParam);
                    pm.writePin(cmd.strParam, GPIOState::STATE_LOW);
                } else if (cmd.subAction == "PWM") {
                    pm.writePWM(cmd.strParam, (uint32_t)cmd.intParam);
                } else if (cmd.subAction == "BLINK") {
                    uint16_t interval = cmd.intParam > 0 ? (uint16_t)cmd.intParam : 500;
                    pm.startActionTicker(cmd.strParam, 1, interval);
                } else if (cmd.subAction == "BREATHE") {
                    uint16_t speed = cmd.intParam > 0 ? (uint16_t)cmd.intParam : 2000;
                    pm.startActionTicker(cmd.strParam, 2, speed);
                } else if (cmd.subAction == "STOP") {
                    pm.stopActionTicker(cmd.strParam);
                }

                LOGGER.infof("[Script] PERIPH %s %s", cmd.strParam.c_str(), cmd.subAction.c_str());
                break;
            }

            case ScriptCmdType::CMD_MQTT: {
#if FASTBEE_ENABLE_MQTT
                if (!mqtt) {
                    LOGGER.warning("[Script] MQTT not available, skipping MQTT command");
                    break;
                }
                String message = processRandomExpressions(cmd.strParam);
                bool ok = mqtt->publishToTopic((size_t)cmd.intParam, message);
                LOGGER.infof("[Script] MQTT publish topic[%d] %s: %s",
                    cmd.intParam, ok ? "OK" : "FAIL", message.c_str());
#else
                LOGGER.warning("[Script] MQTT disabled, skipping MQTT command");
#endif
                break;
            }
        }
    }

    LOGGER.infof("[Script] Completed (%d commands, %lums)", cmds.size(), millis() - startTime);
    return true;
}

bool ScriptEngine::scriptDelay(uint32_t ms, unsigned long scriptStartTime) {
    unsigned long endTime = millis() + ms;

    while (millis() < endTime) {
        // 总执行超时检测
        if (millis() - scriptStartTime > MAX_EXECUTION_TIME_MS) {
            return false;
        }
        delay(10); // 短延时, ESP32 delay() 内含 yield() 喂 WDT
    }

    return true;
}

// ========== 随机数表达式处理 ==========

String ScriptEngine::processRandomExpressions(const String& input) {
    String result = input;

    // 处理 RANDOMF(min,max,decimals) — 必须先于 RANDOM 处理, 避免前缀匹配冲突
    while (true) {
        int pos = result.indexOf("RANDOMF(");
        if (pos < 0) break;

        int closePos = result.indexOf(')', pos + 8);
        if (closePos < 0) break;

        // 提取括号内参数: "min,max,decimals"
        String params = result.substring(pos + 8, closePos);
        int comma1 = params.indexOf(',');
        if (comma1 < 0) break;
        int comma2 = params.indexOf(',', comma1 + 1);
        if (comma2 < 0) break;

        float minVal = params.substring(0, comma1).toFloat();
        float maxVal = params.substring(comma1 + 1, comma2).toFloat();
        int decimals = params.substring(comma2 + 1).toInt();
        if (decimals < 0) decimals = 0;
        if (decimals > 6) decimals = 6;

        // 确保 min <= max
        if (minVal > maxVal) {
            float tmp = minVal; minVal = maxVal; maxVal = tmp;
        }

        // 使用硬件随机数生成浮点值
        uint32_t randVal = esp_random();
        float range = maxVal - minVal;
        float randomFloat = minVal + (float)(randVal % 1000000UL) / 1000000.0f * range;

        // 格式化为指定小数位数
        char buf[32];
        snprintf(buf, sizeof(buf), "%.*f", decimals, randomFloat);

        // 替换表达式
        result = result.substring(0, pos) + String(buf) + result.substring(closePos + 1);
    }

    // 处理 RANDOM(min,max) — 整数随机
    while (true) {
        int pos = result.indexOf("RANDOM(");
        if (pos < 0) break;

        int closePos = result.indexOf(')', pos + 7);
        if (closePos < 0) break;

        // 提取括号内参数: "min,max"
        String params = result.substring(pos + 7, closePos);
        int commaIdx = params.indexOf(',');
        if (commaIdx < 0) break;

        int32_t minVal = params.substring(0, commaIdx).toInt();
        int32_t maxVal = params.substring(commaIdx + 1).toInt();

        // 确保 min <= max
        if (minVal > maxVal) {
            int32_t tmp = minVal; minVal = maxVal; maxVal = tmp;
        }

        // 使用硬件随机数生成整数值
        uint32_t randVal = esp_random();
        int32_t range = maxVal - minVal + 1;
        int32_t randomInt = minVal + (int32_t)(randVal % (uint32_t)range);

        // 替换表达式
        result = result.substring(0, pos) + String(randomInt) + result.substring(closePos + 1);
    }

    return result;
}
#endif // FASTBEE_ENABLE_RULE_SCRIPT
