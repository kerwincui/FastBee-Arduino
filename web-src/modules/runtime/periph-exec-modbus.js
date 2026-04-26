/**
 * 外设执行管理 - Modbus 专用模块
 * Modbus 设备选择、寄存器配置、轮询设置、风险提示等
 * 依赖: periph-exec.js (核心模块) 先加载
 */
(function() {
    Object.assign(AppState, {

        // ============ Risk notice / Modbus health ============

        _clearPeriphExecRiskNotice() {
            var noteEl = document.getElementById('periph-exec-risk-note');
            if (!noteEl) return;
            noteEl.classList.add('is-hidden');
            noteEl.innerHTML = '';
        },

        _getPeriphExecCachedHealth() {
            if (this._peModbusHealth) return this._peModbusHealth;
            return this._modbusStatus && this._modbusStatus.health ? this._modbusStatus.health : null;
        },

        _ensurePeriphExecModbusHealth(forceRefresh) {
            var cached = this._getPeriphExecCachedHealth();
            var now = Date.now();
            if (!forceRefresh && cached && this._peModbusHealthFetchedAt > 0 && (now - this._peModbusHealthFetchedAt) < 15000) {
                return Promise.resolve(cached);
            }
            if (!forceRefresh && this._peModbusHealthPromise) {
                return this._peModbusHealthPromise;
            }
            this._peModbusHealthPromise = apiGetSilent('/api/modbus/status')
                .then((res) => {
                    var health = res && res.success && res.data ? (res.data.health || null) : null;
                    this._peModbusHealth = health;
                    this._peModbusHealthFetchedAt = health ? Date.now() : 0;
                    return health;
                })
                .catch(() => {
                    this._peModbusHealth = null;
                    this._peModbusHealthFetchedAt = 0;
                    return null;
                })
                .finally(() => {
                    this._peModbusHealthPromise = null;
                    this._refreshPeriphExecRiskNotice({ allowFetch: false });
                });
            return this._peModbusHealthPromise;
        },

        _getPeriphExecRiskMeta(level) {
            switch (String(level || 'medium').toLowerCase()) {
                case 'high':
                    return { text: i18n.t('modbus-master-risk-high'), className: 'modbus-risk-high' };
                case 'low':
                    return { text: i18n.t('modbus-master-risk-low'), className: 'modbus-risk-low' };
                default:
                    return { text: i18n.t('modbus-master-risk-medium'), className: 'modbus-risk-medium' };
            }
        },

        _formatPeriphExecAge(ageSec) {
            var age = Number(ageSec || 0);
            if (!age) return '--';
            if (age < 60) return age + 's';
            if (age < 3600) return Math.floor(age / 60) + 'm';
            return Math.floor(age / 3600) + 'h';
        },

        _formatPeriphExecPercent(value) {
            var num = Number(value || 0);
            if (!isFinite(num)) num = 0;
            return (Math.round(num * 10) / 10) + '%';
        },

        _buildPeriphExecFormWarnings(triggers) {
            var warnings = [];
            (triggers || []).filter(function(trigger) {
                return trigger && trigger.triggerType === 5;
            }).forEach(function(trigger) {
                var intervalSec = Number(trigger.intervalSec || 0);
                var timeoutMs = Number(trigger.pollResponseTimeout || 0);
                var retries = Number(trigger.pollMaxRetries || 0);
                var interDelayMs = Number(trigger.pollInterPollDelay || 0);
                if (intervalSec > 0 && intervalSec < 5) {
                    warnings.push(i18n.t('periph-exec-poll-interval-label') + ' < 5s');
                }
                if (timeoutMs > 3000) {
                    warnings.push(i18n.t('periph-exec-poll-timeout-label') + ' > 3000ms');
                }
                if (retries > 2) {
                    warnings.push(i18n.t('periph-exec-poll-retries-label') + ' > 2');
                }
                if (interDelayMs > 0 && interDelayMs < 100) {
                    warnings.push(i18n.t('periph-exec-poll-delay-label') + ' < 100ms');
                }
            });
            return warnings.filter(function(msg, idx, list) {
                return msg && list.indexOf(msg) === idx;
            });
        },

        _resolvePeriphExecRiskLevel(formWarningCount, health) {
            var current = health && health.riskLevel ? String(health.riskLevel).toLowerCase() : 'low';
            if (formWarningCount >= 2) return 'high';
            if (formWarningCount >= 1 && current === 'low') return 'medium';
            if (current === 'low') return 'medium';
            return current;
        },

        _refreshPeriphExecRiskNotice(options) {
            var noteEl = document.getElementById('periph-exec-risk-note');
            if (!noteEl) return;

            var triggers = this._collectPeriphExecTriggers();
            var actions = this._collectPeriphExecActions();
            var hasPollTrigger = triggers.some(function(trigger) {
                return trigger && trigger.triggerType === 5;
            });
            var hasModbusPollAction = actions.some(function(action) {
                return action && action.actionType === 18;
            });

            if (!(hasPollTrigger && hasModbusPollAction)) {
                noteEl.classList.add('is-hidden');
                noteEl.innerHTML = '';
                return;
            }

            var health = this._getPeriphExecCachedHealth();
            if (!health && (!options || options.allowFetch !== false)) {
                this._ensurePeriphExecModbusHealth(false);
            }

            var formWarnings = this._buildPeriphExecFormWarnings(triggers);
            var runtimeWarnings = health && Array.isArray(health.warnings) ? health.warnings.slice(0, 2) : [];
            var warnings = formWarnings.concat(runtimeWarnings).filter(function(msg, idx, list) {
                return msg && list.indexOf(msg) === idx;
            }).slice(0, 4);
            var riskMeta = this._getPeriphExecRiskMeta(this._resolvePeriphExecRiskLevel(formWarnings.length, health));

            var html = '<div class="u-header-between-wrap u-mb-12">' +
                '<h4 class="u-note-title u-note-title-warning u-mb-0"><i class="fas fa-exclamation-triangle"></i> ' +
                escapeHtml(i18n.t('periph-exec-risk-title')) + '</h4>' +
                '<span class="modbus-risk-badge ' + riskMeta.className + '">' + escapeHtml(riskMeta.text) + '</span>' +
                '</div>' +
                '<p class="u-text-secondary u-mb-0">' + escapeHtml(i18n.t('periph-exec-risk-body')) + '</p>';

            if (health) {
                html += '<div class="modbus-health-metrics u-mb-12">' +
                    '<span class="modbus-health-metric"><span class="u-text-muted u-fs-12">' + escapeHtml(i18n.t('modbus-master-enabled-tasks')) + '</span><strong>' + escapeHtml(String(health.enabledTaskCount ?? 0)) + '</strong></span>' +
                    '<span class="modbus-health-metric"><span class="u-text-muted u-fs-12">' + escapeHtml(i18n.t('modbus-master-min-interval')) + '</span><strong>' + escapeHtml(health.minPollInterval ? (health.minPollInterval + 's') : '--') + '</strong></span>' +
                    '<span class="modbus-health-metric"><span class="u-text-muted u-fs-12">' + escapeHtml(i18n.t('modbus-master-last-poll-age')) + '</span><strong>' + escapeHtml(this._formatPeriphExecAge(health.lastPollAgeSec)) + '</strong></span>' +
                    '<span class="modbus-health-metric"><span class="u-text-muted u-fs-12">' + escapeHtml(i18n.t('modbus-master-timeout-rate')) + '</span><strong>' + escapeHtml(this._formatPeriphExecPercent(health.timeoutRate)) + '</strong></span>' +
                    '</div>';
            }

            if (warnings.length > 0) {
                html += '<div class="modbus-health-warnings">' + warnings.map(function(msg) {
                    return '<div class="modbus-health-warning"><i class="fas fa-exclamation-triangle"></i><span>' +
                        escapeHtml(msg || '') + '</span></div>';
                }).join('') + '</div>';
            }

            html += '<p class="u-note-tip">' + escapeHtml(i18n.t('periph-exec-risk-guard')) + '</p>';
            noteEl.innerHTML = html;
            noteEl.classList.remove('is-hidden');
        },

        // ============ Periph select with Modbus groups ============

        _populatePeriphSelect(selectEl, selectedValue, pollOnly) {
            if (!selectEl) return;
            var html = '<option value="">' + i18n.t('periph-exec-select-periph') + '</option>';
            if (pollOnly) {
                // 轮询触发模式: 仅显示 Modbus 采集类子设备
                var tasks = this._masterTasks || [];
                var fcNames = {1: 'FC01', 2: 'FC02', 3: 'FC03', 4: 'FC04'};
                var hasTask = false;
                for (var ti = 0; ti < tasks.length; ti++) { if (tasks[ti].enabled !== false) { hasTask = true; break; } }
                if (hasTask) {
                    html += '<optgroup label="' + escapeHtml(i18n.t('modbus-type-sensor') || '采集设备') + '">';
                    for (var i = 0; i < tasks.length; i++) {
                        var t = tasks[i];
                        if (t.enabled === false) continue;
                        var label = t.name || t.label || ('Slave ' + (t.slaveAddress || 1));
                        var desc = (fcNames[t.functionCode] || 'FC03') + ' @' + (t.startAddress || 0) + ' x' + (t.quantity || 10);
                        var val = 'modbus-task:' + i;
                        html += '<option value="' + escapeHtml(val) + '">' + escapeHtml(label + ' (' + desc + ')') + '</option>';
                    }
                    html += '</optgroup>';
                }
            } else {
                var gpioPeriphs = [];
                (this._pePeripherals || []).forEach(p => {
                    if (p.type !== 51) gpioPeriphs.push(p);
                });
                var modbusDevices = this._modbusDevices || [];
                var typeLabels = {relay: i18n.t('modbus-type-relay') || '继电器', pwm: i18n.t('modbus-type-pwm') || 'PWM', pid: i18n.t('modbus-type-pid') || 'PID', motor: i18n.t('modbus-type-motor') || '步进电机'};
                if (gpioPeriphs.length > 0) {
                    html += '<optgroup label="' + escapeHtml(i18n.t('periph-exec-periph-group') || '硬件外设') + '">';
                    gpioPeriphs.forEach(p => {
                        html += '<option value="' + escapeHtml(p.id) + '">' + escapeHtml(p.name + ' (' + p.id + ')') + '</option>';
                    });
                    html += '</optgroup>';
                }
                var hasModbus = false;
                for (var mi = 0; mi < modbusDevices.length; mi++) {
                    if (modbusDevices[mi].enabled !== false) { hasModbus = true; break; }
                }
                if (hasModbus) {
                    html += '<optgroup label="' + escapeHtml(i18n.t('periph-exec-modbus-group') || 'Modbus 子设备') + '">';
                    for (var j = 0; j < modbusDevices.length; j++) {
                        var d = modbusDevices[j];
                        if (d.enabled === false) continue;
                        var devId = 'modbus:' + j;
                        var dt = typeLabels[d.deviceType] || d.deviceType || '';
                        var label = (d.name || ('Device ' + (j + 1))) + ' (' + dt + ', Addr ' + (d.slaveAddress || 1) + ')';
                        html += '<option value="' + escapeHtml(devId) + '">' + escapeHtml(label) + '</option>';
                    }
                    html += '</optgroup>';
                }
            }
            selectEl.innerHTML = html;
            if (selectedValue) selectEl.value = selectedValue;
        },

        // ============ Data sources ============

        _extractDataSources(protocolConfig) {
            const sources = [];
            if (!protocolConfig) return sources;
            const rtu = protocolConfig.modbusRtu;
            if (rtu && rtu.enabled && rtu.master && rtu.master.tasks) {
                rtu.master.tasks.forEach(task => {
                    if (!task.enabled || !task.mappings) return;
                    task.mappings.forEach(m => {
                        if (m.sensorId) sources.push({ id: m.sensorId, label: (task.name || task.label || 'Modbus') + '/' + m.sensorId });
                    });
                });
            }
            const tcp = protocolConfig.modbusTcp;
            if (tcp && tcp.enabled && tcp.master && tcp.master.tasks) {
                tcp.master.tasks.forEach(task => {
                    if (!task.enabled || !task.mappings) return;
                    task.mappings.forEach(m => {
                        if (m.sensorId) sources.push({ id: m.sensorId, label: 'TCP/' + m.sensorId });
                    });
                });
            }
            return sources;
        },

        // ============ Sensor periph select ============

        _populateSensorPeriphSelect(blockEl, category, selectedValue) {
            var sel = blockEl.querySelector('.pe-target-periph');
            if (!sel) return;
            var periphs = this._pePeripherals || [];
            var analogTypes = [15, 26]; var digitalTypes = [11, 13, 14]; var pulseTypes = [46];
            var allowedTypes;
            if (category === 'digital') allowedTypes = digitalTypes;
            else if (category === 'pulse') allowedTypes = pulseTypes;
            else allowedTypes = analogTypes;
            var prev = selectedValue || sel.value;
            sel.innerHTML = '<option value="">--</option>';
            periphs.filter(function(p) { return allowedTypes.indexOf(p.type) >= 0; }).forEach(function(p) {
                var opt = document.createElement('option');
                opt.value = p.id;
                var pinInfo = (p.pins && p.pins.length > 0) ? (' (Pin ' + p.pins[0] + ')') : '';
                opt.textContent = p.name + pinInfo;
                sel.appendChild(opt);
            });
            if (prev) sel.value = prev;
        },

        onSensorCategoryChange(selectEl, index) {
            var block = this._getPeriphExecBlock('periph-exec-actions', index);
            if (!block) return;
            this._populateSensorPeriphSelect(block, selectEl.value);
        },

        // ============ Poll tasks / Modbus device panel ============

        // 轮询触发专用: 仅显示采集传感器任务(无控制设备)
        _populatePollTasksOnly(container, actionValue) {
            if (!container) return;
            var tasks = this._masterTasks || [];
            if (tasks.length === 0) {
                container.innerHTML = '<span class="pe-empty-inline">' + (i18n.t('modbus-no-tasks') || '暂无采集任务') + '</span>';
                return;
            }
            // 解析已选任务索引
            var selTaskIdx = -1;
            if (actionValue) {
                if (actionValue.charAt(0) === '{') {
                    try { var p = JSON.parse(actionValue); if (p.poll && p.poll.length > 0) selTaskIdx = p.poll[0]; } catch(e) {}
                } else {
                    var parts = actionValue.split(',');
                    if (parts.length > 0 && parts[0].trim()) selTaskIdx = parseInt(parts[0].trim());
                }
            }
            var fcNames = {1: 'FC01', 2: 'FC02', 3: 'FC03', 4: 'FC04'};
            var html = '<div class="pe-modbus-select-grid">';
            html += '<div class="fb-form-group pe-field-stack-compact">';
            html += '<label class="pe-field-label-compact">' + (i18n.t('periph-exec-poll-select-task') || '选择采集任务') + '</label>';
            html += '<select class="pe-poll-task-select u-fs-13">';
            html += '<option value="">--</option>';
            for (var i = 0; i < tasks.length; i++) {
                var t = tasks[i];
                if (t.enabled === false) continue;
                var label = t.name || t.label || ('Slave ' + (t.slaveAddress || 1));
                var desc = (fcNames[t.functionCode] || 'FC03') + ' @' + (t.startAddress || 0) + ' x' + (t.quantity || 10);
                var val = i;
                html += '<option value="' + val + '"' + (val === selTaskIdx ? ' selected' : '') + '>' + escapeHtml(label + ' (' + desc + ')') + '</option>';
            }
            html += '</select></div></div>';
            container.innerHTML = html;
        },

        _populateModbusDevicePanel(container, actionValue) {
            if (!container) return;
            var tasks = this._masterTasks || [];
            if (tasks.length === 0) {
                container.innerHTML = '<span class="pe-empty-inline">' + (i18n.t('modbus-no-devices') || '暂无子设备') + '</span>';
                return;
            }
            var selTaskIdx = '';
            if (actionValue) {
                if (actionValue.charAt(0) === '{') {
                    try {
                        var parsed = JSON.parse(actionValue);
                        if (parsed.poll && parsed.poll.length > 0) selTaskIdx = parsed.poll[0];
                    } catch(e) {}
                } else {
                    var parts = actionValue.split(',');
                    if (parts.length > 0 && parts[0].trim()) selTaskIdx = parseInt(parts[0].trim());
                }
            }
            var fcNames = {1: 'FC01', 2: 'FC02', 3: 'FC03', 4: 'FC04'};
            var html = '<div class="pe-modbus-select-grid">';
            html += '<div class="fb-form-group pe-field-stack-compact">';
            html += '<label class="pe-field-label-compact">' + (i18n.t('periph-exec-poll-select-task') || '选择采集任务') + '</label>';
            html += '<select class="pe-modbus-device-select u-fs-13">';
            html += '<option value="">--</option>';
            for (var i = 0; i < tasks.length; i++) {
                var t = tasks[i]; if (t.enabled === false) continue;
                var label = t.name || t.label || ('Slave ' + (t.slaveAddress || 1));
                var desc = (fcNames[t.functionCode] || 'FC03') + ' @' + (t.startAddress || 0) + ' x' + (t.quantity || 10);
                var val = 'sensor-' + i;
                html += '<option value="' + val + '"' + (val === ('sensor-' + selTaskIdx) ? ' selected' : '') + '>' + escapeHtml(label + ' (' + desc + ')') + '</option>';
            }
            html += '</select></div></div>';
            container.innerHTML = html;
        },

        _onPeriphDeviceSelect(selectEl) {
            // _populateModbusDevicePanel 已精简为仅采集任务选择器，无需动态切换通道/动作面板
        },

        _onPeriphChannelSelect(selectEl) { },

        // ============ Target periph change (Modbus routing) ============

        _onTargetPeriphChange(selectEl, index) {
            var block = this._getPeriphExecBlock('periph-exec-actions', index);
            if (!block) return;
            var periphId = selectEl.value;
            var isModbus = periphId && periphId.indexOf('modbus:') === 0;
            var isModbusTask = periphId && periphId.indexOf('modbus-task:') === 0;
            var actionTypeGroup = block.querySelector('.pe-action-type-group');
            var actionValueGroup = block.querySelector('.pe-action-value-group');
            var recvGroup = block.querySelector('.pe-use-received-value-group');
            var scriptGroup = block.querySelector('.pe-script-group');
            var pollTasksGroup = block.querySelector('.pe-poll-tasks-group');
            var sensorGroup = block.querySelector('.pe-sensor-group');
            var ctrlPanel = block.querySelector('.pe-modbus-ctrl-panel');
            if (isModbusTask) {
                // 采集类子设备：隐藏所有动作配置字段（仅轮询采集，无需控制）
                this._setSectionVisible(actionTypeGroup, false);
                this._setSectionVisible(actionValueGroup, false);
                this._setSectionVisible(recvGroup, false);
                this._setSectionVisible(scriptGroup, false);
                this._setSectionVisible(pollTasksGroup, false);
                this._setSectionVisible(sensorGroup, false);
                this._setSectionVisible(ctrlPanel, false);
            } else if (isModbus && !isModbusTask) {
                this._setSectionVisible(actionTypeGroup, false);
                this._setSectionVisible(actionValueGroup, false);
                this._setSectionVisible(recvGroup, false);
                this._setSectionVisible(scriptGroup, false);
                this._setSectionVisible(pollTasksGroup, false);
                this._setSectionVisible(sensorGroup, false);
                this._showModbusCtrlPanel(ctrlPanel, periphId, '');
            } else {
                this._setSectionVisible(ctrlPanel, false);
                this._setSectionVisible(actionTypeGroup, true);
                var actionType = block.querySelector('.pe-action-type');
                if (actionType) this.onPeriphExecActionTypeChangeInBlock(actionType.value, index);
            }
        },

        // ============ Modbus device / control panels ============

        _getModbusDeviceByPeriphId(periphId) {
            if (!periphId || periphId.indexOf('modbus:') !== 0) return null;
            var idx = parseInt(periphId.substring(7));
            var devices = this._modbusDevices || [];
            return (idx >= 0 && idx < devices.length) ? devices[idx] : null;
        },

        _showModbusCtrlPanel(container, periphId, actionValue) {
            if (!container) return;
            var dev = this._getModbusDeviceByPeriphId(periphId);
            if (!dev) {
                container.innerHTML = '<span class="pe-empty-inline">' + (i18n.t('modbus-device-not-found') || '未找到对应子设备') + '</span>';
                this._setSectionVisible(container, true);
                return;
            }
            var selChannel = ''; var selAction = ''; var selValue = ''; var selParam = '';
            if (actionValue) {
                try {
                    var parsed = JSON.parse(actionValue);
                    if (parsed.ctrl && parsed.ctrl.length > 0) {
                        var c = parsed.ctrl[0];
                        selChannel = String(c.c || 0); selAction = c.a || ''; selValue = String(c.v || 0); selParam = c.p || '';
                    }
                } catch(e) {}
            }
            var html = '<div class="pe-modbus-select-grid">';
            var dt = dev.deviceType || 'relay';
            if (dt !== 'motor') {
                html += '<div class="fb-form-group pe-field-stack-compact">';
                html += '<label class="pe-field-label-compact">' + (i18n.t('periph-exec-select-channel') || '选择通道') + '</label>';
                html += '<select class="pe-modbus-channel-select u-fs-13">';
                for (var ch = 0; ch < (dev.channelCount || 2); ch++) {
                    html += '<option value="' + ch + '"' + (String(ch) === selChannel ? ' selected' : '') + '>CH' + ch + '</option>';
                }
                html += '</select></div>';
            }
            if (dt === 'relay') {
                html += '<div class="fb-form-group pe-field-stack-compact">';
                html += '<label class="pe-field-label-compact">' + (i18n.t('periph-exec-select-action') || '子设备动作') + '</label>';
                html += '<select class="pe-modbus-action-select u-fs-13">' +
                    '<option value="on"' + (selAction === 'off' ? '' : ' selected') + '>' + (i18n.t('periph-exec-ctrl-on') || '打开') + '</option>' +
                    '<option value="off"' + (selAction === 'off' ? ' selected' : '') + '>' + (i18n.t('periph-exec-ctrl-off') || '关闭') + '</option></select>';
                html += '</div>';
            } else if (dt === 'pwm') {
                var maxPwm = dev.pwmResolution === 10 ? 1023 : (dev.pwmResolution === 16 ? 65535 : 255);
                html += '<div class="fb-form-group pe-field-stack-compact">';
                html += '<label class="pe-field-label-compact">' + (i18n.t('periph-exec-ctrl-duty') || '占空比') + '</label>';
                html += '<input type="number" class="pe-modbus-action-value u-fs-13" min="0" max="' + maxPwm + '" value="' + (selValue || 0) + '">';
                html += '</div>';
            } else if (dt === 'pid') {
                html += '<div class="pe-pid-fields">';
                html += '<div class="fb-form-group pe-field-stack-compact">';
                html += '<label class="pe-field-label-compact">' + (i18n.t('periph-exec-ctrl-pid-param') || '参数') + '</label>';
                html += '<select class="pe-modbus-action-param u-fs-13">' +
                    '<option value="P"' + (selParam === 'I' || selParam === 'D' ? '' : ' selected') + '>P</option>' +
                    '<option value="I"' + (selParam === 'I' ? ' selected' : '') + '>I</option>' +
                    '<option value="D"' + (selParam === 'D' ? ' selected' : '') + '>D</option></select>';
                html += '</div>';
                html += '<div class="fb-form-group pe-field-stack-compact">';
                html += '<label class="pe-field-label-compact">' + (i18n.t('periph-exec-ctrl-value') || '值') + '</label>';
                html += '<input type="number" class="pe-modbus-action-value u-fs-13" value="' + (selValue || 0) + '">';
                html += '</div>';
                html += '</div>';
            } else if (dt === 'motor') {
                html += '<div class="fb-form-group pe-field-stack-compact">';
                html += '<label class="pe-field-label-compact">' + (i18n.t('periph-exec-select-action') || '子设备动作') + '</label>';
                html += '<select class="pe-modbus-action-select u-fs-13">' +
                    '<option value="forward"' + (selAction === 'forward' ? ' selected' : '') + '>' + (i18n.t('periph-exec-ctrl-forward') || '正转') + '</option>' +
                    '<option value="reverse"' + (selAction === 'reverse' ? ' selected' : '') + '>' + (i18n.t('periph-exec-ctrl-reverse') || '反转') + '</option>' +
                    '<option value="stop"' + (selAction === 'stop' ? ' selected' : '') + '>' + (i18n.t('periph-exec-ctrl-stop') || '停止') + '</option></select>';
                html += '</div>';
            }
            html += '</div>';
            container.innerHTML = html;
            this._setSectionVisible(container, true);
        },

        _showModbusTriggerCtrlPanel(container, deviceIndex, actionValue) {
            if (!container) return;
            var devices = this._modbusDevices || [];
            var dev = devices[deviceIndex];
            if (!dev) {
                container.innerHTML = '<span class="pe-empty-inline">' + (i18n.t('modbus-device-not-found') || '未找到对应子设备') + '</span>';
                this._setSectionVisible(container, true);
                return;
            }
            var selChannel = ''; var selAction = ''; var selValue = ''; var selParam = '';
            if (actionValue) {
                try {
                    var parsed = JSON.parse(actionValue);
                    if (parsed.ctrl && parsed.ctrl.length > 0) {
                        var c = parsed.ctrl[0];
                        selChannel = String(c.c || 0); selAction = c.a || ''; selValue = String(c.v || 0); selParam = c.p || '';
                    }
                } catch(e) {}
            }
            var html = '<div class="pe-modbus-select-grid">';
            var dt = dev.deviceType || 'relay';
            if (dt !== 'motor') {
                html += '<div class="fb-form-group pe-field-stack-compact">';
                html += '<label class="pe-field-label-compact">' + (i18n.t('periph-exec-match-channel') || '匹配通道') + '</label>';
                html += '<select class="pe-modbus-channel-select u-fs-13">';
                for (var ch = 0; ch < (dev.channelCount || 2); ch++) {
                    html += '<option value="' + ch + '"' + (String(ch) === selChannel ? ' selected' : '') + '>CH' + ch + '</option>';
                }
                html += '</select></div>';
            }
            if (dt === 'relay') {
                html += '<div class="fb-form-group pe-field-stack-compact">';
                html += '<label class="pe-field-label-compact">' + (i18n.t('periph-exec-match-action') || '匹配动作') + '</label>';
                html += '<select class="pe-modbus-action-select u-fs-13">' +
                    '<option value="on"' + (selAction === 'off' ? '' : ' selected') + '>' + (i18n.t('periph-exec-ctrl-on') || '打开') + '</option>' +
                    '<option value="off"' + (selAction === 'off' ? ' selected' : '') + '>' + (i18n.t('periph-exec-ctrl-off') || '关闭') + '</option></select>';
                html += '</div>';
            } else if (dt === 'motor') {
                html += '<div class="fb-form-group pe-field-stack-compact">';
                html += '<label class="pe-field-label-compact">' + (i18n.t('periph-exec-match-action') || '匹配动作') + '</label>';
                html += '<select class="pe-modbus-action-select u-fs-13">' +
                    '<option value="forward"' + (selAction === 'forward' ? ' selected' : '') + '>' + (i18n.t('periph-exec-ctrl-forward') || '正转') + '</option>' +
                    '<option value="reverse"' + (selAction === 'reverse' ? ' selected' : '') + '>' + (i18n.t('periph-exec-ctrl-reverse') || '反转') + '</option>' +
                    '<option value="stop"' + (selAction === 'stop' ? ' selected' : '') + '>' + (i18n.t('periph-exec-ctrl-stop') || '停止') + '</option></select>';
                html += '</div>';
            } else if (dt === 'pwm') {
                var maxPwm = dev.pwmResolution === 10 ? 1023 : (dev.pwmResolution === 16 ? 65535 : 255);
                html += '<div class="fb-form-group pe-field-stack-compact">';
                html += '<label class="pe-field-label-compact">' + (i18n.t('periph-exec-ctrl-duty') || '占空比') + '</label>';
                html += '<input type="number" class="pe-modbus-action-value u-fs-13" min="0" max="' + maxPwm + '" value="' + (selValue || 0) + '">';
                html += '</div>';
            } else if (dt === 'pid') {
                html += '<div class="pe-pid-fields">';
                html += '<div class="fb-form-group pe-field-stack-compact">';
                html += '<label class="pe-field-label-compact">' + (i18n.t('periph-exec-ctrl-pid-param') || '参数') + '</label>';
                html += '<select class="pe-modbus-action-param u-fs-13">' +
                    '<option value="P"' + (selParam === 'I' || selParam === 'D' ? '' : ' selected') + '>P</option>' +
                    '<option value="I"' + (selParam === 'I' ? ' selected' : '') + '>I</option>' +
                    '<option value="D"' + (selParam === 'D' ? ' selected' : '') + '>D</option></select>';
                html += '</div>';
                html += '<div class="fb-form-group pe-field-stack-compact">';
                html += '<label class="pe-field-label-compact">' + (i18n.t('periph-exec-ctrl-value') || '值') + '</label>';
                html += '<input type="number" class="pe-modbus-action-value u-fs-13" value="' + (selValue || 0) + '">';
                html += '</div>';
                html += '</div>';
            }
            html += '</div>';
            container.innerHTML = html;
            this._setSectionVisible(container, true);
        },

        _onCtrlDeviceToggle(checkbox) {
            var panel = checkbox.closest('.pe-ctrl-device-item').querySelector('.pe-ctrl-options');
            this._setSectionVisible(panel, checkbox.checked);
        }

    });
})();
