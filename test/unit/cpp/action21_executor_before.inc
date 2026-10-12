
            // Canonical schema: targetPeriphId is the event ID and actionValue
            // is payload. Keep a legacy fallback for old files, while all new
            // Manager saves require the explicit target.
            String triggerEventId = action.targetPeriphId;
            if (triggerEventId.isEmpty()) {
                triggerEventId = effectiveValue.isEmpty() ? rule.id : effectiveValue;
            }
            LOGGER.infof("[PeriphExec] Trigger device event: id=%s name=%s (data=%s)",
                         triggerEventId.c_str(), rule.name.c_str(), effectiveValue.c_str());
            // 分发给规则（供其他规则订阅事件作为触发源），不受 reportAfterExec 控制
            const FastBee::UartEventOrigin eventOrigin = traceOut ? traceOut->uartOrigin : FastBee::UartEventOrigin{};
            PeriphExecManager::getInstance().triggerEventById(triggerEventId, effectiveValue, eventOrigin);
            // 仅在勾选“是否上报数据”且未被外部抑制时，通过 DEVICE_EVENT 主题上报
            if (rule.reportAfterExec && !suppressReport) {
                PeriphExecManager::getInstance().notifyMqttEventPublish(
                    triggerEventId, rule.name, effectiveValue, eventOrigin);
            }
            ok = true;
            appendActionResult(
                actionResults,
                triggerEventId,
                effectiveValue,
                ok,
                "event_triggered"
            );
        
