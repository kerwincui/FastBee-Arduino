/**
 * 外设执行管理模块
 * 包含外设执行规则的创建、编辑、删除、启用/禁用
 * 触发器/动作的动态配置块管理
 */
(function() {
    AppState.registerModule('periph-exec', {

        // ============ 状态变量 ============
        _pePeripherals: [],
        _peDataSources: [],
        _peCurrentPage: 1,
        _pePageSize: 10,
        _peTotalRules: 0,

        // ============ 事件绑定 ============
        setupPeriphExecEvents() {
            var closePeriphExecModal = document.getElementById('close-periph-exec-modal');
            if (closePeriphExecModal) closePeriphExecModal.addEventListener('click', () => this.closePeriphExecModal());
            var cancelPeriphExecBtn = document.getElementById('cancel-periph-exec-btn');
            if (cancelPeriphExecBtn) cancelPeriphExecBtn.addEventListener('click', () => this.closePeriphExecModal());
            var savePeriphExecBtn = document.getElementById('save-periph-exec-btn');
            if (savePeriphExecBtn) savePeriphExecBtn.addEventListener('click', () => this.savePeriphExecRule());
        },

        // ============ 模态框 ============

        openPeriphExecModal(editId) {
            const modal = document.getElementById('periph-exec-modal');
            if (!modal) return;
            const titleEl = document.getElementById('periph-exec-modal-title');
            var safeId = (editId && editId !== 'null' && editId !== 'undefined') ? editId : '';
            document.getElementById('periph-exec-original-id').value = safeId;
            document.getElementById('periph-exec-error').style.display = 'none';
            if (safeId) {
                if (titleEl) titleEl.textContent = i18n.t('periph-exec-edit-modal-title');
            } else {
                if (titleEl) titleEl.textContent = i18n.t('periph-exec-add-modal-title');
                document.getElementById('periph-exec-form').reset();
            }
            const triggersContainer = document.getElementById('periph-exec-triggers');
            const actionsContainer = document.getElementById('periph-exec-actions');
            if (triggersContainer) triggersContainer.innerHTML = '';
            if (actionsContainer) actionsContainer.innerHTML = '';
            if (safeId) { modal.style.display = 'flex'; return; }
            this._pePeripherals = [];
            this._peDataSources = [];
            apiGet('/api/peripherals?pageSize=50').then(res => {
                if (res && res.success && res.data) this._pePeripherals = res.data.filter(p => p.enabled);
                return apiGet('/api/protocol/config');
            }).then(protoRes => {
                if (protoRes && protoRes.success && protoRes.data) {
                    const protoData = protoRes.data;
                    this._peDataSources = this._extractDataSources(protoData);
                    if (protoData.modbusRtu && protoData.modbusRtu.master) {
                        this._masterTasks = protoData.modbusRtu.master.tasks || [];
                        this._modbusDevices = protoData.modbusRtu.master.devices || [];
                    }
                }
                this._createPeriphExecTriggerElement({}, 0);
                this._createPeriphExecActionElement({}, 0);
            }).catch(err => {
                console.error('Failed to load periph exec data:', err);
                this._createPeriphExecTriggerElement({}, 0);
                this._createPeriphExecActionElement({}, 0);
            });
            modal.style.display = 'flex';
        },

        openPeriphExecModalStandalone() { this.openPeriphExecModal(); },

        closePeriphExecModal() {
            const modal = document.getElementById('periph-exec-modal');
            if (modal) modal.style.display = 'none';
            this._pePeripherals = [];
            this._peDataSources = [];
        },

        // ============ 辅助方法 ============

        _populatePeriphSelect(selectEl, selectedValue) {
            if (!selectEl) return;
            selectEl.innerHTML = '<option value="">' + i18n.t('periph-exec-select-periph') + '</option>';
            (this._pePeripherals || []).forEach(p => {
                const opt = document.createElement('option');
                opt.value = p.id;
                opt.textContent = p.name + ' (' + p.id + ')';
                selectEl.appendChild(opt);
            });
            if (selectedValue) selectEl.value = selectedValue;
        },

        _populateTriggerPeriphSelect(selectEl, selectedValue) {
            if (!selectEl) return;
            selectEl.innerHTML = '<option value="">' + i18n.t('periph-exec-select-periph') + '</option>';
            if (this._peDataSources && this._peDataSources.length > 0) {
                const grp = document.createElement('optgroup');
                grp.label = i18n.t('periph-exec-datasource-group') || '数据源';
                this._peDataSources.forEach(ds => {
                    const opt = document.createElement('option');
                    opt.value = ds.id; opt.textContent = ds.label + ' (' + ds.id + ')';
                    grp.appendChild(opt);
                });
                selectEl.appendChild(grp);
            }
            if (this._pePeripherals && this._pePeripherals.length > 0) {
                const grp = document.createElement('optgroup');
                grp.label = i18n.t('periph-exec-periph-group') || '硬件外设';
                this._pePeripherals.forEach(p => {
                    const opt = document.createElement('option');
                    opt.value = p.id; opt.textContent = p.name + ' (' + p.id + ')';
                    grp.appendChild(opt);
                });
                selectEl.appendChild(grp);
            }
            if (selectedValue) selectEl.value = selectedValue;
        },

        _extractDataSources(protocolConfig) {
            const sources = [];
            if (!protocolConfig) return sources;
            const rtu = protocolConfig.modbusRtu;
            if (rtu && rtu.enabled && rtu.master && rtu.master.tasks) {
                rtu.master.tasks.forEach(task => {
                    if (!task.enabled || !task.mappings) return;
                    task.mappings.forEach(m => {
                        if (m.sensorId) sources.push({ id: m.sensorId, label: (task.label || 'Modbus') + '/' + m.sensorId });
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
            var container = document.getElementById('periph-exec-actions');
            if (!container) return;
            var block = container.querySelectorAll('.periph-exec-config-item')[index];
            if (!block) return;
            this._populateSensorPeriphSelect(block, selectEl.value);
        },

        _populateModbusDevicePanel(container, actionValue) {
            if (!container) return;
            var tasks = this._masterTasks || [];
            var devices = this._modbusDevices || [];
            if (tasks.length === 0 && devices.length === 0) {
                container.innerHTML = '<span style="color:#999;font-size:12px;">' + (i18n.t('modbus-no-devices') || '暂无子设备') + '</span>';
                return;
            }
            var selDevice = ''; var selChannel = ''; var selAction = ''; var selValue = ''; var selParam = '';
            if (actionValue) {
                if (actionValue.charAt(0) === '{') {
                    try {
                        var parsed = JSON.parse(actionValue);
                        if (parsed.poll && parsed.poll.length > 0) selDevice = 'sensor-' + parsed.poll[0];
                        else if (parsed.ctrl && parsed.ctrl.length > 0) {
                            var c = parsed.ctrl[0]; selDevice = 'control-' + c.d;
                            selChannel = String(c.c || 0); selAction = c.a || ''; selValue = String(c.v || 0); selParam = c.p || '';
                        }
                    } catch(e) {}
                } else {
                    var parts = actionValue.split(',');
                    if (parts.length > 0 && parts[0].trim()) selDevice = 'sensor-' + parts[0].trim();
                }
            }
            var fcNames = {1: 'FC01', 2: 'FC02', 3: 'FC03', 4: 'FC04'};
            var typeLabels = {relay: i18n.t('modbus-type-relay') || '继电器', pwm: i18n.t('modbus-type-pwm') || 'PWM', pid: i18n.t('modbus-type-pid') || 'PID'};
            var html = '<div class="pe-modbus-select-flow">';
            html += '<div class="pure-control-group" style="margin-bottom:8px;">';
            html += '<label style="font-size:12px;font-weight:bold;">' + (i18n.t('periph-exec-select-device') || '选择子设备') + '</label>';
            html += '<select class="pure-input-1 pe-modbus-device-select" onchange="AppState._onPeriphDeviceSelect(this)" style="font-size:13px;">';
            html += '<option value="">--</option>';
            if (tasks.length > 0) {
                html += '<optgroup label="' + (i18n.t('modbus-type-sensor') || '采集设备') + '">';
                for (var i = 0; i < tasks.length; i++) {
                    var t = tasks[i]; var label = t.label || ('Slave ' + (t.slaveAddress || 1));
                    var desc = (fcNames[t.functionCode] || 'FC03') + ' @' + (t.startAddress || 0) + ' x' + (t.quantity || 10);
                    var val = 'sensor-' + i;
                    html += '<option value="' + val + '"' + (val === selDevice ? ' selected' : '') + '>' + escapeHtml(label) + ' (' + desc + ')</option>';
                }
                html += '</optgroup>';
            }
            if (devices.length > 0) {
                html += '<optgroup label="' + (i18n.t('modbus-type-control') || '控制设备') + '">';
                for (var j = 0; j < devices.length; j++) {
                    var d = devices[j]; var dt = d.deviceType || 'relay';
                    var devName = d.name || ('Device ' + (j + 1)); var tl = typeLabels[dt] || dt; var val2 = 'control-' + j;
                    html += '<option value="' + val2 + '"' + (val2 === selDevice ? ' selected' : '') + '>' + escapeHtml(devName) + ' (' + tl + ', Addr ' + (d.slaveAddress || 1) + ')</option>';
                }
                html += '</optgroup>';
            }
            html += '</select></div>';
            var isCtrl = selDevice.indexOf('control-') === 0;
            var chDisplay = isCtrl ? 'block' : 'none';
            html += '<div class="pure-control-group pe-modbus-channel-group" style="display:' + chDisplay + ';margin-bottom:8px;">';
            html += '<label style="font-size:12px;font-weight:bold;">' + (i18n.t('periph-exec-select-channel') || '选择通道') + '</label>';
            html += '<select class="pure-input-1 pe-modbus-channel-select" onchange="AppState._onPeriphChannelSelect(this)" style="font-size:13px;">';
            if (isCtrl) {
                var cidx = parseInt(selDevice.split('-')[1]); var cdev = devices[cidx];
                if (cdev) { for (var ch = 0; ch < (cdev.channelCount || 2); ch++) html += '<option value="' + ch + '"' + (String(ch) === selChannel ? ' selected' : '') + '>CH' + ch + '</option>'; }
            }
            html += '</select></div>';
            var actDisplay = isCtrl ? 'block' : 'none';
            html += '<div class="pure-control-group pe-modbus-action-group" style="display:' + actDisplay + ';margin-bottom:4px;">';
            html += '<label style="font-size:12px;font-weight:bold;">' + (i18n.t('periph-exec-select-action') || '子设备动作') + '</label>';
            html += '<div class="pe-modbus-action-content">';
            if (isCtrl) { var aidx = parseInt(selDevice.split('-')[1]); var adev = devices[aidx]; if (adev) html += this._buildPeriphActionUI(adev, selAction, selChannel, selValue, selParam); }
            html += '</div></div></div>';
            container.innerHTML = html;
        },

        _buildPeriphActionUI(dev, action, channel, value, param) {
            var dt = dev.deviceType || 'relay';
            var html = '<div style="display:flex;gap:8px;align-items:center;flex-wrap:wrap;">';
            if (dt === 'relay') {
                html += '<select class="pure-input-1 pe-modbus-action-select" style="max-width:120px;font-size:13px;">' +
                    '<option value="on"' + (action === 'off' ? '' : ' selected') + '>' + (i18n.t('periph-exec-ctrl-on') || '打开') + '</option>' +
                    '<option value="off"' + (action === 'off' ? ' selected' : '') + '>' + (i18n.t('periph-exec-ctrl-off') || '关闭') + '</option></select>';
            } else if (dt === 'pwm') {
                var maxPwm = dev.pwmResolution === 10 ? 1023 : (dev.pwmResolution === 16 ? 65535 : 255);
                html += '<label style="font-size:12px;">' + (i18n.t('periph-exec-ctrl-duty') || '占空比') + ': ';
                html += '<input type="number" class="pe-modbus-action-value" min="0" max="' + maxPwm + '" value="' + (value || 0) + '" style="width:80px;font-size:12px;padding:2px 4px;"></label>';
            } else if (dt === 'pid') {
                html += '<label style="font-size:12px;">' + (i18n.t('periph-exec-ctrl-pid-param') || '参数') + ': ';
                html += '<select class="pe-modbus-action-param" style="font-size:12px;padding:2px 4px;">' +
                    '<option value="P"' + (param === 'I' || param === 'D' ? '' : ' selected') + '>P</option>' +
                    '<option value="I"' + (param === 'I' ? ' selected' : '') + '>I</option>' +
                    '<option value="D"' + (param === 'D' ? ' selected' : '') + '>D</option></select></label>';
                html += '<label style="font-size:12px;">' + (i18n.t('periph-exec-ctrl-value') || '值') + ': ';
                html += '<input type="number" class="pe-modbus-action-value" value="' + (value || 0) + '" style="width:80px;font-size:12px;padding:2px 4px;"></label>';
            }
            html += '</div>';
            return html;
        },

        _onPeriphDeviceSelect(selectEl) {
            var flow = selectEl.closest('.pe-modbus-select-flow');
            if (!flow) return;
            var val = selectEl.value;
            var chGroup = flow.querySelector('.pe-modbus-channel-group');
            var actGroup = flow.querySelector('.pe-modbus-action-group');
            if (!val || val.indexOf('sensor-') === 0) {
                if (chGroup) chGroup.style.display = 'none';
                if (actGroup) actGroup.style.display = 'none';
                return;
            }
            var idx = parseInt(val.split('-')[1]);
            var devices = this._modbusDevices || [];
            var dev = devices[idx];
            if (!dev) return;
            var chSelect = flow.querySelector('.pe-modbus-channel-select');
            if (chSelect) {
                var chHtml = '';
                for (var ch = 0; ch < (dev.channelCount || 2); ch++) chHtml += '<option value="' + ch + '">CH' + ch + '</option>';
                chSelect.innerHTML = chHtml;
            }
            if (chGroup) chGroup.style.display = 'block';
            var actContent = flow.querySelector('.pe-modbus-action-content');
            if (actContent) actContent.innerHTML = this._buildPeriphActionUI(dev, '', '', '', '');
            if (actGroup) actGroup.style.display = 'block';
        },

        _onPeriphChannelSelect(selectEl) { },

        _onCtrlDeviceToggle(checkbox) {
            var panel = checkbox.closest('.pe-ctrl-device-item').querySelector('.pe-ctrl-options');
            if (panel) panel.style.display = checkbox.checked ? 'block' : 'none';
        },

        // ============ Part 2: Trigger/action element creation ============

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
            const showPoll = triggerType === '5';
            const timerMode = String(data.timerMode ?? 0);
            const showInterval = timerMode === '0';
            const showDaily = timerMode === '1';
            const opVal = String(data.operatorType ?? 0);
            const mappedOp = (opVal === '0' || opVal === '1') ? opVal : '0';
            const showCompareValue = showPlatform && mappedOp !== '1';
            div.innerHTML = '<span class="mqtt-topic-index">' + (index + 1) + '</span>' +
                '<button type="button" class="mqtt-topic-delete" onclick="app.deletePeriphExecTrigger(' + index + ')">' + i18n.t('peripheral-delete') + '</button>' +
                '<div class="config-form-grid">' +
                    '<div class="pure-control-group"><label>' + i18n.t('periph-exec-trigger-type-label') + '</label>' +
                    '<select class="pure-input-1 pe-trigger-type" onchange="app.onPeriphExecTriggerTypeChangeInBlock(this.value, ' + index + ')">' +
                        '<option value="0"' + (triggerType === '0' ? ' selected' : '') + '>' + i18n.t('periph-exec-trigger-platform') + '</option>' +
                        '<option value="1"' + (triggerType === '1' ? ' selected' : '') + '>' + i18n.t('periph-exec-trigger-timer') + '</option>' +
                        '<option value="4"' + (triggerType === '4' ? ' selected' : '') + '>' + i18n.t('periph-exec-trigger-event') + '</option>' +
                        '<option value="5"' + (triggerType === '5' ? ' selected' : '') + '>' + i18n.t('periph-exec-trigger-poll') + '</option>' +
                    '</select></div>' +
                    '<div class="pure-control-group pe-trigger-periph-group" style="display:' + ((showPlatform || showPoll) ? 'block' : 'none') + '">' +
                    '<label>' + i18n.t('periph-exec-trigger-periph-label') + '</label>' +
                    '<select class="pure-input-1 pe-trigger-periph"><option value="">' + i18n.t('periph-exec-select-periph') + '</option></select></div>' +
                '</div>' +
                '<div class="pe-platform-condition" style="display:' + (showPlatform ? 'block' : 'none') + '">' +
                    '<div class="config-form-grid">' +
                    '<div class="pure-control-group"><label>' + i18n.t('periph-exec-operator-label') + '</label>' +
                    '<select class="pure-input-1 pe-operator" onchange="app.onPeriphExecOperatorChangeInBlock(this.value, ' + index + ')">' +
                        '<option value="0"' + (mappedOp === '0' ? ' selected' : '') + '>' + i18n.t('periph-exec-handle-match') + '</option>' +
                        '<option value="1"' + (mappedOp === '1' ? ' selected' : '') + '>' + i18n.t('periph-exec-handle-set') + '</option>' +
                    '</select></div>' +
                    '<div class="pure-control-group pe-compare-value-group" style="display:' + (showCompareValue ? 'block' : 'none') + '">' +
                    '<label>' + i18n.t('periph-exec-compare-label') + '</label>' +
                    '<input type="text" class="pure-input-1 pe-compare-value" value="' + escapeHtml(data.compareValue) + '" placeholder="' + escapeHtml(i18n.t('periph-exec-compare-hint')) + '"></div>' +
                    '</div></div>' +
                '<div class="pe-poll-params" style="display:' + (showPoll ? 'block' : 'none') + '">' +
                    '<div class="config-form-grid">' +
                    '<div class="pure-control-group"><label>' + i18n.t('periph-exec-poll-interval-label') + '</label><input type="number" class="pure-input-1 pe-poll-interval" value="' + (data.intervalSec || 60) + '" min="5" max="86400"></div>' +
                    '<div class="pure-control-group"><label>' + i18n.t('periph-exec-poll-timeout-label') + '</label><input type="number" class="pure-input-1 pe-poll-timeout" value="' + (data.pollResponseTimeout || 1000) + '" min="100" max="30000"></div>' +
                    '<div class="pure-control-group"><label>' + i18n.t('periph-exec-poll-retries-label') + '</label><input type="number" class="pure-input-1 pe-poll-retries" value="' + (data.pollMaxRetries ?? 2) + '" min="0" max="10"></div>' +
                    '<div class="pure-control-group"><label>' + i18n.t('periph-exec-poll-delay-label') + '</label><input type="number" class="pure-input-1 pe-poll-inter-delay" value="' + (data.pollInterPollDelay || 100) + '" min="10" max="5000"></div>' +
                    '</div></div>' +
                '<div class="pe-timer-config" style="display:' + (showTimer ? 'block' : 'none') + '">' +
                    '<div class="config-form-grid">' +
                    '<div class="pure-control-group"><label>' + i18n.t('periph-exec-timer-mode-label') + '</label>' +
                    '<select class="pure-input-1 pe-timer-mode" onchange="app.onPeriphExecTimerModeChangeInBlock(this.value, ' + index + ')">' +
                        '<option value="0"' + (timerMode === '0' ? ' selected' : '') + '>' + i18n.t('periph-exec-timer-interval') + '</option>' +
                        '<option value="1"' + (timerMode === '1' ? ' selected' : '') + '>' + i18n.t('periph-exec-timer-daily') + '</option>' +
                    '</select></div>' +
                    '<div class="pure-control-group pe-interval-fields" style="display:' + (showInterval ? 'block' : 'none') + '"><label>' + i18n.t('periph-exec-interval-label') + '</label><input type="number" class="pure-input-1 pe-interval" value="' + (data.intervalSec || 60) + '" min="1" max="86400"></div>' +
                    '<div class="pure-control-group pe-daily-fields" style="display:' + (showDaily ? 'block' : 'none') + '"><label>' + i18n.t('periph-exec-timepoint-label') + '</label><input type="time" class="pure-input-1 pe-timepoint" value="' + (data.timePoint || '08:00') + '"></div>' +
                    '</div></div>' +
                '<div class="pe-event-group" style="display:' + (showEvent ? 'block' : 'none') + '">' +
                    '<div class="config-form-grid">' +
                    '<div class="pure-control-group"><label>' + i18n.t('periph-exec-event-category-label') + '</label>' +
                    '<select class="pure-input-1 pe-event-category" onchange="app.onPeriphExecEventCategoryChangeInBlock(this.value, ' + index + ')">' +
                    '<option value="">' + i18n.t('periph-exec-select-category') + '</option></select></div>' +
                    '<div class="pure-control-group"><label>' + i18n.t('periph-exec-event-label') + '</label>' +
                    '<select class="pure-input-1 pe-event"><option value="">' + i18n.t('periph-exec-select-event') + '</option></select></div>' +
                    '</div></div>';
            container.appendChild(div);
            this._populateTriggerPeriphSelect(div.querySelector('.pe-trigger-periph'), data.triggerPeriphId || data.sourcePeriphId || '');
            if (showEvent) this._populateEventCategoriesInBlock(div, data.eventId);
        },

        _createPeriphExecActionElement(data, index) {
            const container = document.getElementById('periph-exec-actions');
            if (!container) return;
            const div = document.createElement('div');
            div.className = 'periph-exec-config-item';
            div.dataset.index = index;
            const actionType = String(data.actionType ?? 0);
            const actionTypeInt = parseInt(actionType);
            const isModbusPoll = actionTypeInt === 18;
            const isSensorRead = actionTypeInt === 19;
            const showPeriphGroup = !((actionTypeInt >= 6 && actionTypeInt <= 11) || actionTypeInt === 15 || isModbusPoll);
            const needsValue = (actionTypeInt >= 2 && actionTypeInt <= 5);
            const isScript = actionTypeInt === 15;
            const sel = (v) => actionType === String(v) ? 'selected' : '';
            var sensorCfg = {sensorCategory:'analog',periphId:'',scaleFactor:1,offset:0,decimalPlaces:2,sensorLabel:'',unit:''};
            if (isSensorRead && data.actionValue) { try { Object.assign(sensorCfg, JSON.parse(data.actionValue)); } catch(e) {} }
            const selCat = (v) => sensorCfg.sensorCategory === v ? 'selected' : '';
            div.innerHTML = '<span class="mqtt-topic-index">' + (index + 1) + '</span>' +
                '<button type="button" class="mqtt-topic-delete" onclick="app.deletePeriphExecAction(' + index + ')">' + i18n.t('peripheral-delete') + '</button>' +
                '<div class="pe-action-grid">' +
                    '<div class="pure-control-group pe-action-type-group"><label>' + i18n.t('periph-exec-action-type-label') + '</label>' +
                    '<select class="pure-input-1 pe-action-type" onchange="app.onPeriphExecActionTypeChangeInBlock(this.value, ' + index + ')">' +
                        '<optgroup label="' + i18n.t('periph-exec-action-cat-gpio') + '">' +
                        '<option value="0" ' + sel(0) + '>' + i18n.t('periph-exec-action-high') + '</option>' +
                        '<option value="1" ' + sel(1) + '>' + i18n.t('periph-exec-action-low') + '</option>' +
                        '<option value="2" ' + sel(2) + '>' + i18n.t('periph-exec-action-blink') + '</option>' +
                        '<option value="3" ' + sel(3) + '>' + i18n.t('periph-exec-action-breathe') + '</option></optgroup>' +
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
                        '<option value="18" ' + sel(18) + '>' + i18n.t('periph-exec-action-modbus-poll') + '</option>' +
                        '<option value="19" ' + sel(19) + '>' + i18n.t('periph-exec-action-sensor-read') + '</option></optgroup>' +
                    '</select></div>' +
                    '<div class="pure-control-group pe-target-group" style="display:' + (showPeriphGroup ? 'block' : 'none') + '">' +
                    '<label>' + i18n.t('periph-exec-target-periph-label') + '</label>' +
                    '<select class="pure-input-1 pe-target-periph"><option value="">' + i18n.t('periph-exec-select-periph') + '</option></select></div>' +
                    '<div class="pure-control-group pe-action-value-group" style="display:' + (needsValue ? 'block' : 'none') + '; grid-column: 1 / -1;">' +
                    '<label>' + i18n.t('periph-exec-action-value-label') + '</label>' +
                    '<input type="text" class="pure-input-1 pe-action-value" value="' + (isScript ? '' : escapeHtml(data.actionValue)) + '" placeholder="' + escapeHtml(i18n.t('periph-exec-action-value-hint')) + '"' + (data.useReceivedValue ? ' readonly' : '') + '>' +
                    '<small style="color: #999;">' + i18n.t('periph-exec-action-value-help') + '</small></div>' +
                    '<div class="pure-control-group pe-use-received-value-group">' +
                    '<label class="pe-checkbox-label"><input type="checkbox" class="pe-use-received-value"' + (data.useReceivedValue ? ' checked' : '') + ' onchange="app.onPeriphExecUseRecvChangeInBlock(this, ' + index + ')">' +
                    '<span>' + i18n.t('periph-exec-use-received-value') + '</span></label>' +
                    '<small style="color: #999;">' + i18n.t('periph-exec-use-received-value-help') + '</small></div>' +
                    '<div class="pure-control-group pe-sync-delay-group">' +
                    '<label>' + i18n.t('periph-exec-sync-delay-label') + '</label>' +
                    '<input type="number" class="pure-input-1 pe-sync-delay" min="0" max="10000" step="100" value="' + (data.syncDelayMs || 0) + '" placeholder="0">' +
                    '<small style="color: #999;">' + i18n.t('periph-exec-sync-delay-help') + '</small></div>' +
                    '<div class="pure-control-group pe-script-group" style="display:' + (isScript ? 'block' : 'none') + '; grid-column: 1 / -1;">' +
                    '<label>' + i18n.t('periph-exec-script-label') + '</label>' +
                    '<textarea class="pure-input-1 pe-script" rows="6" style="font-family: monospace; font-size: 13px; resize: vertical;" maxlength="1024" placeholder="' + escapeHtml(i18n.t('periph-exec-script-placeholder')) + '">' + (isScript ? escapeHtml(data.actionValue) : '') + '</textarea>' +
                    '<small style="color: #999;">' + i18n.t('periph-exec-script-help') + '</small></div>' +
                    '<div class="pure-control-group pe-poll-tasks-group" style="display:' + (isModbusPoll ? 'block' : 'none') + '; grid-column: 1 / -1;">' +
                    '<label>' + i18n.t('periph-exec-poll-tasks-label') + '</label>' +
                    '<div class="pe-poll-tasks-list" style="border:1px solid #ddd;border-radius:4px;padding:8px;max-height:200px;overflow-y:auto;background:#fafafa;"></div>' +
                    '<small style="color: #999;">' + i18n.t('periph-exec-poll-tasks-help') + '</small></div>' +
                    '<div class="pure-control-group pe-sensor-group" style="display:' + (isSensorRead ? 'block' : 'none') + '; grid-column: 1 / -1;">' +
                    '<div class="pe-sensor-config-grid">' +
                    '<div class="pure-control-group"><label>' + i18n.t('periph-exec-sensor-category') + '</label>' +
                    '<select class="pure-input-1 pe-sensor-category" onchange="app.onSensorCategoryChange(this, ' + index + ')">' +
                    '<option value="analog" ' + selCat('analog') + '>' + i18n.t('periph-exec-sensor-cat-analog') + '</option>' +
                    '<option value="digital" ' + selCat('digital') + '>' + i18n.t('periph-exec-sensor-cat-digital') + '</option>' +
                    '<option value="pulse" ' + selCat('pulse') + '>' + i18n.t('periph-exec-sensor-cat-pulse') + '</option></select></div>' +
                    '<div class="pure-control-group"><label>' + i18n.t('periph-exec-sensor-scale') + '</label><input type="number" class="pure-input-1 pe-sensor-scale" step="any" value="' + sensorCfg.scaleFactor + '"></div>' +
                    '<div class="pure-control-group"><label>' + i18n.t('periph-exec-sensor-offset') + '</label><input type="number" class="pure-input-1 pe-sensor-offset" step="any" value="' + sensorCfg.offset + '"></div>' +
                    '<div class="pure-control-group"><label>' + i18n.t('periph-exec-sensor-decimals') + '</label><input type="number" class="pure-input-1 pe-sensor-decimals" min="0" max="6" value="' + sensorCfg.decimalPlaces + '"></div>' +
                    '<div class="pure-control-group"><label>' + i18n.t('periph-exec-sensor-label') + '</label><input type="text" class="pure-input-1 pe-sensor-label" value="' + escapeHtml(sensorCfg.sensorLabel) + '" placeholder="' + i18n.t('periph-exec-sensor-label') + '"></div>' +
                    '<div class="pure-control-group"><label>' + i18n.t('periph-exec-sensor-unit') + '</label><input type="text" class="pure-input-1 pe-sensor-unit" value="' + escapeHtml(sensorCfg.unit) + '" maxlength="8" placeholder="°C, %, V..."></div>' +
                    '</div></div></div>';
            container.appendChild(div);
            if (isSensorRead) {
                this._populateSensorPeriphSelect(div, sensorCfg.sensorCategory || 'analog', sensorCfg.periphId || data.targetPeriphId || '');
            } else {
                this._populatePeriphSelect(div.querySelector('.pe-target-periph'), data.targetPeriphId || '');
            }
            if (isModbusPoll) this._populateModbusDevicePanel(div.querySelector('.pe-poll-tasks-list'), data.actionValue || '');
        },

        addPeriphExecTrigger() {
            const container = document.getElementById('periph-exec-triggers');
            if (!container) return;
            this._createPeriphExecTriggerElement({}, container.children.length);
        },

        deletePeriphExecTrigger(index) {
            const container = document.getElementById('periph-exec-triggers');
            if (!container) return;
            const items = container.querySelectorAll('.periph-exec-config-item');
            if (items.length <= 1) return;
            if (items[index]) items[index].remove();
            this._reindexPeriphExecBlocks(container, 'Trigger');
        },

        addPeriphExecAction() {
            const container = document.getElementById('periph-exec-actions');
            if (!container) return;
            const hasSet = this._hasSetModeTrigger();
            this._createPeriphExecActionElement(hasSet ? { useReceivedValue: true } : {}, container.children.length);
        },

        deletePeriphExecAction(index) {
            const container = document.getElementById('periph-exec-actions');
            if (!container) return;
            const items = container.querySelectorAll('.periph-exec-config-item');
            if (items.length <= 1) return;
            if (items[index]) items[index].remove();
            this._reindexPeriphExecBlocks(container, 'Action');
        },

        _reindexPeriphExecBlocks(container, type) {
            const items = container.querySelectorAll('.periph-exec-config-item');
            const fnPrefix = type === 'Trigger' ? 'deletePeriphExecTrigger' : 'deletePeriphExecAction';
            items.forEach((item, idx) => {
                item.dataset.index = idx;
                const indexSpan = item.querySelector('.mqtt-topic-index');
                if (indexSpan) indexSpan.textContent = idx + 1;
                const deleteBtn = item.querySelector('.mqtt-topic-delete');
                if (deleteBtn) deleteBtn.setAttribute('onclick', 'app.' + fnPrefix + '(' + idx + ')');
                if (type === 'Trigger') {
                    const triggerTypeSel = item.querySelector('.pe-trigger-type');
                    if (triggerTypeSel) triggerTypeSel.setAttribute('onchange', 'app.onPeriphExecTriggerTypeChangeInBlock(this.value, ' + idx + ')');
                    const timerModeSel = item.querySelector('.pe-timer-mode');
                    if (timerModeSel) timerModeSel.setAttribute('onchange', 'app.onPeriphExecTimerModeChangeInBlock(this.value, ' + idx + ')');
                    const eventCatSel = item.querySelector('.pe-event-category');
                    if (eventCatSel) eventCatSel.setAttribute('onchange', 'app.onPeriphExecEventCategoryChangeInBlock(this.value, ' + idx + ')');
                    const operatorSel = item.querySelector('.pe-operator');
                    if (operatorSel) operatorSel.setAttribute('onchange', 'app.onPeriphExecOperatorChangeInBlock(this.value, ' + idx + ')');
                } else {
                    const actionTypeSel = item.querySelector('.pe-action-type');
                    if (actionTypeSel) actionTypeSel.setAttribute('onchange', 'app.onPeriphExecActionTypeChangeInBlock(this.value, ' + idx + ')');
                    const useRecvCb = item.querySelector('.pe-use-received-value');
                    if (useRecvCb) useRecvCb.setAttribute('onchange', 'app.onPeriphExecUseRecvChangeInBlock(this, ' + idx + ')');
                }
            });
        }
    });

    // Part 3: Event handlers, data collection, CRUD, page loading
    Object.assign(AppState, {

        onPeriphExecTriggerTypeChangeInBlock(val, index) {
            const container = document.getElementById('periph-exec-triggers');
            if (!container) return;
            const block = container.querySelectorAll('.periph-exec-config-item')[index];
            if (!block) return;
            const triggerPeriphGroup = block.querySelector('.pe-trigger-periph-group');
            const platformCondition = block.querySelector('.pe-platform-condition');
            const pollParams = block.querySelector('.pe-poll-params');
            const timerConfig = block.querySelector('.pe-timer-config');
            const eventGroup = block.querySelector('.pe-event-group');
            if (triggerPeriphGroup) triggerPeriphGroup.style.display = (val === '0' || val === '5') ? 'block' : 'none';
            if (platformCondition) platformCondition.style.display = (val === '0') ? 'block' : 'none';
            if (pollParams) pollParams.style.display = (val === '5') ? 'block' : 'none';
            if (timerConfig) timerConfig.style.display = (val === '1') ? 'block' : 'none';
            if (eventGroup) {
                eventGroup.style.display = (val === '4') ? 'block' : 'none';
                if (val === '4') {
                    this._populateEventCategoriesInBlock(block);
                    this._populateEventSelectInBlock(block);
                }
            }
            if (val === '5') {
                this._populateTriggerPeriphSelect(block.querySelector('.pe-trigger-periph'), '');
            }
            if (val !== '0') {
                this._checkAndSyncSetMode();
            }
        },

        onPeriphExecOperatorChangeInBlock(val, index) {
            const container = document.getElementById('periph-exec-triggers');
            if (!container) return;
            const block = container.querySelectorAll('.periph-exec-config-item')[index];
            if (!block) return;
            const compareGroup = block.querySelector('.pe-compare-value-group');
            if (compareGroup) compareGroup.style.display = (val === '1') ? 'none' : 'block';
            this._syncSetModeToActions(val === '1');
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
                if (checkbox) checkbox.checked = isSetMode;
                if (valueInput) {
                    if (isSetMode) {
                        valueInput.setAttribute('readonly', '');
                    } else {
                        valueInput.removeAttribute('readonly');
                    }
                }
            });
        },

        onPeriphExecTimerModeChangeInBlock(val, index) {
            const container = document.getElementById('periph-exec-triggers');
            if (!container) return;
            const block = container.querySelectorAll('.periph-exec-config-item')[index];
            if (!block) return;
            const intervalFields = block.querySelector('.pe-interval-fields');
            const dailyFields = block.querySelector('.pe-daily-fields');
            if (intervalFields) intervalFields.style.display = (val === '0') ? 'block' : 'none';
            if (dailyFields) dailyFields.style.display = (val === '1') ? 'block' : 'none';
        },

        onPeriphExecEventCategoryChangeInBlock(val, index) {
            const container = document.getElementById('periph-exec-triggers');
            if (!container) return;
            const block = container.querySelectorAll('.periph-exec-config-item')[index];
            if (!block) return;
            this._populateEventSelectInBlock(block, val);
        },

        onPeriphExecActionTypeChangeInBlock(val, index) {
            const container = document.getElementById('periph-exec-actions');
            if (!container) return;
            const block = container.querySelectorAll('.periph-exec-config-item')[index];
            if (!block) return;
            const actionType = parseInt(val);
            const targetGroup = block.querySelector('.pe-target-group');
            const valueGroup = block.querySelector('.pe-action-value-group');
            const scriptGroup = block.querySelector('.pe-script-group');
            const pollTasksGroup = block.querySelector('.pe-poll-tasks-group');
            const isModbusPoll = actionType === 18;
            const isSensorRead = actionType === 19;
            if (targetGroup) targetGroup.style.display = ((actionType >= 6 && actionType <= 11) || actionType === 15 || isModbusPoll) ? 'none' : 'block';
            const needsValue = (actionType >= 2 && actionType <= 5);
            if (valueGroup) valueGroup.style.display = needsValue ? 'block' : 'none';
            if (scriptGroup) scriptGroup.style.display = actionType === 15 ? 'block' : 'none';
            if (pollTasksGroup) {
                pollTasksGroup.style.display = isModbusPoll ? 'block' : 'none';
                if (isModbusPoll) {
                    this._populateModbusDevicePanel(block.querySelector('.pe-poll-tasks-list'), '');
                }
            }
            const sensorGroup = block.querySelector('.pe-sensor-group');
            if (sensorGroup) {
                sensorGroup.style.display = isSensorRead ? 'block' : 'none';
            }
            if (isSensorRead) {
                var cat = block.querySelector('.pe-sensor-category')?.value || 'analog';
                this._populateSensorPeriphSelect(block, cat);
            } else if (targetGroup && targetGroup.style.display !== 'none') {
                this._populatePeriphSelect(block.querySelector('.pe-target-periph'), '');
            }
        },

        onPeriphExecModeChangeInBlock(val) {
            const hidden = document.getElementById('periph-exec-exec-mode');
            if (hidden) hidden.value = val;
            const container = document.getElementById('periph-exec-actions');
            if (!container) return;
            container.querySelectorAll('.pe-exec-mode').forEach(s => { s.value = val; });
        },

        _populateEventCategoriesInBlock(blockEl, eventIdToSet) {
            const sel = blockEl.querySelector('.pe-event-category');
            if (!sel) return;
            apiGet('/api/periph-exec/events/categories').then(res => {
                if (!res || !res.success || !res.data) return;
                let opts = '<option value="">' + i18n.t('periph-exec-select-category') + '</option>';
                res.data.forEach(cat => {
                    const translatedCat = i18n.t('event-cat-' + cat.name) || cat.name;
                    opts += '<option value="' + cat.name + '">' + translatedCat + '</option>';
                });
                sel.innerHTML = opts;
                if (eventIdToSet) {
                    this._populateEventSelectInBlock(blockEl, '', eventIdToSet);
                }
            });
        },

        _populateEventSelectInBlock(blockEl, categoryFilter, eventIdToSet) {
            const sel = blockEl.querySelector('.pe-event');
            if (!sel) return;
            Promise.all([
                apiGet('/api/periph-exec/events/static'),
                apiGet('/api/periph-exec/events/dynamic')
            ]).then(([staticRes, dynamicRes]) => {
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
                sel.innerHTML = opts;
                if (eventIdToSet) sel.value = eventIdToSet;
            });
        },

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
                } else if (triggerType === '5') {
                    trigger.triggerPeriphId = item.querySelector('.pe-trigger-periph')?.value || '';
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
            container.querySelectorAll('.periph-exec-config-item').forEach(item => {
                const actionType = item.querySelector('.pe-action-type')?.value || '0';
                const action = {
                    targetPeriphId: item.querySelector('.pe-target-periph')?.value || '',
                    actionType: parseInt(actionType, 10) || 0
                };
                if (actionType === '15') {
                    action.actionValue = item.querySelector('.pe-script')?.value || '';
                } else if (actionType === '18') {
                    var devSel = item.querySelector('.pe-modbus-device-select');
                    var devVal = devSel ? devSel.value : '';
                    var jsonObj = {};
                    if (devVal.indexOf('sensor-') === 0) {
                        jsonObj.poll = [parseInt(devVal.split('-')[1])];
                    } else if (devVal.indexOf('control-') === 0) {
                        var dIdx = parseInt(devVal.split('-')[1]);
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
                        jsonObj.ctrl = [ctrl];
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
                actions.push(action);
            });
            return actions;
        }
    });

    // Part 4: CRUD operations and page loading
    Object.assign(AppState, {

        savePeriphExecRule() {
            const errEl = document.getElementById('periph-exec-error');
            errEl.style.display = 'none';
            const originalId = document.getElementById('periph-exec-original-id').value;
            const isEdit = originalId !== '' && originalId !== 'null' && originalId !== 'undefined';

            const ruleData = {
                name: document.getElementById('periph-exec-name').value.trim(),
                enabled: document.getElementById('periph-exec-enabled').checked,
                execMode: parseInt(document.getElementById('periph-exec-exec-mode').value, 10) || 0,
                reportAfterExec: document.getElementById('periph-exec-report').value === 'true'
            };

            if (!ruleData.name) {
                errEl.textContent = i18n.t('periph-exec-validate-name');
                errEl.style.display = 'block';
                return;
            }

            const triggers = this._collectPeriphExecTriggers();
            ruleData.triggers = triggers;

            const actions = this._collectPeriphExecActions();
            ruleData.actions = actions;
            for (let i = 0; i < actions.length; i++) {
                if (actions[i].actionType === 15) {
                    if (!actions[i].actionValue.trim()) {
                        errEl.textContent = i18n.t('periph-exec-script-empty');
                        errEl.style.display = 'block';
                        return;
                    }
                    if (actions[i].actionValue.length > 1024) {
                        errEl.textContent = i18n.t('periph-exec-script-too-long');
                        errEl.style.display = 'block';
                        return;
                    }
                }
                if (actions[i].actionType === 19) {
                    var sv = {};
                    try { sv = JSON.parse(actions[i].actionValue); } catch(e) {}
                    if (!sv.periphId) {
                        errEl.textContent = i18n.t('periph-exec-sensor-no-periph');
                        errEl.style.display = 'block';
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
                        errEl.textContent = res?.error || i18n.t('periph-exec-save-fail');
                        errEl.style.display = 'block';
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
                    if (isNetworkError) {
                        errEl.textContent = i18n.t('device-offline-error');
                    } else {
                        errEl.textContent = i18n.t('periph-exec-save-fail');
                    }
                    errEl.style.display = 'block';
                });
        },

        editPeriphExecRule(id) {
            this.openPeriphExecModal(id);
            Promise.all([apiGet('/api/periph-exec?id=' + id), apiGet('/api/peripherals?pageSize=50'), apiGet('/api/protocol/config')])
                .then(([execRes, periphRes, protoRes]) => {
                    if (periphRes && periphRes.success && periphRes.data) {
                        this._pePeripherals = periphRes.data.filter(p => p.enabled);
                    }
                    this._peDataSources = [];
                    if (protoRes && protoRes.success && protoRes.data) {
                        const protoData = protoRes.data;
                        this._peDataSources = this._extractDataSources(protoData);
                        if (protoData.modbusRtu && protoData.modbusRtu.master) {
                            this._masterTasks = protoData.modbusRtu.master.tasks || [];
                            this._modbusDevices = protoData.modbusRtu.master.devices || [];
                        }
                    }
                    if (!execRes || !execRes.success || !execRes.data) return;
                    const rule = execRes.data;
                    if (!rule) return;

                    document.getElementById('periph-exec-name').value = rule.name || '';
                    document.getElementById('periph-exec-enabled').checked = !!rule.enabled;
                    document.getElementById('periph-exec-exec-mode').value = String(rule.execMode || 0);
                    document.getElementById('periph-exec-report').value = rule.reportAfterExec !== false ? 'true' : 'false';

                    let triggers = rule.triggers || [];
                    if (triggers.length === 0) {
                        triggers = [{
                            triggerType: String(rule.triggerType ?? 0),
                            triggerPeriphId: rule.triggerPeriphId || rule.sourcePeriphId || '',
                            operatorType: String(rule.operatorType ?? 0),
                            compareValue: rule.compareValue || '',
                            timerMode: String(rule.timerMode ?? 0),
                            intervalSec: rule.intervalSec || 60,
                            timePoint: rule.timePoint || '08:00',
                            eventId: rule.eventId || ''
                        }];
                    }

                    let actions = rule.actions || [];
                    if (actions.length === 0) {
                        actions = [{
                            targetPeriphId: rule.targetPeriphId || '',
                            actionType: String(rule.actionType ?? 0),
                            actionValue: rule.actionValue || ''
                        }];
                    }

                    const triggersContainer = document.getElementById('periph-exec-triggers');
                    if (triggersContainer) triggersContainer.innerHTML = '';
                    triggers.forEach((t, i) => this._createPeriphExecTriggerElement(t, i));

                    const actionsContainer = document.getElementById('periph-exec-actions');
                    if (actionsContainer) actionsContainer.innerHTML = '';
                    actions.forEach((a, i) => this._createPeriphExecActionElement(a, i));

                    if (this._hasSetModeTrigger()) {
                        this._syncSetModeToActions(true);
                    }
                });
        },

        deletePeriphExecRule(id) {
            if (!confirm(i18n.t('periph-exec-confirm-delete'))) return;
            apiDelete('/api/periph-exec/', { id: id })
                .then(res => {
                    if (res && res.success) {
                        Notification.success(i18n.t('periph-exec-delete-ok'), i18n.t('periph-exec-title'));
                        if (this.currentPage === 'periph-exec') this.loadPeriphExecPage();
                    } else {
                        Notification.error(res?.error || i18n.t('periph-exec-delete-fail'), i18n.t('periph-exec-title'));
                    }
                })
                .catch(err => {
                    console.error('Delete periph exec rule failed:', err);
                    const isNetworkError = err && (
                        err.name === 'TypeError' ||
                        (err.message && (
                            err.message.includes('Failed to fetch') ||
                            err.message.includes('fetch') ||
                            err.message.includes('network')
                        ))
                    );
                    if (isNetworkError) {
                        Notification.error(i18n.t('device-offline-error'), i18n.t('periph-exec-title'));
                    } else {
                        Notification.error(i18n.t('periph-exec-delete-fail'), i18n.t('periph-exec-title'));
                    }
                });
        },

        togglePeriphExecRule(id, enable) {
            const url = enable ? '/api/periph-exec/enable' : '/api/periph-exec/disable';
            apiPost(url, { id: id })
                .then(res => {
                    if (res && res.success) {
                        if (this.currentPage === 'periph-exec') this.loadPeriphExecPage();
                    } else {
                        Notification.error(res?.error || i18n.t('periph-exec-toggle-fail'), i18n.t('periph-exec-title'));
                    }
                })
                .catch(err => {
                    console.error('Toggle periph exec rule failed:', err);
                    const isNetworkError = err && (
                        err.name === 'TypeError' ||
                        (err.message && (
                            err.message.includes('Failed to fetch') ||
                            err.message.includes('fetch') ||
                            err.message.includes('network')
                        ))
                    );
                    if (isNetworkError) {
                        Notification.error(i18n.t('device-offline-error'), i18n.t('periph-exec-title'));
                    } else {
                        Notification.error(i18n.t('periph-exec-toggle-fail'), i18n.t('periph-exec-title'));
                    }
                });
        },

        runPeriphExecOnce(id) {
            apiPost('/api/periph-exec/run', { id: id })
                .then(res => {
                    if (res && res.success) {
                        Notification.success(i18n.t('periph-exec-run-submitted'), i18n.t('periph-exec-title'));
                        if (this.currentPage === 'periph-exec') this.loadPeriphExecPage();
                    } else {
                        Notification.error(res?.error || i18n.t('periph-exec-run-fail'), i18n.t('periph-exec-title'));
                    }
                })
                .catch(err => {
                    console.error('Run periph exec rule failed:', err);
                    const isNetworkError = err && (
                        err.name === 'TypeError' ||
                        (err.message && (
                            err.message.includes('Failed to fetch') ||
                            err.message.includes('fetch') ||
                            err.message.includes('network')
                        ))
                    );
                    if (isNetworkError) {
                        Notification.error(i18n.t('device-offline-error'), i18n.t('periph-exec-title'));
                    } else {
                        Notification.error(i18n.t('periph-exec-run-fail'), i18n.t('periph-exec-title'));
                    }
                });
        },

        loadPeriphExecPage() {
            const tbody = document.getElementById('periph-exec-table-body');
            if (!tbody) return;
            const filterSel = document.getElementById('periph-exec-filter-periph');
            const filterPeriphName = filterSel ? filterSel.value : '';

            this._populatePeriphExecFilter();

            const apiUrl = '/api/periph-exec?page=' + this._peCurrentPage + '&pageSize=' + this._pePageSize;
            apiGet(apiUrl)
                .then(execRes => {
                    this._peTotalRules = execRes && execRes.total ? execRes.total : 0;
                    const currentPage = execRes && execRes.page ? execRes.page : 1;
                    const currentPageSize = execRes && execRes.pageSize ? execRes.pageSize : 10;
                    
                    if (!execRes || !execRes.success || !execRes.data || execRes.data.length === 0) {
                        tbody.innerHTML = '<tr><td colspan="7" style="text-align:center;color:#999;">' + i18n.t('periph-exec-no-data') + '</td></tr>';
                        this._renderPeriphExecPagination(this._peTotalRules, currentPage, currentPageSize);
                        return;
                    }
                    let rules = execRes.data;
                    if (filterPeriphName) {
                        rules = rules.filter(r => r.targetPeriphName === filterPeriphName);
                    }
                    rules.sort((a, b) => {
                        if (a.enabled !== b.enabled) return a.enabled ? -1 : 1;
                        return (a.name || '').localeCompare(b.name || '', 'zh');
                    });
                    if (rules.length === 0) {
                        tbody.innerHTML = '<tr><td colspan="7" style="text-align:center;color:#999;">' + i18n.t('periph-exec-no-data') + '</td></tr>';
                        this._renderPeriphExecPagination(this._peTotalRules, currentPage, currentPageSize);
                        return;
                    }
                    const triggerLabels = {
                        0: i18n.t('periph-exec-trigger-platform'),
                        1: i18n.t('periph-exec-trigger-timer'),
                        4: i18n.t('periph-exec-trigger-event'),
                        5: i18n.t('periph-exec-trigger-poll'),
                        6: i18n.t('periph-exec-trigger-periph-exec')
                    };
                    const actionLabels = {
                        0: i18n.t('periph-exec-action-high'), 1: i18n.t('periph-exec-action-low'),
                        2: i18n.t('periph-exec-action-blink'), 3: i18n.t('periph-exec-action-breathe'),
                        4: i18n.t('periph-exec-action-pwm'), 5: i18n.t('periph-exec-action-dac'),
                        6: i18n.t('periph-exec-action-restart'), 7: i18n.t('periph-exec-action-factory'),
                        8: i18n.t('periph-exec-action-ntp'), 9: i18n.t('periph-exec-action-ota'),
                        10: i18n.t('periph-exec-action-ap'), 11: i18n.t('periph-exec-action-ble'),
                        12: i18n.t('periph-exec-action-call-periph'),
                        15: i18n.t('periph-exec-action-script')
                    };
                    let html = '';
                    rules.forEach(r => {
                        const statusBadge = r.enabled
                            ? '<span class="badge badge-success">' + i18n.t('periph-exec-status-on') + '</span>'
                            : '<span class="badge badge-info">' + i18n.t('periph-exec-status-off') + '</span>';
                        // 触发器摘要 - 使用 triggerSummary (第一个trigger类型)
                        let triggerText = triggerLabels[r.triggerSummary] || '?';
                        if (r.triggerCount > 1) triggerText += ' (+' + (r.triggerCount - 1) + ')';
                        // 动作摘要 - 使用 actionSummary (第一个action类型)
                        let actionText = actionLabels[r.actionSummary] || '?';
                        if (r.targetPeriphName) actionText += ' \u2192 ' + r.targetPeriphName;
                        if (r.actionCount > 1) actionText += ' (+' + (r.actionCount - 1) + ')';
                        const periphName = r.targetPeriphName || '-';
                        const statsText = i18n.t('periph-exec-stats-count') + ': ' + (r.triggerCount || 0);
                        html += '<tr>';
                        html += '<td>' + (r.name || r.id) + '</td>';
                        html += '<td>' + statusBadge + '</td>';
                        html += '<td style="font-size:12px;">' + triggerText + '</td>';
                        html += '<td>' + periphName + '</td>';
                        html += '<td style="font-size:12px;">' + actionText + '</td>';
                        html += '<td style="font-size:12px;">' + statsText + '</td>';
                        html += '<td style="white-space:nowrap;">';
                        html += '<button class="btn btn-sm btn-run" onclick="app.runPeriphExecOnce(\'' + r.id + '\')">' + i18n.t('periph-exec-run-once') + '</button> ';
                        html += '<button class="btn btn-sm btn-edit" onclick="app.editPeriphExecRule(\'' + r.id + '\')">' + i18n.t('peripheral-edit') + '</button> ';
                        html += '<button class="btn btn-sm ' + (r.enabled ? 'btn-disable' : 'btn-enable') + '" onclick="app.togglePeriphExecRule(\'' + r.id + '\', ' + (r.enabled ? 'false' : 'true') + ')">' + (r.enabled ? i18n.t('peripheral-disable') : i18n.t('peripheral-enable')) + '</button> ';
                        html += '<button class="btn btn-sm btn-delete" onclick="app.deletePeriphExecRule(\'' + r.id + '\')">' + i18n.t('peripheral-delete') + '</button>';
                        html += '</td>';
                        html += '</tr>';
                    });
                    tbody.innerHTML = html;
                    this._renderPeriphExecPagination(this._peTotalRules, currentPage, currentPageSize);
                })
                .catch(() => {
                    tbody.innerHTML = '<tr><td colspan="7" style="text-align:center;color:#999;">' + i18n.t('periph-exec-no-data') + '</td></tr>';
                });
        },

        _renderPeriphExecPagination(total, page, pageSize) {
            const container = document.getElementById('periph-exec-pagination');
            if (!container) return;
            const totalPages = Math.ceil(total / pageSize);
            if (totalPages <= 1) {
                container.innerHTML = '<span style="color:#999;font-size:12px;">' + i18n.t('periph-exec-total') + ': ' + total + '</span>';
                return;
            }

            let html = '<div class="pagination" style="display:flex;justify-content:center;align-items:center;gap:5px;flex-wrap:wrap;">';
            if (page > 1) {
                html += '<button class="btn btn-sm" onclick="app._peCurrentPage=' + (page-1) + ';app.loadPeriphExecPage()">\u00AB</button>';
            } else {
                html += '<button class="btn btn-sm" disabled>\u00AB</button>';
            }
            const maxVisiblePages = 5;
            let startPage = Math.max(1, page - Math.floor(maxVisiblePages / 2));
            let endPage = Math.min(totalPages, startPage + maxVisiblePages - 1);
            if (endPage - startPage + 1 < maxVisiblePages) {
                startPage = Math.max(1, endPage - maxVisiblePages + 1);
            }
            if (startPage > 1) {
                html += '<button class="btn btn-sm" onclick="app._peCurrentPage=1;app.loadPeriphExecPage()">1</button>';
                if (startPage > 2) html += '<span style="padding:0 5px;">...</span>';
            }
            for (let i = startPage; i <= endPage; i++) {
                if (i === page) {
                    html += '<button class="btn btn-sm btn-primary" disabled>' + i + '</button>';
                } else {
                    html += '<button class="btn btn-sm" onclick="app._peCurrentPage=' + i + ';app.loadPeriphExecPage()">' + i + '</button>';
                }
            }
            if (endPage < totalPages) {
                if (endPage < totalPages - 1) html += '<span style="padding:0 5px;">...</span>';
                html += '<button class="btn btn-sm" onclick="app._peCurrentPage=' + totalPages + ';app.loadPeriphExecPage()">' + totalPages + '</button>';
            }
            if (page < totalPages) {
                html += '<button class="btn btn-sm" onclick="app._peCurrentPage=' + (page+1) + ';app.loadPeriphExecPage()">\u00BB</button>';
            } else {
                html += '<button class="btn btn-sm" disabled>\u00BB</button>';
            }
            html += ' <span style="color:#999;font-size:12px;margin-left:10px;">' + i18n.t('periph-exec-total') + ': ' + total + '</span>';
            html += '</div>';
            container.innerHTML = html;
        },

        _populatePeriphExecFilter() {
            const sel = document.getElementById('periph-exec-filter-periph');
            if (!sel || sel._populated) return;
            apiGet('/api/peripherals?pageSize=50').then(res => {
                if (!res || !res.success || !res.data) return;
                const currentVal = sel.value;
                let opts = '<option value="">' + i18n.t('periph-exec-filter-all') + '</option>';
                res.data.forEach(p => {
                    opts += '<option value="' + p.name + '">' + p.name + '</option>';
                });
                sel.innerHTML = opts;
                if (currentVal) sel.value = currentVal;
                sel._populated = true;
            });
        }
    });

    // 自动绑定事件
    if (typeof AppState.setupPeriphExecEvents === 'function') {
        AppState.setupPeriphExecEvents();
    }
})();
