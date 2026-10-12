void PeriphExecManager::dispatchEventMatchedRules(const String& eventId, const String& eventData,
                                                const FastBee::UartEventOrigin& uartOrigin) {
    // Observe before taking rulesMutex; never hold the PM lock across callbacks.
    if (!PeripheralManager::getInstance().isUartEventOriginCurrent(uartOrigin)) return;
    int motorResultIndex = -1;
    if (eventId.startsWith("mc:")) {
        JsonDocument result;
        if (!deserializeJson(result, eventData.c_str(), eventData.length()) && result["origin"] == "motor-result") motorResultIndex = result["d"] | -1;
    }
    struct MatchedRule {
        String ruleId;
        size_t triggerIndex;
        uint64_t revision;
    };
    std::vector<MatchedRule> triggeredRules;

    {
        MutexGuard lock(_rulesMutex);

        for (auto& pair : rules) {
            PeriphExecRule& rule = pair.second;
            if (!rule.enabled) continue;

            // A motor acknowledgement must not start that motor again through
            // an event rule. Other targets can still react to the result.
            if (motorResultIndex >= 0) {
                bool feedback = false;
                for (const auto& action : rule.actions) {
                    if (action.targetPeriphId == "modbus:" + String(motorResultIndex)) feedback = true;
                    JsonDocument value;
                    if (!deserializeJson(value, action.actionValue.c_str(), action.actionValue.length())) {
                        for (JsonObjectConst control : value["ctrl"].as<JsonArrayConst>()) {
                            if ((control["d"] | -1) == motorResultIndex) feedback = true;
                        }
                    }
                }
                if (feedback) continue;
            }

            for (size_t triggerIndex = 0; triggerIndex < rule.triggers.size(); ++triggerIndex) {
                auto& trigger = rule.triggers[triggerIndex];
                if (trigger.triggerType != static_cast<uint8_t>(ExecTriggerType::EVENT_TRIGGER)) continue;
                if (trigger.eventId.isEmpty()) continue;
                if (trigger.eventId != eventId) continue;

                // Modbus 控制事件(mc:): 检查 compareValue 中的通道/动作是否匹配
                if (eventId.startsWith("mc:") && !trigger.compareValue.isEmpty()) {
                    bool ctrlMatched = false;
                    JsonDocument trigDoc;
                    if (!deserializeJson(trigDoc, trigger.compareValue.c_str(), trigger.compareValue.length()) && trigDoc.is<JsonObject>()) {
                        JsonArray trigCtrlArr = trigDoc["ctrl"].as<JsonArray>();
                        if (trigCtrlArr && trigCtrlArr.size() > 0) {
                            JsonObject trigCtrl = trigCtrlArr[0].as<JsonObject>();
                            // 解析事件数据
                            JsonDocument evDoc;
                            if (!deserializeJson(evDoc, eventData.c_str(), eventData.length()) && evDoc.is<JsonObject>()) {
                                JsonObject evObj = evDoc.as<JsonObject>();
                                // 匹配通道和动作
                                // 支持两种事件数据格式：
                                //   标准格式: {"d":0,"c":1,"a":"on"}
                                //   旧格式:   {"d":0,"id":"relay_0_1","v":"1"}
                                uint8_t trigCh = trigCtrl["c"] | 255;  // 255 表示不限制
                                const char* trigAct = trigCtrl["a"] | "";
                                uint8_t evCh = 255;   // 未知状态
                                String evActStr;      // 动作字符串（延用生命用）
                                // 标准格式：直接取 c 和 a 字段
                                if (evObj.containsKey("c")) {
                                    evCh = evObj["c"];
                                }
                                if (evObj.containsKey("a")) {
                                    evActStr = evObj["a"].as<String>();
                                }
                                // 旧格式降级：从 id 字段解析通道号
                                if (evCh == 255 && evObj.containsKey("id")) {
                                    String evId = evObj["id"].as<String>();
                                    int lastUnderscore = evId.lastIndexOf('_');
                                    if (lastUnderscore > 0) {
                                        evCh = evId.substring(lastUnderscore + 1).toInt();
                                    }
                                }
                                // 旧格式降级：从 v 字段推导动作
                                if (evActStr.isEmpty() && evObj.containsKey("v")) {
                                    String evVal = evObj["v"].as<String>();
                                    evActStr = (evVal == "1" || evVal.equalsIgnoreCase("on") || evVal.equalsIgnoreCase("true")) ? "on" : "off";
                                }
                                if (evCh == 255) evCh = 0;  // 无法解析通道时默认为 0
                                const char* evAct = evActStr.c_str();
                                if ((trigCh == 255 || trigCh == evCh) &&
                                    (strlen(trigAct) == 0 || strcmp(trigAct, evAct) == 0)) {
                                    ctrlMatched = true;
                                }
                            }
                        }
                    } else {
                        // compareValue 无法解析，仅匹配 eventId
                        ctrlMatched = true;
                    }
                    if (!ctrlMatched) continue;
                }

                // 红外事件(ir_code_received): compareValue 非空时按编码或按键名过滤
                // 支持格式：
                //   "key:名称"           —— 按已学习按键名匹配（推荐，可跨规则复用）
                //   "NEC:0x00FF6897"     —— 限定协议 + 编码
                //   "0x00FF6897"/"00FF6897" —— 任意协议 + 编码
                if (eventId == "ir_code_received" && !trigger.compareValue.isEmpty()) {
                    bool codeMatched = false;
                    JsonDocument evDoc;
                    if (!deserializeJson(evDoc, eventData.c_str(), eventData.length()) && evDoc.is<JsonObject>()) {
                        String want = trigger.compareValue;
                        want.trim();
                        int keyColon = want.indexOf(':');
                        String wantPrefix = keyColon > 0 ? want.substring(0, keyColon) : String("");
                        if (wantPrefix.equalsIgnoreCase("key")) {
                            // 按键名匹配（大小写不敏感）
                            String wantName = want.substring(keyColon + 1);
                            wantName.trim();
                            String evKey = evDoc["key"] | "";
                            codeMatched = (wantName.length() > 0) && wantName.equalsIgnoreCase(evKey);
                        } else {
                            // 编码匹配（兼容旧规则）
                            String evCode = evDoc["code"] | "";        // 如 "0x00FF6897"
                            String evProto = evDoc["protocol"] | "";   // 如 "NEC"
                            if (keyColon > 0) {
                                if (!wantPrefix.equalsIgnoreCase(evProto)) want = "";  // 协议不符
                                else want = want.substring(keyColon + 1);
                            }
                            if (want.length() > 0) {
                                if (!want.startsWith("0x") && !want.startsWith("0X")) want = "0x" + want;
                                codeMatched = want.equalsIgnoreCase(evCode);
                            }
                        }
                    }
                    if (!codeMatched) continue;
                }

                // RF事件(rf_code_received): compareValue 非空时按编码或按键名过滤
                // 支持格式：
                //   "key:名称"           —— 按已学习按键名匹配
                //   "0xABCDEF"/"ABCDEF"  —— 按编码值匹配
                if (eventId == "rf_code_received" && !trigger.compareValue.isEmpty()) {
                    bool rfMatched = false;
                    JsonDocument rfDoc;
                    if (!deserializeJson(rfDoc, eventData.c_str(), eventData.length()) && rfDoc.is<JsonObject>()) {
                        String want = trigger.compareValue;
                        want.trim();
                        if (want.startsWith("key:") || want.startsWith("KEY:")) {
                            String wantName = want.substring(4);
                            wantName.trim();
                            String evKey = rfDoc["key"] | "";
                            rfMatched = (wantName.length() > 0) && wantName.equalsIgnoreCase(evKey);
                        } else {
                            String evCode = rfDoc["code"] | "";
                            if (!want.startsWith("0x") && !want.startsWith("0X")) want = "0x" + want;
                            rfMatched = want.equalsIgnoreCase(evCode);
                        }
                    }
                    if (!rfMatched) continue;
                }

                // 防重复触发：同一触发器最小间隔 1 秒
                unsigned long now = millis();
                if (trigger.lastTriggerTime > 0 && (now - trigger.lastTriggerTime) < 1000) continue;

                LOGGER.infof("[PeriphExec] Event rule matched: '%s' (event=%s)",
                    rule.name.c_str(), eventId.c_str());

                triggeredRules.push_back({rule.id, triggerIndex, rule.runtimeRevision});
                break;  // 一个触发器匹配即可
            }
        }
    }
    // 锁已释放

    // 阶段2: 无锁异步分发
    for (const auto& matched : triggeredRules) {
        dispatchAndCommitTrigger(matched.ruleId, matched.triggerIndex,
                                 eventData, "event", uartOrigin, matched.revision);
    }
}
