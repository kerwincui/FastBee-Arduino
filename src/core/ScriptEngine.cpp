#include "core/FeatureFlags.h"
#if FASTBEE_ENABLE_COMMAND_SCRIPT

#include "core/ScriptEngine.h"
#include "core/NativeOutputDispatch.h"
#include "core/ScriptRandomExpansion.h"
#include "core/FeatureObservationCounter.h"
#include "core/PeripheralManager.h"
#include "core/AsyncExecTypes.h"
#include "core/ChipConfig.h"
#if FASTBEE_ENABLE_PERIPH_EXEC
#include "core/PeriphExecExecutor.h"
#endif
#if FASTBEE_ENABLE_SEVEN_SEGMENT
#include "peripherals/SevenSegmentDriver.h"
#endif
#if FASTBEE_ENABLE_LCD
#include "peripherals/LCDManager.h"
#endif
#include "systems/LoggerSystem.h"
#include <esp_random.h>
#include <utility>

namespace {
bool isLcdPeripheral(const String& id) {
    PeripheralConfig snapshot;
    return PeripheralManager::getInstance().getPeripheralSnapshot(id, snapshot) &&
           snapshot.type == PeripheralType::LCD;
}
}

// ========== 解析 ==========

std::vector<ScriptCommand> ScriptEngine::parse(const String& script) {
    std::vector<ScriptCommand> cmds;

    if (script.isEmpty() || script.length() > MAX_SCRIPT_SIZE ||
        std::memchr(script.c_str(), '\0', script.length())) {
        return cmds;
    }

    int start = 0;
    int len = script.length();

    while (start < len) {
        int end = script.indexOf('\n', start);
        if (end < 0) end = len;

        String line = script.substring(start, end);
        if (line.length() != static_cast<size_t>(end - start)) return {};
        line.trim();
        start = end + 1;

        // 跳过空行和注释
        if (line.isEmpty() || line.charAt(0) == '#') {
            continue;
        }

        if (cmds.size() >= MAX_COMMANDS) return {};
        ScriptCommand cmd{};
        if (!parseLine(line, cmd)) {
            LOGGER.warning("[Script] Parse error: invalid command or incomplete allocation");
            return std::vector<ScriptCommand>(); // 解析失败返回空
        }
        cmds.push_back(std::move(cmd));
    }

    return cmds;
}

bool ScriptEngine::parseLine(const String& line, ScriptCommand& cmd) {
    auto tokens = tokenize(line);
    if (tokens.empty()) return false;

    String cmdName = tokens[0];
    if (cmdName.length() != tokens[0].length()) return false;
    cmdName.toUpperCase();
    auto integer = [&](size_t index, int32_t low, int32_t high, int32_t& value) {
        if (index >= tokens.size()) return false;
        const String& token = tokens[index];
        return FastBee::ScriptRandomDetail::integer(token.c_str(), token.c_str() + token.length(), value) &&
               value >= low && value <= high;
    };
    auto pin = [&](size_t index) {
        int32_t value = 0;
        if (!integer(index, 0, UINT8_MAX, value)) return false;
        cmd.pin = static_cast<uint8_t>(value); return true;
    };
    auto join = [&](size_t first, String& output) {
        output.clear();
        size_t expected = 0;
        for (size_t i = first; i < tokens.size(); ++i) {
            if (i > first) { ++expected; if (!output.concat(' ')) return false; }
            expected += tokens[i].length();
            if (!output.concat(tokens[i]) || output.length() != expected) return false;
        }
        return true;
    };

    if (cmdName == "GPIO") {
        // GPIO <pin> HIGH|LOW
        if (tokens.size() != 3 || !pin(1)) return false;
        cmd.type = ScriptCmdType::CMD_GPIO;
        String level = tokens[2];
        if (level.length() != tokens[2].length()) return false;
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
        if (tokens.size() != 2 || !integer(1, INT32_MIN, INT32_MAX, cmd.intParam)) return false;
        cmd.type = ScriptCmdType::CMD_DELAY;
        if (cmd.intParam < 0) cmd.intParam = 0;
        return true;
    }

    if (cmdName == "PWM") {
        // PWM <pin> <duty>
        if (tokens.size() != 3 || !pin(1) || !integer(2, 0, 255, cmd.intParam)) return false;
        cmd.type = ScriptCmdType::CMD_PWM;
        return true;
    }

    if (cmdName == "DAC") {
        // DAC <pin> <value>
        if (tokens.size() != 3 || !pin(1) || !integer(2, INT32_MIN, INT32_MAX, cmd.intParam)) return false;
        cmd.type = ScriptCmdType::CMD_DAC;
        if (cmd.intParam < 0) cmd.intParam = 0;
        if (cmd.intParam > 255) cmd.intParam = 255;
        return true;
    }

    if (cmdName == "LOG") {
        // LOG <message...>
        if (tokens.size() < 2) return false;
        cmd.type = ScriptCmdType::CMD_LOG;
        return join(1, cmd.strParam);
    }

    if (cmdName == "PERIPH") {
        // PERIPH <id> <subAction> [param]
        if (tokens.size() < 3) return false;
        cmd.type = ScriptCmdType::CMD_PERIPH;
        cmd.strParam = tokens[1]; // 外设ID
        cmd.subAction = tokens[2];
        if (cmd.strParam.length() != tokens[1].length() ||
            cmd.subAction.length() != tokens[2].length()) return false;
        cmd.subAction.toUpperCase();

        if (cmd.subAction == "HIGH" || cmd.subAction == "LOW" || cmd.subAction == "STOP" ||
            cmd.subAction == "TOGGLE" || cmd.subAction == "CLEAR") {
            if (tokens.size() != 3) return false;
        } else if (cmd.subAction == "SHOW") {
            // LCD/OLED 显示传感器页面：可选页码（默认轮播）
            if (tokens.size() > 4) return false;
            if (tokens.size() == 4) {
                int32_t page = 0;
                if (!integer(3, -1, INT8_MAX, page)) return false;
                cmd.extraParam = tokens[3];
                if (cmd.extraParam.length() != tokens[3].length()) return false;
            } else {
                cmd.extraParam = "";
            }
        } else if (cmd.subAction == "PWM" || cmd.subAction == "DAC" || cmd.subAction == "BLINK" || cmd.subAction == "BREATHE" ||
                   cmd.subAction == "BRIGHTNESS") {
            if (tokens.size() != 4 || !integer(3, INT32_MIN, INT32_MAX, cmd.intParam)) return false;
            if (cmd.subAction == "PWM" && cmd.intParam < 0) return false;
            if (cmd.subAction == "DAC" && (cmd.intParam < 0 || cmd.intParam > 255)) return false;
            if ((cmd.subAction == "BLINK" || cmd.subAction == "BREATHE") &&
                (cmd.intParam < 0 || cmd.intParam > UINT16_MAX)) return false;
        } else if (cmd.subAction == "DISPLAY" || cmd.subAction == "TEXT") {
            // 显示内容：合并剩余 token，保留空格，支持 ${id.field} 模板
            if (tokens.size() < 4) return false;
            if (!join(3, cmd.extraParam)) return false;
        } else {
            return false;
        }
        return true;
    }

    if (cmdName == "MQTT") {
        // MQTT <topicIndex> <message...>
        if (tokens.size() < 3 || !integer(1, 0, INT32_MAX, cmd.intParam)) return false;
        cmd.type = ScriptCmdType::CMD_MQTT;
        return join(2, cmd.strParam);
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
        String token = line.substring(start, i);
        if (token.length() != static_cast<size_t>(i - start)) return {};
        tokens.push_back(std::move(token));
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

bool ScriptEngine::execute(const std::vector<ScriptCommand>& cmds, ScriptMqttPublisher mqtt,
                           const FastBee::UartEventOrigin& uartOrigin, bool* sourceUnconfirmed,
                           const FastBee::OrdinaryControlContext* control,
                           FastBee::OrdinaryControlResult* controlResult) {
    FastBee::CommandScriptExecutionObservation executionObservation;
    (void)mqtt; // MQTT can be compiled out while command scripts remain enabled.
    if (sourceUnconfirmed) *sourceUnconfirmed = false;
    unsigned long startTime = millis();
    PeripheralManager& pm = PeripheralManager::getInstance();
    auto sourceCurrent = [&]() {
        if (pm.isUartEventOriginCurrent(uartOrigin)) return true;
        if (sourceUnconfirmed) *sourceUnconfirmed = true;
        return false;
    };
    if (!sourceCurrent()) return false;

    for (size_t i = 0; i < cmds.size(); i++) {
        if (!sourceCurrent()) return false;
        // 总执行超时检测
        if (millis() - startTime > MAX_EXECUTION_TIME_MS) {
            LOGGER.warning("[Script] Execution timeout, aborted");
            return false;
        }

        const auto& cmd = cmds[i];

        switch (cmd.type) {
            case ScriptCmdType::CMD_GPIO: {
                RecursiveMutexGuard controlGuard(pm.getMutex(), 0);
                if (!controlGuard.isLocked() || pm.controlOutputPinIsClaimed(cmd.pin)) return false;
                if (!sourceCurrent()) return false;
                if (!pm.prepareScriptPin(cmd.pin, 0) || !sourceCurrent()) return false;
                if (!pm.writeScriptGpio(cmd.pin, cmd.intParam != 0)) return false;
                LOGGER.infof("[Script] GPIO %d = %s", cmd.pin, cmd.intParam ? "HIGH" : "LOW");
                break;
            }

            case ScriptCmdType::CMD_DELAY: {
                if (!scriptDelay((uint32_t)cmd.intParam, startTime, uartOrigin, sourceUnconfirmed)) {
                    LOGGER.warning("[Script] Delay interrupted; timeout or source unconfirmed");
                    return false;
                }
                break;
            }

            case ScriptCmdType::CMD_PWM: {
                RecursiveMutexGuard controlGuard(pm.getMutex(), 0);
                if (!controlGuard.isLocked() || pm.controlOutputPinIsClaimed(cmd.pin)) return false;
                if (!sourceCurrent()) return false;
                if (!pm.prepareScriptPin(cmd.pin, 1) || !sourceCurrent()) return false;
                if (!pm.writeScriptPwm(cmd.pin, (uint32_t)cmd.intParam)) return false;
                LOGGER.infof("[Script] PWM pin %d = %d", cmd.pin, cmd.intParam);
                break;
            }

            case ScriptCmdType::CMD_DAC: {
                RecursiveMutexGuard controlGuard(pm.getMutex(), 0);
                if (!controlGuard.isLocked() || pm.controlOutputPinIsClaimed(cmd.pin)) return false;
                if (!sourceCurrent()) return false;
#if CHIP_HAS_DAC
                if (!pm.prepareScriptPin(cmd.pin, 2) || !sourceCurrent()) return false;
                if (!pm.writeScriptDac(cmd.pin, (uint8_t)cmd.intParam)) return false;
                LOGGER.infof("[Script] DAC pin %d = %d", cmd.pin, cmd.intParam);
#else
                LOGGER.warning("[Script] DAC not supported on this chip");
                return false;
#endif
                break;
            }

            case ScriptCmdType::CMD_LOG: {
                String logMsg;
                if (!processRandomExpressions(cmd.strParam, logMsg)) return false;
                if (!sourceCurrent()) return false;
                LOGGER.infof("[Script] %s", logMsg.c_str());
                break;
            }

            case ScriptCmdType::CMD_PERIPH: {
                if (!pm.hasPeripheral(cmd.strParam)) {
                    LOGGER.warningf("[Script] Peripheral not found: %s", cmd.strParam.c_str());
                    break; // 宽容策略: 跳过继续执行
                }
                if (!sourceCurrent()) return false;

                PeripheralConfig nativeTarget;
                if (pm.getPeripheralSnapshot(cmd.strParam, nativeTarget) &&
                    FastBee::isOrdinaryDigitalTarget(nativeTarget)) {
                    using Command = FastBee::DigitalOutputCommand;
                    Command command;
                    uint32_t intervalMs = 0;
                    if (cmd.subAction == "HIGH") command = Command::High;
                    else if (cmd.subAction == "LOW") command = Command::Low;
                    else if (cmd.subAction == "STOP") command = Command::Stop;
                    else if (cmd.subAction == "TOGGLE") command = Command::Toggle;
                    else if (cmd.subAction == "BLINK") {
                        command = Command::Blink;
                        if (cmd.intParam < 0 || cmd.intParam > 65535)
                            return FastBee::rejectOrdinaryDigitalAction(cmd.strParam,
                                "CONTROL_PARAMETER_INVALID", controlResult);
                        intervalMs = cmd.intParam ? static_cast<uint32_t>(cmd.intParam) : 500;
                    } else {
                        return FastBee::rejectOrdinaryDigitalAction(cmd.strParam,
                            "CONTROL_COMMAND_UNSUPPORTED", controlResult);
                    }
                    if (!FastBee::dispatchOrdinaryDigitalAction(pm, cmd.strParam, command,
                        intervalMs, control, controlResult)) return false;
                    break;
                }

                if (pm.getPeripheralSnapshot(cmd.strParam, nativeTarget) &&
                    FastBee::isOrdinaryScalarTarget(nativeTarget)) {
                    using Scalar = FastBee::ScalarOutputCommand;
                    Scalar command = nativeTarget.type == PeripheralType::DAC ? Scalar::Dac : Scalar::Pwm;
                    uint32_t value = 0, maximum = 0;
                    if (cmd.subAction == "STOP") command = Scalar::Stop;
                    else if (!FastBee::ordinaryScalarMaximum(nativeTarget, maximum))
                        return FastBee::rejectOrdinaryDigitalAction(cmd.strParam,
                            "CONTROL_PARAMETER_INVALID", controlResult);
                    else if (cmd.subAction == "HIGH") {
                        if (!FastBee::ordinaryScalarHighValue(nativeTarget, value))
                            return FastBee::rejectOrdinaryDigitalAction(cmd.strParam,
                                "CONTROL_PARAMETER_INVALID", controlResult);
                    }
                    else if (cmd.subAction == "LOW") value = 0;
                    else if (cmd.subAction == "TOGGLE") command = Scalar::Toggle;
                    else if ((cmd.subAction == "BREATHE" || cmd.subAction == "BLINK") &&
                             nativeTarget.type != PeripheralType::DAC) {
                        command = cmd.subAction == "BLINK" ? Scalar::Blink : Scalar::Breathe;
                        if (cmd.intParam < 0 || cmd.intParam > 65535)
                            return FastBee::rejectOrdinaryDigitalAction(cmd.strParam,
                                "CONTROL_PARAMETER_INVALID", controlResult);
                        value = cmd.intParam ? static_cast<uint32_t>(cmd.intParam) :
                            command == Scalar::Blink ? 500 : 2000;
                    } else if ((cmd.subAction == "PWM" && command == Scalar::Pwm) ||
                               (cmd.subAction == "DAC" && command == Scalar::Dac)) {
                        if (cmd.intParam < 0 || static_cast<uint32_t>(cmd.intParam) > maximum)
                            return FastBee::rejectOrdinaryDigitalAction(cmd.strParam,
                                "CONTROL_PARAMETER_INVALID", controlResult);
                        value = static_cast<uint32_t>(cmd.intParam);
                    } else return FastBee::rejectOrdinaryDigitalAction(cmd.strParam,
                        "CONTROL_COMMAND_UNSUPPORTED", controlResult);
                    if (!FastBee::dispatchOrdinaryScalarAction(pm, cmd.strParam, command,
                        value, control, controlResult)) return false;
                    break;
                }
                if (pm.getPeripheralSnapshot(cmd.strParam, nativeTarget) &&
                    nativeTarget.type == PeripheralType::PWM_SERVO) {
                    if (cmd.subAction != "PWM")
                        return FastBee::rejectOrdinaryDigitalAction(cmd.strParam,
                            "CONTROL_COMMAND_UNSUPPORTED", controlResult);
                    if (!pm.submitOrdinaryServo(cmd.strParam, cmd.intParam, true, control, controlResult))
                        return false;
                    break;
                }

                if (pm.getPeripheralSnapshot(cmd.strParam, nativeTarget) &&
                    nativeTarget.isModbusPeripheral()) {
                    FastBee::OrdinaryControlResult result;
                    result.evaluated=true;
                    bool accepted=false;
                    if(!control)result.reason="CONTROL_CONTEXT_REQUIRED";
                    else if(!sourceCurrent())result.reason="CONTROL_SOURCE_STALE";
                    else if(cmd.subAction=="HIGH" || cmd.subAction=="LOW")
                        accepted=pm.writeModbusPin(cmd.strParam,
                            cmd.subAction=="HIGH"?GPIOState::STATE_HIGH:GPIOState::STATE_LOW,*control,result);
                    else if(cmd.subAction=="PWM") {
                        if(cmd.intParam<0 || static_cast<uint32_t>(cmd.intParam)>65535U)
                            result.reason="CONTROL_PARAMETER_INVALID";
                        else accepted=pm.writeModbusPWM(cmd.strParam,static_cast<uint32_t>(cmd.intParam),*control,result);
                    } else result.reason="CONTROL_COMMAND_UNSUPPORTED";
                    FastBee::rememberOrdinaryControlResult(result,controlResult);
                    if(!accepted) {sourceCurrent();return false;}
                    break;
                }

#if FASTBEE_ENABLE_SEVEN_SEGMENT
                if (pm.getPeripheralSnapshot(cmd.strParam, nativeTarget) &&
                    nativeTarget.type == PeripheralType::SEVEN_SEGMENT_TM1637) {
                    using Display = FastBee::DisplayOutputCommand;
                    Display command;
                    String text;
                    int32_t value = 0;
                    if (cmd.subAction == "DISPLAY" || cmd.subAction == "TEXT") {
                        command = cmd.subAction == "DISPLAY" ? Display::Number : Display::Text;
                        text = cmd.extraParam;
#if FASTBEE_ENABLE_PERIPH_EXEC
                        text = PeriphExecExecutor::resolveSensorTemplate(text);
#endif
                    } else if (cmd.subAction == "CLEAR") command = Display::Clear;
                    else if (cmd.subAction == "BRIGHTNESS") {
                        command = Display::Brightness; value = cmd.intParam;
                    } else return FastBee::rejectOrdinaryDigitalAction(cmd.strParam,
                        "CONTROL_COMMAND_UNSUPPORTED", controlResult);
                    if (!sourceCurrent() || !pm.submitOrdinaryDisplay(cmd.strParam, command,
                        text, value, control, controlResult)) return false;
                    break;
                }
#endif

                if (cmd.subAction == "HIGH") {
                    pm.stopActionTicker(cmd.strParam);
                    if (!sourceCurrent()) return false;
                    if (!pm.writePin(cmd.strParam, GPIOState::STATE_HIGH)) {
                        sourceCurrent(); return false;
                    }
                } else if (cmd.subAction == "LOW") {
                    pm.stopActionTicker(cmd.strParam);
                    if (!sourceCurrent()) return false;
                    if (!pm.writePin(cmd.strParam, GPIOState::STATE_LOW)) {
                        sourceCurrent(); return false;
                    }
                } else if (cmd.subAction == "PWM") {
                    if (!pm.writePWM(cmd.strParam, (uint32_t)cmd.intParam)) {
                        sourceCurrent(); return false;
                    }
                } else if (cmd.subAction == "BLINK") {
                    uint16_t interval = cmd.intParam > 0 ? (uint16_t)cmd.intParam : 500;
                    if (!pm.startActionTicker(cmd.strParam, 1, interval, uartOrigin)) {
                        sourceCurrent(); return false;
                    }
                } else if (cmd.subAction == "BREATHE") {
                    uint16_t speed = cmd.intParam > 0 ? (uint16_t)cmd.intParam : 2000;
                    if (!pm.startActionTicker(cmd.strParam, 2, speed, uartOrigin)) {
                        sourceCurrent(); return false;
                    }
                } else if (cmd.subAction == "STOP") {
                    pm.stopActionTicker(cmd.strParam);
                }
#if FASTBEE_ENABLE_LCD
                // LCD/OLED 优先分流：当目标外设类型为 LCD 时，TEXT/DISPLAY 子动作走自定义多行显示
                else if ((cmd.subAction == "TEXT" || cmd.subAction == "DISPLAY") &&
                         isLcdPeripheral(cmd.strParam)) {
                    String val = cmd.extraParam;
#if FASTBEE_ENABLE_PERIPH_EXEC
                    val = PeriphExecExecutor::resolveSensorTemplate(val);
#endif
                    if (!sourceCurrent()) return false;
                    FastBee::LcdOutputRequest output;
                    output.command = FastBee::LcdOutputCommand::CustomText;
                    output.text = val;
                    const bool ok = pm.submitOrdinaryLcd(cmd.strParam, output, control, controlResult);
                    LOGGER.infof("[Script] PERIPH %s LCD TEXT %d bytes %s",
                                 cmd.strParam.c_str(), (int)val.length(),
                                 ok ? "OK" : "FAIL");
                    if (!ok) return false;
                    break;
                }
#endif


#if FASTBEE_ENABLE_LCD
                // LCD/OLED 显示屏渲染子动作
                if (cmd.subAction == "SHOW" && isLcdPeripheral(cmd.strParam)) {
                    // 显示已注册的传感器数据页面，自动轮播，extraParam 可指定页码
                    long page = -1;
                    if (!cmd.extraParam.isEmpty() &&
                        (!FastBee::parseStrictActionLong(cmd.extraParam.c_str(), cmd.extraParam.length(), page) ||
                         page < -1 || page > 127)) return false;
                    FastBee::LcdOutputRequest output;
                    output.command = FastBee::LcdOutputCommand::SensorPage;
                    output.value = static_cast<int16_t>(page);
                    if (!pm.submitOrdinaryLcd(cmd.strParam, output, control, controlResult)) return false;
                    break;
                } else if (cmd.subAction == "CLEAR" && isLcdPeripheral(cmd.strParam)) {
                    FastBee::LcdOutputRequest output;
                    output.command = FastBee::LcdOutputCommand::Clear;
                    if (!pm.submitOrdinaryLcd(cmd.strParam, output, control, controlResult)) return false;
                    break;
                }
#endif

                LOGGER.infof("[Script] PERIPH %s %s", cmd.strParam.c_str(), cmd.subAction.c_str());
                break;
            }

            case ScriptCmdType::CMD_MQTT: {
#if FASTBEE_ENABLE_MQTT
                if (!mqtt.publish || cmd.intParam < 0) {
                    LOGGER.warning("[Script] MQTT publisher unavailable or topic invalid; aborted");
                    return false;
                }
                String message;
                if (!processRandomExpressions(cmd.strParam, message)) {
                    LOGGER.warning("[Script] MQTT message expansion invalid or incomplete; aborted");
                    return false;
                }
                if (!sourceCurrent()) return false;
                const auto result = mqtt.publish(mqtt.context, static_cast<size_t>(cmd.intParam), message);
                LOGGER.infof("[Script] MQTT topic[%d] %s; delivery unverified",
                    cmd.intParam, FastBee::scriptPublishResultName(result));
                if (result != FastBee::ScriptPublishResult::Accepted && result != FastBee::ScriptPublishResult::Queued) return false;
#else
                LOGGER.warning("[Script] MQTT disabled; aborted");
                return false;
#endif
                break;
            }
        }
    }

    if (!sourceCurrent()) return false;
    LOGGER.infof("[Script] Completed (%d commands, %lums)", cmds.size(), millis() - startTime);
    return true;
}

bool ScriptEngine::scriptDelay(uint32_t ms, unsigned long scriptStartTime,
                               const FastBee::UartEventOrigin& uartOrigin, bool* sourceUnconfirmed) {
    unsigned long delayStart = millis();

    // 减法写法兼容 millis 回绕（millis()+ms 溢出会导致 delay 被直接跳过）
    while (millis() - delayStart < ms) {
        if (!PeripheralManager::getInstance().isUartEventOriginCurrent(uartOrigin)) {
            if (sourceUnconfirmed) *sourceUnconfirmed = true;
            return false;
        }
        // 总执行超时检测
        if (millis() - scriptStartTime > MAX_EXECUTION_TIME_MS) {
            return false;
        }
        delay(10); // 短延时, ESP32 delay() 内含 yield() 喂 WDT
    }

    if (!PeripheralManager::getInstance().isUartEventOriginCurrent(uartOrigin)) {
        if (sourceUnconfirmed) *sourceUnconfirmed = true;
        return false;
    }
    return true;
}

// ========== 随机数表达式处理 ==========

bool ScriptEngine::processRandomExpressions(const String& input, String& output) {
    if (&input == &output) return false;
    output.clear();
    if (input.length() > MAX_SCRIPT_SIZE || !output.reserve(input.length())) return false;
    const bool complete = FastBee::expandScriptRandom(input.c_str(), input.length(),
        MAX_EXPANDED_MESSAGE_SIZE, [] { return esp_random(); },
        [&output](const char* data, size_t count) {
            const size_t expected = output.length() + count;
            for (size_t i = 0; i < count; ++i) if (!output.concat(data[i])) return false;
            return output.length() == expected;
        });
    if (!complete) output.clear();
    return complete;
}
#endif // FASTBEE_ENABLE_COMMAND_SCRIPT
