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
            const isIrEvent = showEvent && String(data.eventId) === 'ir_code_received';
            const isRfEvent = showEvent && String(data.eventId) === 'rf_code_received';
            const showPoll = triggerType === '5';
            const timerMode = String(data.timerMode ?? 0);
            const showInterval = timerMode === '0';
            const showDaily = timerMode === '1';
            const opVal = String(data.operatorType ?? 0);
            const mappedOp = (opVal === '0' || opVal === '1') ? opVal : '0';
            const showCompareValue = showPlatform && mappedOp !== '1';
            div.innerHTML = '<span class="mqtt-topic-index">' + (index + 1) + '</span>' +
                '<button type="button" class="mqtt-topic-delete">' + t('pef-delete', '删除') + '</button>' +
                '<div class="config-form-grid">' +
                    '<div class="fb-form-group"><label>' + '触发类型' + '</label>' +
                    '<select class="pe-trigger-type">' +
                        '<option value="0"' + (triggerType === '0' ? ' selected' : '') + '>' + t('pef-trigger-platform', '平台触发 (MQTT)') + '</option>' +
                        '<option value="4"' + (triggerType === '4' ? ' selected' : '') + '>' + t('pef-trigger-event', '事件触发') + '</option>' +
                        '<option value="1"' + (triggerType === '1' ? ' selected' : '') + '>' + t('pef-trigger-timer', '定时触发') + '</option>' +
                        '<option value="5"' + (triggerType === '5' ? ' selected' : '') + '>' + t('pef-trigger-poll', '轮询触发 (本地数据)') + '</option>' +
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
                    '<div class="fb-form-group pe-ir-key-group' + this._hiddenClass(isIrEvent) + '"><label>' + '红外按键' + '</label>' +
                    '<select class="pe-ir-key"><option value="">' + '-- 任意红外按键 --' + '</option></select>' +
                    '<small class="pe-help-text">' + '选择已学习的红外按键；留空表示任意红外编码均可触发' + '</small></div>' +
                    '<div class="fb-form-group pe-rf-key-group' + this._hiddenClass(isRfEvent) + '"><label>' + 'RF按键' + '</label>' +
                    '<select class="pe-rf-key"><option value="">' + '-- 任意RF按键 --' + '</option></select>' +
                    '<small class="pe-help-text">' + '选择已学习的RF按键；留空表示任意RF编码均可触发' + '</small></div>' +
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
            if (isIrEvent) this._populateIrKeySelect(div, data.compareValue || '');
            if (isRfEvent) this._populateRfKeySelect(div, data.compareValue || '');
            this._updatePeriphExecAddButtons();
        },

        _createPeriphExecActionElement(data, index) {
            const container = document.getElementById('periph-exec-actions');
            if (!container) return;
            const div = document.createElement('div');
            div.className = 'periph-exec-config-item';
            div.dataset.index = index;
            const isPollMode = this._isPollTriggerActive();
            // 仅在轮询模式且为新动作（无已有 actionType）时默认 Modbus 轮询(18)
            // 编辑已有规则时必须保留原始 actionType（如 19=传感器读取, 27=OLED显示）
            const hasExistingActionType = data.actionType !== undefined && data.actionType !== null && data.actionType !== '';
            let actionType = String((isPollMode && !hasExistingActionType) ? 18 : (data.actionType ?? 0));
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
            const showPeriphGroup = isRuleCtrlAction || (isModbusTarget || !((actionTypeInt >= 6 && actionTypeInt <= 9) || isModbusPoll || isTriggerEvent));
            const needsValue = !isModbusTarget && !isRuleCtrlAction && ((actionTypeInt >= 2 && actionTypeInt <= 5) || actionTypeInt === 10 || actionTypeInt === 11 || actionTypeInt === 12 || actionTypeInt === 24 || actionTypeInt === 25 || actionTypeInt === 28 || actionTypeInt === 29 || isOledDisplay || isScriptAction || isTriggerEvent);
            const showRecv = !isModbusTarget && !isRuleCtrlAction && this._hasSetModeTrigger() && needsValue && !isOledDisplay && !isScriptAction && !isTriggerEvent;
            const showActionType = !isModbusTarget;
            const showExecRow = true;
            const execMode = parseInt(data.execMode ?? 0);
            const sel = (v) => actionType === String(v) ? 'selected' : '';
            var sensorCfg = {sensorCategory:'analog',periphId:'',scaleFactor:1,offset:0,decimalPlaces:1,sensorLabel:'',unit:'',dataField:'',deviceIndex:0};
            if (isSensorRead && data.actionValue) { try { Object.assign(sensorCfg, JSON.parse(data.actionValue)); } catch(e) {} }
            // 根据传感器类别设置合理默认值（数字传感器不需要缩放系数）
            var digitalSensors = ['dht11','dht22','ds18b20','sht31','aht20','bh1750','bmp280','mpu6050'];
            var isDigitalSensor = digitalSensors.indexOf(String(sensorCfg.sensorCategory || '').toLowerCase()) >= 0;
            if (isDigitalSensor && (!data.actionValue || sensorCfg.scaleFactor === 0.00080586)) {
                sensorCfg.scaleFactor = 1;
                sensorCfg.offset = 0;
            }
            if (!sensorCfg.dataField) {
                var sensorCatLower = String(sensorCfg.sensorCategory || '').toLowerCase();
                var fieldMap = {analog:'voltage',radar:'presence',dht11:'temperature',dht22:'temperature',
                    ds18b20:'temperature',sht31:'temperature',aht20:'temperature',
                    ultrasonic:'distance',current:'current',voltage:'voltage',ntc:'temperature',
                    bh1750:'lux',bmp280:'pressure',mpu6050:'accel_x',rf:'rssi',
                    digital:'state',pulse:'frequency'};
                sensorCfg.dataField = fieldMap[sensorCatLower] || 'value';
            }
            if (!sensorCfg.sensorLabel) {
                var labelMap = {temperature:'温度',humidity:'湿度',distance:'距离',current:'电流',
                    voltage:'电压',presence:'存在',lux:'光照',pressure:'气压',
                    accel_x:'加速度',rssi:'信号强度',state:'状态',frequency:'频率',value:'数值'};
                sensorCfg.sensorLabel = labelMap[sensorCfg.dataField] || sensorCfg.dataField;
            }
            if (!sensorCfg.unit) {
                var unitMap = {temperature:'°C',humidity:'%RH',distance:'cm',current:'A',
                    voltage:'V',presence:'',lux:'lx',pressure:'hPa',
                    accel_x:'g',rssi:'dBm',state:'',frequency:'Hz',value:''};
                sensorCfg.unit = unitMap[sensorCfg.dataField] || '';
            }
            if (sensorCfg.decimalPlaces === undefined || sensorCfg.decimalPlaces === null) {
                sensorCfg.decimalPlaces = isDigitalSensor ? 1 : 2;
            }
            const sensorBaseKeys = {
                periphId: 1, sensorCategory: 1, scaleFactor: 1, offset: 1, decimalPlaces: 1,
                sensorLabel: 1, unit: 1, dataField: 1, deviceIndex: 1,
                bCoefficient: 1, seriesResistor: 1, nominalResistance: 1
            };
            var sensorExtra = {};
            Object.keys(sensorCfg).forEach(function(k) {
                if (!sensorBaseKeys[k]) sensorExtra[k] = sensorCfg[k];
            });
            const sensorExtraJson = Object.keys(sensorExtra).length ? JSON.stringify(sensorExtra) : '';
            const selCat = (v) => sensorCfg.sensorCategory === v ? 'selected' : '';
            const sensorFieldOptions = this._renderSensorDataFieldOptions(sensorCfg.sensorCategory, sensorCfg.dataField);
            div.innerHTML = '<span class="mqtt-topic-index">' + (index + 1) + '</span>' +
                '<button type="button" class="mqtt-topic-delete">' + t('pef-delete', '删除') + '</button>' +
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
                        '<optgroup label="' + t('pef-action-gpio', 'GPIO操作') + '">' +
                        '<option value="0" ' + sel(0) + '>' + t('pef-action-gpio-high', '设置高电平') + '</option>' +
                        '<option value="1" ' + sel(1) + '>' + t('pef-action-gpio-low', '设置低电平') + '</option>' +
                        '<option value="2" ' + sel(2) + '>' + t('pef-action-blink', '闪烁') + '</option>' +
                        '<option value="3" ' + sel(3) + '>' + t('pef-action-breathe', '呼吸灯') + '</option>' +
                        '<option value="13" ' + sel(13) + '>' + t('pef-action-toggle-high', '高电平反转') + '</option>' +
                        '<option value="14" ' + sel(14) + '>' + t('pef-action-toggle-low', '低电平反转') + '</option>' +
                        '<option value="30" ' + sel(30) + '>' + t('pef-action-toggle', '电平翻转') + '</option></optgroup>' +
                        '<optgroup label="' + t('pef-action-analog', '模拟输出') + '">' +
                        '<option value="4" ' + sel(4) + '>' + t('pef-action-pwm', '设置PWM') + '</option>' +
                        '<option value="5" ' + sel(5) + '>' + t('pef-action-dac', '设置DAC') + '</option></optgroup>' +
                        '<optgroup label="' + t('pef-action-system', '系统功能') + '">' +
                        '<option value="6" ' + sel(6) + '>' + t('pef-action-reboot', '系统重启') + '</option>' +
                        '<option value="7" ' + sel(7) + '>' + t('pef-action-factory-reset', '恢复出厂') + '</option>' +
                        '<option value="8" ' + sel(8) + '>' + t('pef-action-ntp', 'NTP同步') + '</option>' +
                        '<option value="31" ' + sel(31) + '>' + t('pef-action-enable-feature', '启用功能(webServer/mdns)') + '</option>' +
                        '<option value="32" ' + sel(32) + '>' + t('pef-action-disable-feature', '禁用功能(webServer/mdns)') + '</option></optgroup>' +
                        '<optgroup label="' + t('pef-action-advanced', '高级功能') + '">' +
                        '<option value="11" ' + sel(11) + '>' + t('pef-action-led-ctrl', '灯效控制') + '</option>' +
                        '<option value="12" ' + sel(12) + '>' + t('pef-action-motor-ctrl', '电机控制') + '</option>' +
                        '<option value="28" ' + sel(28) + '>' + t('pef-action-rf-send', '射频发送') + '</option>' +
                        '<option value="29" ' + sel(29) + '>' + t('pef-action-uart-send', '串口发送') + '</option>' +
                        '<option value="10" ' + sel(10) + '>' + t('pef-action-send-cmd', '发送指令') + '</option>' +
                        '<option value="19" ' + sel(19) + '>' + t('pef-action-sensor-read', '传感器数据读取') + '</option>' +
                        '<option value="21" ' + sel(21) + '>' + t('pef-action-trigger-event', '触发设备事件') + '</option>' +
                        '<option value="15" ' + sel(15) + '>' + t('pef-action-script', '命令脚本') + '</option></optgroup>' +
                        '<optgroup label="' + t('pef-action-rule-ctrl', '规则控制') + '">' +
                        '<option value="22" ' + sel(22) + '>' + t('pef-action-enable-rule', '启用执行规则') + '</option>' +
                        '<option value="23" ' + sel(23) + '>' + t('pef-action-disable-rule', '禁用执行规则') + '</option></optgroup>' +
                        '<optgroup label="' + t('pef-action-display', '显示屏') + '">' +
                        '<option value="24" ' + sel(24) + '>' + t('pef-action-display-num', '显示数字') + '</option>' +
                        '<option value="25" ' + sel(25) + '>' + t('pef-action-display-text', '显示文本') + '</option>' +
                        '<option value="26" ' + sel(26) + '>' + t('pef-action-display-clear', '数码管清屏') + '</option>' +
                        '<option value="27" ' + sel(27) + '>' + t('pef-action-oled', 'OLED自定义显示') + '</option></optgroup>' +
                    '</select></div>' +
                    '<div class="fb-form-group pe-target-group' + this._hiddenClass(showPeriphGroup) + '">' +
                    '<label>' + (isRuleCtrlAction ? t('pef-target-rule', '目标执行规则') : t('pef-target-periph', '执行外设')) + '</label>' +
                    '<select class="pe-target-periph"><option value="">' + (isRuleCtrlAction ? t('pef-select-rule', '-- 选择执行规则 --') : t('pef-select-periph', '-- 选择外设 --')) + '</option></select></div>' +
                    '<div class="fb-form-group pe-modbus-ctrl-panel' + this._hiddenClass(isModbusTarget) + '"></div>' +
                    '<div class="fb-form-group pe-action-value-group' + this._hiddenClass(needsValue) + '">' +
                    '<label>' + '动作参数' + '</label>' +
                    '<input type="text" class="pe-action-value' + ((isOledDisplay || isScriptAction) ? ' is-hidden' : '') + '" value="' + ((isOledDisplay || isScriptAction) ? '' : escapeHtml(data.actionValue)) + '" placeholder="' + escapeHtml(isDisplayAction ? '支持 ${periphId.field} 模板（例如 ${dht_01.temperature} / ${adc.voltage}），将从传感器缓存取最新值' : '如: PWM值、闪烁间隔ms') + '"' + (showRecv && data.useReceivedValue !== false ? ' readonly' : '') + '>' +
                    '<textarea class="pe-action-value-oled pe-script-textarea' + (isOledDisplay ? '' : ' is-hidden') + '" rows="6" maxlength="512" placeholder="' + escapeHtml('多行文本，每行一条。首行以 # 开头为居中标题。支持 ${periphId.field}（传感器缓存）和 $value（下发变量）模板。例：\n# 环境监测\n温度:${dht_01.temperature}°C\n湿度:${dht_01.humidity}%\n设备:$value') + '">' + (isOledDisplay ? escapeHtml(data.actionValue || '') : '') + '</textarea>' +
                                        '<small class="pe-oled-counter' + (isOledDisplay ? '' : ' is-hidden') + '">' + this._formatOledCounterStats(data.actionValue || '') + '</small>' +
                    '<textarea class="pe-action-value-script pe-script-textarea' + (isScriptAction ? '' : ' is-hidden') + '" rows="7" maxlength="1024" placeholder="' + escapeHtml('每行一条命令，如:\nGPIO 2 HIGH\nDELAY 500\nGPIO 2 LOW\nLOG 执行完成') + '">' + (isScriptAction ? escapeHtml(data.actionValue || '') : '') + '</textarea>' +
                    '<small class="pe-help-text">' + (actionTypeInt === 10 ? t('pef-help-send-cmd', '支持 JSON 格式或纯文本发送指令。支持 $value 占位符接收平台下发的值') : actionTypeInt === 11 ? t('pef-help-led', '选择预设灯效或输入自定义颜色，支持 $value 占位符接收平台下发的值') : actionTypeInt === 12 ? t('pef-help-motor', '输入电机指令: forward(正转)、reverse(反转)、stop(停止)、数字(步数)') : actionTypeInt === 28 ? t('pef-help-rf', '输入射频编码，如十六进制编码字符串') : actionTypeInt === 29 ? t('pef-help-uart', '输入要发送的文本，支持 $value 占位符') : (isScriptAction ? t('pef-help-script', '可用: GPIO pin HIGH/LOW, DELAY ms, PWM pin duty, DAC pin val, LOG msg, PERIPH id 动作') : (isOledDisplay ? t('pef-help-oled', '最多 6 行、可自动适配 OLED 显示。\n 换行。首行 # 开头为居中标题带分隔线。支持变量：${外设id.字段} 从传感器缓存取值，$value 取 MQTT/规则下发的原始值') : (isDisplayAction ? t('pef-help-template', '支持 ${periphId.field} 模板（例如 ${dht_01.temperature} / ${adc.voltage}），将从传感器缓存取最新值') : t('pef-help-default', '闪烁/呼吸灯填间隔ms, PWM填占空比, DAC填0-255'))))) + '</small>' +
                    '<select class="pe-neopixel-preset is-hidden">' +
                    '<option value="">' + t('pef-led-select', '-- 选择灯效 --') + '</option>' +
                    '<option value="red">red-' + t('pef-led-red', '红色') + '</option>' +
                    '<option value="green">green-' + t('pef-led-green', '绿色') + '</option>' +
                    '<option value="blue">blue-' + t('pef-led-blue', '蓝色') + '</option>' +
                    '<option value="yellow">yellow-' + t('pef-led-yellow', '黄色') + '</option>' +
                    '<option value="orange">orange-' + t('pef-led-orange', '橙色') + '</option>' +
                    '<option value="purple">purple-' + t('pef-led-purple', '紫色') + '</option>' +
                    '<option value="cyan">cyan-' + t('pef-led-cyan', '青色') + '</option>' +
                    '<option value="white">white-' + t('pef-led-white', '白色') + '</option>' +
                    '<option value="off">off-' + t('pef-led-off', '关闭') + '</option>' +
                    '<option value="rainbow">rainbow-' + t('pef-led-rainbow', '彩虹循环') + '</option>' +
                    '<option value="chase">chase-' + t('pef-led-chase', '流水灯') + '</option>' +
                    '<option value="theater_chase">theater_chase-' + t('pef-led-theater-chase', '剧院追逐') + '</option>' +
                    '<option value="strobe">strobe-' + t('pef-led-strobe', '频闪') + '</option>' +
                    '<option value="twinkle">twinkle-' + t('pef-led-twinkle', '闪烁') + '</option>' +
                    '<option value="fade">fade-' + t('pef-led-fade', '渐变') + '</option>' +
                    '<option value="breathing">breathing-' + t('pef-led-breathing', '呼吸') + '</option>' +
                    '<option value="color_wipe">color_wipe-' + t('pef-led-color-wipe', '色彩擦除') + '</option>' +
                    '<option value="fire">fire-' + t('pef-led-fire', '火焰') + '</option>' +
                    '<option value="platform_color">$value-' + t('pef-led-platform', '平台下发') + '</option>' +
                    '<option value="custom">custom-' + t('pef-led-custom', '自定义') + '</option>' +
                    '</select>' +
                    '<small class="pe-neopixel-help is-hidden">' + t('pef-led-help', '选择预设灯效，或在"自定义"中输入 JSON 参数') + '</small>' +
                    '<select class="pe-trigger-event-select is-hidden">' +
                    '<option value="">-- 选择事件 --</option>' +
                    '<optgroup label="WiFi">' +
                    '<option value="wifi_connected">WiFi连接成功</option>' +
                    '<option value="wifi_disconnected">WiFi断开连接</option>' +
                    '<option value="wifi_conn_failed">WiFi连接失败</option></optgroup>' +
                    '<optgroup label="MQTT">' +
                    '<option value="mqtt_connected">MQTT连接成功</option>' +
                    '<option value="mqtt_disconnected">MQTT断开连接</option>' +
                    '<option value="mqtt_conn_failed">MQTT连接失败</option>' +
                    '<option value="mqtt_enabled">MQTT协议启用</option></optgroup>' +
                    '<optgroup label="系统">' +
                    '<option value="system_boot">系统启动</option>' +
                    '<option value="system_ready">系统就绪</option>' +
                    '<option value="system_error">系统错误</option>' +
                    '<option value="factory_reset">恢复出厂设置</option>' +
                    '<option value="ntp_synced">NTP同步完成</option>' +
                    '<option value="ota_start">OTA升级开始</option>' +
                    '<option value="ota_success">OTA升级成功</option>' +
                    '<option value="ota_failed">OTA升级失败</option>' +
                    '<option value="break_down">设备故障</option>' +
                    '<option value="restart">设备重启</option>' +
                    '<option value="device_alarm">设备告警</option>' +
                    '<option value="low_power">低电量预警</option></optgroup>' +
                    '<optgroup label="按键">' +
                    '<option value="button_click">按键单击</option>' +
                    '<option value="button_double_click">按键双击</option>' +
                    '<option value="button_long_press_2s">按键长按2秒</option>' +
                    '<option value="button_long_press_5s">按键长按5秒</option>' +
                    '<option value="button_long_press_10s">按键长按10秒</option>' +
                    '<option value="button_press">按键按下</option>' +
                    '<option value="button_release">按键释放</option></optgroup>' +
                    '<optgroup label="数据">' +
                    '<option value="data_receive">数据接收</option>' +
                    '<option value="data_report">数据上报</option></optgroup>' +
                    '<optgroup label="外设执行">' +
                    '<option value="periph_exec_completed">外设执行完成</option></optgroup>' +
                    '<optgroup label="RFID/红外/RF">' +
                    '<option value="rfid_card_detected">RFID卡片检测</option>' +
                    '<option value="rfid_card_removed">RFID卡片移开</option>' +
                    '<option value="ir_code_received">红外编码接收</option>' +
                    '<option value="rf_code_received">RF编码接收</option></optgroup>' +
                    '</select>' +
                    '<small class="pe-trigger-event-help is-hidden">选择要模拟触发的设备事件</small>' +
                    '</div>' +
                    '<div class="fb-form-group pe-use-received-value-group' + this._hiddenClass(showRecv) + '">' +
                    '<label class="pe-checkbox-label pe-check-align"><input type="checkbox" class="pe-use-received-value"' + (showRecv && data.useReceivedValue !== false ? ' checked' : '') + '>' +
                    t('pef-use-recv-value', '勾选后动作参数将使用平台下发或触发源的值') + '</label></div>' +
                    '<div class="fb-form-group pe-poll-tasks-group pe-span-all' + this._hiddenClass(isModbusPoll && !isModbusTarget) + '">' +
                    '<label>' + '选择子设备' + '</label>' +
                    '<div class="pe-poll-tasks-list"></div>' +
                    '<small class="pe-help-text">' + '选择要执行的 Modbus 子设备' + '</small></div>' +
                    '<div class="fb-form-group pe-sensor-group pe-span-all' + this._hiddenClass(isSensorRead) + '">' +
                    '<div class="pe-sensor-config-grid">' +
                    '<div class="fb-form-group"><label>' + '传感器类别' + '</label>' +
                    '<select class="pe-sensor-category">' +
                    '<option value="analog" ' + selCat('analog') + '>' + t('pef-sensor-analog', '模拟量 (ADC)') + '</option>' +
                    '<option value="digital" ' + selCat('digital') + '>' + t('pef-sensor-digital', '数字量 (DI)') + '</option>' +
                    '<option value="pulse" ' + selCat('pulse') + '>' + t('pef-sensor-pulse', '脉冲/频率') + '</option>' +
                    '<option value="dht11" ' + selCat('dht11') + '>' + t('pef-sensor-dht11', 'DHT11 温湿度') + '</option>' +
                    '<option value="dht22" ' + selCat('dht22') + '>' + t('pef-sensor-dht22', 'DHT22 温湿度') + '</option>' +
                    '<option value="ds18b20" ' + selCat('ds18b20') + '>' + t('pef-sensor-ds18b20', 'DS18B20 温度') + '</option>' +
                    '<option value="ultrasonic" ' + selCat('ultrasonic') + '>' + t('pef-sensor-ultrasonic', 'HC-SR04 超声波') + '</option>' +
                    '<option value="radar" ' + selCat('radar') + '>' + t('pef-sensor-radar', '雷达存在检测') + '</option>' +
                    '<option value="rf" ' + selCat('rf') + '>' + t('pef-sensor-rf', '射频接收电平') + '</option>' +
                    '<option value="current" ' + selCat('current') + '>' + t('pef-sensor-current', '电流型传感器') + '</option>' +
                    '<option value="voltage" ' + selCat('voltage') + '>' + t('pef-sensor-voltage', '电压型传感器') + '</option>' +
                    '<option value="ntc" ' + selCat('ntc') + '>' + t('pef-sensor-ntc', 'NTC 热敏电阻') + '</option>' +
                    '<option value="SHT31" ' + selCat('SHT31') + '>' + t('pef-sensor-sht31', 'SHT31 温湿度') + '</option>' +
                    '<option value="AHT20" ' + selCat('AHT20') + '>' + t('pef-sensor-aht20', 'AHT20 温湿度') + '</option>' +
                    '<option value="BH1750" ' + selCat('BH1750') + '>' + t('pef-sensor-bh1750', 'BH1750 光照') + '</option>' +
                    '<option value="BMP280" ' + selCat('BMP280') + '>' + t('pef-sensor-bmp280', 'BMP280 气压') + '</option>' +
                    '<option value="MPU6050" ' + selCat('MPU6050') + '>' + t('pef-sensor-mpu6050', 'MPU6050 姿态') + '</option></select></div>' +
                    '<div class="fb-form-group pe-sensor-datafield-group"><label>' + '数据字段' + '</label>' +
                    '<select class="pe-sensor-datafield">' +
                    sensorFieldOptions + '</select></div>' +
                    '<div class="fb-form-group pe-sensor-devindex-group' + this._hiddenClass(sensorCfg.sensorCategory === 'ds18b20') + '"><label>' + '设备索引' + '</label>' +
                    '<input type="number" class="pe-sensor-devindex" min="0" max="15" value="' + sensorCfg.deviceIndex + '"></div>' +
                    '<div class="fb-form-group pe-sensor-ntc-group pe-span-all' + this._hiddenClass(sensorCfg.sensorCategory === 'ntc') + '">' +
                    '<div class="fb-form-group"><label>' + t('pef-sensor-ntc-b', 'NTC B值') + '</label>' +
                    '<input type="number" class="pe-sensor-ntc-b" step="any" value="' + (sensorCfg.bCoefficient || 3950) + '"></div>' +
                    '<div class="fb-form-group"><label>' + t('pef-sensor-ntc-series', '串联电阻(Ω)') + '</label>' +
                    '<input type="number" class="pe-sensor-ntc-series" step="any" value="' + (sensorCfg.seriesResistor || 10000) + '"></div>' +
                    '<div class="fb-form-group"><label>' + t('pef-sensor-ntc-nominal', '标称电阻(Ω)') + '</label>' +
                    '<input type="number" class="pe-sensor-ntc-nominal" step="any" value="' + (sensorCfg.nominalResistance || 10000) + '"></div>' +
                    '<small class="pe-help-text">' + t('pef-sensor-ntc-help', '电路: VCC→串联电阻→ADC引脚→NTC→GND。常用 10K/B3950') + '</small>' +
                    '</div>' +
                    '<div class="fb-form-group pe-sensor-calibration-group' + this._hiddenClass(isDigitalSensor) + '">' +
                    '<div class="fb-form-group"><label>' + '缩放系数' + '</label><input type="number" class="pe-sensor-scale" step="any" value="' + sensorCfg.scaleFactor + '"></div>' +
                    '<div class="fb-form-group"><label>' + '偏移量' + '</label><input type="number" class="pe-sensor-offset" step="any" value="' + sensorCfg.offset + '"></div>' +
                    '</div>' +
                    '<div class="fb-form-group"><label>' + '小数位数' + '</label><input type="number" class="pe-sensor-decimals" min="0" max="6" value="' + sensorCfg.decimalPlaces + '"></div>' +
                    '<div class="fb-form-group"><label>' + '数据标签' + '</label><input type="text" class="pe-sensor-label" value="' + escapeHtml(sensorCfg.sensorLabel) + '" placeholder="' + '数据标签' + '"></div>' +
                    '<div class="fb-form-group"><label>' + '单位' + '</label><input type="text" class="pe-sensor-unit" value="' + escapeHtml(sensorCfg.unit) + '" maxlength="8" placeholder="°C, %, V..."></div>' +
                    '<div class="fb-form-group pe-sensor-advanced-group pe-span-all' + this._hiddenClass(isDigitalSensor) + '"><label>' + '高级参数(JSON)' + '</label>' +
                    '<textarea class="pe-sensor-extra-json" rows="2" maxlength="512" placeholder="' + escapeHtml(isDigitalSensor ? '' : '{"driverParams":{"addr":"0x44","sda":21,"scl":22}}') + '">' + escapeHtml(sensorExtraJson) + '</textarea>' +
                    '<small class="pe-help-text">' + (isDigitalSensor ? '数字传感器（DHT/DS18B20等）无需高级参数' : '可填写分压比、电流零点、I2C 地址等高级参数；留空则使用默认值。') + '</small></div>' +
                    '</div></div>' +
                    '</div>';
            container.appendChild(div);
            // OLED 计数器：实时显示字符数/行数
            var oledTa = div.querySelector('.pe-action-value-oled');
            var oledCnt = div.querySelector('.pe-oled-counter');
            if (oledTa && oledCnt) {
                oledTa.addEventListener('input', function() {
                    var v = this.value;
                    var lines = v ? v.split('\n').length : 0;
                    oledCnt.textContent = t('pef-oled-input', '已输入') + ' ' + v.length + '/512 ' + t('pef-oled-chars', '字符，') + lines + '/6 ' + t('pef-oled-lines', '行');
                    if (lines > 6) oledCnt.style.color = '#e74c3c';
                    else oledCnt.style.color = '';
                });
            }
            if (!isModbusPoll && isSensorRead) {
                this._populateSensorPeriphSelect(div, sensorCfg.sensorCategory || 'analog', sensorCfg.periphId || data.targetPeriphId || '');
            } else {
                var initActType = parseInt(data.actionType);
                // 仅 Modbus 轮询动作(18)限制为 Modbus 采集任务；传感器读取(19)、OLED显示(27)、GPIO 等本地动作即使在轮询触发模式下也显示全部外设
                var effectivePollOnly = isPollMode && isModbusPoll;
                this._populatePeriphSelect(div.querySelector('.pe-target-periph'), data.targetPeriphId || '', effectivePollOnly, initActType === 21, (initActType === 22 || initActType === 23));
            }
            if (isModbusPoll && !isModbusTarget) this._populateModbusDevicePanel(div.querySelector('.pe-poll-tasks-list'), data.actionValue || '');
            if (isModbusTarget) this._showModbusCtrlPanel(div.querySelector('.pe-modbus-ctrl-panel'), data.targetPeriphId, data.actionValue || '');
            // 触发设备事件(actionType=21): 初始化事件下拉框并恢复可见
            // 模板默认带 is-hidden，重建(切换触发类型)/编辑已有规则时必须移除，否则事件选择框保持隐藏
            if (isTriggerEvent) {
                var evtSel = div.querySelector('.pe-trigger-event-select');
                if (evtSel) {
                    evtSel.classList.remove('is-hidden');
                    if (data.actionValue) evtSel.value = data.actionValue;
                }
                var evtHelp = div.querySelector('.pe-trigger-event-help');
                if (evtHelp) evtHelp.classList.remove('is-hidden');
            }
            // NeoPixel 预设灯效：初始化下拉框并同步显示状态
            if ((data.actionType ?? 0) == 11) {
                this._initNeoPixelPresetFromData(div, data.actionValue, 11);
            } else if (this._isNeoPixelTarget(data.targetPeriphId) && (data.actionType ?? 0) == 10) {
                // 自动迁移：NeoPixel 目标的 actionType=10（旧格式）→ 11（灯效控制）
                var actSel = div.querySelector('.pe-action-type');
                if (actSel) actSel.value = '11';
                // 同步 UI 状态（帮助文本、显隐等）
                this.onPeriphExecActionTypeChangeInBlock('11', Array.from(container.children).indexOf(div));
                this._initNeoPixelPresetFromData(div, data.actionValue, 11);
            } else if (this._isSetPwmTarget(data.targetPeriphId) && (data.actionType ?? 0) == 10) {
                // 自动迁移：PWM 输出目标的 actionType=10（发送指令，旧格式）→ 4（设置PWM）
                // 覆盖两类目标：type=17 原生 PWM 输出（本项目无源蜂鸣器）、type=12+passive 无源蜂鸣器接数字口
                // 均由 PWM 占空比驱动（HIGH=50%占空比发声），将命令格式迁移为占空比
                var actSelBz = div.querySelector('.pe-action-type');
                if (actSelBz) actSelBz.value = '4';
                var valInputBz = div.querySelector('.pe-action-value');
                if (valInputBz) {
                    var bzVal = (valInputBz.value || '').trim().toLowerCase();
                    if (bzVal === 'on' || bzVal === 'high' || bzVal === '1' || bzVal === 'beep' || bzVal === 'toggle') valInputBz.value = '128';
                    else if (bzVal === 'off' || bzVal === 'low' || bzVal === '0' || bzVal === 'stop') valInputBz.value = '0';
                    // 数字占空比及其他值保持不变
                }
                this.onPeriphExecActionTypeChangeInBlock('4', Array.from(container.children).indexOf(div));
            } else if (this._isRfModuleTarget(data.targetPeriphId) && (data.actionType ?? 0) == 10) {
                // 自动迁移：RF 模块目标的 actionType=10（发送指令，旧格式）→ 28（射频发送）
                // 两者均由 executeCallPeripheralAction 执行，actionValue（RF 编码）保持不变
                var actSelRf = div.querySelector('.pe-action-type');
                if (actSelRf) actSelRf.value = '28';
                this.onPeriphExecActionTypeChangeInBlock('28', Array.from(container.children).indexOf(div));
            }
            this._updateNeoPixelPresetVisibility(div);
            this._updatePeriphExecAddButtons();
        },

        _formatOledCounterStats(text) {
            var len = (text || '').length;
            var lines = text ? text.split('\n').length : 0;
            return t('pef-oled-input', '已输入') + ' ' + len + '/512 ' + t('pef-oled-chars', '字符，') + lines + '/6 ' + t('pef-oled-lines', '行');
        },

        _getPeriphExecBlockLimit(kind) {
            return kind === 'trigger' ? 3 : 4;
        },

        _updatePeriphExecAddButtons() {
            var triggerContainer = document.getElementById('periph-exec-triggers');
            var actionContainer = document.getElementById('periph-exec-actions');
            var triggerBtn = document.querySelector('[data-action="addPeriphExecTrigger"]');
            var actionBtn = document.querySelector('[data-action="addPeriphExecAction"]');
            var triggerLimit = this._getPeriphExecBlockLimit('trigger');
            var actionLimit = this._getPeriphExecBlockLimit('action');
            if (triggerBtn && triggerContainer) {
                var triggerFull = triggerContainer.children.length >= triggerLimit;
                triggerBtn.disabled = triggerFull;
                triggerBtn.title = triggerFull ? (t('pef-max-triggers', '每条规则最多') + ' ' + triggerLimit + ' ' + t('pef-max-triggers-suffix', '个触发器')) : '';
            }
            if (actionBtn && actionContainer) {
                var actionFull = actionContainer.children.length >= actionLimit;
                actionBtn.disabled = actionFull;
                actionBtn.title = actionFull ? (t('pef-max-actions', '每条规则最多') + ' ' + actionLimit + ' ' + t('pef-max-actions-suffix', '个动作')) : '';
            }
        },

        addPeriphExecTrigger() {
            const container = document.getElementById('periph-exec-triggers');
            if (!container) return;
            var limit = this._getPeriphExecBlockLimit('trigger');
            if (container.children.length >= limit) {
                Notification.warning(t('pef-max-triggers', '每条规则最多') + ' ' + limit + ' ' + t('pef-max-triggers-suffix', '个触发器'), t('pef-title', '外设执行'));
                this._updatePeriphExecAddButtons();
                return;
            }
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
            this._updatePeriphExecAddButtons();
            this._refreshPeriphExecRiskNotice({ allowFetch: false });
        },

        addPeriphExecAction() {
            const container = document.getElementById('periph-exec-actions');
            if (!container) return;
            var limit = this._getPeriphExecBlockLimit('action');
            if (container.children.length >= limit) {
                Notification.warning(t('pef-max-actions', '每条规则最多') + ' ' + limit + ' ' + t('pef-max-actions-suffix', '个动作'), t('pef-title', '外设执行'));
                this._updatePeriphExecAddButtons();
                return;
            }
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
            this._updatePeriphExecAddButtons();
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
                const needsValue = (actionType >= 2 && actionType <= 5) || actionType === 10 || actionType === 11 || actionType === 12 || actionType === 24 || actionType === 25 || actionType === 28 || actionType === 29;
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
            var isIrEvt = val === 'ir_code_received';
            var isRfEvt = val === 'rf_code_received';
            this._setSectionVisible(block.querySelector('.pe-event-condition-group'), isDataSource);
            this._setSectionVisible(block.querySelector('.pe-event-compare-group'), isDataSource);
            // 红外编码接收事件：展示已学习按键下拉
            var irKeyGroup = block.querySelector('.pe-ir-key-group');
            if (irKeyGroup) {
                this._setSectionVisible(irKeyGroup, isIrEvt);
                if (isIrEvt) this._populateIrKeySelect(block, '');
            }
            // RF编码接收事件：展示已学习RF按键下拉
            var rfKeyGroup = block.querySelector('.pe-rf-key-group');
            if (rfKeyGroup) {
                this._setSectionVisible(rfKeyGroup, isRfEvt);
                if (isRfEvt) this._populateRfKeySelect(block, '');
            }
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
            const actionType = parseInt(val);
            const targetGroup = block.querySelector('.pe-target-group');
            const valueGroup = block.querySelector('.pe-action-value-group');
            const pollTasksGroup = block.querySelector('.pe-poll-tasks-group');
            const isModbusPoll = actionType === 18;
            const isSensorRead = actionType === 19;
            const isTriggerEvent = actionType === 21;
            const isRuleCtrlAction = (actionType === 22 || actionType === 23);
            // 检查当前目标是否为 Modbus 设备（与创建路径保持一致）
            const isModbusTarget = (block.querySelector('.pe-target-periph')?.value || '').indexOf('modbus:') === 0;
            const showTargetGroup = isRuleCtrlAction || !((actionType >= 6 && actionType <= 9) || isModbusPoll || isTriggerEvent);
            this._setSectionVisible(targetGroup, showTargetGroup);
            // 切换目标分组的 label 和空选项提示（规则 vs 外设）
            if (targetGroup) {
                var labelEl = targetGroup.querySelector('label');
                if (labelEl) labelEl.textContent = isRuleCtrlAction ? t('pef-target-rule', '目标执行规则') : t('pef-target-periph', '执行外设');
            }
            const needsValue = !isModbusTarget && !isRuleCtrlAction && ((actionType >= 2 && actionType <= 5) || actionType === 10 || actionType === 11 || actionType === 12 || actionType === 15 || actionType === 24 || actionType === 25 || actionType === 27 || actionType === 28 || actionType === 29);
            // 触发设备事件(21)也需要显示 value-group（内含事件下拉框）
            this._setSectionVisible(valueGroup, needsValue || isTriggerEvent);
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
                // OLED 计数器同步显隐
                const oledCounter = valueGroup.querySelector('.pe-oled-counter');
                if (oledCounter) {
                    if (isOled) { oledCounter.classList.remove('is-hidden'); if (multiEl) multiEl.dispatchEvent(new Event('input')); }
                    else oledCounter.classList.add('is-hidden');
                }
                if (scriptEl) {
                    if (isScript) scriptEl.classList.remove('is-hidden');
                    else scriptEl.classList.add('is-hidden');
                }
                // 提示文案同步切换
                const helpEl = valueGroup.querySelector('.pe-help-text');
                if (helpEl) {
                    if (actionType === 10) helpEl.textContent = t('pef-help-send-cmd', '支持 JSON 格式或纯文本发送指令。支持 $value 占位符接收平台下发的值');
                    else if (actionType === 11) helpEl.textContent = t('pef-help-led', '选择预设灯效或输入自定义颜色，支持 $value 占位符接收平台下发的值');
                    else if (actionType === 12) helpEl.textContent = t('pef-help-motor', '输入电机指令: forward(正转)、reverse(反转)、stop(停止)、数字(步数)');
                    else if (actionType === 28) helpEl.textContent = t('pef-help-rf', '输入射频编码，如十六进制编码字符串');
                    else if (actionType === 29) helpEl.textContent = t('pef-help-uart', '输入要发送的文本，支持 $value 占位符');
                    else if (isScript) helpEl.textContent = t('pef-help-script', '可用: GPIO pin HIGH/LOW, DELAY ms, PWM pin duty, DAC pin val, LOG msg, PERIPH id 动作');
                    else if (isOled) helpEl.textContent = t('pef-help-oled', '最多 6 行、可自动适配 OLED 显示。\n 换行。首行 # 开头为居中标题带分隔线。支持变量：${外设id.字段} 从传感器缓存取值，$value 取 MQTT/规则下发的原始值');
                    else if (actionType === 24 || actionType === 25 || actionType === 26) helpEl.textContent = t('pef-help-template', '支持 ${periphId.field} 模板（例如 ${dht_01.temperature} / ${adc.voltage}），将从传感器缓存取最新值');
                    else helpEl.textContent = t('pef-help-default', '闪烁/呼吸灯填间隔ms, PWM填占空比, DAC填0-255');
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
            // 触发设备事件(actionType=21): 显示/隐藏事件选择下拉框
            var triggerEventSelect = block.querySelector('.pe-trigger-event-select');
            var triggerEventHelp = block.querySelector('.pe-trigger-event-help');
            if (triggerEventSelect) {
                this._setSectionVisible(triggerEventSelect, isTriggerEvent);
                this._setSectionVisible(triggerEventHelp, isTriggerEvent);
                if (isTriggerEvent && valueGroup) {
                    var singleEl21 = valueGroup.querySelector('.pe-action-value');
                    var helpTextEl21 = valueGroup.querySelector('.pe-help-text');
                    if (singleEl21) singleEl21.classList.add('is-hidden');
                    if (helpTextEl21) helpTextEl21.classList.add('is-hidden');
                }
            }
            // 切换动作类型时，隐藏 Modbus 控制面板（ctrlPanel 仅在选择 modbus:xxx 目标时由 _onTargetPeriphChange 管理）
            const ctrlPanel = block.querySelector('.pe-modbus-ctrl-panel');
            this._setSectionVisible(ctrlPanel, false);
            if (targetGroup && !targetGroup.classList.contains('is-hidden')) {
                var peSelect = block.querySelector('.pe-target-periph');
                // 仅 Modbus 轮询动作(18)限制为 Modbus 采集任务；其他本地动作显示全部外设
                var effectivePollOnly = this._isPollTriggerActive() && isModbusPoll;
                this._populatePeriphSelect(peSelect, peSelect ? peSelect.value : '', effectivePollOnly, isTriggerEvent, isRuleCtrlAction);
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
            // NeoPixel 预设灯效：当 actionType=10 且目标是 NeoPixel 时显示下拉框
            this._updateNeoPixelPresetVisibility(block);
            // 切换动作类型时，重新映射预设下拉框，确保旧 actionValue 正确迁移到新格式
            if (actionType === 11 || actionType === 10) {
                var oldVal = block.querySelector('.pe-action-value')?.value || '';
                if (oldVal) this._initNeoPixelPresetFromData(block, oldVal, actionType);
                // 重新同步显隐状态（_initNeoPixelPresetFromData 只设值，不控制显隐）
                var presetSel = block.querySelector('.pe-neopixel-preset');
                if (presetSel) this._onNeoPixelPresetChange(presetSel, index);
            }
        },

        // ============ NeoPixel 预设灯效 ============

        _isNeoPixelTarget(periphId) {
            if (!periphId) return false;
            return (this._pePeripherals || []).some(function(p) {
                return p.id === periphId && p.type === 45;
            });
        },

        // 应使用"设置PWM"(4) 动作的目标外设（动作类型应为"设置PWM"而非通用"发送指令"(10)）：
        //   - GPIO_PWM_OUTPUT(type=17)：原生 PWM 输出（本项目无源蜂鸣器即此类型，pwmFrequency=2700Hz 发声）
        //   - GPIO数字输出(type=12) + passive 标志：无源蜂鸣器接数字口、由 PWM 占空比驱动
        _isSetPwmTarget(periphId) {
            if (!periphId) return false;
            return (this._pePeripherals || []).some(function(p) {
                return p.id === periphId &&
                    (p.type === 17 || (p.type === 12 && p.passive === true));
            });
        },

        // 433MHz RF 模块：type=48 (RF_MODULE)
        // RF 发送动作应显示为"射频发送"(28) 而非通用"发送指令"(10)
        _isRfModuleTarget(periphId) {
            if (!periphId) return false;
            return (this._pePeripherals || []).some(function(p) {
                return p.id === periphId && p.type === 48;
            });
        },

        _updateNeoPixelPresetVisibility(block) {
            var dropdown = block.querySelector('.pe-neopixel-preset');
            var helpEl = block.querySelector('.pe-neopixel-help');
            var singleEl = block.querySelector('.pe-action-value');
            var helpTextEl = block.querySelector('.pe-help-text');
            if (!dropdown) return;
            var actionType = parseInt(block.querySelector('.pe-action-type')?.value || '0');
            var targetId = block.querySelector('.pe-target-periph')?.value || '';
            // 灯效控制(11) 始终显示预设下拉框；发送指令(10) + NeoPixel目标 向后兼容
            var showPreset = actionType === 11 || (actionType === 10 && this._isNeoPixelTarget(targetId));
            this._setSectionVisible(dropdown, showPreset);
            this._setSectionVisible(helpEl, showPreset);
            if (showPreset) {
                var presetVal = dropdown.value;
                // 输入框始终可见，但选择预设时禁用，选择 custom 时启用
                if (singleEl) {
                    singleEl.classList.remove('is-hidden');
                    if (presetVal && presetVal !== 'custom' && presetVal !== '') {
                        singleEl.disabled = true;
                        singleEl.value = presetVal;
                    } else {
                        singleEl.disabled = false;
                    }
                }
                if (helpTextEl) helpTextEl.classList.add('is-hidden');
            } else {
                // 触发设备事件(21): 隐藏输入框和帮助文本，只显示事件下拉框
                var curActionType = parseInt(block.querySelector('.pe-action-type')?.value || '0');
                if (curActionType === 21) {
                    if (singleEl) singleEl.classList.add('is-hidden');
                    if (helpTextEl) helpTextEl.classList.add('is-hidden');
                } else if (curActionType === 27 || curActionType === 15) {
                    // OLED自定义显示(27)/命令脚本(15): 单行输入框保持隐藏（由 textarea 接管），帮助文本可见
                    if (singleEl) singleEl.classList.add('is-hidden');
                    if (helpTextEl) helpTextEl.classList.remove('is-hidden');
                } else {
                    if (singleEl) singleEl.classList.remove('is-hidden');
                    if (helpTextEl) helpTextEl.classList.remove('is-hidden');
                }
            }
        },

        _onNeoPixelPresetChange(selectEl, index) {
            var block = this._getPeriphExecBlock('periph-exec-actions', index);
            if (!block) return;
            var presetVal = selectEl.value;
            var singleEl = block.querySelector('.pe-action-value');
            var helpTextEl = block.querySelector('.pe-help-text');
            // 输入框始终可见，选择预设时禁用并显示预设值，选择 custom 时启用编辑
            if (presetVal === 'custom' || presetVal === '') {
                if (singleEl) {
                    singleEl.classList.remove('is-hidden');
                    singleEl.disabled = false;
                    singleEl.value = '';
                }
                if (helpTextEl) helpTextEl.classList.remove('is-hidden');
            } else {
                if (singleEl) {
                    singleEl.classList.remove('is-hidden');
                    singleEl.disabled = true;
                    singleEl.value = presetVal;
                }
                if (helpTextEl) helpTextEl.classList.add('is-hidden');
            }
        },

        _initNeoPixelPresetFromData(block, actionValue, actionType) {
            var dropdown = block.querySelector('.pe-neopixel-preset');
            if (!dropdown) return;
            // 统一的简单文本映射（供 actionType 11 和 10 的纯文本向后兼容）
            var simpleMap = {
                'red': 'red', 'orange': 'orange', 'yellow': 'yellow',
                'green': 'green', 'cyan': 'cyan', 'blue': 'blue',
                'purple': 'purple', 'white': 'white',
                'off': 'off', 'clear': 'off', 'black': 'off',
                'rainbow': 'rainbow', 'cycle': 'rainbow', 'next': 'rainbow',
                'chase': 'chase', 'theater_chase': 'theater_chase',
                'strobe': 'strobe', 'twinkle': 'twinkle',
                'fade': 'fade', 'breathing': 'breathing',
                'color_wipe': 'color_wipe', 'fire': 'fire',
                '$value': 'platform_color'
            };
            // actionType 11 (灯效控制): actionValue 为简单文本 ("red", "rainbow", "$value" 等)
            if (actionType === 11) {
                if (!actionValue) return;
                var preset = simpleMap[actionValue.toLowerCase()];
                if (preset) {
                    dropdown.value = preset;
                } else {
                    dropdown.value = 'custom';
                }
                return;
            }
            // actionType 10 (发送指令): 优先解析 JSON，回退到纯文本（向后兼容动画预设）
            if (!actionValue) return;
            if (actionValue.startsWith('{')) {
                try {
                    var parsed = JSON.parse(actionValue);
                    var action = parsed.action || '';
                    var color = parsed.color || parsed.value || '';
                    var presetMap = {
                        'color:red': 'red', 'color:orange': 'orange', 'color:yellow': 'yellow',
                        'color:green': 'green', 'color:cyan': 'cyan', 'color:blue': 'blue',
                        'color:purple': 'purple', 'color:white': 'white',
                        'off': 'off', 'clear': 'off', 'black': 'off',
                        'rainbow': 'rainbow', 'cycle': 'rainbow', 'next': 'rainbow'
                    };
                    var key = (action === 'off' || action === 'clear' || action === 'black')
                        ? action : (action + ':' + color);
                    var preset = presetMap[key];
                    if (color === '$value') preset = 'platform_color';
                    if (preset) dropdown.value = preset;
                    return;
                } catch(e) {
                    // JSON 解析失败，回退到纯文本匹配
                }
            }
            // actionType 10 纯文本回退：兼容 actionType=11 保存的动画预设名
            var textPreset = simpleMap[actionValue.toLowerCase()];
            if (textPreset) dropdown.value = textPreset;
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
                            var _evNameMap = {'wifi_connected':'WiFi连接成功','wifi_disconnected':'WiFi断开连接','wifi_conn_failed':'WiFi连接失败','mqtt_connected':'MQTT连接成功','mqtt_disconnected':'MQTT断开连接','mqtt_conn_failed':'MQTT连接失败','mqtt_enabled':'MQTT协议启用','net_mode_ap':'网络模式切换为AP','net_mode_sta':'网络模式切换为STA','modbus_rtu_enabled':'Modbus RTU启用','tcp_enabled':'TCP协议启用','http_enabled':'HTTP协议启用','coap_enabled':'CoAP协议启用','ntp_synced':'NTP时间同步完成','ota_start':'OTA升级开始','ota_success':'OTA升级成功','ota_failed':'OTA升级失败','rule_exec_time':'规则脚本执行时间','rule_exec_error':'规则脚本执行错误','system_boot':'系统启动','system_ready':'系统就绪','system_error':'系统错误','factory_reset':'恢复出厂设置','button_click':'按键单击','button_double_click':'按键双击','button_long_press_2s':'按键长按2秒','button_long_press_5s':'按键长按5秒','button_long_press_10s':'按键长按10秒','button_press':'按键按下','button_release':'按键释放','periph_exec_completed':'外设执行完成','data_receive':'数据接收','data_report':'数据上报'};
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
                    var isIr = sel.value === 'ir_code_received';
                    var isRf = sel.value === 'rf_code_received';
                    this._setSectionVisible(blockEl.querySelector('.pe-event-condition-group'), isDs);
                    this._setSectionVisible(blockEl.querySelector('.pe-event-compare-group'), isDs);
                    // 红外编码接收事件：回填/展示已学习按键下拉
                    var irKeyGroup = blockEl.querySelector('.pe-ir-key-group');
                    if (irKeyGroup) {
                        this._setSectionVisible(irKeyGroup, isIr);
                        if (isIr) this._populateIrKeySelect(blockEl, compareValue || '');
                    }
                    // RF编码接收事件：回填/展示已学习RF按键下拉
                    var rfKeyGroup = blockEl.querySelector('.pe-rf-key-group');
                    if (rfKeyGroup) {
                        this._setSectionVisible(rfKeyGroup, isRf);
                        if (isRf) this._populateRfKeySelect(blockEl, compareValue || '');
                    }
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

        // 填充红外按键下拉（来自 /api/ir/keys），选中值形如 "key:名称"
        _populateIrKeySelect(blockEl, selectedCompareValue) {
            const sel = blockEl.querySelector('.pe-ir-key');
            if (!sel) return;
            var want = selectedCompareValue || '';
            var getter = (typeof apiGetFresh === 'function') ? apiGetFresh : apiGet;
            getter('/api/ir/keys')
                .then(function(res) {
                    var keys = (res && res.success && res.data && res.data.keys) ? res.data.keys : [];
                    var opts = '<option value="">' + '-- 任意红外按键 --' + '</option>';
                    keys.forEach(function(k) {
                        if (!k || !k.name) return;
                        var v = 'key:' + k.name;
                        var label = k.name + (k.code ? (' (' + k.code + ')') : '');
                        opts += '<option value="' + escapeHtml(v) + '">' + escapeHtml(label) + '</option>';
                    });
                    sel.innerHTML = opts;
                    if (want) {
                        sel.value = want;
                        // 回填值未在列表中（如原始编码或已删除按键），补充自定义选项以保留
                        if (sel.value !== want) {
                            var extra = document.createElement('option');
                            extra.value = want;
                            extra.textContent = want;
                            sel.appendChild(extra);
                            sel.value = want;
                        }
                    }
                })
                .catch(function() {
                    // 加载失败时保留已选值，避免丢失配置
                    if (want) {
                        sel.innerHTML = '<option value="">' + '-- 任意红外按键 --' + '</option>' +
                            '<option value="' + escapeHtml(want) + '" selected>' + escapeHtml(want) + '</option>';
                    }
                });
        },

        // 填充RF按键下拉（来自 /api/rf/keys），选中值形如 "key:名称"
        _populateRfKeySelect(blockEl, selectedCompareValue) {
            const sel = blockEl.querySelector('.pe-rf-key');
            if (!sel) return;
            var want = selectedCompareValue || '';
            var getter = (typeof apiGetFresh === 'function') ? apiGetFresh : apiGet;
            getter('/api/rf/keys')
                .then(function(res) {
                    var keys = (res && res.success && res.data && res.data.keys) ? res.data.keys : [];
                    var opts = '<option value="">' + '-- 任意RF按键 --' + '</option>';
                    keys.forEach(function(k) {
                        if (!k || !k.name) return;
                        var v = 'key:' + k.name;
                        var label = k.name + (k.code ? (' (' + k.code + ')') : '');
                        opts += '<option value="' + escapeHtml(v) + '">' + escapeHtml(label) + '</option>';
                    });
                    sel.innerHTML = opts;
                    if (want) {
                        sel.value = want;
                        if (sel.value !== want) {
                            var extra = document.createElement('option');
                            extra.value = want;
                            extra.textContent = want;
                            sel.appendChild(extra);
                            sel.value = want;
                        }
                    }
                })
                .catch(function() {
                    if (want) {
                        sel.innerHTML = '<option value="">' + '-- 任意RF按键 --' + '</option>' +
                            '<option value="' + escapeHtml(want) + '" selected>' + escapeHtml(want) + '</option>';
                    }
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
                    } else if (trigger.eventId === 'ir_code_received') {
                        // 红外编码接收：按键下拉值形如 "key:名称"，留空表示任意编码
                        trigger.compareValue = item.querySelector('.pe-ir-key')?.value?.trim() || '';
                    } else if (trigger.eventId === 'rf_code_received') {
                        // RF编码接收：按键下拉值形如 "key:名称"，留空表示任意编码
                        trigger.compareValue = item.querySelector('.pe-rf-key')?.value?.trim() || '';
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
                // 轮询触发模式下仅 Modbus 轮询(18)或默认动作强制为 actionType=18
                // 传感器读取(19)、OLED(27)等保留其原始 actionType 和采集逻辑
                var curActType = parseInt(item.querySelector('.pe-action-type')?.value || '0');
                if (isPollMode && (curActType === 18 || curActType === 0)) {
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
                // 非 Modbus 轮询动作：继续正常采集流程
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
                    // 数字传感器无需缩放系数，强制重置避免隐藏字段残留值
                    var _digSensors = ['dht11','dht22','ds18b20','sht31','aht20','bh1750','bmp280','mpu6050'];
                    if (_digSensors.indexOf(String(sensorCat).toLowerCase()) >= 0) {
                        scaleFactor = 1;
                        sOffset = 0;
                    }
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
                    if (String(sensorCat).toLowerCase() === 'ntc') {
                        // NTC 校准参数：顶层字段直通执行器（B 参数法测温）
                        var ntcB = parseFloat(item.querySelector('.pe-sensor-ntc-b')?.value);
                        var ntcSeries = parseFloat(item.querySelector('.pe-sensor-ntc-series')?.value);
                        var ntcNominal = parseFloat(item.querySelector('.pe-sensor-ntc-nominal')?.value);
                        sensorData.bCoefficient = isNaN(ntcB) ? 3950 : ntcB;
                        sensorData.seriesResistor = isNaN(ntcSeries) ? 10000 : ntcSeries;
                        sensorData.nominalResistance = isNaN(ntcNominal) ? 10000 : ntcNominal;
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
                } else if (action.actionType === 21) {
                    // 触发设备事件: 从事件下拉框获取事件 ID
                    var evtSelect = item.querySelector('.pe-trigger-event-select');
                    action.actionValue = evtSelect ? (evtSelect.value || '') : '';
                } else if (action.actionType === 11) {
                    // 灯效控制: 从预设下拉框生成简单文本值
                    var neoPreset = item.querySelector('.pe-neopixel-preset');
                    if (neoPreset && !neoPreset.classList.contains('is-hidden') && neoPreset.value && neoPreset.value !== 'custom') {
                        var pv = neoPreset.value;
                        if (pv === 'platform_color') {
                            action.actionValue = '$value';
                        } else {
                            action.actionValue = pv; // "red", "green", "off", "rainbow", "chase" 等
                        }
                    } else {
                        action.actionValue = item.querySelector('.pe-action-value')?.value?.trim() || '';
                    }
                } else {
                    // NeoPixel 预设灯效：从下拉框生成 JSON
                    var neoPreset = item.querySelector('.pe-neopixel-preset');
                    if (neoPreset && !neoPreset.classList.contains('is-hidden') && neoPreset.value && neoPreset.value !== 'custom') {
                        var pv = neoPreset.value;
                        if (pv === 'off') {
                            action.actionValue = JSON.stringify({action: 'off'});
                        } else if (pv === 'rainbow') {
                            action.actionValue = JSON.stringify({action: 'rainbow'});
                        } else if (pv === 'platform_color') {
                            action.actionValue = JSON.stringify({action: 'color', color: '$value'});
                        } else {
                            action.actionValue = JSON.stringify({action: 'color', color: pv});
                        }
                    } else {
                        action.actionValue = item.querySelector('.pe-action-value')?.value?.trim() || '';
                    }
                }
                action.useReceivedValue = item.querySelector('.pe-use-received-value')?.checked || false;
                action.syncDelayMs = parseInt(item.querySelector('.pe-sync-delay')?.value || '0', 10) || 0;
                action.execMode = parseInt(item.querySelector('.pe-exec-mode')?.value || '0', 10) || 0;
                actions.push(action);
            });
            return actions;
        },

        // ============ Save (form validation + API call) ============

        _highlightFieldError(field, msg, errEl) {
            if (!field) return;
            field.style.borderColor = '#e74c3c';
            field.style.boxShadow = '0 0 0 2px rgba(231,76,60,0.2)';
            this.showInlineError(errEl, msg);
            field.addEventListener('change', function _clr() {
                this.style.borderColor = '';
                this.style.boxShadow = '';
                this.removeEventListener('change', _clr);
            }, {once: false});
        },

        _clearAllFieldErrors() {
            document.querySelectorAll('.periph-exec-config-item .pe-field-error').forEach(function(el) {
                el.style.borderColor = '';
                el.style.boxShadow = '';
                el.classList.remove('pe-field-error');
            });
        },

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
                this.showInlineError(errEl, t('pef-err-name-required', '请输入规则名称'));
                return;
            }

            const triggers = this._collectPeriphExecTriggers();
            if (triggers.length > this._getPeriphExecBlockLimit('trigger')) {
                this.showInlineError(errEl, t('pef-max-triggers', '每条规则最多') + ' ' + this._getPeriphExecBlockLimit('trigger') + ' ' + t('pef-max-triggers-suffix', '个触发器'));
                return;
            }
            ruleData.triggers = triggers;

            const actions = this._collectPeriphExecActions();
            if (actions.length > this._getPeriphExecBlockLimit('action')) {
                this.showInlineError(errEl, t('pef-max-actions', '每条规则最多') + ' ' + this._getPeriphExecBlockLimit('action') + ' ' + t('pef-max-actions-suffix', '个动作'));
                return;
            }
            if (this._peSensorExtraJsonError) {
                this.showInlineError(errEl, t('pef-err-sensor-json', '传感器高级参数必须是 JSON 对象'));
                return;
            }
            ruleData.actions = actions;
            this._clearAllFieldErrors();
            // 获取动作块 DOM 引用，用于字段级高亮
            var actionBlocks = document.getElementById('periph-exec-actions')?.querySelectorAll('.periph-exec-config-item') || [];
            for (let i = 0; i < actions.length; i++) {
                var at = actions[i].actionType;
                var blk = actionBlocks[i];
                if (at === 19) {
                    var sv = {};
                    try { sv = JSON.parse(actions[i].actionValue); } catch(e) {}
                    if (!sv.periphId) {
                        this._highlightFieldError(blk?.querySelector('.pe-target-periph'), t('pef-err-select-sensor', '请选择传感器外设'), errEl);
                        return;
                    }
                } else if (at === 15) {
                    var scriptValue = actions[i].actionValue || '';
                    if (!scriptValue.trim()) {
                        this._highlightFieldError(blk?.querySelector('.pe-action-value-script'), t('pef-err-script-empty', '脚本内容不能为空'), errEl);
                        return;
                    }
                    if (scriptValue.length > 1024) {
                        this.showInlineError(errEl, t('pef-err-script-too-long', '脚本超过最大长度限制(1024字节)'));
                        return;
                    }
                } else if (at === 27) {
                    var oledValue = actions[i].actionValue || '';
                    if (!oledValue.trim()) {
                        this._highlightFieldError(blk?.querySelector('.pe-action-value-oled'), t('pef-err-oled-empty', 'OLED 显示内容不能为空'), errEl);
                        return;
                    }
                } else if (at === 24 || at === 25) {
                    var dispValue = actions[i].actionValue || '';
                    if (!dispValue.trim()) {
                        this._highlightFieldError(blk?.querySelector('.pe-action-value'), at === 24 ? t('pef-err-display-num-empty', '显示数字的值不能为空') : t('pef-err-display-text-empty', '显示文本的内容不能为空'), errEl);
                        return;
                    }
                } else if (at === 18) {
                    // Modbus 轮询: 检查采集任务是否配置有效
                    var pollPeriph = actions[i].targetPeriphId || '';
                    if (!pollPeriph) {
                        this._highlightFieldError(blk?.querySelector('.pe-poll-task-select'), t('pef-err-modbus-poll', 'Modbus 轮询需选择至少一个采集任务'), errEl);
                        return;
                    }
                }
                // 通用：非系统/规则控制动作必须有目标外设
                var needsTarget = !(at >= 6 && at <= 9) && at !== 18 && at !== 21 && at !== 22 && at !== 23;
                if (needsTarget && !actions[i].targetPeriphId) {
                    this._highlightFieldError(blk?.querySelector('.pe-target-periph'), t('pef-err-select-periph', '请选择执行外设'), errEl);
                    return;
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
                        Notification.success(isEdit ? t('pef-rule-updated', '规则更新成功') : t('pef-rule-added', '规则添加成功'), t('pef-title', '外设执行'));
                        this.closePeriphExecModal();
                        if (this.currentPage === 'periph-exec') this.loadPeriphExecPage();
                    } else {
                        this.showInlineError(errEl, res?.error || t('pef-err-save-failed', '保存失败'));
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
                    this.showInlineError(errEl, isNetworkError ? t('pef-err-offline', '设备离线或不可达，请检查网络连接') : t('pef-err-save-failed', '保存失败'));
                });
        }

    });
})();
