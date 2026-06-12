/**
 * 外设执行管理 - 表单模块
 * 规则编辑表单渲染、字段验证、表单提交处理
 * 依赖: periph-exec.js (核心) 和 periph-exec-modbus.js 先加载
 */
(function() {
    Object.assign(AppState, {

        // ============ Trigger/action element creation ============

        _createPeriphExecTriggerElement(data, index) {
            const container = document.getElementById('periph-exec-triggers');
            if (!container) return;
            const div = document.createElement('div');
            div.className = 'periph-exec-config-item';
            div.dataset.index = index;
            const triggerType = String(data.triggerType ?? 0);
            const showPlatform = triggerType === '0';
            const showTimer = triggerType === '1';
            const showEvent = triggerType === '4';
            const isDataSourceEvent = showEvent && data.eventId && String(data.eventId).indexOf('ds:') === 0;
            const isModbusCtrlEvent = showEvent && data.eventId && String(data.eventId).indexOf('mc:') === 0;
            const isButtonEvent = showEvent && data.eventId && String(data.eventId).indexOf('button_') === 0;
            const showPoll = triggerType === '5';
            const timerMode = String(data.timerMode ?? 0);
            const showInterval = timerMode === '0';
            const showDaily = timerMode === '1';
            const opVal = String(data.operatorType ?? 0);
            const mappedOp = (opVal === '0' || opVal === '1') ? opVal : '0';
            const showCompareValue = showPlatform && mappedOp !== '1';
            div.innerHTML = '<span class="mqtt-topic-index">' + (index + 1) + '</span>' +
                '<button type="button" class="mqtt-topic-delete">' + '删除' + '</button>' +
                '<div class="config-form-grid">' +
                    '<div class="fb-form-group"><label>' + '触发类型' + '</label>' +
                    '<select class="pe-trigger-type">' +
                        '<option value="0"' + (triggerType === '0' ? ' selected' : '') + '>' + '平台触发 (MQTT)' + '</option>' +
                        '<option value="4"' + (triggerType === '4' ? ' selected' : '') + '>' + '事件触发' + '</option>' +
                        '<option value="1"' + (triggerType === '1' ? ' selected' : '') + '>' + '定时触发' + '</option>' +
                        '<option value="5"' + (triggerType === '5' ? ' selected' : '') + '>' + '轮询触发 (本地数据)' + '</option>' +
                    '</select></div>' +
                    '<div class="fb-form-group pe-trigger-periph-group' + this._hiddenClass(showPlatform || isButtonEvent) + '">' +
                    '<label>' + '触发外设' + '</label>' +
                    '<select class="pe-trigger-periph"><option value="">' + '-- 选择外设 --' + '</option></select></div>' +
                '</div>' +
                '<div class="pe-platform-condition' + this._hiddenClass(showPlatform) + '">' +
                    '<div class="config-form-grid">' +
                    '<div class="fb-form-group"><label>' + '处理方式' + '</label>' +
                    '<select class="pe-operator">' +
                        '<option value="0"' + (mappedOp === '0' ? ' selected' : '') + '>' + '匹配' + '</option>' +
                        '<option value="1"' + (mappedOp === '1' ? ' selected' : '') + '>' + '设置' + '</option>' +
                    '</select></div>' +
                    '<div class="fb-form-group pe-compare-value-group' + this._hiddenClass(showCompareValue) + '">' +
                    '<label>' + '匹配值' + '</label>' +
                    '<input type="text" class="pe-compare-value" value="' + escapeHtml(data.compareValue) + '" placeholder="' + escapeHtml('如: 1 或 ON') + '"></div>' +
                    '</div></div>' +
                '<div class="pe-poll-params' + this._hiddenClass(showPoll) + '">' +
                    '<div class="config-form-grid">' +
                    '<div class="fb-form-group"><label>' + '轮询间隔 (秒)' + '</label><input type="number" class="pe-poll-interval" value="' + (data.intervalSec || 60) + '" min="5" max="86400"></div>' +
                    '<div class="fb-form-group"><label>' + '响应超时 (ms)' + '</label><input type="number" class="pe-poll-timeout" value="' + (data.pollResponseTimeout || 1000) + '" min="100" max="5000"></div>' +
                    '<div class="fb-form-group"><label>' + '最大重试次数' + '</label><input type="number" class="pe-poll-retries" value="' + (data.pollMaxRetries ?? 2) + '" min="0" max="3"></div>' +
                    '<div class="fb-form-group"><label>' + '请求间隔 (ms)' + '</label><input type="number" class="pe-poll-inter-delay" value="' + (data.pollInterPollDelay || 100) + '" min="20" max="1000"></div>' +
                    '</div></div>' +
                '<div class="pe-timer-config' + this._hiddenClass(showTimer) + '">' +
                    '<div class="config-form-grid">' +
                    '<div class="fb-form-group"><label>' + '定时模式' + '</label>' +
                    '<select class="pe-timer-mode">' +
                        '<option value="0"' + (timerMode === '0' ? ' selected' : '') + '>' + '间隔触发' + '</option>' +
                        '<option value="1"' + (timerMode === '1' ? ' selected' : '') + '>' + '每日时间点' + '</option>' +
                    '</select></div>' +
                    '<div class="fb-form-group pe-interval-fields' + this._hiddenClass(showInterval) + '"><label>' + '间隔 (秒)' + '</label><input type="number" class="pe-interval" value="' + (data.intervalSec || 60) + '" min="1" max="86400"></div>' +
                    '<div class="fb-form-group pe-daily-fields' + this._hiddenClass(showDaily) + '"><label>' + '执行时间 (HH:MM)' + '</label><input type="time" class="pe-timepoint" value="' + (data.timePoint || '08:00') + '"></div>' +
                    '</div></div>' +
                '<div class="pe-event-group' + this._hiddenClass(showEvent) + '">' +
                    '<div class="config-form-grid">' +
                    '<div class="fb-form-group"><label>' + '事件分类' + '</label>' +
                    '<select class="pe-event-category">' +
                    '<option value="">' + '-- 选择分类 --' + '</option></select></div>' +
                    '<div class="fb-form-group"><label>' + '触发事件' + '</label>' +
                    '<select class="pe-event"><option value="">' + '-- 选择事件 --' + '</option></select></div>' +
                    '<div class="fb-form-group pe-event-condition-group' + this._hiddenClass(isDataSourceEvent) + '"><label>' + '比较操作符' + '</label>' +
                    '<select class="pe-event-operator">' +
                    '<option value="0"' + (opVal === '0' ? ' selected' : '') + '>' + '等于 (=)' + '</option>' +
                    '<option value="1"' + (opVal === '1' ? ' selected' : '') + '>' + '不等于 (!=)' + '</option>' +
                    '<option value="2"' + (opVal === '2' ? ' selected' : '') + '>' + '大于 (>)' + '</option>' +
                    '<option value="3"' + (opVal === '3' ? ' selected' : '') + '>' + '小于 (<)' + '</option>' +
                    '<option value="4"' + (opVal === '4' ? ' selected' : '') + '>' + '大于等于 (>=)' + '</option>' +
                    '<option value="5"' + (opVal === '5' ? ' selected' : '') + '>' + '小于等于 (<=)' + '</option>' +
                    '<option value="6"' + (opVal === '6' ? ' selected' : '') + '>' + '区间内 (a,b)' + '</option>' +
                    '<option value="7"' + (opVal === '7' ? ' selected' : '') + '>' + '区间外' + '</option>' +
                    '<option value="8"' + (opVal === '8' ? ' selected' : '') + '>' + '包含' + '</option>' +
                    '<option value="9"' + (opVal === '9' ? ' selected' : '') + '>' + '不包含' + '</option>' +
                    '</select></div>' +
                    '<div class="fb-form-group pe-event-compare-group' + this._hiddenClass(isDataSourceEvent) + '"><label>' + '比较值' + '</label>' +
                    '<input type="text" class="pe-event-compare" value="' + escapeHtml(data.compareValue || '') + '" placeholder="' + escapeHtml('如: 25.5') + '"></div>' +
                    '</div>' +
                    '<div class="pe-event-modbus-ctrl-panel' + this._hiddenClass(isModbusCtrlEvent) + '"></div>' +
                    '</div>';
            container.appendChild(div);
            // 合并后的触发外设下拉：平台触发列全部外设，按键事件仅列按键类型
            const periphSel = div.querySelector('.pe-trigger-periph');
            if (showPlatform) {
                this._populateTriggerPeriphSelect(periphSel, data.triggerPeriphId || data.sourcePeriphId || '');
            } else if (isButtonEvent) {
                this._populateButtonPeriphSelect(periphSel, data.triggerPeriphId || '');
            }
            if (showEvent) this._populateEventCategoriesInBlock(div, data.eventId, data.compareValue || '');
        },

        _createPeriphExecActionElement(data, index) {
            const container = document.getElementById('periph-exec-actions');
            if (!container) return;
            const div = document.createElement('div');
            div.className = 'periph-exec-config-item';
            div.dataset.index = index;
            const isPollMode = this._isPollTriggerActive();
            let actionType = String(isPollMode ? 18 : (data.actionType ?? 0));
            if (!isPollMode && actionType === '9') {
                actionType = '0';
            }
            const actionTypeInt = parseInt(actionType);
            const isModbusPoll = actionTypeInt === 18;
            const isSensorRead = actionTypeInt === 19;
            const isScriptAction = actionTypeInt === 15;
            const isModbusTarget = data.targetPeriphId && data.targetPeriphId.indexOf('modbus:') === 0;
            const isTriggerEvent = actionTypeInt === 21;
            const isRuleCtrlAction = (actionTypeInt === 22 || actionTypeInt === 23);
            const isDisplayAction = (actionTypeInt === 24 || actionTypeInt === 25 || actionTypeInt === 26);
            const isOledDisplay = (actionTypeInt === 27);
            const showPeriphGroup = isPollMode || isRuleCtrlAction || (isModbusTarget || !((actionTypeInt >= 6 && actionTypeInt <= 9) || isModbusPoll || isTriggerEvent));
            const needsValue = !isPollMode && !isModbusTarget && !isRuleCtrlAction && ((actionTypeInt >= 2 && actionTypeInt <= 5) || actionTypeInt === 10 || actionTypeInt === 24 || actionTypeInt === 25 || isOledDisplay || isScriptAction);
            const showRecv = !isPollMode && !isModbusTarget && !isRuleCtrlAction && this._hasSetModeTrigger() && needsValue && !isOledDisplay && !isScriptAction;
            const showActionType = !isPollMode && !isModbusTarget;
            const showExecRow = true;
            const execMode = parseInt(data.execMode ?? 0);
            const sel = (v) => actionType === String(v) ? 'selected' : '';
            var sensorCfg = {sensorCategory:'analog',periphId:'',scaleFactor:0.00080586,offset:0,decimalPlaces:2,sensorLabel:'电压',unit:'V',dataField:'voltage',deviceIndex:0};
            if (isSensorRead && data.actionValue) { try { Object.assign(sensorCfg, JSON.parse(data.actionValue)); } catch(e) {} }
            if (!sensorCfg.dataField) {
                var sensorCatLower = String(sensorCfg.sensorCategory || '').toLowerCase();
                sensorCfg.dataField = sensorCatLower === 'analog' ? 'voltage' :
                    (sensorCatLower === 'radar' ? 'presence' : 'temperature');
            }
            const sensorBaseKeys = {
                periphId: 1, sensorCategory: 1, scaleFactor: 1, offset: 1, decimalPlaces: 1,
                sensorLabel: 1, unit: 1, dataField: 1, deviceIndex: 1
            };
            var sensorExtra = {};
            Object.keys(sensorCfg).forEach(function(k) {
                if (!sensorBaseKeys[k]) sensorExtra[k] = sensorCfg[k];
            });
            const sensorExtraJson = Object.keys(sensorExtra).length ? JSON.stringify(sensorExtra) : '';
            const selCat = (v) => sensorCfg.sensorCategory === v ? 'selected' : '';
            const sensorFieldOptions = this._renderSensorDataFieldOptions(sensorCfg.sensorCategory, sensorCfg.dataField);
            div.innerHTML = '<span class="mqtt-topic-index">' + (index + 1) + '</span>' +
                '<button type="button" class="mqtt-topic-delete">' + '删除' + '</button>' +
                '<div class="pe-action-grid">' +
                    '<div class="pe-exec-row pe-span-all' + this._hiddenClass(showExecRow) + '">' +
                    '<div class="fb-form-group pe-exec-mode-group">' +
                    '<label>' + '执行模式' + '</label>' +
                    '<select class="pe-exec-mode">' +
                    '<option value="0"' + (execMode === 0 ? ' selected' : '') + '>' + '异步执行' + '</option>' +
                    '<option value="1"' + (execMode === 1 ? ' selected' : '') + '>' + '同步执行' + '</option>' +
                    '</select></div>' +
                    '<div class="fb-form-group pe-sync-delay-group">' +
                    '<label>' + '执行前延时(0-10000ms)' + '</label>' +
                    '<input type="number" class="pe-sync-delay" min="0" max="10000" step="100" value="' + (data.syncDelayMs || 0) + '" placeholder="0"></div></div>' +
                    '<div class="fb-form-group pe-action-type-group' + this._hiddenClass(showActionType) + '"><label>' + '执行动作' + '</label>' +
                    '<select class="pe-action-type">' +
                        '<optgroup label="' + 'GPIO操作' + '">' +
                        '<option value="0" ' + sel(0) + '>' + '设置高电平' + '</option>' +
                        '<option value="1" ' + sel(1) + '>' + '设置低电平' + '</option>' +
                        '<option value="2" ' + sel(2) + '>' + '闪烁' + '</option>' +
                        '<option value="3" ' + sel(3) + '>' + '呼吸灯' + '</option>' +
                        '<option value="13" ' + sel(13) + '>' + '高电平反转' + '</option>' +
                        '<option value="14" ' + sel(14) + '>' + '低电平反转' + '</option></optgroup>' +
                        '<optgroup label="' + '模拟输出' + '">' +
                        '<option value="4" ' + sel(4) + '>' + '设置PWM' + '</option>' +
                        '<option value="5" ' + sel(5) + '>' + '设置DAC' + '</option></optgroup>' +
                        '<optgroup label="' + '系统功能' + '">' +
                        '<option value="6" ' + sel(6) + '>' + '系统重启' + '</option>' +
                        '<option value="7" ' + sel(7) + '>' + '恢复出厂' + '</option>' +
                        '<option value="8" ' + sel(8) + '>' + 'NTP同步' + '</option></optgroup>' +
                        '<optgroup label="' + '高级功能' + '">' +
                        '<option value="10" ' + sel(10) + '>' + '调用外设' + '</option>' +
                        '<option value="19" ' + sel(19) + '>' + '传感器数据读取' + '</option>' +
                        '<option value="21" ' + sel(21) + '>' + '触发设备事件' + '</option>' +
                        '<option value="15" ' + sel(15) + '>' + '命令脚本' + '</option></optgroup>' +
                        '<optgroup label="' + '规则控制' + '">' +
                        '<option value="22" ' + sel(22) + '>' + '启用执行规则' + '</option>' +
                        '<option value="23" ' + sel(23) + '>' + '禁用执行规则' + '</option></optgroup>' +
                        '<optgroup label="' + '显示屏' + '">' +
                        '<option value="24" ' + sel(24) + '>' + '显示数字' + '</option>' +
                        '<option value="25" ' + sel(25) + '>' + '显示文本' + '</option>' +
                        '<option value="26" ' + sel(26) + '>' + '数码管清屏' + '</option>' +
                        '<option value="27" ' + sel(27) + '>' + 'OLED自定义显示' + '</option></optgroup>' +
                    '</select></div>' +
                    '<div class="fb-form-group pe-target-group' + this._hiddenClass(showPeriphGroup) + '">' +
                    '<label>' + (isRuleCtrlAction ? '目标执行规则' : '执行外设') + '</label>' +
                    '<select class="pe-target-periph"><option value="">' + (isRuleCtrlAction ? '-- 选择执行规则 --' : '-- 选择外设 --') + '</option></select></div>' +
                    '<div class="fb-form-group pe-modbus-ctrl-panel' + this._hiddenClass(isModbusTarget) + '"></div>' +
                    '<div class="fb-form-group pe-action-value-group' + this._hiddenClass(needsValue) + '">' +
                    '<label>' + '动作参数' + '</label>' +
                    '<input type="text" class="pe-action-value' + ((isOledDisplay || isScriptAction) ? ' is-hidden' : '') + '" value="' + ((isOledDisplay || isScriptAction) ? '' : escapeHtml(data.actionValue)) + '" placeholder="' + escapeHtml(isDisplayAction ? '支持 ${periphId.field} 模板（例如 ${dht_01.temperature} / ${adc.voltage}），将从传感器缓存取最新值' : '如: PWM值、闪烁间隔ms') + '"' + (showRecv && data.useReceivedValue !== false ? ' readonly' : '') + '>' +
                    '<textarea class="pe-action-value-oled pe-script-textarea' + (isOledDisplay ? '' : ' is-hidden') + '" rows="6" maxlength="512" placeholder="' + escapeHtml('多行文本，每行一条。首行以 # 开头为居中标题。支持 ${periphId.field}（传感器缓存）和 $value（下发变量）模板。例：\n# 环境监测\n温度:${dht_01.temperature}°C\n湿度:${dht_01.humidity}%\n设备:$value') + '">' + (isOledDisplay ? escapeHtml(data.actionValue || '') : '') + '</textarea>' +
                    '<textarea class="pe-action-value-script pe-script-textarea' + (isScriptAction ? '' : ' is-hidden') + '" rows="7" maxlength="1024" placeholder="' + escapeHtml('每行一条命令，如:\nGPIO 2 HIGH\nDELAY 500\nGPIO 2 LOW\nLOG 执行完成') + '">' + (isScriptAction ? escapeHtml(data.actionValue || '') : '') + '</textarea>' +
                    '<small class="pe-help-text">' + (actionTypeInt === 10 ? '调用外设 JSON，例如 {"periphId":"stepper","action":"forward"}、{"periphId":"ws2812b","action":"color","value":"#ff0000"}、{"periphId":"uart_debug","action":"send","value":"hello"}；UART 目标也可直接填写要发送的文本' : (isScriptAction ? '可用: GPIO pin HIGH/LOW, DELAY ms, PWM pin duty, DAC pin val, LOG msg, PERIPH id 动作' : (isOledDisplay ? '最多 6 行、可自动适配 OLED 显示。\n 换行。首行 # 开头为居中标题带分隔线。支持变量：${外设id.字段} 从传感器缓存取值，$value 取 MQTT/规则下发的原始值' : (isDisplayAction ? '支持 ${periphId.field} 模板（例如 ${dht_01.temperature} / ${adc.voltage}），将从传感器缓存取最新值' : '闪烁/呼吸灯填间隔ms, PWM填占空比, DAC填0-255')))) + '</small></div>' +
                    '<div class="fb-form-group pe-use-received-value-group' + this._hiddenClass(showRecv) + '">' +
                    '<label class="pe-checkbox-label pe-check-align"><input type="checkbox" class="pe-use-received-value"' + (showRecv && data.useReceivedValue !== false ? ' checked' : '') + '>' +
                    '勾选后动作参数将使用平台下发或触发源的值' + '</label></div>' +
                    '<div class="fb-form-group pe-poll-tasks-group pe-span-all' + this._hiddenClass(!isPollMode && isModbusPoll && !isModbusTarget) + '">' +
                    '<label>' + '选择子设备' + '</label>' +
                    '<div class="pe-poll-tasks-list"></div>' +
                    '<small class="pe-help-text">' + '选择要执行的 Modbus 子设备' + '</small></div>' +
                    '<div class="fb-form-group pe-sensor-group pe-span-all' + this._hiddenClass(isSensorRead) + '">' +
                    '<div class="pe-sensor-config-grid">' +
                    '<div class="fb-form-group"><label>' + '传感器类别' + '</label>' +
                    '<select class="pe-sensor-category">' +
                    '<option value="analog" ' + selCat('analog') + '>' + '模拟量 (ADC)' + '</option>' +
                    '<option value="digital" ' + selCat('digital') + '>' + '数字量 (DI)' + '</option>' +
                    '<option value="pulse" ' + selCat('pulse') + '>' + '脉冲/频率' + '</option>' +
                    '<option value="dht11" ' + selCat('dht11') + '>' + 'DHT11 温湿度' + '</option>' +
                    '<option value="dht22" ' + selCat('dht22') + '>' + 'DHT22 温湿度' + '</option>' +
                    '<option value="ds18b20" ' + selCat('ds18b20') + '>' + 'DS18B20 温度' + '</option>' +
                    '<option value="ultrasonic" ' + selCat('ultrasonic') + '>' + 'HC-SR04 超声波' + '</option>' +
                    '<option value="radar" ' + selCat('radar') + '>雷达存在检测</option>' +
                    '<option value="rf" ' + selCat('rf') + '>射频接收电平</option>' +
                    '<option value="current" ' + selCat('current') + '>' + '电流型传感器' + '</option>' +
                    '<option value="voltage" ' + selCat('voltage') + '>' + '电压型传感器' + '</option>' +
                    '<option value="SHT31" ' + selCat('SHT31') + '>' + 'SHT31 温湿度' + '</option>' +
                    '<option value="AHT20" ' + selCat('AHT20') + '>' + 'AHT20 温湿度' + '</option>' +
                    '<option value="BH1750" ' + selCat('BH1750') + '>' + 'BH1750 光照' + '</option>' +
                    '<option value="BMP280" ' + selCat('BMP280') + '>' + 'BMP280 气压' + '</option>' +
                    '<option value="MPU6050" ' + selCat('MPU6050') + '>' + 'MPU6050 姿态' + '</option></select></div>' +
                    '<div class="fb-form-group pe-sensor-datafield-group"><label>' + '数据字段' + '</label>' +
                    '<select class="pe-sensor-datafield">' +
                    sensorFieldOptions + '</select></div>' +
                    '<div class="fb-form-group pe-sensor-devindex-group' + this._hiddenClass(sensorCfg.sensorCategory === 'ds18b20') + '"><label>' + '设备索引' + '</label>' +
                    '<input type="number" class="pe-sensor-devindex" min="0" max="15" value="' + sensorCfg.deviceIndex + '"></div>' +
                    '<div class="fb-form-group"><label>' + '缩放系数' + '</label><input type="number" class="pe-sensor-scale" step="any" value="' + sensorCfg.scaleFactor + '"></div>' +
                    '<div class="fb-form-group"><label>' + '偏移量' + '</label><input type="number" class="pe-sensor-offset" step="any" value="' + sensorCfg.offset + '"></div>' +
                    '<div class="fb-form-group"><label>' + '小数位数' + '</label><input type="number" class="pe-sensor-decimals" min="0" max="6" value="' + sensorCfg.decimalPlaces + '"></div>' +
                    '<div class="fb-form-group"><label>' + '数据标签' + '</label><input type="text" class="pe-sensor-label" value="' + escapeHtml(sensorCfg.sensorLabel) + '" placeholder="' + '数据标签' + '"></div>' +
                    '<div class="fb-form-group"><label>' + '单位' + '</label><input type="text" class="pe-sensor-unit" value="' + escapeHtml(sensorCfg.unit) + '" maxlength="8" placeholder="°C, %, V..."></div>' +
                    '<div class="fb-form-group pe-span-all"><label>' + '高级参数(JSON)' + '</label>' +
                    '<textarea class="pe-sensor-extra-json" rows="2" maxlength="512" placeholder="' + escapeHtml('{"driverParams":{"addr":"0x44","sda":21,"scl":22}}') + '">' + escapeHtml(sensorExtraJson) + '</textarea>' +
                    '<small class="pe-help-text">' + '可填写分压比、电流零点、I2C 地址等高级参数；留空则使用默认值。' + '</small></div>' +
                    '</div></div>' +
                    '</div>';
            container.appendChild(div);
            if (!isPollMode && isSensorRead) {
                this._populateSensorPeriphSelect(div, sensorCfg.sensorCategory || 'analog', sensorCfg.periphId || data.targetPeriphId || '');
            } else {
                var initActType = parseInt(data.actionType);
                this._populatePeriphSelect(div.querySelector('.pe-target-periph'), data.targetPeriphId || '', isPollMode, initActType === 21, (initActType === 22 || initActType === 23));
            }
            if (!isPollMode && isModbusPoll && !isModbusTarget) this._populateModbusDevicePanel(div.querySelector('.pe-poll-tasks-list'), data.actionValue || '');
            if (isModbusTarget) this._showModbusCtrlPanel(div.querySelector('.pe-modbus-ctrl-panel'), data.targetPeriphId, data.actionValue || '');
        },

        addPeriphExecTrigger() {
            const container = document.getElementById('periph-exec-triggers');
            if (!container) return;
            this._createPeriphExecTriggerElement({}, container.children.length);
            this._refreshPeriphExecRiskNotice({ allowFetch: true });
        },

        deletePeriphExecTrigger(index) {
            const container = document.getElementById('periph-exec-triggers');
            if (!container) return;
            const items = container.querySelectorAll('.periph-exec-config-item');
            if (items.length <= 1) return;
            if (items[index]) items[index].remove();
            this._reindexPeriphExecBlocks(container);
            this._refreshPeriphExecRiskNotice({ allowFetch: false });
        },

        addPeriphExecAction() {
            const container = document.getElementById('periph-exec-actions');
            if (!container) return;
            this._createPeriphExecActionElement({}, container.children.length);
            this._refreshPeriphExecRiskNotice({ allowFetch: true });
        },

        deletePeriphExecAction(index) {
            const container = document.getElementById('periph-exec-actions');
            if (!container) return;
            const items = container.querySelectorAll('.periph-exec-config-item');
            if (items.length <= 1) return;
            if (items[index]) items[index].remove();
            this._reindexPeriphExecBlocks(container);
            this._refreshPeriphExecRiskNotice({ allowFetch: false });
        },

        _reindexPeriphExecBlocks(container) {
            const items = container.querySelectorAll('.periph-exec-config-item');
            items.forEach((item, idx) => {
                item.dataset.index = idx;
                const indexSpan = item.querySelector('.mqtt-topic-index');
                if (indexSpan) indexSpan.textContent = idx + 1;
            });
        },

        // ============ Change handlers ============

        onPeriphExecTriggerTypeChangeInBlock(val, index) {
            const block = this._getPeriphExecBlock('periph-exec-triggers', index);
            if (!block) return;
            const triggerPeriphGroup = block.querySelector('.pe-trigger-periph-group');
            const platformCondition = block.querySelector('.pe-platform-condition');
            const pollParams = block.querySelector('.pe-poll-params');
            const timerConfig = block.querySelector('.pe-timer-config');
            const eventGroup = block.querySelector('.pe-event-group');
            const periphSel = block.querySelector('.pe-trigger-periph');
            // 触发外设下拉：平台触发则展示并填充全部外设；事件触发则依据当前已选事件是否按键决定显示，其他类型隐藏
            if (val === '0') {
                this._setSectionVisible(triggerPeriphGroup, true);
                if (periphSel) this._populateTriggerPeriphSelect(periphSel, periphSel.value || '');
            } else if (val === '4') {
                const eventSel = block.querySelector('.pe-event');
                const curEv = eventSel ? eventSel.value : '';
                const isBtn = curEv && String(curEv).indexOf('button_') === 0;
                this._setSectionVisible(triggerPeriphGroup, !!isBtn);
                if (isBtn && periphSel) this._populateButtonPeriphSelect(periphSel, periphSel.value || '');
            } else {
                this._setSectionVisible(triggerPeriphGroup, false);
            }
            this._setSectionVisible(platformCondition, val === '0');
            this._setSectionVisible(pollParams, val === '5');
            this._setSectionVisible(timerConfig, val === '1');
            if (eventGroup) {
                this._setSectionVisible(eventGroup, val === '4');
                if (val === '4') {
                    this._populateEventCategoriesInBlock(block);
                }
            }
            if (val !== '0') {
                this._checkAndSyncSetMode();
            }
            // 触发类型变更时重建执行动作区域(轮询触发模式UI完全不同)
            this._rebuildActionBlocksForTriggerChange();
        },

        onPeriphExecOperatorChangeInBlock(val, index) {
            const block = this._getPeriphExecBlock('periph-exec-triggers', index);
            if (!block) return;
            const compareGroup = block.querySelector('.pe-compare-value-group');
            this._setSectionVisible(compareGroup, val !== '1');
            this._checkAndSyncSetMode();
        },

        onPeriphExecUseRecvChangeInBlock(checkbox, index) {
            const block = checkbox.closest('.periph-exec-config-item');
            if (!block) return;
            const valueInput = block.querySelector('.pe-action-value');
            if (valueInput) {
                if (checkbox.checked) {
                    valueInput.setAttribute('readonly', '');
                } else {
                    valueInput.removeAttribute('readonly');
                }
            }
        },

        _checkAndSyncSetMode() {
            const hasSetMode = this._hasSetModeTrigger();
            this._syncSetModeToActions(hasSetMode);
        },

        _hasSetModeTrigger() {
            const container = document.getElementById('periph-exec-triggers');
            if (!container) return false;
            const items = container.querySelectorAll('.periph-exec-config-item');
            for (let i = 0; i < items.length; i++) {
                const triggerType = items[i].querySelector('.pe-trigger-type');
                const operator = items[i].querySelector('.pe-operator');
                if (triggerType && triggerType.value === '0' && operator && operator.value === '1') {
                    return true;
                }
            }
            return false;
        },

        _syncSetModeToActions(isSetMode) {
            const container = document.getElementById('periph-exec-actions');
            if (!container) return;
            container.querySelectorAll('.periph-exec-config-item').forEach(function (item) {
                const checkbox = item.querySelector('.pe-use-received-value');
                const valueInput = item.querySelector('.pe-action-value');
                const recvGroup = item.querySelector('.pe-use-received-value-group');
                const actionType = parseInt(item.querySelector('.pe-action-type')?.value || '0');
                const needsValue = (actionType >= 2 && actionType <= 5) || actionType === 10;
                const showRecv = isSetMode && needsValue;
                if (recvGroup) {
                    if (showRecv) recvGroup.classList.remove('is-hidden');
                    else recvGroup.classList.add('is-hidden');
                }
                if (checkbox) checkbox.checked = showRecv;
                if (valueInput) {
                    if (showRecv) valueInput.setAttribute('readonly', '');
                    else valueInput.removeAttribute('readonly');
                }
            });
        },

        onPeriphExecTimerModeChangeInBlock(val, index) {
            const block = this._getPeriphExecBlock('periph-exec-triggers', index);
            if (!block) return;
            const intervalFields = block.querySelector('.pe-interval-fields');
            const dailyFields = block.querySelector('.pe-daily-fields');
            this._setSectionVisible(intervalFields, val === '0');
            this._setSectionVisible(dailyFields, val === '1');
        },

        onPeriphExecEventCategoryChangeInBlock(val, index) {
            const block = this._getPeriphExecBlock('periph-exec-triggers', index);
            if (!block) return;
            this._populateEventSelectInBlock(block, val);
            // 切换分类时默认隐藏触发外设下拉，待选定具体按键事件后再显示
            this._setSectionVisible(block.querySelector('.pe-trigger-periph-group'), false);
        },

        onPeriphExecEventChangeInBlock(val, index) {
            const block = this._getPeriphExecBlock('periph-exec-triggers', index);
            if (!block) return;
            var isDataSource = val && val.indexOf('ds:') === 0;
            var isModbusCtrl = val && val.indexOf('mc:') === 0;
            var isButtonEvt = val && String(val).indexOf('button_') === 0;
            this._setSectionVisible(block.querySelector('.pe-event-condition-group'), isDataSource);
            this._setSectionVisible(block.querySelector('.pe-event-compare-group'), isDataSource);
            // 按键事件时显示触发外设下拉并填充按键类型外设；否则隐藏
            var triggerPeriphGroup = block.querySelector('.pe-trigger-periph-group');
            if (triggerPeriphGroup) {
                this._setSectionVisible(triggerPeriphGroup, !!isButtonEvt);
                if (isButtonEvt) {
                    var btnSel = block.querySelector('.pe-trigger-periph');
                    if (btnSel) this._populateButtonPeriphSelect(btnSel, btnSel.value || '');
                }
            }
            var ctrlPanel = block.querySelector('.pe-event-modbus-ctrl-panel');
            if (ctrlPanel) {
                if (isModbusCtrl) {
                    var devIdx = parseInt(val.substring(3));
                    this._showModbusTriggerCtrlPanel(ctrlPanel, devIdx, '');
                }
                this._setSectionVisible(ctrlPanel, isModbusCtrl);
            }
        },

        onPeriphExecActionTypeChangeInBlock(val, index) {
            const block = this._getPeriphExecBlock('periph-exec-actions', index);
            if (!block) return;
            // 轮询触发模式不处理动作类型变更
            if (this._isPollTriggerActive()) return;
            const actionType = parseInt(val);
            const targetGroup = block.querySelector('.pe-target-group');
            const valueGroup = block.querySelector('.pe-action-value-group');
            const pollTasksGroup = block.querySelector('.pe-poll-tasks-group');
            const isModbusPoll = actionType === 18;
            const isSensorRead = actionType === 19;
            const isTriggerEvent = actionType === 21;
            const isRuleCtrlAction = (actionType === 22 || actionType === 23);
            const showTargetGroup = isRuleCtrlAction || !((actionType >= 6 && actionType <= 9) || isModbusPoll || isTriggerEvent);
            this._setSectionVisible(targetGroup, showTargetGroup);
            // 切换目标分组的 label 和空选项提示（规则 vs 外设）
            if (targetGroup) {
                var labelEl = targetGroup.querySelector('label');
                if (labelEl) labelEl.textContent = isRuleCtrlAction ? '目标执行规则' : '执行外设';
            }
            const needsValue = !isRuleCtrlAction && ((actionType >= 2 && actionType <= 5) || actionType === 10 || actionType === 15 || actionType === 24 || actionType === 25 || actionType === 27);
            this._setSectionVisible(valueGroup, needsValue);
            // OLED 自定义显示：在 value-group 内 input / textarea 两种控件互斥显示
            if (valueGroup) {
                const singleEl = valueGroup.querySelector('.pe-action-value');
                const multiEl = valueGroup.querySelector('.pe-action-value-oled');
                const scriptEl = valueGroup.querySelector('.pe-action-value-script');
                const isOled = actionType === 27;
                const isScript = actionType === 15;
                if (singleEl) {
                    if (isOled || isScript) singleEl.classList.add('is-hidden');
                    else singleEl.classList.remove('is-hidden');
                }
                if (multiEl) {
                    if (isOled) multiEl.classList.remove('is-hidden');
                    else multiEl.classList.add('is-hidden');
                }
                if (scriptEl) {
                    if (isScript) scriptEl.classList.remove('is-hidden');
                    else scriptEl.classList.add('is-hidden');
                }
                // 提示文案同步切换
                const helpEl = valueGroup.querySelector('.pe-help-text');
                if (helpEl) {
                    if (actionType === 10) helpEl.textContent = '调用外设 JSON，例如 {"periphId":"stepper","action":"forward"}、{"periphId":"ws2812b","action":"color","value":"#ff0000"}、{"periphId":"uart_debug","action":"send","value":"hello"}；UART 目标也可直接填写要发送的文本';
                    else if (isScript) helpEl.textContent = '可用: GPIO pin HIGH/LOW, DELAY ms, PWM pin duty, DAC pin val, LOG msg, PERIPH id 动作';
                    else if (isOled) helpEl.textContent = '最多 6 行、可自动适配 OLED 显示。\n 换行。首行 # 开头为居中标题带分隔线。支持变量：${外设id.字段} 从传感器缓存取值，$value 取 MQTT/规则下发的原始值';
                    else if (actionType === 24 || actionType === 25 || actionType === 26) helpEl.textContent = '支持 ${periphId.field} 模板（例如 ${dht_01.temperature} / ${adc.voltage}），将从传感器缓存取最新值';
                    else helpEl.textContent = '闪烁/呼吸灯填间隔ms, PWM填占空比, DAC填0-255';
                }
            }
            if (pollTasksGroup) {
                this._setSectionVisible(pollTasksGroup, isModbusPoll);
                if (isModbusPoll) {
                    this._populateModbusDevicePanel(block.querySelector('.pe-poll-tasks-list'), '');
                }
            }
            const sensorGroup = block.querySelector('.pe-sensor-group');
            this._setSectionVisible(sensorGroup, isSensorRead);
            // 切换动作类型时，隐藏 Modbus 控制面板（ctrlPanel 仅在选择 modbus:xxx 目标时由 _onTargetPeriphChange 管理）
            const ctrlPanel = block.querySelector('.pe-modbus-ctrl-panel');
            this._setSectionVisible(ctrlPanel, false);
            if (isSensorRead) {
                var cat = block.querySelector('.pe-sensor-category')?.value || 'analog';
            }
            if (targetGroup && !targetGroup.classList.contains('is-hidden')) {
                var peSelect = block.querySelector('.pe-target-periph');
                this._populatePeriphSelect(peSelect, peSelect ? peSelect.value : '', this._isPollTriggerActive(), isTriggerEvent, isRuleCtrlAction);
            }
            // 使用接收值: 仅在平台触发+设置+需要数值的动作类型时显示（OLED_DISPLAY 不需要整体替换，通过 $value 占位符嵌入）
            const recvGroup = block.querySelector('.pe-use-received-value-group');
            const isSetMode = this._hasSetModeTrigger();
            const showRecv = isSetMode && needsValue && actionType !== 15 && actionType !== 27;
            this._setSectionVisible(recvGroup, showRecv);
            const checkbox = block.querySelector('.pe-use-received-value');
            if (checkbox) checkbox.checked = showRecv;
            const valueInput = block.querySelector('.pe-action-value');
            if (valueInput) {
                if (showRecv) valueInput.setAttribute('readonly', '');
                else valueInput.removeAttribute('readonly');
            }
        },

        // ============ Event population ============

        _clearPeriphExecEventCatalogCache() {
            this._peEventCategories = null;
            this._peEventCategoriesFetchedAt = 0;
            this._peEventCategoriesPromise = null;
            this._peStaticEvents = null;
            this._peStaticEventsFetchedAt = 0;
            this._peStaticEventsPromise = null;
            this._peDynamicEvents = null;
            this._peDynamicEventsFetchedAt = 0;
            this._peDynamicEventsPromise = null;
        },

        _getPeriphExecEventCategories(forceFresh) {
            var self = this;
            var now = Date.now();
            if (!forceFresh && Array.isArray(this._peEventCategories) &&
                (now - this._peEventCategoriesFetchedAt) < 120000) {
                return Promise.resolve(this._peEventCategories);
            }
            if (!forceFresh && this._peEventCategoriesPromise) {
                return this._peEventCategoriesPromise;
            }

            var getter = (forceFresh && typeof apiGetFresh === 'function') ? apiGetFresh : apiGet;
            this._peEventCategoriesPromise = getter('/api/periph-exec/events/categories')
                .then(function(res) {
                    var list = (res && res.success && Array.isArray(res.data)) ? res.data.slice() : [];
                    self._peEventCategories = list;
                    self._peEventCategoriesFetchedAt = Date.now();
                    return list;
                })
                .catch(function(err) {
                    if (Array.isArray(self._peEventCategories) && self._peEventCategories.length > 0) {
                        return self._peEventCategories;
                    }
                    throw err;
                })
                .finally(function() {
                    self._peEventCategoriesPromise = null;
                });
            return this._peEventCategoriesPromise;
        },

        _getPeriphExecStaticEvents(forceFresh) {
            var self = this;
            var now = Date.now();
            if (!forceFresh && Array.isArray(this._peStaticEvents) &&
                (now - this._peStaticEventsFetchedAt) < 120000) {
                return Promise.resolve(this._peStaticEvents);
            }
            if (!forceFresh && this._peStaticEventsPromise) {
                return this._peStaticEventsPromise;
            }

            var getter = (forceFresh && typeof apiGetFresh === 'function') ? apiGetFresh : apiGet;
            this._peStaticEventsPromise = getter('/api/periph-exec/events/static')
                .then(function(res) {
                    var list = (res && res.success && Array.isArray(res.data)) ? res.data.slice() : [];
                    self._peStaticEvents = list;
                    self._peStaticEventsFetchedAt = Date.now();
                    return list;
                })
                .catch(function(err) {
                    if (Array.isArray(self._peStaticEvents) && self._peStaticEvents.length > 0) {
                        return self._peStaticEvents;
                    }
                    throw err;
                })
                .finally(function() {
                    self._peStaticEventsPromise = null;
                });
            return this._peStaticEventsPromise;
        },

        _getPeriphExecDynamicEvents(forceFresh) {
            var self = this;
            var now = Date.now();
            if (!forceFresh && Array.isArray(this._peDynamicEvents) &&
                (now - this._peDynamicEventsFetchedAt) < 15000) {
                return Promise.resolve(this._peDynamicEvents);
            }
            if (!forceFresh && this._peDynamicEventsPromise) {
                return this._peDynamicEventsPromise;
            }

            var getter = (forceFresh && typeof apiGetFresh === 'function') ? apiGetFresh : apiGet;
            this._peDynamicEventsPromise = getter('/api/periph-exec/events/dynamic')
                .then(function(res) {
                    var list = (res && res.success && Array.isArray(res.data)) ? res.data.slice() : [];
                    self._peDynamicEvents = list;
                    self._peDynamicEventsFetchedAt = Date.now();
                    return list;
                })
                .catch(function(err) {
                    if (Array.isArray(self._peDynamicEvents) && self._peDynamicEvents.length > 0) {
                        return self._peDynamicEvents;
                    }
                    throw err;
                })
                .finally(function() {
                    self._peDynamicEventsPromise = null;
                });
            return this._peDynamicEventsPromise;
        },

        _populateEventCategoriesInBlock(blockEl, eventIdToSet, compareValue) {
            const sel = blockEl.querySelector('.pe-event-category');
            if (!sel) return;
            var eventId = eventIdToSet ? String(eventIdToSet) : '';
            var isDs = eventId && eventId.indexOf('ds:') === 0;
            var isMc = eventId && eventId.indexOf('mc:') === 0;
            var isBtn = eventId && eventId.indexOf('button_') === 0;
            var self = this;
            var localSources = (typeof this._getPeriphExecLocalSensorSources === 'function') ? this._getPeriphExecLocalSensorSources() : [];
            var hasDataSources = localSources.length > 0 || ((this._peDataSources || []).length > 0);
            // 并行加载分类列表与事件列表，以便从 eventId 推导类别
            Promise.all([
                this._getPeriphExecEventCategories(false),
                (isDs || isMc || isBtn || !eventId) ? Promise.resolve([]) : this._getPeriphExecStaticEvents(false),
                (isDs || isMc || isBtn || !eventId) ? Promise.resolve([]) : this._getPeriphExecDynamicEvents(false)
            ]).then(function(results) {
                var categoryList = Array.isArray(results[0]) ? results[0] : [];
                var staticEvents = Array.isArray(results[1]) ? results[1] : [];
                var dynamicEvents = Array.isArray(results[2]) ? results[2] : [];
                var opts = '<option value="">' + '-- 选择分类 --' + '</option>';
                var seenCategories = {};
                function addCategoryOption(categoryName) {
                    if (!categoryName || seenCategories[categoryName]) return;
                    seenCategories[categoryName] = true;
                var _eventCatMap = {'WiFi':'WiFi','MQTT':'MQTT','网络':'网络','协议':'协议','系统':'系统','规则':'规则','Modbus子设备':'Modbus子设备','按键':'按键','外设执行':'外设执行','数据':'数据','数据源':'数据源','modbus-acquisition':'采集设备','local-sensor':'本地传感器','modbus-control':'控制设备'};
                    var translatedCat = _eventCatMap[categoryName] || categoryName;
                    opts += '<option value="' + categoryName + '">' + translatedCat + '</option>';
                }
                categoryList.forEach(function(cat) {
                    addCategoryOption(cat && cat.name ? cat.name : '');
                });
                if (hasDataSources) addCategoryOption('数据源');
                sel.innerHTML = opts;

                // 根据 eventId 推导 category
                var resolvedCategory = '';
                if (isDs) {
                    resolvedCategory = '数据源';
                } else if (isMc) {
                    resolvedCategory = 'Modbus子设备';
                } else if (isBtn) {
                    resolvedCategory = '按键';
                } else if (eventId) {
                    // 先查静态事件表
                    if (staticEvents.length > 0) {
                        var found = staticEvents.find(function(e) { return e.id === eventId; });
                        if (found && found.category) resolvedCategory = found.category;
                    }
                    // 再查动态事件表（规则 ID / Modbus 控制事件等）
                    if (!resolvedCategory && dynamicEvents.length > 0) {
                        var foundD = dynamicEvents.find(function(e) { return e.id === eventId; });
                        if (foundD && foundD.category) resolvedCategory = foundD.category;
                    }
                }

                if (resolvedCategory) {
                    sel.value = resolvedCategory;
                    self._populateEventSelectInBlock(blockEl, resolvedCategory, eventId, compareValue);
                } else {
                    self._populateEventSelectInBlock(blockEl, '', eventId, compareValue);
                }
            }).catch(function(err) {
                console.warn('Failed to load periph exec event categories:', err);
                sel.innerHTML = '<option value="">' + '-- 选择分类 --' + '</option>';
                self._populateEventSelectInBlock(blockEl, '', eventId, compareValue);
            });
        },

        _populateEventSelectInBlock(blockEl, categoryFilter, eventIdToSet, compareValue) {
            const sel = blockEl.querySelector('.pe-event');
            if (!sel) return;
            var localSources = (typeof this._getPeriphExecLocalSensorSources === 'function') ? this._getPeriphExecLocalSensorSources() : [];
            var modbusSources = this._peDataSources || [];
            if (categoryFilter === '数据源') {
                var dataOpts = '<option value="">' + '-- 选择事件 --' + '</option>';
                if (localSources.length > 0) {
                    dataOpts += '<optgroup label="' + escapeHtml('本地传感器') + '">';
                    localSources.forEach(function(ds) {
                        dataOpts += '<option value="ds:' + escapeHtml(ds.id) + '">' + escapeHtml(ds.label) + '</option>';
                    });
                    dataOpts += '</optgroup>';
                }
                if (modbusSources.length > 0) {
                    dataOpts += '<optgroup label="' + escapeHtml('采集设备') + '">';
                    modbusSources.forEach(function(ds) {
                        dataOpts += '<option value="ds:' + escapeHtml(ds.id) + '">' + escapeHtml(ds.label) + '</option>';
                    });
                    dataOpts += '</optgroup>';
                }
                sel.innerHTML = dataOpts;
                if (eventIdToSet) sel.value = eventIdToSet;
                var isDataSource = sel.value && sel.value.indexOf('ds:') === 0;
                this._setSectionVisible(blockEl.querySelector('.pe-event-condition-group'), isDataSource);
                this._setSectionVisible(blockEl.querySelector('.pe-event-compare-group'), isDataSource);
                var dataCtrlPanel = blockEl.querySelector('.pe-event-modbus-ctrl-panel');
                if (dataCtrlPanel) this._setSectionVisible(dataCtrlPanel, false);
                return;
            }
            // Modbus子设备类别：从协议配置中提取，不从API加载
            if (categoryFilter === 'Modbus子设备') {
                var sources = this._peDataSources || [];
                var ctrlDevices = this._modbusDevices || [];
                var opts = '<option value="">' + '-- 选择事件 --' + '</option>';
                if (sources.length > 0) {
                    var acqCatLabel = '采集设备';
                    opts += '<optgroup label="' + acqCatLabel + '">';
                    sources.forEach(function(ds) {
                        opts += '<option value="ds:' + ds.id + '">' + escapeHtml(ds.label) + '</option>';
                    });
                    opts += '</optgroup>';
                }
                if (ctrlDevices.length > 0) {
                    var ctrlCatLabel = '控制设备';
                    opts += '<optgroup label="' + ctrlCatLabel + '">';
                    ctrlDevices.forEach(function(dev, idx) {
                        if (dev.enabled === false) return;
                        var dt = dev.deviceType || 'relay';
                        if (dt === 'relay' || dt === 'pwm' || dt === 'pid' || dt === 'motor') {
                            opts += '<option value="mc:' + idx + '">' + escapeHtml(dev.name || dt) + '</option>';
                        }
                    });
                    opts += '</optgroup>';
                }
                sel.innerHTML = opts;
                if (eventIdToSet) sel.value = eventIdToSet;
                var isDs = sel.value && sel.value.indexOf('ds:') === 0;
                var isMc = sel.value && sel.value.indexOf('mc:') === 0;
                this._setSectionVisible(blockEl.querySelector('.pe-event-condition-group'), isDs);
                this._setSectionVisible(blockEl.querySelector('.pe-event-compare-group'), isDs);
                var ctrlPanel = blockEl.querySelector('.pe-event-modbus-ctrl-panel');
                if (ctrlPanel) {
                    if (isMc) {
                        var devIdx = parseInt(sel.value.substring(3));
                        this._showModbusTriggerCtrlPanel(ctrlPanel, devIdx, compareValue || '');
                    }
                    this._setSectionVisible(ctrlPanel, isMc);
                }
                return;
            }
            // 顺序加载：先静态事件，再动态事件（避免 ESP32 并发连接限制）
            Promise.all([
                this._getPeriphExecStaticEvents(false),
                this._getPeriphExecDynamicEvents(false)
            ]).then(([staticEvents, dynamicEvents]) => {
                    let allEvents = [];
                    if (Array.isArray(staticEvents)) allEvents = allEvents.concat(staticEvents);
                    if (Array.isArray(dynamicEvents)) allEvents = allEvents.concat(dynamicEvents);
                    if (categoryFilter) allEvents = allEvents.filter(e => e.category === categoryFilter);
                    const categories = {};
                    allEvents.forEach(e => {
                        if (!e || !e.id || !e.category) return;
                        if (!categories[e.category]) categories[e.category] = [];
                        categories[e.category].push(e);
                    });
                    let opts = '<option value="">' + '-- 选择事件 --' + '</option>';
                    for (const cat in categories) {
                        var _evCatMap2 = {'WiFi':'WiFi','MQTT':'MQTT','网络':'网络','协议':'协议','系统':'系统','规则':'规则','Modbus子设备':'Modbus子设备','按键':'按键','外设执行':'外设执行','数据':'数据','数据源':'数据源','modbus-acquisition':'采集设备','local-sensor':'本地传感器','modbus-control':'控制设备'};
                        const translatedCat = _evCatMap2[cat] || cat;
                        opts += '<optgroup label="' + translatedCat + '">';
                        categories[cat].forEach(e => {
                            var _evNameMap = {'wifi_connected':'WiFi连接成功','wifi_disconnected':'WiFi断开连接','wifi_conn_failed':'WiFi连接失败','mqtt_connected':'MQTT连接成功','mqtt_disconnected':'MQTT断开连接','mqtt_conn_failed':'MQTT连接失败','mqtt_enabled':'MQTT协议启用','net_mode_ap':'网络模式切换为AP','net_mode_sta':'网络模式切换为STA','modbus_rtu_enabled':'Modbus RTU启用','modbus_tcp_enabled':'Modbus TCP启用','tcp_enabled':'TCP协议启用','http_enabled':'HTTP协议启用','coap_enabled':'CoAP协议启用','ntp_synced':'NTP时间同步完成','ota_start':'OTA升级开始','ota_success':'OTA升级成功','ota_failed':'OTA升级失败','rule_exec_time':'规则脚本执行时间','rule_exec_error':'规则脚本执行错误','system_boot':'系统启动','system_ready':'系统就绪','system_error':'系统错误','factory_reset':'恢复出厂设置','button_click':'按键单击','button_double_click':'按键双击','button_long_press_2s':'按键长按2秒','button_long_press_5s':'按键长按5秒','button_long_press_10s':'按键长按10秒','button_press':'按键按下','button_release':'按键释放','periph_exec_completed':'外设执行完成','data_receive':'数据接收','data_report':'数据上报'};
                            const translatedName = e.isDynamic ? e.name : (_evNameMap[e.id] || e.name);
                            opts += '<option value="' + e.id + '">' + translatedName + '</option>';
                        });
                        opts += '</optgroup>';
                    }
                    // 未选分类时，追加Modbus子设备选项
                    if (!categoryFilter && localSources.length > 0) {
                        opts += '<optgroup label="' + escapeHtml('本地传感器') + '">';
                        localSources.forEach(function(ds) {
                            opts += '<option value="ds:' + escapeHtml(ds.id) + '">' + escapeHtml(ds.label) + '</option>';
                        });
                        opts += '</optgroup>';
                    }
                    if (!categoryFilter) {
                        var sources = this._peDataSources || [];
                        var ctrlDevices = this._modbusDevices || [];
                        if (sources.length > 0 || ctrlDevices.length > 0) {
                            var dsCatLabel = 'Modbus子设备';
                            opts += '<optgroup label="' + dsCatLabel + '">';
                            sources.forEach(function(ds) {
                                opts += '<option value="ds:' + ds.id + '">' + escapeHtml(ds.label) + '</option>';
                            });
                            ctrlDevices.forEach(function(dev, idx) {
                                if (dev.enabled === false) return;
                                var dt = dev.deviceType || 'relay';
                                if (dt === 'relay' || dt === 'pwm' || dt === 'pid' || dt === 'motor') {
                                    opts += '<option value="mc:' + idx + '">' + escapeHtml(dev.name || dt) + '</option>';
                                }
                            });
                            opts += '</optgroup>';
                        }
                    }
                    sel.innerHTML = opts;
                    if (eventIdToSet) sel.value = eventIdToSet;
                    var isDs = sel.value && sel.value.indexOf('ds:') === 0;
                    var isMc = sel.value && sel.value.indexOf('mc:') === 0;
                    this._setSectionVisible(blockEl.querySelector('.pe-event-condition-group'), isDs);
                    this._setSectionVisible(blockEl.querySelector('.pe-event-compare-group'), isDs);
                    var ctrlPanel = blockEl.querySelector('.pe-event-modbus-ctrl-panel');
                    if (ctrlPanel) {
                        if (isMc) {
                            var devIdx = parseInt(sel.value.substring(3));
                            this._showModbusTriggerCtrlPanel(ctrlPanel, devIdx, compareValue || '');
                        }
                        this._setSectionVisible(ctrlPanel, isMc);
                    }
            }).catch((err) => {
                console.warn('Failed to load periph exec events:', err);
                sel.innerHTML = '<option value="">' + '-- 选择事件 --' + '</option>';
            });
        },

        // ============ Data collection ============

        _collectPeriphExecTriggers() {
            const container = document.getElementById('periph-exec-triggers');
            if (!container) return [];
            const triggers = [];
            container.querySelectorAll('.periph-exec-config-item').forEach(item => {
                const triggerType = item.querySelector('.pe-trigger-type')?.value || '0';
                const trigger = { triggerType: parseInt(triggerType, 10) || 0 };
                if (triggerType === '0') {
                    trigger.triggerPeriphId = item.querySelector('.pe-trigger-periph')?.value || '';
                    trigger.operatorType = parseInt(item.querySelector('.pe-operator')?.value || '0', 10);
                    trigger.compareValue = item.querySelector('.pe-compare-value')?.value?.trim() || '';
                } else if (triggerType === '1') {
                    trigger.timerMode = parseInt(item.querySelector('.pe-timer-mode')?.value || '0', 10);
                    trigger.intervalSec = parseInt(item.querySelector('.pe-interval')?.value || '60', 10);
                    trigger.timePoint = item.querySelector('.pe-timepoint')?.value || '08:00';
                } else if (triggerType === '4') {
                    trigger.eventId = item.querySelector('.pe-event')?.value || '';
                    if (trigger.eventId.indexOf('ds:') === 0) {
                        trigger.operatorType = parseInt(item.querySelector('.pe-event-operator')?.value || '0', 10);
                        trigger.compareValue = item.querySelector('.pe-event-compare')?.value?.trim() || '';
                    } else if (trigger.eventId.indexOf('button_') === 0) {
                        // 按键事件：读取统一后的触发外设下拉（留空表示任意按键外设均可触发）
                        trigger.triggerPeriphId = item.querySelector('.pe-trigger-periph')?.value || '';
                    } else if (trigger.eventId.indexOf('mc:') === 0) {
                        // Modbus控制设备触发器：从控制面板读取通道/动作配置
                        var ctrlPanel = item.querySelector('.pe-event-modbus-ctrl-panel');
                        if (ctrlPanel) {
                            var devIdx = parseInt(trigger.eventId.substring(3));
                            var ctrl = {d: devIdx};
                            var chSel = ctrlPanel.querySelector('.pe-modbus-channel-select');
                            if (chSel) ctrl.c = parseInt(chSel.value || '0');
                            var actSel = ctrlPanel.querySelector('.pe-modbus-action-select');
                            if (actSel) {
                                ctrl.a = actSel.value || 'on';
                            } else {
                                var paramSel = ctrlPanel.querySelector('.pe-modbus-action-param');
                                if (paramSel) {
                                    ctrl.a = 'pid';
                                    ctrl.p = paramSel.value || 'P';
                                } else {
                                    ctrl.a = 'pwm';
                                }
                            }
                            var valInput = ctrlPanel.querySelector('.pe-modbus-action-value');
                            if (valInput) ctrl.v = parseInt(valInput.value || '0');
                            trigger.compareValue = JSON.stringify({ctrl: [ctrl]});
                        }
                    }
                } else if (triggerType === '5') {
                    trigger.intervalSec = parseInt(item.querySelector('.pe-poll-interval')?.value || '60', 10);
                    trigger.pollResponseTimeout = parseInt(item.querySelector('.pe-poll-timeout')?.value || '1000', 10);
                    trigger.pollMaxRetries = parseInt(item.querySelector('.pe-poll-retries')?.value || '2', 10);
                    trigger.pollInterPollDelay = parseInt(item.querySelector('.pe-poll-inter-delay')?.value || '100', 10);
                }
                triggers.push(trigger);
            });
            return triggers;
        },

        _collectPeriphExecActions() {
            const container = document.getElementById('periph-exec-actions');
            if (!container) return [];
            this._peSensorExtraJsonError = false;
            const actions = [];
            const isPollMode = this._isPollTriggerActive();
            container.querySelectorAll('.periph-exec-config-item').forEach(item => {
                // 轮询触发模式: 从目标外设下拉读取 modbus-task:N，自动生成 ACTION_MODBUS_POLL 动作
                if (isPollMode) {
                    var periphVal = item.querySelector('.pe-target-periph')?.value || '';
                    var jsonObj = {};
                    if (periphVal.indexOf('modbus-task:') === 0) {
                        var taskIdx = parseInt(periphVal.substring(12));
                        if (!isNaN(taskIdx)) jsonObj.poll = [taskIdx];
                    }
                    actions.push({
                        targetPeriphId: periphVal,
                        actionType: 18,
                        actionValue: JSON.stringify(jsonObj),
                        useReceivedValue: false,
                        syncDelayMs: parseInt(item.querySelector('.pe-sync-delay')?.value || '0'),
                        execMode: parseInt(item.querySelector('.pe-exec-mode')?.value || '0')
                    });
                    return;
                }
                const targetPeriphId = item.querySelector('.pe-target-periph')?.value || '';
                const isModbusTarget = targetPeriphId && targetPeriphId.indexOf('modbus:') === 0;
                const actionType = item.querySelector('.pe-action-type')?.value || '0';
                const action = {
                    targetPeriphId: targetPeriphId,
                    actionType: parseInt(actionType, 10) || 0
                };
                if (isModbusTarget) {
                    var dIdx = parseInt(targetPeriphId.substring(7));
                    var ctrl = {d: dIdx};
                    var chSel = item.querySelector('.pe-modbus-channel-select');
                    if (chSel) ctrl.c = parseInt(chSel.value || '0');
                    var actSel = item.querySelector('.pe-modbus-action-select');
                    if (actSel) {
                        ctrl.a = actSel.value || 'on';
                    } else {
                        var paramSel = item.querySelector('.pe-modbus-action-param');
                        if (paramSel) {
                            ctrl.a = 'pid';
                            ctrl.p = paramSel.value || 'P';
                        } else {
                            ctrl.a = 'pwm';
                        }
                    }
                    var valInput = item.querySelector('.pe-modbus-action-value');
                    if (valInput) ctrl.v = parseInt(valInput.value || '0');
                    action.actionType = 18;
                    action.actionValue = JSON.stringify({ctrl: [ctrl]});
                } else if (actionType === '18') {
                    // Modbus轮询采集：仅支持采集任务，控制设备通过 modbus:xxx 目标外设 + ctrlPanel 配置
                    var devSel = item.querySelector('.pe-modbus-device-select');
                    var devVal = devSel ? devSel.value : '';
                    var jsonObj = {};
                    if (devVal.indexOf('sensor-') === 0) {
                        jsonObj.poll = [parseInt(devVal.split('-')[1])];
                    }
                    action.actionValue = JSON.stringify(jsonObj);
                } else if (actionType === '19') {
                    var sensorCat = item.querySelector('.pe-sensor-category')?.value || 'analog';
                    var sensorPeriphId = item.querySelector('.pe-target-periph')?.value || '';
                    var scaleFactor = parseFloat(item.querySelector('.pe-sensor-scale')?.value);
                    if (isNaN(scaleFactor)) scaleFactor = 1;
                    var sOffset = parseFloat(item.querySelector('.pe-sensor-offset')?.value);
                    if (isNaN(sOffset)) sOffset = 0;
                    var decimals = parseInt(item.querySelector('.pe-sensor-decimals')?.value);
                    if (isNaN(decimals)) decimals = 2;
                    var sensorLabel = item.querySelector('.pe-sensor-label')?.value?.trim() || '';
                    var sUnit = item.querySelector('.pe-sensor-unit')?.value?.trim() || '';
                    var sensorData = {
                        periphId: sensorPeriphId,
                        sensorCategory: sensorCat,
                        scaleFactor: scaleFactor,
                        offset: sOffset,
                        decimalPlaces: decimals,
                        sensorLabel: sensorLabel,
                        unit: sUnit
                    };
                    sensorData.dataField = item.querySelector('.pe-sensor-datafield')?.value ||
                        (sensorCat === 'analog' ? 'voltage' : (sensorCat === 'radar' ? 'presence' : 'temperature'));
                    if (sensorCat === 'ds18b20') {
                        sensorData.deviceIndex = parseInt(item.querySelector('.pe-sensor-devindex')?.value) || 0;
                    }
                    var extraJson = item.querySelector('.pe-sensor-extra-json')?.value?.trim() || '';
                    if (extraJson) {
                        try {
                            var extraObj = JSON.parse(extraJson);
                            if (extraObj && typeof extraObj === 'object' && !Array.isArray(extraObj)) {
                                Object.keys(extraObj).forEach(function(k) { sensorData[k] = extraObj[k]; });
                            } else {
                                this._peSensorExtraJsonError = true;
                            }
                        } catch(e) {
                            this._peSensorExtraJsonError = true;
                        }
                    }
                    action.actionValue = JSON.stringify(sensorData);
                } else if (action.actionType === 27) {
                    // OLED 自定义显示：多行文本，不做 trim，保留用户显式换行
                    action.actionValue = item.querySelector('.pe-action-value-oled')?.value || '';
                } else if (action.actionType === 15) {
                    action.actionValue = item.querySelector('.pe-action-value-script')?.value || '';
                } else {
                    action.actionValue = item.querySelector('.pe-action-value')?.value?.trim() || '';
                }
                action.useReceivedValue = item.querySelector('.pe-use-received-value')?.checked || false;
                action.syncDelayMs = parseInt(item.querySelector('.pe-sync-delay')?.value || '0', 10) || 0;
                action.execMode = parseInt(item.querySelector('.pe-exec-mode')?.value || '0', 10) || 0;
                actions.push(action);
            });
            return actions;
        },

        // ============ Save (form validation + API call) ============

        savePeriphExecRule() {
            if (!this.guardDeveloperModeAction()) return;
            const errEl = document.getElementById('periph-exec-error');
            this.clearInlineError(errEl);
            const originalId = document.getElementById('periph-exec-original-id').value;
            const isEdit = originalId !== '' && originalId !== 'null' && originalId !== 'undefined';

            const ruleData = {
                name: document.getElementById('periph-exec-name').value.trim(),
                enabled: document.getElementById('periph-exec-enabled').checked,
                reportAfterExec: document.getElementById('periph-exec-report').checked
            };

            if (!ruleData.name) {
                this.showInlineError(errEl, '请输入规则名称');
                return;
            }

            const triggers = this._collectPeriphExecTriggers();
            ruleData.triggers = triggers;

            const actions = this._collectPeriphExecActions();
            if (this._peSensorExtraJsonError) {
                this.showInlineError(errEl, '传感器高级参数必须是 JSON 对象');
                return;
            }
            ruleData.actions = actions;
            for (let i = 0; i < actions.length; i++) {
                if (actions[i].actionType === 19) {
                    var sv = {};
                    try { sv = JSON.parse(actions[i].actionValue); } catch(e) {}
                    if (!sv.periphId) {
                        this.showInlineError(errEl, '请选择传感器外设');
                        return;
                    }
                } else if (actions[i].actionType === 15) {
                    var scriptValue = actions[i].actionValue || '';
                    if (!scriptValue.trim()) {
                        this.showInlineError(errEl, '脚本内容不能为空');
                        return;
                    }
                    if (scriptValue.length > 1024) {
                        this.showInlineError(errEl, '脚本超过最大长度限制(1024字节)');
                        return;
                    }
                }
            }

            if (isEdit) ruleData.id = originalId;
            const url = isEdit ? '/api/periph-exec/update' : '/api/periph-exec';
            apiPostJson(url, ruleData)
                .then(res => {
                    if (res && res.success) {
                        if (typeof this._invalidatePeriphExecRuntimeCaches === 'function') {
                            this._invalidatePeriphExecRuntimeCaches();
                        }
                        Notification.success(isEdit ? '规则更新成功' : '规则添加成功', '外设执行');
                        this.closePeriphExecModal();
                        if (this.currentPage === 'periph-exec') this.loadPeriphExecPage();
                    } else {
                        this.showInlineError(errEl, res?.error || '保存失败');
                    }
                })
                .catch(err => {
                    console.error('Save periph exec rule failed:', err);
                    if (err?.data?.error || err?.data?.message) {
                        this.showInlineError(errEl, err.data.error || err.data.message);
                        return;
                    }
                    const isNetworkError = err && (
                        err.name === 'TypeError' ||
                        (err.message && (
                            err.message.includes('Failed to fetch') ||
                            err.message.includes('fetch') ||
                            err.message.includes('network')
                        ))
                    );
                    this.showInlineError(errEl, isNetworkError ? '设备离线或不可达，请检查网络连接' : '保存失败');
                });
        }

    });
})();
