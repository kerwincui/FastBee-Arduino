/**
 * 外设执行管理 - 表单模块
 * 规则编辑表单渲染、字段验证、表单提交处理
 * 依赖：periph-exec.js；Modbus、传感器和灯效能力由独立模块按需补充
 */
(function() {
    // The editor can be opened before the peripheral configuration module.
    var t = typeof window.t === 'function' ? window.t : function(key, fallback) { return fallback || key; };
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

        _buildPeriphExecModbusPanel() {
            return '<div class="fb-form-group pe-poll-tasks-group pe-span-all">' +
                '<label>选择子设备</label>' +
                '<div class="pe-poll-tasks-list"></div>' +
                '<small class="pe-help-text">选择要执行的 Modbus 子设备</small></div>';
        },

        _mountPeriphExecLazyActionPanel(block, actionType, data, forceMount) {
            if (!block) return;
            var host = block.querySelector('.pe-action-lazy-host');
            if (!host) return;
            var oldType = block.dataset.peLazyActionType;
            var requestedType = String(actionType);
            var targetId = block.querySelector('.pe-target-periph')?.value || (data && data.targetPeriphId) || '';
            if (!forceMount && !data && oldType === requestedType) {
                if (actionType !== 10) return;
                var hasNeoPixelPanel = !!host.querySelector('.pe-neopixel-preset');
                if (hasNeoPixelPanel === this._isNeoPixelTarget(targetId)) return;
            }
            block._peLazyPanelDrafts = block._peLazyPanelDrafts || {};
            if (oldType !== undefined && (oldType !== requestedType || host.querySelector('input, select, textarea'))) {
                block._peLazyPanelDrafts[oldType] = this._snapshotPeriphExecLazyPanel(host);
            }
            block._peLazyLoadToken = (block._peLazyLoadToken || 0) + 1;
            host.innerHTML = '';
            var actionValue = data && data.actionValue ? data.actionValue : '';
            var moduleName = this._getPeriphExecLazyActionModule(actionType, targetId);
            var moduleReady = !moduleName ||
                (moduleName === 'periph-exec-sensor' && typeof this._buildPeriphExecSensorPanel === 'function') ||
                (moduleName === 'periph-exec-effects' && typeof this._buildPeriphExecEffectsDisplayPanel === 'function');
            block.dataset.peLazyActionType = requestedType;
            if (!moduleReady) {
                this._loadPeriphExecLazyActionModule(moduleName, block, actionType, data);
                return;
            }
            delete block.dataset.peLazyModuleError;
            if (actionType === 18) {
                host.innerHTML = this._buildPeriphExecModbusPanel();
                this._populateModbusDevicePanel(host.querySelector('.pe-poll-tasks-list'), actionValue);
            } else if (actionType === 19) {
                host.innerHTML = this._buildPeriphExecSensorPanel(data || {});
            } else if (actionType === 15 || actionType === 21) {
                host.innerHTML = this._buildPeriphExecInlineActionPanel(actionType, actionValue);
            } else if (moduleName === 'periph-exec-effects') {
                host.innerHTML = this._buildPeriphExecEffectsDisplayPanel(actionType, actionValue);
            }
            this._restorePeriphExecLazyPanel(host, block._peLazyPanelDrafts[requestedType]);
            if (actionType === 19) {
                var category = host.querySelector('.pe-sensor-category');
                this._populateSensorPeriphSelect(block, category ? category.value : 'analog', targetId);
                if (category) this.onSensorCategoryChange(category, this._getPeriphExecBlockIndex(block), true);
                if (category?.value === 'configured') {
                    var configuredSensor = {};
                    try { configuredSensor = JSON.parse(actionValue || '{}'); } catch (error) {}
                    this._syncConfiguredSensorAction(block, configuredSensor);
                }
            }
            if (actionType === 21 && actionValue) {
                var eventSelect = host.querySelector('.pe-trigger-event-select');
                if (eventSelect) eventSelect.value = actionValue;
            }
            if (actionType === 27) {
                var oled = host.querySelector('.pe-action-value-oled');
                var counter = host.querySelector('.pe-oled-counter');
                if (oled && counter) {
                    oled.addEventListener('input', function() {
                        var lines = this.value ? this.value.split('\n').length : 0;
                        counter.textContent = '已输入 ' + this.value.length + '/512 字符，' + lines + '/6 行';
                        counter.style.color = lines > 6 ? '#e74c3c' : '';
                    });
                    oled.dispatchEvent(new Event('input'));
                }
            }
            if (moduleName === 'periph-exec-effects') {
                this._initNeoPixelPresetFromData(block, actionValue || block.querySelector('.pe-action-value')?.value || '', actionType);
                this._updateNeoPixelPresetVisibility(block);
            }
        },

        _populateRemoteResourceSelect(selectEl, actionType, selectedValue) {
            if (!selectEl) return;
            var isChannel = Number(actionType) === 33;
            var items = isChannel ? (this._peRemoteChannels || []) : (this._peWeatherSources || []);
            var prefix = isChannel ? 'remote.' : 'weather.';
            var normalized = String(selectedValue || '');
            if (normalized.indexOf(prefix) === 0) normalized = normalized.substring(prefix.length);
            selectEl.innerHTML = '';
            var empty = document.createElement('option');
            empty.value = '';
            empty.textContent = isChannel ? '-- 选择远程通道 --' : '-- 选择天气来源 --';
            selectEl.appendChild(empty);
            items.forEach(function(item) {
                var option = document.createElement('option');
                option.value = String(item.id || '');
                option.textContent = (item.label || item.id) + ' (' + item.id + ')';
                selectEl.appendChild(option);
            });
            if (normalized && !items.some(function(item) { return String(item.id) === normalized; })) {
                var unavailable = document.createElement('option');
                unavailable.value = normalized;
                unavailable.textContent = normalized + ' [不可用]';
                selectEl.appendChild(unavailable);
            }
            selectEl.value = normalized;
        },

        _syncRemoteActionValueState(block) {
            if (!block) return;
            var remoteValue = block.querySelector('.pe-remote-action-value');
            if (remoteValue) remoteValue.disabled = block.querySelector('.pe-use-received-value')?.checked === true;
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
            const isModbusTarget = data.targetPeriphId && data.targetPeriphId.indexOf('modbus:') === 0;
            if (isModbusTarget && !['16', '17', '18'].includes(actionType)) actionType = '18';
            const actionTypeInt = parseInt(actionType);
            const isModbusWrite = actionTypeInt === 16 || actionTypeInt === 17;
            const isModbusPoll = actionTypeInt === 18;
            const isSensorRead = actionTypeInt === 19;
            const isScriptAction = actionTypeInt === 15;
            const isTriggerEvent = actionTypeInt === 21;
            const isRuleCtrlAction = (actionTypeInt === 22 || actionTypeInt === 23);
            const isRemoteChannel = actionTypeInt === 33;
            const isRemoteRead = actionTypeInt === 34;
            const isRemoteAction = isRemoteChannel || isRemoteRead;
            const isDisplayAction = (actionTypeInt === 24 || actionTypeInt === 25 || actionTypeInt === 26);
            const isOledDisplay = (actionTypeInt === 27);
            const showPeriphGroup = isRuleCtrlAction || (isModbusTarget || !((actionTypeInt >= 6 && actionTypeInt <= 9) || isModbusPoll || isTriggerEvent));
            const needsValue = isModbusWrite || (!isModbusTarget && !isRuleCtrlAction && ((actionTypeInt >= 2 && actionTypeInt <= 5) || actionTypeInt === 10 || actionTypeInt === 11 || actionTypeInt === 12 || actionTypeInt === 24 || actionTypeInt === 25 || actionTypeInt === 28 || actionTypeInt === 29 || isOledDisplay || isScriptAction || isTriggerEvent || isRemoteChannel));
            const showRecv = (!isModbusTarget || isModbusWrite) && !isRuleCtrlAction && this._hasSetModeTrigger() && needsValue && !isOledDisplay && !isScriptAction && !isTriggerEvent;
            const showActionType = true;
            const receiveChecked = showRecv && (isModbusWrite ? data.useReceivedValue === true : data.useReceivedValue !== false);
            const showExecRow = true;
            const execMode = parseInt(data.execMode ?? 0);
            const sel = (v) => actionType === String(v) ? 'selected' : '';
            const showScalarValue = needsValue && !isScriptAction && !isOledDisplay && !isTriggerEvent && !isRemoteChannel;
            const scalarActionValue = (isScriptAction || isOledDisplay || isTriggerEvent || isSensorRead || isModbusPoll) ? '' : (data.actionValue || '');
            const remoteValue = ['on', 'off', 'trigger'].indexOf(data.actionValue) >= 0 ? data.actionValue : 'on';
            const remoteOptions = (this._peRemoteResourcesAvailable || isRemoteAction) ?
                '<optgroup label="' + t('pef-action-remote', '远程资源') + '">' +
                '<option value="33" ' + sel(33) + '>' + t('pef-action-remote-channel', '远程通道控制') + '</option>' +
                '<option value="34" ' + sel(34) + '>' + t('pef-action-remote-read', '远程天气读取') + '</option></optgroup>' : '';
            const targetLabel = isRuleCtrlAction ? t('pef-target-rule', '目标执行规则') :
                (isRemoteChannel ? t('pef-target-remote-channel', '远程通道') :
                    (isRemoteRead ? t('pef-target-weather-source', '天气来源') : t('pef-target-periph', '执行外设')));
            const targetPlaceholder = isRuleCtrlAction ? t('pef-select-rule', '-- 选择执行规则 --') :
                (isRemoteChannel ? t('pef-select-remote-channel', '-- 选择远程通道 --') :
                    (isRemoteRead ? t('pef-select-weather-source', '-- 选择天气来源 --') : t('pef-select-periph', '-- 选择外设 --')));
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
                        '<optgroup label="Modbus">' +
                        '<option value="16" ' + sel(16) + '>写单个线圈/开关</option>' +
                        '<option value="17" ' + sel(17) + '>写单个寄存器 (FC06)</option>' +
                        '<option value="18" ' + sel(18) + '>采集任务 / 子设备动作</option></optgroup>' +
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
                        remoteOptions +
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
                    '<label>' + targetLabel + '</label>' +
                    '<select class="pe-target-periph"><option value="">' + targetPlaceholder + '</option></select></div>' +
                    '<div class="fb-form-group pe-modbus-ctrl-panel' + this._hiddenClass(isModbusTarget && !isModbusWrite) + '"></div>' +
                    '<div class="fb-form-group pe-remote-value-group' + this._hiddenClass(isRemoteChannel) + '">' +
                    '<label>' + t('pef-remote-command', '远程命令') + '</label>' +
                    '<select class="pe-remote-action-value">' +
                    '<option value="on"' + (remoteValue === 'on' ? ' selected' : '') + '>' + t('pef-remote-on', '开启 (on)') + '</option>' +
                    '<option value="off"' + (remoteValue === 'off' ? ' selected' : '') + '>' + t('pef-remote-off', '关闭 (off)') + '</option>' +
                    '<option value="trigger"' + (remoteValue === 'trigger' ? ' selected' : '') + '>' + t('pef-remote-trigger', '触发 (trigger)') + '</option>' +
                    '</select></div>' +
                    '<div class="fb-form-group pe-action-value-group' + this._hiddenClass(showScalarValue) + '">' +
                    '<label>' + '动作参数' + '</label>' +
                    '<input type="text" class="pe-action-value" value="' + escapeHtml(scalarActionValue) + '" placeholder="' + escapeHtml(isDisplayAction ? '支持 ${periphId.field} 模板（例如 ${dht_01.temperature} / ${adc.voltage}），将从传感器缓存取最新值' : '如: PWM值、闪烁间隔ms') + '"' + (receiveChecked ? ' readonly' : '') + '>' +
                    '<small class="pe-help-text">' + (actionTypeInt === 10 ? t('pef-help-send-cmd', '支持 JSON 格式或纯文本发送指令。支持 $value 占位符接收平台下发的值') : actionTypeInt === 11 ? t('pef-help-led', '选择预设灯效或输入自定义颜色，支持 $value 占位符接收平台下发的值') : actionTypeInt === 12 ? t('pef-help-motor', '输入电机指令: forward(正转)、reverse(反转)、stop(停止)、数字(步数)') : actionTypeInt === 28 ? t('pef-help-rf', '输入射频编码，如十六进制编码字符串') : actionTypeInt === 29 ? t('pef-help-uart', '输入要发送的文本，支持 $value 占位符') : (isScriptAction ? t('pef-help-script', '可用: GPIO pin HIGH/LOW, DELAY ms, PWM pin duty, DAC pin val, LOG msg, PERIPH id 动作') : (isOledDisplay ? t('pef-help-oled', '最多 6 行、可自动适配 OLED 显示。\n 换行。首行 # 开头为居中标题带分隔线。支持变量：${外设id.字段} 从传感器缓存取值，$value 取 MQTT/规则下发的原始值') : (isDisplayAction ? t('pef-help-template', '支持 ${periphId.field} 模板（例如 ${dht_01.temperature} / ${adc.voltage}），将从传感器缓存取最新值') : t('pef-help-default', '闪烁/呼吸灯填间隔ms, PWM填占空比, DAC填0-255'))))) + '</small>' +
                    '</div>' +
                    '<div class="pe-action-lazy-host pe-span-all"></div>' +
                    '<div class="fb-form-group pe-use-received-value-group' + this._hiddenClass(showRecv) + '">' +
                    '<label class="pe-checkbox-label pe-check-align"><input type="checkbox" class="pe-use-received-value"' + (receiveChecked ? ' checked' : '') + '>' +
                    t('pef-use-recv-value', '勾选后动作参数将使用平台下发或触发源的值') + '</label></div>' +
                    '</div>';
            container.appendChild(div);
            var initActType = parseInt(actionType, 10);
            // 先恢复目标，再挂载依赖目标类型的动态面板（如 NeoPixel 与传感器）。
            var effectivePollOnly = isPollMode && isModbusPoll && !isModbusTarget;
            if (isRemoteAction) this._populateRemoteResourceSelect(div.querySelector('.pe-target-periph'), initActType, data.targetPeriphId || '');
            else this._populatePeriphSelect(div.querySelector('.pe-target-periph'), data.targetPeriphId || '', effectivePollOnly, initActType === 21, (initActType === 22 || initActType === 23));
            this._syncRemoteActionValueState(div);
            this._mountPeriphExecLazyActionPanel(div, isModbusTarget ? -1 : actionTypeInt, data);
            this._updateExpanderActionAvailability(div);
            if (isModbusTarget && !isModbusWrite) this._showModbusCtrlPanel(div.querySelector('.pe-modbus-ctrl-panel'), data.targetPeriphId, data.actionValue || '');
            if (typeof this._syncModbusActionOptions === 'function') this._syncModbusActionOptions(div);
            if (isModbusWrite && typeof this._updateModbusWriteHelp === 'function') this._updateModbusWriteHelp(div, actionTypeInt);
            // NeoPixel 预设灯效：初始化下拉框并同步显示状态
            if (this._isNeoPixelTarget(data.targetPeriphId) && (data.actionType ?? 0) == 10) {
                // 自动迁移：NeoPixel 目标的 actionType=10（旧格式）→ 11（灯效控制）
                var actSel = div.querySelector('.pe-action-type');
                if (actSel) actSel.value = '11';
                // 同步 UI 状态（帮助文本、显隐等）
                this.onPeriphExecActionTypeChangeInBlock('11', Array.from(container.children).indexOf(div));
                if (typeof this._initNeoPixelPresetFromData === 'function') {
                    this._initNeoPixelPresetFromData(div, data.actionValue, 11);
                }
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
            if (typeof this._updateNeoPixelPresetVisibility === 'function') {
                this._updateNeoPixelPresetVisibility(div);
            }
            this._updatePeriphExecAddButtons();
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

        _updatePeriphExecPanelOptions() {
            var history = document.getElementById('periph-exec-record-history');
            if (!history) return;
            var historyLink = document.getElementById('periph-exec-history-settings-link');
            var panelAvailable = !!this._featureActive && this._featureActive.screenPanel === true;
            if (historyLink) {
                historyLink.classList.toggle('fb-hidden', !panelAvailable);
                historyLink.hidden = !panelAvailable;
                historyLink.disabled = !panelAvailable;
            }
            var blocks = document.querySelectorAll('#periph-exec-actions .periph-exec-config-item');
            var hasRecordableAction = Array.from(blocks).some(function(block) {
                var actionType = Number(block.querySelector('.pe-action-type')?.value);
                return actionType === 18 || actionType === 19 || actionType === 34;
            });
            history.disabled = !hasRecordableAction;
            if (!hasRecordableAction) history.checked = false;
            history.closest('label')?.classList.toggle('is-disabled', !hasRecordableAction);
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
            this._updatePeriphExecPanelOptions();
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
            this._updatePeriphExecPanelOptions();
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
            this._syncRemoteActionValueState(block);
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
                const needsValue = (actionType >= 2 && actionType <= 5) || actionType === 10 || actionType === 11 || actionType === 12 || actionType === 24 || actionType === 25 || actionType === 28 || actionType === 29 || actionType === 33;
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
                var remoteValue = item.querySelector('.pe-remote-action-value');
                if (remoteValue) remoteValue.disabled = showRecv;
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

        _updateExpanderActionAvailability(block) {
            if (!block) return;
            var targetId = block.querySelector('.pe-target-periph')?.value;
            var target = (this._pePeripherals || []).find(function(item) { return item.id === targetId; });
            var params = target?.params || {};
            var logical = params.ioSource === 'expander' || Number(params.ioSource) === 1;
            var parentOnly = Number(target?.type) === 55;
            var allowed = [6, 7, 8, 9, 15, 21, 22, 23, 31, 32];
            if (logical) allowed = allowed.concat(Number(target.type) === 12 ? [0, 1, 2, 10, 13, 14, 30] : [19]);
            block.querySelectorAll('.pe-action-type option').forEach(function(option) {
                if (option.dataset.ioUnsupported) { option.disabled = false; delete option.dataset.ioUnsupported; option.removeAttribute('title'); }
                if ((logical || parentOnly) && allowed.indexOf(Number(option.value)) < 0) {
                    option.disabled = true;
                    option.dataset.ioUnsupported = 'true';
                    option.title = parentOnly ? '请选择该扩展器的逻辑输入或输出外设' : '该 IO 扩展通道不支持此动作';
                }
            });
        },

        onPeriphExecActionTypeChangeInBlock(val, index) {
            const block = this._getPeriphExecBlock('periph-exec-actions', index);
            if (!block) return;
            const actionType = parseInt(val);
            this._mountPeriphExecLazyActionPanel(block, actionType);
            const targetGroup = block.querySelector('.pe-target-group');
            const valueGroup = block.querySelector('.pe-action-value-group');
            const pollTasksGroup = block.querySelector('.pe-poll-tasks-group');
            const isModbusPoll = actionType === 18;
            const isModbusWrite = actionType === 16 || actionType === 17;
            const isSensorRead = actionType === 19;
            const isTriggerEvent = actionType === 21;
            const isRuleCtrlAction = (actionType === 22 || actionType === 23);
            const isRemoteChannel = actionType === 33;
            const isRemoteRead = actionType === 34;
            const isRemoteAction = isRemoteChannel || isRemoteRead;
            // 检查当前目标是否为 Modbus 设备（与创建路径保持一致）
            const isModbusTarget = (block.querySelector('.pe-target-periph')?.value || '').indexOf('modbus:') === 0;
            const showTargetGroup = isModbusTarget || isRuleCtrlAction || !((actionType >= 6 && actionType <= 9) || isModbusPoll || isTriggerEvent);
            this._setSectionVisible(targetGroup, showTargetGroup);
            // 切换目标分组的 label 和空选项提示（规则 vs 外设）
            if (targetGroup) {
                var labelEl = targetGroup.querySelector('label');
                if (labelEl) labelEl.textContent = isRuleCtrlAction ? t('pef-target-rule', '目标执行规则') :
                    (isRemoteChannel ? t('pef-target-remote-channel', '远程通道') :
                        (isRemoteRead ? t('pef-target-weather-source', '天气来源') : t('pef-target-periph', '执行外设')));
            }
            const needsValue = isModbusWrite || (!isModbusTarget && !isRuleCtrlAction && ((actionType >= 2 && actionType <= 5) || actionType === 10 || actionType === 11 || actionType === 12 || actionType === 15 || actionType === 24 || actionType === 25 || actionType === 27 || actionType === 28 || actionType === 29 || isRemoteChannel));
            const showScalarValue = needsValue && actionType !== 15 && actionType !== 27 && !isRemoteChannel;
            this._setSectionVisible(valueGroup, showScalarValue);
            this._setSectionVisible(block.querySelector('.pe-remote-value-group'), isRemoteChannel);
            if (valueGroup) {
                const singleEl = valueGroup.querySelector('.pe-action-value');
                if (singleEl) singleEl.classList.remove('is-hidden');
                // 提示文案同步切换
                const helpEl = valueGroup.querySelector('.pe-help-text');
                if (helpEl) {
                    if (actionType === 10) helpEl.textContent = t('pef-help-send-cmd', '支持 JSON 格式或纯文本发送指令。支持 $value 占位符接收平台下发的值');
                    else if (actionType === 11) helpEl.textContent = t('pef-help-led', '选择预设灯效或输入自定义颜色，支持 $value 占位符接收平台下发的值');
                    else if (actionType === 12) helpEl.textContent = t('pef-help-motor', '输入电机指令: forward(正转)、reverse(反转)、stop(停止)、数字(步数)');
                    else if (actionType === 28) helpEl.textContent = t('pef-help-rf', '输入射频编码，如十六进制编码字符串');
                    else if (actionType === 29) helpEl.textContent = t('pef-help-uart', '输入要发送的文本，支持 $value 占位符');
                    else if (actionType === 24 || actionType === 25 || actionType === 26) helpEl.textContent = t('pef-help-template', '支持 ${periphId.field} 模板（例如 ${dht_01.temperature} / ${adc.voltage}），将从传感器缓存取最新值');
                    else helpEl.textContent = t('pef-help-default', '闪烁/呼吸灯填间隔ms, PWM填占空比, DAC填0-255');
                }
            }
            if (pollTasksGroup) {
                this._setSectionVisible(pollTasksGroup, isModbusPoll && !isModbusTarget);
                if (isModbusPoll && !isModbusTarget) {
                    this._populateModbusDevicePanel(block.querySelector('.pe-poll-tasks-list'), '');
                }
            }
            const sensorGroup = block.querySelector('.pe-sensor-group');
            this._setSectionVisible(sensorGroup, isSensorRead);
            // 切换动作类型时，隐藏 Modbus 控制面板（ctrlPanel 仅在选择 modbus:xxx 目标时由 _onTargetPeriphChange 管理）
            const ctrlPanel = block.querySelector('.pe-modbus-ctrl-panel');
            this._setSectionVisible(ctrlPanel, false);
            if (targetGroup && !targetGroup.classList.contains('is-hidden')) {
                var peSelect = block.querySelector('.pe-target-periph');
                // 仅 Modbus 轮询动作(18)限制为 Modbus 采集任务；其他本地动作显示全部外设
                var effectivePollOnly = this._isPollTriggerActive() && isModbusPoll && !isModbusTarget;
                if (isSensorRead && typeof this._populateSensorPeriphSelect === 'function') {
                    this._populateSensorPeriphSelect(block, block.querySelector('.pe-sensor-category')?.value || 'analog', peSelect?.value);
                } else if (isRemoteAction) {
                    this._populateRemoteResourceSelect(peSelect, actionType, '');
                } else {
                    this._populatePeriphSelect(peSelect, peSelect ? peSelect.value : '', effectivePollOnly, isTriggerEvent, isRuleCtrlAction);
                }
            }
            this._updateExpanderActionAvailability(block);
            if (typeof this._syncModbusActionOptions === 'function') this._syncModbusActionOptions(block);
            if (isModbusTarget && isModbusPoll) this._showModbusCtrlPanel(ctrlPanel, block.querySelector('.pe-target-periph').value, '');
            if (isModbusWrite && typeof this._updateModbusWriteHelp === 'function') this._updateModbusWriteHelp(block, actionType);
            // 使用接收值: 仅在平台触发+设置+需要数值的动作类型时显示（OLED_DISPLAY 不需要整体替换，通过 $value 占位符嵌入）
            const recvGroup = block.querySelector('.pe-use-received-value-group');
            const isSetMode = this._hasSetModeTrigger();
            const showRecv = isSetMode && needsValue && actionType !== 15 && actionType !== 27;
            this._setSectionVisible(recvGroup, showRecv);
            const checkbox = block.querySelector('.pe-use-received-value');
            if (checkbox) checkbox.checked = showRecv && !isModbusWrite;
            const valueInput = block.querySelector('.pe-action-value');
            if (valueInput) {
                if (showRecv && !isModbusWrite) valueInput.setAttribute('readonly', '');
                else valueInput.removeAttribute('readonly');
            }
            this._syncRemoteActionValueState(block);
            // NeoPixel 预设灯效：当 actionType=10 且目标是 NeoPixel 时显示下拉框
            if (typeof this._updateNeoPixelPresetVisibility === 'function') {
                this._updateNeoPixelPresetVisibility(block);
            }
            // 切换动作类型时，重新映射预设下拉框，确保旧 actionValue 正确迁移到新格式
            if (actionType === 11 || actionType === 10) {
                var oldVal = block.querySelector('.pe-action-value')?.value || '';
                if (oldVal && typeof this._initNeoPixelPresetFromData === 'function') {
                    this._initNeoPixelPresetFromData(block, oldVal, actionType);
                }
                // 重新同步显隐状态（_initNeoPixelPresetFromData 只设值，不控制显隐）
                var presetSel = block.querySelector('.pe-neopixel-preset');
                if (presetSel && typeof this._onNeoPixelPresetChange === 'function') {
                    this._onNeoPixelPresetChange(presetSel, index);
                }
            }
            this._updatePeriphExecPanelOptions();
        },

        // ============ NeoPixel 预设灯效 ============

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
            var localSources = (typeof this._getPeriphExecLocalSensorSources === 'function')
                ? this._getPeriphExecLocalSensorSources() : (this._peSensorSources || []);
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
            var localSources = (typeof this._getPeriphExecLocalSensorSources === 'function')
                ? this._getPeriphExecLocalSensorSources() : (this._peSensorSources || []);
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
            this._peConfiguredSensorError = '';
            this._peLazyModuleError = '';
            const actions = [];
            const isPollMode = this._isPollTriggerActive();
            container.querySelectorAll('.periph-exec-config-item').forEach(item => {
                // 轮询触发模式下仅 Modbus 轮询(18)或默认动作强制为 actionType=18
                // 传感器读取(19)、OLED(27)等保留其原始 actionType 和采集逻辑
                var curActType = parseInt(item.querySelector('.pe-action-type')?.value || '0');
                if (isPollMode && (curActType === 18 || curActType === 0) &&
                    !(item.querySelector('.pe-target-periph')?.value || '').startsWith('modbus:')) {
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
                const useReceivedValue = item.querySelector('.pe-use-received-value')?.checked === true;
                if (isModbusTarget && actionType === '18') {
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
                    if (typeof this._collectPeriphExecSensorAction === 'function') {
                        this._collectPeriphExecSensorAction(item, action);
                    } else {
                        this._peLazyModuleError = 'periph-exec-sensor';
                        action.actionValue = '';
                    }
                } else if (action.actionType === 27 || action.actionType === 11) {
                    if (typeof this._collectPeriphExecEffectsAction === 'function') {
                        this._collectPeriphExecEffectsAction(item, action);
                    } else {
                        this._peLazyModuleError = 'periph-exec-effects';
                        action.actionValue = '';
                    }
                } else if (action.actionType === 15) {
                    action.actionValue = item.querySelector('.pe-action-value-script')?.value || '';
                } else if (action.actionType === 21) {
                    var eventSelect = item.querySelector('.pe-trigger-event-select');
                    action.actionValue = eventSelect ? (eventSelect.value || '') : '';
                } else if (action.actionType === 33) {
                    action.actionValue = useReceivedValue ? '' : (item.querySelector('.pe-remote-action-value')?.value || '');
                } else if (action.actionType === 34) {
                    action.actionValue = '';
                } else {
                    var hasEffectsPanel = !!item.querySelector('.pe-neopixel-preset');
                    if (hasEffectsPanel && typeof this._collectPeriphExecEffectsAction === 'function') {
                        this._collectPeriphExecEffectsAction(item, action);
                    } else {
                        action.actionValue = item.querySelector('.pe-action-value')?.value?.trim() || '';
                    }
                }
                action.useReceivedValue = action.actionType === 34 ? false : useReceivedValue;
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
                reportAfterExec: document.getElementById('periph-exec-report').checked,
                allowPanelControl: document.getElementById('periph-exec-panel-control')?.checked === true,
                recordHistory: document.getElementById('periph-exec-record-history')?.checked === true
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
            if (this._peLazyModuleError) {
                this.showInlineError(errEl, t('pef-err-module-loading', '配置模块仍在加载，请稍后再保存'));
                return;
            }
            if (this._peConfiguredSensorError) {
                this.showInlineError(errEl, this._peConfiguredSensorError);
                return;
            }
            if (this._peSensorExtraJsonError) {
                this.showInlineError(errEl, t('pef-err-sensor-json', '传感器高级参数必须是 JSON 对象'));
                return;
            }
            ruleData.actions = actions;
            if (ruleData.recordHistory && !actions.some(function(action) {
                return action.actionType === 18 || action.actionType === 19 || action.actionType === 34;
            })) {
                this.showInlineError(errEl, '记录历史仅适用于 Modbus 采集、传感器读取或远程读取动作');
                return;
            }
            this._clearAllFieldErrors();

            // Keep browser validation aligned with the persisted firmware
            // contract. Scheduler never applies a hidden runtime-only interval.
            var triggerBlocks = document.getElementById('periph-exec-triggers')?.querySelectorAll('.periph-exec-config-item') || [];
            for (let i = 0; i < triggers.length; i++) {
                var trigger = triggers[i];
                var triggerBlock = triggerBlocks[i];
                if (trigger.triggerType === 1 && trigger.timerMode === 0 &&
                    (!Number.isInteger(trigger.intervalSec) || trigger.intervalSec < 1 || trigger.intervalSec > 86400)) {
                    this._highlightFieldError(
                        triggerBlock?.querySelector('.pe-interval'),
                        t('pef-err-timer-interval', '定时间隔必须为 1 到 86400 秒'),
                        errEl);
                    return;
                }
                if (trigger.triggerType === 5 &&
                    (!Number.isInteger(trigger.intervalSec) || trigger.intervalSec < 5 || trigger.intervalSec > 86400)) {
                    this._highlightFieldError(
                        triggerBlock?.querySelector('.pe-poll-interval'),
                        t('pef-err-poll-interval', '轮询间隔必须为 5 到 86400 秒'),
                        errEl);
                    return;
                }
            }

            // 获取动作块 DOM 引用，用于字段级高亮
            var actionBlocks = document.getElementById('periph-exec-actions')?.querySelectorAll('.periph-exec-config-item') || [];
            for (let i = 0; i < actions.length; i++) {
                var at = actions[i].actionType;
                var blk = actionBlocks[i];
                if (blk?.querySelector('.pe-action-type')?.selectedOptions[0]?.dataset.ioUnsupported) {
                    this._highlightFieldError(blk.querySelector('.pe-action-type'), '该 IO 扩展通道不支持此动作，请选择兼容的数字 IO 操作', errEl);
                    return;
                }
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
                } else if (at === 16 || at === 17) {
                    if (!(actions[i].targetPeriphId || '').startsWith('modbus:')) {
                        this._highlightFieldError(blk?.querySelector('.pe-target-periph'), '请选择 Modbus 子设备', errEl);
                        return;
                    }
                    if (!actions[i].useReceivedValue) {
                        var writeParts = String(actions[i].actionValue || '').split(':');
                        var validWrite = writeParts.length <= 2 && writeParts.every(function(part) {
                            return /^\d+$/.test(part) && Number(part) <= 65535;
                        });
                        if (at === 16 && Number(writeParts[writeParts.length - 1]) > 1) validWrite = false;
                        if (!validWrite) {
                            this._highlightFieldError(blk?.querySelector('.pe-action-value'),
                                at === 17 ? '请输入十进制 地址:数值，范围 0-65535' : '请输入十进制 通道偏移:状态，状态为 0 或 1', errEl);
                            return;
                        }
                    }
                } else if (at === 18) {
                    // Modbus 轮询: 检查采集任务是否配置有效
                    var pollPeriph = actions[i].targetPeriphId || '';
                    if (!pollPeriph) {
                        this._highlightFieldError(blk?.querySelector('.pe-poll-task-select'), t('pef-err-modbus-poll', 'Modbus 轮询需选择至少一个采集任务'), errEl);
                        return;
                    }
                } else if (at === 33) {
                    var remoteChannel = (this._peRemoteChannels || []).find(function(item) {
                        return String(item.id) === String(actions[i].targetPeriphId);
                    });
                    if (!remoteChannel) {
                        this._highlightFieldError(blk?.querySelector('.pe-target-periph'), t('pef-err-remote-channel', '请选择已启用的远程开关或动作通道'), errEl);
                        return;
                    }
                    if (!actions[i].useReceivedValue && ['on', 'off', 'trigger'].indexOf(actions[i].actionValue) < 0) {
                        this._highlightFieldError(blk?.querySelector('.pe-remote-action-value'), t('pef-err-remote-command', '请选择 on、off 或 trigger'), errEl);
                        return;
                    }
                } else if (at === 34) {
                    var weatherSource = (this._peWeatherSources || []).find(function(item) {
                        return String(item.id) === String(actions[i].targetPeriphId);
                    });
                    if (!weatherSource) {
                        this._highlightFieldError(blk?.querySelector('.pe-target-periph'), t('pef-err-weather-source', '请选择已启用的天气来源'), errEl);
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
