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
            const showPoll = triggerType === '5';
            const timerMode = String(data.timerMode ?? 0);
            const showInterval = timerMode === '0';
            const showDaily = timerMode === '1';
            const opVal = String(data.operatorType ?? 0);
            const mappedOp = (opVal === '0' || opVal === '1') ? opVal : '0';
            const showCompareValue = showPlatform && mappedOp !== '1';
            div.innerHTML = '<span class="mqtt-topic-index">' + (index + 1) + '</span>' +
                '<button type="button" class="mqtt-topic-delete">' + i18n.t('peripheral-delete') + '</button>' +
                '<div class="config-form-grid">' +
                    '<div class="fb-form-group"><label>' + i18n.t('periph-exec-trigger-type-label') + '</label>' +
                    '<select class="pe-trigger-type">' +
                        '<option value="0"' + (triggerType === '0' ? ' selected' : '') + '>' + i18n.t('periph-exec-trigger-platform') + '</option>' +
                        '<option value="4"' + (triggerType === '4' ? ' selected' : '') + '>' + i18n.t('periph-exec-trigger-event') + '</option>' +
                        '<option value="1"' + (triggerType === '1' ? ' selected' : '') + '>' + i18n.t('periph-exec-trigger-timer') + '</option>' +
                        '<option value="5"' + (triggerType === '5' ? ' selected' : '') + '>' + i18n.t('periph-exec-trigger-poll') + '</option>' +
                    '</select></div>' +
                    '<div class="fb-form-group pe-trigger-periph-group' + this._hiddenClass(showPlatform) + '">' +
                    '<label>' + i18n.t('periph-exec-trigger-periph-label') + '</label>' +
                    '<select class="pe-trigger-periph"><option value="">' + i18n.t('periph-exec-select-periph') + '</option></select></div>' +
                '</div>' +
                '<div class="pe-platform-condition' + this._hiddenClass(showPlatform) + '">' +
                    '<div class="config-form-grid">' +
                    '<div class="fb-form-group"><label>' + i18n.t('periph-exec-operator-label') + '</label>' +
                    '<select class="pe-operator">' +
                        '<option value="0"' + (mappedOp === '0' ? ' selected' : '') + '>' + i18n.t('periph-exec-handle-match') + '</option>' +
                        '<option value="1"' + (mappedOp === '1' ? ' selected' : '') + '>' + i18n.t('periph-exec-handle-set') + '</option>' +
                    '</select></div>' +
                    '<div class="fb-form-group pe-compare-value-group' + this._hiddenClass(showCompareValue) + '">' +
                    '<label>' + i18n.t('periph-exec-compare-label') + '</label>' +
                    '<input type="text" class="pe-compare-value" value="' + escapeHtml(data.compareValue) + '" placeholder="' + escapeHtml(i18n.t('periph-exec-compare-hint')) + '"></div>' +
                    '</div></div>' +
                '<div class="pe-poll-params' + this._hiddenClass(showPoll) + '">' +
                    '<div class="config-form-grid">' +
                    '<div class="fb-form-group"><label>' + i18n.t('periph-exec-poll-interval-label') + '</label><input type="number" class="pe-poll-interval" value="' + (data.intervalSec || 60) + '" min="5" max="86400"></div>' +
                    '<div class="fb-form-group"><label>' + i18n.t('periph-exec-poll-timeout-label') + '</label><input type="number" class="pe-poll-timeout" value="' + (data.pollResponseTimeout || 1000) + '" min="100" max="5000"></div>' +
                    '<div class="fb-form-group"><label>' + i18n.t('periph-exec-poll-retries-label') + '</label><input type="number" class="pe-poll-retries" value="' + (data.pollMaxRetries ?? 2) + '" min="0" max="3"></div>' +
                    '<div class="fb-form-group"><label>' + i18n.t('periph-exec-poll-delay-label') + '</label><input type="number" class="pe-poll-inter-delay" value="' + (data.pollInterPollDelay || 100) + '" min="20" max="1000"></div>' +
                    '</div></div>' +
                '<div class="pe-timer-config' + this._hiddenClass(showTimer) + '">' +
                    '<div class="config-form-grid">' +
                    '<div class="fb-form-group"><label>' + i18n.t('periph-exec-timer-mode-label') + '</label>' +
                    '<select class="pe-timer-mode">' +
                        '<option value="0"' + (timerMode === '0' ? ' selected' : '') + '>' + i18n.t('periph-exec-timer-interval') + '</option>' +
                        '<option value="1"' + (timerMode === '1' ? ' selected' : '') + '>' + i18n.t('periph-exec-timer-daily') + '</option>' +
                    '</select></div>' +
                    '<div class="fb-form-group pe-interval-fields' + this._hiddenClass(showInterval) + '"><label>' + i18n.t('periph-exec-interval-label') + '</label><input type="number" class="pe-interval" value="' + (data.intervalSec || 60) + '" min="1" max="86400"></div>' +
                    '<div class="fb-form-group pe-daily-fields' + this._hiddenClass(showDaily) + '"><label>' + i18n.t('periph-exec-timepoint-label') + '</label><input type="time" class="pe-timepoint" value="' + (data.timePoint || '08:00') + '"></div>' +
                    '</div></div>' +
                '<div class="pe-event-group' + this._hiddenClass(showEvent) + '">' +
                    '<div class="config-form-grid">' +
                    '<div class="fb-form-group"><label>' + i18n.t('periph-exec-event-category-label') + '</label>' +
                    '<select class="pe-event-category">' +
                    '<option value="">' + i18n.t('periph-exec-select-category') + '</option></select></div>' +
                    '<div class="fb-form-group"><label>' + i18n.t('periph-exec-event-label') + '</label>' +
                    '<select class="pe-event"><option value="">' + i18n.t('periph-exec-select-event') + '</option></select></div>' +
                    '<div class="fb-form-group pe-event-condition-group' + this._hiddenClass(isDataSourceEvent) + '"><label>' + (i18n.t('periph-exec-event-operator-label') || '比较操作符') + '</label>' +
                    '<select class="pe-event-operator">' +
                    '<option value="0"' + (opVal === '0' ? ' selected' : '') + '>' + i18n.t('periph-exec-op-eq') + '</option>' +
                    '<option value="1"' + (opVal === '1' ? ' selected' : '') + '>' + i18n.t('periph-exec-op-neq') + '</option>' +
                    '<option value="2"' + (opVal === '2' ? ' selected' : '') + '>' + i18n.t('periph-exec-op-gt') + '</option>' +
                    '<option value="3"' + (opVal === '3' ? ' selected' : '') + '>' + i18n.t('periph-exec-op-lt') + '</option>' +
                    '<option value="4"' + (opVal === '4' ? ' selected' : '') + '>' + i18n.t('periph-exec-op-gte') + '</option>' +
                    '<option value="5"' + (opVal === '5' ? ' selected' : '') + '>' + i18n.t('periph-exec-op-lte') + '</option>' +
                    '<option value="6"' + (opVal === '6' ? ' selected' : '') + '>' + i18n.t('periph-exec-op-between') + '</option>' +
                    '<option value="7"' + (opVal === '7' ? ' selected' : '') + '>' + i18n.t('periph-exec-op-not-between') + '</option>' +
                    '<option value="8"' + (opVal === '8' ? ' selected' : '') + '>' + i18n.t('periph-exec-op-contain') + '</option>' +
                    '<option value="9"' + (opVal === '9' ? ' selected' : '') + '>' + i18n.t('periph-exec-op-not-contain') + '</option>' +
                    '</select></div>' +
                    '<div class="fb-form-group pe-event-compare-group' + this._hiddenClass(isDataSourceEvent) + '"><label>' + (i18n.t('periph-exec-event-compare-label') || '比较值') + '</label>' +
                    '<input type="text" class="pe-event-compare" value="' + escapeHtml(data.compareValue || '') + '" placeholder="' + escapeHtml(i18n.t('periph-exec-event-compare-hint') || '如: 25.5') + '"></div>' +
                    '</div>' +
                    '<div class="pe-event-modbus-ctrl-panel' + this._hiddenClass(isModbusCtrlEvent) + '"></div>' +
                    '</div>';
            container.appendChild(div);
            this._populateTriggerPeriphSelect(div.querySelector('.pe-trigger-periph'), data.triggerPeriphId || data.sourcePeriphId || '');
            if (showEvent) this._populateEventCategoriesInBlock(div, data.eventId, data.compareValue || '');
        },

        _createPeriphExecActionElement(data, index) {
            const container = document.getElementById('periph-exec-actions');
            if (!container) return;
            const div = document.createElement('div');
            div.className = 'periph-exec-config-item';
            div.dataset.index = index;
            const isPollMode = this._isPollTriggerActive();
            const actionType = String(isPollMode ? 18 : (data.actionType ?? 0));
            const actionTypeInt = parseInt(actionType);
            const isModbusPoll = actionTypeInt === 18;
            const isSensorRead = actionTypeInt === 19;
            const isModbusTarget = data.targetPeriphId && data.targetPeriphId.indexOf('modbus:') === 0;
            const showPeriphGroup = isPollMode || (isModbusTarget || !((actionTypeInt >= 6 && actionTypeInt <= 11) || actionTypeInt === 15 || isModbusPoll));
            const needsValue = !isPollMode && !isModbusTarget && (actionTypeInt >= 2 && actionTypeInt <= 5);
            const showRecv = !isPollMode && !isModbusTarget && this._hasSetModeTrigger() && needsValue;
            const isScript = !isPollMode && actionTypeInt === 15;
            const showActionType = !isPollMode && !isModbusTarget;
            const showExecRow = true;
            const execMode = parseInt(data.execMode ?? 0);
            const sel = (v) => actionType === String(v) ? 'selected' : '';
            var sensorCfg = {sensorCategory:'analog',periphId:'',scaleFactor:1,offset:0,decimalPlaces:2,sensorLabel:'',unit:''};
            if (isSensorRead && data.actionValue) { try { Object.assign(sensorCfg, JSON.parse(data.actionValue)); } catch(e) {} }
            const selCat = (v) => sensorCfg.sensorCategory === v ? 'selected' : '';
            div.innerHTML = '<span class="mqtt-topic-index">' + (index + 1) + '</span>' +
                '<button type="button" class="mqtt-topic-delete">' + i18n.t('peripheral-delete') + '</button>' +
                '<div class="pe-action-grid">' +
                    '<div class="pe-exec-row pe-span-all' + this._hiddenClass(showExecRow) + '">' +
                    '<div class="fb-form-group pe-exec-mode-group">' +
                    '<label>' + i18n.t('periph-exec-exec-mode-label') + '</label>' +
                    '<select class="pe-exec-mode">' +
                    '<option value="0"' + (execMode === 0 ? ' selected' : '') + '>' + i18n.t('periph-exec-exec-mode-async') + '</option>' +
                    '<option value="1"' + (execMode === 1 ? ' selected' : '') + '>' + i18n.t('periph-exec-exec-mode-sync') + '</option>' +
                    '</select></div>' +
                    '<div class="fb-form-group pe-sync-delay-group">' +
                    '<label>' + i18n.t('periph-exec-sync-delay-label') + '</label>' +
                    '<input type="number" class="pe-sync-delay" min="0" max="10000" step="100" value="' + (data.syncDelayMs || 0) + '" placeholder="0"></div></div>' +
                    '<div class="fb-form-group pe-target-group' + this._hiddenClass(showPeriphGroup) + '">' +
                    '<label>' + i18n.t('periph-exec-target-periph-label') + '</label>' +
                    '<select class="pe-target-periph"><option value="">' + i18n.t('periph-exec-select-periph') + '</option></select></div>' +
                    '<div class="fb-form-group pe-modbus-ctrl-panel' + this._hiddenClass(isModbusTarget) + '"></div>' +
                    '<div class="fb-form-group pe-action-type-group' + this._hiddenClass(showActionType) + '"><label>' + i18n.t('periph-exec-action-type-label') + '</label>' +
                    '<select class="pe-action-type">' +
                        '<optgroup label="' + i18n.t('periph-exec-action-cat-gpio') + '">' +
                        '<option value="0" ' + sel(0) + '>' + i18n.t('periph-exec-action-high') + '</option>' +
                        '<option value="1" ' + sel(1) + '>' + i18n.t('periph-exec-action-low') + '</option>' +
                        '<option value="2" ' + sel(2) + '>' + i18n.t('periph-exec-action-blink') + '</option>' +
                        '<option value="3" ' + sel(3) + '>' + i18n.t('periph-exec-action-breathe') + '</option>' +
                        '<option value="13" ' + sel(13) + '>' + i18n.t('periph-exec-action-high-inverted') + '</option>' +
                        '<option value="14" ' + sel(14) + '>' + i18n.t('periph-exec-action-low-inverted') + '</option></optgroup>' +
                        '<optgroup label="' + i18n.t('periph-exec-action-cat-analog') + '">' +
                        '<option value="4" ' + sel(4) + '>' + i18n.t('periph-exec-action-pwm') + '</option>' +
                        '<option value="5" ' + sel(5) + '>' + i18n.t('periph-exec-action-dac') + '</option></optgroup>' +
                        '<optgroup label="' + i18n.t('periph-exec-action-cat-system') + '">' +
                        '<option value="6" ' + sel(6) + '>' + i18n.t('periph-exec-action-restart') + '</option>' +
                        '<option value="7" ' + sel(7) + '>' + i18n.t('periph-exec-action-factory') + '</option>' +
                        '<option value="8" ' + sel(8) + '>' + i18n.t('periph-exec-action-ntp') + '</option>' +
                        '<option value="9" ' + sel(9) + '>' + i18n.t('periph-exec-action-ota') + '</option>' +
                        '<option value="10" ' + sel(10) + '>' + i18n.t('periph-exec-action-ap') + '</option>' +
                        '<option value="11" ' + sel(11) + '>' + i18n.t('periph-exec-action-ble') + '</option></optgroup>' +
                        '<optgroup label="' + i18n.t('periph-exec-action-cat-advanced') + '">' +
                        '<option value="15" ' + sel(15) + '>' + i18n.t('periph-exec-action-script') + '</option>' +
                        '<option value="19" ' + sel(19) + '>' + i18n.t('periph-exec-action-sensor-read') + '</option></optgroup>' +
                    '</select></div>' +
                    '<div class="fb-form-group pe-action-value-group' + this._hiddenClass(needsValue) + '">' +
                    '<label>' + i18n.t('periph-exec-action-value-label') + '</label>' +
                    '<input type="text" class="pe-action-value" value="' + (isScript ? '' : escapeHtml(data.actionValue)) + '" placeholder="' + escapeHtml(i18n.t('periph-exec-action-value-hint')) + '"' + (showRecv && data.useReceivedValue !== false ? ' readonly' : '') + '>' +
                    '<small class="pe-help-text">' + i18n.t('periph-exec-action-value-help') + '</small></div>' +
                    '<div class="fb-form-group pe-use-received-value-group' + this._hiddenClass(showRecv) + '">' +
                    '<label class="pe-checkbox-label pe-check-align"><input type="checkbox" class="pe-use-received-value"' + (showRecv && data.useReceivedValue !== false ? ' checked' : '') + '>' +
                    i18n.t('periph-exec-use-received-value-help') + '</label></div>' +
                    '<div class="fb-form-group pe-script-group pe-span-all' + this._hiddenClass(isScript) + '">' +
                    '<label>' + i18n.t('periph-exec-script-label') + '</label>' +
                    '<textarea class="pe-script pe-script-textarea" rows="6" maxlength="1024" placeholder="' + escapeHtml(i18n.t('periph-exec-script-placeholder')) + '">' + (isScript ? escapeHtml(data.actionValue) : '') + '</textarea>' +
                    '<small class="pe-help-text">' + i18n.t('periph-exec-script-help') + '</small></div>' +
                    '<div class="fb-form-group pe-poll-tasks-group pe-span-all' + this._hiddenClass(!isPollMode && isModbusPoll && !isModbusTarget) + '">' +
                    '<label>' + i18n.t('periph-exec-poll-tasks-label') + '</label>' +
                    '<div class="pe-poll-tasks-list"></div>' +
                    '<small class="pe-help-text">' + i18n.t('periph-exec-poll-tasks-help') + '</small></div>' +
                    '<div class="fb-form-group pe-sensor-group pe-span-all' + this._hiddenClass(isSensorRead) + '">' +
                    '<div class="pe-sensor-config-grid">' +
                    '<div class="fb-form-group"><label>' + i18n.t('periph-exec-sensor-category') + '</label>' +
                    '<select class="pe-sensor-category">' +
                    '<option value="analog" ' + selCat('analog') + '>' + i18n.t('periph-exec-sensor-cat-analog') + '</option>' +
                    '<option value="digital" ' + selCat('digital') + '>' + i18n.t('periph-exec-sensor-cat-digital') + '</option>' +
                    '<option value="pulse" ' + selCat('pulse') + '>' + i18n.t('periph-exec-sensor-cat-pulse') + '</option></select></div>' +
                    '<div class="fb-form-group"><label>' + i18n.t('periph-exec-sensor-scale') + '</label><input type="number" class="pe-sensor-scale" step="any" value="' + sensorCfg.scaleFactor + '"></div>' +
                    '<div class="fb-form-group"><label>' + i18n.t('periph-exec-sensor-offset') + '</label><input type="number" class="pe-sensor-offset" step="any" value="' + sensorCfg.offset + '"></div>' +
                    '<div class="fb-form-group"><label>' + i18n.t('periph-exec-sensor-decimals') + '</label><input type="number" class="pe-sensor-decimals" min="0" max="6" value="' + sensorCfg.decimalPlaces + '"></div>' +
                    '<div class="fb-form-group"><label>' + i18n.t('periph-exec-sensor-label') + '</label><input type="text" class="pe-sensor-label" value="' + escapeHtml(sensorCfg.sensorLabel) + '" placeholder="' + i18n.t('periph-exec-sensor-label') + '"></div>' +
                    '<div class="fb-form-group"><label>' + i18n.t('periph-exec-sensor-unit') + '</label><input type="text" class="pe-sensor-unit" value="' + escapeHtml(sensorCfg.unit) + '" maxlength="8" placeholder="°C, %, V..."></div>' +
                    '</div></div>' +
                    '</div>';
            container.appendChild(div);
            if (!isPollMode && isSensorRead) {
                this._populateSensorPeriphSelect(div, sensorCfg.sensorCategory || 'analog', sensorCfg.periphId || data.targetPeriphId || '');
            } else {
                this._populatePeriphSelect(div.querySelector('.pe-target-periph'), data.targetPeriphId || '', isPollMode);
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
            this._setSectionVisible(triggerPeriphGroup, val === '0');
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
                const needsValue = (actionType >= 2 && actionType <= 5);
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
        },

        onPeriphExecEventChangeInBlock(val, index) {
            const block = this._getPeriphExecBlock('periph-exec-triggers', index);
            if (!block) return;
            var isDataSource = val && val.indexOf('ds:') === 0;
            var isModbusCtrl = val && val.indexOf('mc:') === 0;
            this._setSectionVisible(block.querySelector('.pe-event-condition-group'), isDataSource);
            this._setSectionVisible(block.querySelector('.pe-event-compare-group'), isDataSource);
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
            const scriptGroup = block.querySelector('.pe-script-group');
            const pollTasksGroup = block.querySelector('.pe-poll-tasks-group');
            const isModbusPoll = actionType === 18;
            const isSensorRead = actionType === 19;
            const showTargetGroup = !((actionType >= 6 && actionType <= 11) || actionType === 15 || isModbusPoll);
            this._setSectionVisible(targetGroup, showTargetGroup);
            const needsValue = (actionType >= 2 && actionType <= 5);
            this._setSectionVisible(valueGroup, needsValue);
            this._setSectionVisible(scriptGroup, actionType === 15);
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
                this._populatePeriphSelect(peSelect, peSelect ? peSelect.value : '', this._isPollTriggerActive());
            }
            // 使用接收值: 仅在平台触发+设置+需要数值的动作类型时显示
            const recvGroup = block.querySelector('.pe-use-received-value-group');
            const isSetMode = this._hasSetModeTrigger();
            const showRecv = isSetMode && needsValue;
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

        _populateEventCategoriesInBlock(blockEl, eventIdToSet, compareValue) {
            const sel = blockEl.querySelector('.pe-event-category');
            if (!sel) return;
            var isDs = eventIdToSet && String(eventIdToSet).indexOf('ds:') === 0;
            var isMc = eventIdToSet && String(eventIdToSet).indexOf('mc:') === 0;
            apiGet('/api/periph-exec/events/categories').then(res => {
                if (!res || !res.success || !res.data) return;
                let opts = '<option value="">' + i18n.t('periph-exec-select-category') + '</option>';
                res.data.forEach(cat => {
                    const translatedCat = i18n.t('event-cat-' + cat.name) || cat.name;
                    opts += '<option value="' + cat.name + '">' + translatedCat + '</option>';
                });
                sel.innerHTML = opts;
                if (isDs || isMc) {
                    sel.value = 'Modbus子设备';
                    this._populateEventSelectInBlock(blockEl, 'Modbus子设备', eventIdToSet, compareValue);
                } else {
                    this._populateEventSelectInBlock(blockEl, '', eventIdToSet, compareValue);
                }
            });
        },

        _populateEventSelectInBlock(blockEl, categoryFilter, eventIdToSet, compareValue) {
            const sel = blockEl.querySelector('.pe-event');
            if (!sel) return;
            // Modbus子设备类别：从协议配置中提取，不从API加载
            if (categoryFilter === 'Modbus子设备') {
                var sources = this._peDataSources || [];
                var ctrlDevices = this._modbusDevices || [];
                var opts = '<option value="">' + i18n.t('periph-exec-select-event') + '</option>';
                if (sources.length > 0) {
                    var acqCatLabel = i18n.t('event-cat-modbus-acquisition') || '采集设备';
                    opts += '<optgroup label="' + acqCatLabel + '">';
                    sources.forEach(function(ds) {
                        opts += '<option value="ds:' + ds.id + '">' + escapeHtml(ds.label) + '</option>';
                    });
                    opts += '</optgroup>';
                }
                if (ctrlDevices.length > 0) {
                    var ctrlCatLabel = i18n.t('event-cat-modbus-control') || '控制设备';
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
            apiGet('/api/periph-exec/events/static').then(staticRes => {
                return apiGet('/api/periph-exec/events/dynamic').then(dynamicRes => {
                    let allEvents = [];
                    if (staticRes && staticRes.success && staticRes.data) allEvents = allEvents.concat(staticRes.data);
                    if (dynamicRes && dynamicRes.success && dynamicRes.data) allEvents = allEvents.concat(dynamicRes.data);
                    if (categoryFilter) allEvents = allEvents.filter(e => e.category === categoryFilter);
                    const categories = {};
                    allEvents.forEach(e => {
                        if (!categories[e.category]) categories[e.category] = [];
                        categories[e.category].push(e);
                    });
                    let opts = '<option value="">' + i18n.t('periph-exec-select-event') + '</option>';
                    for (const cat in categories) {
                        const translatedCat = i18n.t('event-cat-' + cat) || cat;
                        opts += '<optgroup label="' + translatedCat + '">';
                        categories[cat].forEach(e => {
                            const translatedName = e.isDynamic ? e.name : (i18n.t('event-' + e.id) || e.name);
                            opts += '<option value="' + e.id + '">' + translatedName + '</option>';
                        });
                        opts += '</optgroup>';
                    }
                    // 未选分类时，追加Modbus子设备选项
                    if (!categoryFilter) {
                        var sources = this._peDataSources || [];
                        var ctrlDevices = this._modbusDevices || [];
                        if (sources.length > 0 || ctrlDevices.length > 0) {
                            var dsCatLabel = i18n.t('event-cat-Modbus子设备') || 'Modbus子设备';
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
                });
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
                } else if (actionType === '15') {
                    action.actionValue = item.querySelector('.pe-script')?.value || '';
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
                    action.actionValue = JSON.stringify({
                        periphId: sensorPeriphId,
                        sensorCategory: sensorCat,
                        scaleFactor: scaleFactor,
                        offset: sOffset,
                        decimalPlaces: decimals,
                        sensorLabel: sensorLabel,
                        unit: sUnit
                    });
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
                this.showInlineError(errEl, i18n.t('periph-exec-validate-name'));
                return;
            }

            const triggers = this._collectPeriphExecTriggers();
            ruleData.triggers = triggers;

            const actions = this._collectPeriphExecActions();
            ruleData.actions = actions;
            for (let i = 0; i < actions.length; i++) {
                if (actions[i].actionType === 15) {
                    if (!actions[i].actionValue.trim()) {
                        this.showInlineError(errEl, i18n.t('periph-exec-script-empty'));
                        return;
                    }
                    if (actions[i].actionValue.length > 1024) {
                        this.showInlineError(errEl, i18n.t('periph-exec-script-too-long'));
                        return;
                    }
                }
                if (actions[i].actionType === 19) {
                    var sv = {};
                    try { sv = JSON.parse(actions[i].actionValue); } catch(e) {}
                    if (!sv.periphId) {
                        this.showInlineError(errEl, i18n.t('periph-exec-sensor-no-periph'));
                        return;
                    }
                }
            }

            if (isEdit) ruleData.id = originalId;
            const url = isEdit ? '/api/periph-exec/update' : '/api/periph-exec';
            apiPostJson(url, ruleData)
                .then(res => {
                    if (res && res.success) {
                        Notification.success(i18n.t(isEdit ? 'periph-exec-update-ok' : 'periph-exec-add-ok'), i18n.t('periph-exec-title'));
                        this.closePeriphExecModal();
                        if (this.currentPage === 'periph-exec') this.loadPeriphExecPage();
                    } else {
                        this.showInlineError(errEl, res?.error || i18n.t('periph-exec-save-fail'));
                    }
                })
                .catch(err => {
                    console.error('Save periph exec rule failed:', err);
                    const isNetworkError = err && (
                        err.name === 'TypeError' ||
                        (err.message && (
                            err.message.includes('Failed to fetch') ||
                            err.message.includes('fetch') ||
                            err.message.includes('network')
                        ))
                    );
                    this.showInlineError(errEl, isNetworkError ? i18n.t('device-offline-error') : i18n.t('periph-exec-save-fail'));
                });
        }

    });
})();
