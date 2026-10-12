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
            this._peModbusHealthPromise = apiGetSilent('/api/modbus/status?compact=1')
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
                    return { text: '高', className: 'modbus-risk-high' };
                case 'low':
                    return { text: '低', className: 'modbus-risk-low' };
                default:
                    return { text: '中', className: 'modbus-risk-medium' };
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
                    warnings.push('轮询间隔 (秒) < 5s');
                }
                if (timeoutMs > 3000) {
                    warnings.push('响应超时 (ms) > 3000ms');
                }
                if (retries > 2) {
                    warnings.push('最大重试次数 > 2');
                }
                if (interDelayMs > 0 && interDelayMs < 100) {
                    warnings.push('请求间隔 (ms) < 100ms');
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
                '<h4 class="u-note-title u-note-title-warning u-mb-0">' +
                escapeHtml('当前组合会放大轮询和总线负载') + '</h4>' +
                '<span class="modbus-risk-badge ' + riskMeta.className + '">' + escapeHtml(riskMeta.text) + '</span>' +
                '</div>' +
                '<p class="u-text-secondary u-mb-0">' + escapeHtml('轮询触发搭配 Modbus 采集动作时，规则执行、串口总线和 Web 服务会同时承压，频率过高时容易出现响应变慢。') + '</p>';

            if (health) {
                html += '<div class="modbus-health-metrics u-mb-12">' +
                    '<span class="modbus-health-metric"><span class="u-text-muted u-fs-12">' + escapeHtml('启用任务') + '</span><strong>' + escapeHtml(String(health.enabledTaskCount ?? 0)) + '</strong></span>' +
                    '<span class="modbus-health-metric"><span class="u-text-muted u-fs-12">' + escapeHtml('最小间隔') + '</span><strong>' + escapeHtml(health.minPollInterval ? (health.minPollInterval + 's') : '--') + '</strong></span>' +
                    '<span class="modbus-health-metric"><span class="u-text-muted u-fs-12">' + escapeHtml('最近轮询') + '</span><strong>' + escapeHtml(this._formatPeriphExecAge(health.lastPollAgeSec)) + '</strong></span>' +
                    '<span class="modbus-health-metric"><span class="u-text-muted u-fs-12">' + escapeHtml('超时率') + '</span><strong>' + escapeHtml(this._formatPeriphExecPercent(health.timeoutRate)) + '</strong></span>' +
                    '</div>';
            }

            if (warnings.length > 0) {
                html += '<div class="modbus-health-warnings">' + warnings.map(function(msg) {
                    return '<div class="modbus-health-warning"><span>' +
                        escapeHtml(msg || '') + '</span></div>';
                }).join('') + '</div>';
            }

            html += '<p class="u-note-tip">' + escapeHtml('服务端已启用节流、自触发保护和参数钗制；建议轮询间隔保持在 5 秒及以上，超时不超过 3000ms，重试不超过 2 次，请求间隔不少于 100ms。') + '</p>';
            noteEl.innerHTML = html;
            noteEl.classList.remove('is-hidden');
        },

        // ============ Periph select with Modbus groups ============

        _populatePeriphSelect(selectEl, selectedValue, pollOnly, eventOnly, execRuleOnly) {
            if (!selectEl) return;
            var html = '<option value="">' + (execRuleOnly ? '-- 选择执行规则 --' : '-- 选择外设 --') + '</option>';
            if (execRuleOnly) {
                // 规则控制动作：仅显示已配置的外设执行规则
                var rules = this._peExecRules || [];
                if (rules.length > 0) {
                    html += '<optgroup label="' + escapeHtml('执行规则') + '">';
                    rules.forEach(function(r) {
                        var stateTxt = r.enabled !== false ? '启用' : '禁用';
                        html += '<option value="' + escapeHtml(r.id) + '">' + escapeHtml((r.name || r.id) + ' [' + stateTxt + ']') + '</option>';
                    });
                    html += '</optgroup>';
                }
                selectEl.innerHTML = html;
                if (selectedValue) selectEl.value = selectedValue;
                return;
            }
            if (eventOnly) {
                // 触发事件动作: 仅显示设备事件外设 (type=60)
                var eventPeriphs = [];
                (this._pePeripherals || []).forEach(p => {
                    if (p.type === 60) eventPeriphs.push(p);
                });
                if (eventPeriphs.length > 0) {
                    html += '<optgroup label="' + escapeHtml('设备事件') + '">';
                    eventPeriphs.forEach(p => {
                        html += '<option value="' + escapeHtml(p.id) + '">' + escapeHtml(p.name + ' (' + p.id + ')') + '</option>';
                    });
                    html += '</optgroup>';
                }
                selectEl.innerHTML = html;
                if (selectedValue) selectEl.value = selectedValue;
                return;
            }
            if (pollOnly) {
                // 轮询触发模式: 仅显示 Modbus 采集类子设备
                var tasks = this._masterTasks || [];
                var fcNames = {1: 'FC01', 2: 'FC02', 3: 'FC03', 4: 'FC04'};
                var hasTask = false;
                for (var ti = 0; ti < tasks.length; ti++) { if (tasks[ti].enabled !== false) { hasTask = true; break; } }
                if (hasTask) {
                    html += '<optgroup label="' + escapeHtml('采集设备') + '">';
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
                var typeLabels = {relay: '继电器', pwm: 'PWM', pid: 'PID', motor: '步进电机'};
                if (gpioPeriphs.length > 0) {
                    html += '<optgroup label="' + escapeHtml('硬件外设') + '">';
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
                    html += '<optgroup label="' + escapeHtml('Modbus 子设备') + '">';
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
            const isTransparent = rtu && (rtu.transferType === 1);
            if (rtu && rtu.enabled && rtu.master) {
                // 采集任务（tasks）
                if (rtu.master.tasks) {
                    rtu.master.tasks.forEach(task => {
                        if (!task.enabled) return;
                        if (task.mappings && task.mappings.length > 0) {
                            // JSON 模式或透传模式下有映射的任务：从映射提取数据源
                            task.mappings.forEach(m => {
                                if (m.sensorId) sources.push({ id: m.sensorId, label: (task.name || task.label || 'Modbus') + '/' + m.sensorId });
                            });
                        } else if (isTransparent) {
                            // 透传模式下无映射的任务：将整个任务作为数据源
                            var taskId = task.name || task.label || ('Slave' + (task.slaveAddress || 1));
                            var dsId = 'modbus_raw_' + (task.slaveAddress || 0);
                            sources.push({ id: dsId, label: taskId + ' (透传)' });
                        }
                    });
                }
                // 控制设备（devices）不在 _peDataSources 中重复添加
                // 它们通过 _modbusDevices 在事件下拉列表的「控制设备」分组中展示
            }
            return sources;
        },

        // ============ Poll tasks / Modbus device panel ============

        // 轮询触发专用: 仅显示采集传感器任务(无控制设备)

        _populateModbusDevicePanel(container, actionValue) {
            if (!container) return;
            var tasks = this._masterTasks || [];
            if (tasks.length === 0) {
                container.innerHTML = '<span class="pe-empty-inline">暂无子设备</span>';
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
            html += '<label class="pe-field-label-compact">选择采集任务</label>';
            html += '<select class="pe-modbus-device-select">';
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

        _syncModbusActionOptions(block) {
            var isRtc = [35, 36, 37, 38].includes(Number(block.querySelector('.pe-action-type')?.value));
            var isModbus = !isRtc && (block.querySelector('.pe-target-periph')?.value || '').indexOf('modbus:') === 0;
            block.querySelectorAll('.pe-action-type option').forEach(function(option) {
                var restricted = isModbus && ['16', '17', '18'].indexOf(option.value) < 0;
                if (restricted) {
                    option.dataset.modbusRestricted = '1';
                    option.hidden = true;
                    option.disabled = true;
                } else if (option.dataset.modbusRestricted) {
                    delete option.dataset.modbusRestricted;
                    option.hidden = false;
                    option.disabled = !!option.dataset.ioUnsupported;
                }
            });
        },

        _updateModbusWriteHelp(block, actionType) {
            var input = block.querySelector('.pe-action-value');
            var help = block.querySelector('.pe-action-value-group .pe-help-text');
            var text = actionType === 17
                ? '十进制 寄存器地址:数值，例如 5:255；地址和值均为 0-65535。仅填数值时使用设备基地址。'
                : '十进制 通道偏移:状态，例如 0:1；偏移叠加设备基地址，状态为 0 或 1。功能码遵循设备线圈/寄存器模式。';
            if (input) input.placeholder = actionType === 17 ? '5:255' : '0:1';
            if (help) help.textContent = text;
        },

        _onTargetPeriphChange(selectEl, index) {
            var block = this._getPeriphExecBlock('periph-exec-actions', index);
            if (!block) return;
            // Remote targets use their own resource catalogue. Re-entering the
            // local action-type handler clears the just-selected resource.
            var selectedAction = Number(block.querySelector('.pe-action-type')?.value);
            if ([33, 34, 35, 36, 37, 38, 39].includes(selectedAction)) return;
            var periphId = selectEl.value;
            var isModbus = periphId && periphId.indexOf('modbus:') === 0;
            var isModbusTask = periphId && periphId.indexOf('modbus-task:') === 0;
            var actionTypeGroup = block.querySelector('.pe-action-type-group');
            var actionValueGroup = block.querySelector('.pe-action-value-group');
            var recvGroup = block.querySelector('.pe-use-received-value-group');
            var pollTasksGroup = block.querySelector('.pe-poll-tasks-group');
            var sensorGroup = block.querySelector('.pe-sensor-group');
            var ctrlPanel = block.querySelector('.pe-modbus-ctrl-panel');
            if (isModbusTask) {
                // 采集类子设备：隐藏所有动作配置字段（仅轮询采集，无需控制）
                this._mountPeriphExecLazyActionPanel(block, -1);
                this._setSectionVisible(actionTypeGroup, false);
                this._setSectionVisible(actionValueGroup, false);
                this._setSectionVisible(recvGroup, false);
                this._setSectionVisible(pollTasksGroup, false);
                this._setSectionVisible(sensorGroup, false);
                this._setSectionVisible(ctrlPanel, false);
            } else if (isModbus && !isModbusTask) {
                if (selectedAction === 16 || selectedAction === 17) {
                    this._setSectionVisible(actionTypeGroup, true);
                    this.onPeriphExecActionTypeChangeInBlock(selectedAction, index);
                    return;
                }
                this._mountPeriphExecLazyActionPanel(block, -1);
                var actionSelect = block.querySelector('.pe-action-type');
                if (actionSelect) actionSelect.value = '18';
                this._setSectionVisible(actionTypeGroup, true);
                this._setSectionVisible(actionValueGroup, false);
                this._setSectionVisible(recvGroup, false);
                this._setSectionVisible(pollTasksGroup, false);
                this._setSectionVisible(sensorGroup, false);
                this._showModbusCtrlPanel(ctrlPanel, periphId, '');
            } else {
                this._setSectionVisible(ctrlPanel, false);
                this._setSectionVisible(actionTypeGroup, true);
                var actionType = block.querySelector('.pe-action-type');
                if (actionType) this.onPeriphExecActionTypeChangeInBlock(actionType.value, index);
            }
            this._syncModbusActionOptions(block);
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
                container.innerHTML = '<span class="pe-empty-inline">未找到对应子设备</span>';
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
                html += '<label class="pe-field-label-compact">选择通道</label>';
                html += '<select class="pe-modbus-channel-select">';
                for (var ch = 0; ch < (dev.channelCount || 2); ch++) {
                    html += '<option value="' + ch + '"' + (String(ch) === selChannel ? ' selected' : '') + '>CH' + ch + '</option>';
                }
                html += '</select></div>';
            }
            if (dt === 'relay') {
                html += '<div class="fb-form-group pe-field-stack-compact">';
                html += '<label class="pe-field-label-compact">子设备动作</label>';
                html += '<select class="pe-modbus-action-select">' +
                    '<option value="on"' + (selAction === 'off' ? '' : ' selected') + '>打开</option>' +
                    '<option value="off"' + (selAction === 'off' ? ' selected' : '') + '>关闭</option></select>';
                html += '</div>';
            } else if (dt === 'pwm') {
                var maxPwm = dev.pwmResolution === 10 ? 1023 : (dev.pwmResolution === 16 ? 65535 : 255);
                html += '<div class="fb-form-group pe-field-stack-compact">';
                html += '<label class="pe-field-label-compact">占空比</label>';
                html += '<input type="number" class="pe-modbus-action-value" min="0" max="' + maxPwm + '" value="' + (selValue || 0) + '">';
                html += '</div>';
            } else if (dt === 'pid') {
                html += '<div class="pe-pid-fields">';
                html += '<div class="fb-form-group pe-field-stack-compact">';
                html += '<label class="pe-field-label-compact">参数</label>';
                html += '<select class="pe-modbus-action-param">' +
                    '<option value="P"' + (selParam === 'I' || selParam === 'D' ? '' : ' selected') + '>P</option>' +
                    '<option value="I"' + (selParam === 'I' ? ' selected' : '') + '>I</option>' +
                    '<option value="D"' + (selParam === 'D' ? ' selected' : '') + '>D</option></select>';
                html += '</div>';
                html += '<div class="fb-form-group pe-field-stack-compact">';
                html += '<label class="pe-field-label-compact">值</label>';
                html += '<input type="number" class="pe-modbus-action-value" value="' + (selValue || 0) + '">';
                html += '</div>';
                html += '</div>';
            } else if (dt === 'motor') {
                html += '<div class="fb-form-group pe-field-stack-compact">';
                html += '<label class="pe-field-label-compact">子设备动作</label>';
                html += '<select class="pe-modbus-action-select">' +
                    '<option value="forward"' + (selAction === 'forward' ? ' selected' : '') + '>正转</option>' +
                    '<option value="reverse"' + (selAction === 'reverse' ? ' selected' : '') + '>反转</option>' +
                    '<option value="stop"' + (selAction === 'stop' ? ' selected' : '') + '>停止</option></select>';
                html += '</div>';
            }
            html += '</div>';
            container.innerHTML = html;
            // 电机类型只有子设备动作一个字段，添加 pe-ctrl-single 使其与执行外设并排显示
            if (dt === 'motor') {
                container.classList.add('pe-ctrl-single');
            } else {
                container.classList.remove('pe-ctrl-single');
            }
            this._setSectionVisible(container, true);
        },

        _showModbusTriggerCtrlPanel(container, deviceIndex, actionValue) {
            if (!container) return;
            var devices = this._modbusDevices || [];
            var dev = devices[deviceIndex];
            if (!dev) {
                container.innerHTML = '<span class="pe-empty-inline">未找到对应子设备</span>';
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
                html += '<label class="pe-field-label-compact">匹配通道</label>';
                html += '<select class="pe-modbus-channel-select">';
                for (var ch = 0; ch < (dev.channelCount || 2); ch++) {
                    html += '<option value="' + ch + '"' + (String(ch) === selChannel ? ' selected' : '') + '>CH' + ch + '</option>';
                }
                html += '</select></div>';
            }
            if (dt === 'relay') {
                html += '<div class="fb-form-group pe-field-stack-compact">';
                html += '<label class="pe-field-label-compact">匹配动作</label>';
                html += '<select class="pe-modbus-action-select">' +
                    '<option value="on"' + (selAction === 'off' ? '' : ' selected') + '>打开</option>' +
                    '<option value="off"' + (selAction === 'off' ? ' selected' : '') + '>关闭</option></select>';
                html += '</div>';
            } else if (dt === 'motor') {
                html += '<div class="fb-form-group pe-field-stack-compact">';
                html += '<label class="pe-field-label-compact">匹配动作</label>';
                html += '<select class="pe-modbus-action-select">' +
                    '<option value="forward"' + (selAction === 'forward' ? ' selected' : '') + '>正转</option>' +
                    '<option value="reverse"' + (selAction === 'reverse' ? ' selected' : '') + '>反转</option>' +
                    '<option value="stop"' + (selAction === 'stop' ? ' selected' : '') + '>停止</option></select>';
                html += '</div>';
            } else if (dt === 'pwm') {
                var maxPwm = dev.pwmResolution === 10 ? 1023 : (dev.pwmResolution === 16 ? 65535 : 255);
                html += '<div class="fb-form-group pe-field-stack-compact">';
                html += '<label class="pe-field-label-compact">占空比</label>';
                html += '<input type="number" class="pe-modbus-action-value" min="0" max="' + maxPwm + '" value="' + (selValue || 0) + '">';
                html += '</div>';
            } else if (dt === 'pid') {
                html += '<div class="pe-pid-fields">';
                html += '<div class="fb-form-group pe-field-stack-compact">';
                html += '<label class="pe-field-label-compact">参数</label>';
                html += '<select class="pe-modbus-action-param">' +
                    '<option value="P"' + (selParam === 'I' || selParam === 'D' ? '' : ' selected') + '>P</option>' +
                    '<option value="I"' + (selParam === 'I' ? ' selected' : '') + '>I</option>' +
                    '<option value="D"' + (selParam === 'D' ? ' selected' : '') + '>D</option></select>';
                html += '</div>';
                html += '<div class="fb-form-group pe-field-stack-compact">';
                html += '<label class="pe-field-label-compact">值</label>';
                html += '<input type="number" class="pe-modbus-action-value" value="' + (selValue || 0) + '">';
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
