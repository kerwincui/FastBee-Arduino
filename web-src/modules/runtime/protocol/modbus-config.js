/**
 * protocol/modbus-config.js — Modbus Master 任务、设备管理、编辑弹窗、寄存器映射
 */
(function() {
    Object.assign(AppState, {

        // ============ Modbus Master 模式管理 ============

        onModbusModeChange(mode) {
            // 固定为主站模式
        },

        onWorkModeChange(mode) {
            // workMode 已移除，由后端动态推导
        },

        _isTransparentMode() {
            var sel = document.getElementById('rtu-transfer-type');
            return sel && sel.value === '1';
        },

        _onTransferTypeChange() {
            var isTransparent = this._isTransparentMode();
            var addControlItem = document.querySelector('[data-action="_addControlDevice"]');
            if (addControlItem) addControlItem.style.display = isTransparent ? 'none' : '';
            this._renderAllDevices();
        },

        _renderAllDevices() {
            var tbody = document.getElementById('all-devices-body');
            if (!tbody) return;
            var tasks = this._masterTasks || [];
            var devices = this._modbusDevices || [];
            var isTransparent = this._isTransparentMode();
            var visibleDevices = isTransparent ? [] : devices;
            if (tasks.length === 0 && visibleDevices.length === 0) {
                tbody.innerHTML = this._renderProtocolEmptyRow(6, i18n.t('modbus-no-devices') || '暂无子设备');
                return;
            }
            var fcNames = {1: 'FC01', 2: 'FC02', 3: 'FC03', 4: 'FC04'};
            var typeLabels = {
                sensor: i18n.t('modbus-type-sensor') || '采集',
                relay: i18n.t('modbus-type-relay') || '继电器',
                pwm: i18n.t('modbus-type-pwm') || 'PWM',
                pid: i18n.t('modbus-type-pid') || 'PID',
                motor: i18n.t('modbus-type-motor') || '电机'
            };
            var rows = '';
            for (var i = 0; i < tasks.length; i++) {
                rows += this._renderAllDeviceSensorRow(tasks[i], i, fcNames, typeLabels);
            }
            for (var j = 0; j < visibleDevices.length; j++) {
                rows += this._renderAllDeviceControlRow(visibleDevices[j], j, typeLabels);
            }
            tbody.innerHTML = rows;
        },

        _editDevice(source, idx) {
            if (source === 'sensor') {
                this._openTaskEditModal(idx);
            } else {
                this._openEditModal(idx);
            }
        },

        _deleteDevice(source, idx) {
            if (source === 'sensor') {
                if (this._masterTasks) this._masterTasks.splice(idx, 1);
            } else {
                if (this._modbusDevices) this._modbusDevices.splice(idx, 1);
            }
            this._renderAllDevices();
        },

        _showAddDeviceMenu() {
            var menu = document.getElementById('add-device-menu');
            if (!menu) return;
            var isVisible = !menu.classList.contains('fb-hidden');
            if (isVisible) {
                menu.classList.add('fb-hidden');
            } else {
                menu.classList.remove('fb-hidden');
                var closeMenu = function(e) {
                    if (!menu.contains(e.target)) {
                        menu.classList.add('fb-hidden');
                        document.removeEventListener('click', closeMenu);
                    }
                };
                setTimeout(function() { document.addEventListener('click', closeMenu); }, 0);
            }
        },

        _addSensorDevice() {
            var menu = document.getElementById('add-device-menu');
            if (menu) menu.classList.add('fb-hidden');
            if (!this._masterTasks) this._masterTasks = [];
            if (this._masterTasks.length >= 8) {
                Notification.warning('Max 8 sensor devices', i18n.t('modbus-all-devices-title'));
                return;
            }
            this._openTaskEditModal(-1);
        },

        _addControlDevice() {
            var menu = document.getElementById('add-device-menu');
            if (menu) menu.classList.add('fb-hidden');
            if (!this._modbusDevices) this._modbusDevices = [];
            if (this._modbusDevices.length >= 8) {
                Notification.warning('Max 8 control devices', i18n.t('modbus-all-devices-title'));
                return;
            }
            this._openEditModal(-1);
        },

        // ============ 轮询任务编辑弹窗 ============

        _openTaskEditModal(idx) {
            var modal = document.getElementById('task-edit-modal');
            if (!modal) return;
            this._editingTaskIdx = idx;
            var task;
            if (idx >= 0 && this._masterTasks && this._masterTasks[idx]) {
                task = this._masterTasks[idx];
            } else {
                task = { slaveAddress: 1, functionCode: 3, startAddress: 0, quantity: 10, enabled: true, name: '', mappings: [] };
            }
            var f = function(id) { return document.getElementById(id); };
            f('task-edit-slave-addr').value = task.slaveAddress || 1;
            f('task-edit-fc').value = task.functionCode || 3;
            f('task-edit-start-addr').value = task.startAddress || 0;
            f('task-edit-quantity').value = task.quantity || 10;
            f('task-edit-name').value = task.name || task.label || '';
            f('task-edit-type').value = task.deviceType || 'holding';
            f('task-edit-enabled').checked = task.enabled !== false;
            var titleEl = modal.querySelector('.modal-header h3');
            if (titleEl) titleEl.textContent = idx < 0 ? i18n.t('modbus-task-add-title') : i18n.t('modbus-task-edit-title');
            AppState.showModal(modal);
        },

        _closeTaskEditModal() {
            var modal = document.getElementById('task-edit-modal');
            if (modal) AppState.hideModal(modal);
            this._editingTaskIdx = -1;
        },

        _onTaskTypeChange(val) {
            var fcMap = { holding: '3', input: '4', coil: '1', discrete: '2' };
            var fcEl = document.getElementById('task-edit-fc');
            if (fcEl && fcMap[val]) fcEl.value = fcMap[val];
        },

        _saveTaskEditModal() {
            var f = function(id) { return document.getElementById(id); };
            var task = {
                slaveAddress: parseInt(f('task-edit-slave-addr').value) || 1,
                functionCode: parseInt(f('task-edit-fc').value) || 3,
                startAddress: parseInt(f('task-edit-start-addr').value) || 0,
                quantity: parseInt(f('task-edit-quantity').value) || 10,
                name: f('task-edit-name').value || '',
                deviceType: f('task-edit-type').value || 'holding',
                enabled: f('task-edit-enabled').checked
            };
            if (!this._masterTasks) this._masterTasks = [];
            if (this._editingTaskIdx >= 0 && this._masterTasks[this._editingTaskIdx]) {
                var oldTask = this._masterTasks[this._editingTaskIdx];
                task.mappings = oldTask.mappings || [];
                task.pollInterval = oldTask.pollInterval;
                this._masterTasks[this._editingTaskIdx] = task;
            } else {
                task.mappings = [];
                this._masterTasks.push(task);
            }
            this._renderAllDevices();
            this._closeTaskEditModal();
        },

        addMasterPollTask() {
            if (!this._masterTasks) this._masterTasks = [];
            if (this._masterTasks.length >= 8) {
                Notification.warning('Max 8 tasks', i18n.t('modbus-master-title'));
                return;
            }
            this._openTaskEditModal(-1);
        },

        removeMasterPollTask(idx) {
            if (this._masterTasks) {
                this._masterTasks.splice(idx, 1);
                this._renderAllDevices();
            }
        },

        // ============ 寄存器映射管理 ============

        openMappingModal(taskIdx) {
            if (!this._masterTasks || !this._masterTasks[taskIdx]) return;
            this._currentMappingTaskIdx = taskIdx;
            this._currentMappings = JSON.parse(JSON.stringify(this._masterTasks[taskIdx].mappings || []));
            this._renderMappingTable();
            AppState.showModal('mapping-modal');
        },

        closeMappingModal() {
            AppState.hideModal('mapping-modal');
            this._currentMappingTaskIdx = -1;
            this._currentMappings = [];
        },

        saveMappingModal() {
            if (this._currentMappingTaskIdx < 0) return;
            this._collectMappingValues();
            this._masterTasks[this._currentMappingTaskIdx].mappings = this._currentMappings;
            this._renderAllDevices();
            this.closeMappingModal();
        },

        addMapping() {
            if (this._currentMappings.length >= 8) {
                Notification.warning(i18n.t('modbus-mapping-max'));
                return;
            }
            this._collectMappingValues();
            this._currentMappings.push({ regOffset: 0, dataType: 0, scaleFactor: 0.1, decimalPlaces: 1, sensorId: '', unit: '' });
            this._renderMappingTable();
        },

        removeMapping(idx) {
            this._collectMappingValues();
            this._currentMappings.splice(idx, 1);
            this._renderMappingTable();
        },

        _collectMappingValues() {
            const tbody = document.getElementById('mapping-table-body');
            if (!tbody) return;
            const rows = tbody.querySelectorAll('tr');
            rows.forEach((row, idx) => {
                if (idx >= this._currentMappings.length) return;
                const inputs = row.querySelectorAll('input, select');
                if (inputs.length >= 6) {
                    this._currentMappings[idx].regOffset = parseInt(inputs[0].value) || 0;
                    this._currentMappings[idx].dataType = parseInt(inputs[1].value) || 0;
                    this._currentMappings[idx].scaleFactor = parseFloat(inputs[2].value) || 1.0;
                    this._currentMappings[idx].decimalPlaces = parseInt(inputs[3].value) || 1;
                    this._currentMappings[idx].sensorId = inputs[4].value || '';
                    this._currentMappings[idx].unit = inputs[5].value || '';
                }
            });
        },

        _renderMappingTable() {
            const tbody = document.getElementById('mapping-table-body');
            if (!tbody) return;
            const dtOpts = [
                {v: 0, t: 'uint16'}, {v: 1, t: 'int16'},
                {v: 2, t: 'uint32'}, {v: 3, t: 'int32'}, {v: 4, t: 'float32'}
            ];
            if (this._currentMappings.length === 0) {
                tbody.innerHTML = this._renderProtocolEmptyRow(7, i18n.t('modbus-master-no-tasks'));
                return;
            }
            tbody.innerHTML = this._currentMappings.map((m, idx) => {
                const dtSelect = dtOpts.map(o =>
                    '<option value="' + o.v + '"' + (m.dataType === o.v ? ' selected' : '') + '>' + o.t + '</option>'
                ).join('');
                return '<tr>' +
                    '<td><input type="number" class="protocol-mapping-num-sm" value="' + (m.regOffset || 0) + '" min="0" max="124"></td>' +
                    '<td><select class="protocol-mapping-select">' + dtSelect + '</select></td>' +
                    '<td><input type="number" class="protocol-mapping-num-md" value="' + (m.scaleFactor ?? 0.1) + '" step="0.001"></td>' +
                    '<td><input type="number" class="protocol-mapping-num-sm" value="' + (m.decimalPlaces ?? 1) + '" min="0" max="6"></td>' +
                    '<td><input type="text" class="protocol-mapping-text" value="' + (m.sensorId || '') + '" maxlength="15" placeholder="temperature"></td>' +
                    '<td><input type="text" class="protocol-mapping-text protocol-mapping-unit" value="' + (m.unit || '') + '" maxlength="7" placeholder="°C"></td>' +
                    '<td><button type="button" class="fb-btn fb-btn-sm fb-btn-danger protocol-mapping-remove" data-index="' + idx + '">删除</button></td>' +
                '</tr>';
            }).join('');
        },

        // ============ 设备加载与管理 ============

        _loadModbusDevices() {
            var serverDevices = [];
            try {
                var rtu = this._protocolConfig && this._protocolConfig.modbusRtu;
                if (rtu && rtu.master && rtu.master.devices && rtu.master.devices.length > 0) {
                    serverDevices = Array.isArray(rtu.master.devices)
                        ? rtu.master.devices.slice()
                        : Object.keys(rtu.master.devices).map(function(k) { return rtu.master.devices[k]; });
                }
            } catch(e) {}

            if (serverDevices.length === 0) {
                try {
                    var raw = localStorage.getItem('modbus_devices');
                    if (raw) {
                        var localDevices = JSON.parse(raw);
                        if (localDevices && localDevices.length > 0) {
                            serverDevices = localDevices.map(function(d) {
                                return {
                                    name: d.name || 'Device',
                                    sensorId: d.sensorId || '',
                                    deviceType: d.deviceType || d.type || 'relay',
                                    slaveAddress: d.slaveAddress || 1,
                                    channelCount: d.channelCount || 2,
                                    coilBase: d.coilBase || 0,
                                    ncMode: !!d.ncMode,
                                    controlProtocol: d.relayMode === 'register' ? 1 : 0,
                                    batchRegister: d.batchRegister || 0,
                                    pwmRegBase: d.pwmRegBase || 0,
                                    pwmResolution: d.pwmResolution || 8,
                                    pidAddrs: [
                                        d.pidPvAddr || 0, d.pidSvAddr || 1, d.pidOutAddr || 2,
                                        d.pidPAddr || 3, d.pidIAddr || 4, d.pidDAddr || 5
                                    ],
                                    pidDecimals: d.pidDecimals || 1,
                                    enabled: d.enabled !== false
                                };
                            });
                        }
                    }
                } catch(e) {}
            }

            this._modbusDevices = serverDevices;
            for (var i = 0; i < this._modbusDevices.length; i++) {
                if (!this._modbusDevices[i].deviceType) {
                    this._modbusDevices[i].deviceType = this._modbusDevices[i].type || 'relay';
                }
            }
            this._activeDeviceIdx = -1;
            this._renderAllDevices();
        },

        _refreshModbusDeviceList() {
            var btn = document.getElementById('modbus-refresh-devices-btn');
            if (btn && btn.disabled) return;
            if (btn) {
                btn.disabled = true;
                btn.innerHTML = '<span class="fb-spin">&#x21bb;</span> 加载中...';
            }
            this._protocolConfig = null;
            if (typeof window.apiInvalidateCache === 'function') {
                window.apiInvalidateCache('/api/protocol/config');
            }
            var self = this;
            apiGet('/api/protocol/config')
                .then(function(res) {
                    if (res && res.success) {
                        self._protocolConfig = res.data || {};
                        self._fillProtocolForm('modbus-rtu', self._protocolConfig);
                        Notification.success('设备列表已刷新');
                    } else {
                        Notification.error('获取配置失败');
                    }
                })
                .catch(function(err) {
                    if (!(err && err._pageAborted)) {
                        Notification.error('刷新失败，请检查网络');
                    }
                })
                .finally(function() {
                    if (btn) {
                        btn.disabled = false;
                        btn.innerHTML = '&#x21bb; 刷新列表';
                    }
                });
        },

        _renderDeviceTable() {
            var tbody = document.getElementById('modbus-devices-body');
            if (!tbody) return;
            if (!this._modbusDevices || this._modbusDevices.length === 0) {
                tbody.innerHTML = this._renderProtocolEmptyRow(8, i18n.t('modbus-device-no-devices') || '暂无子设备');
                return;
            }
            var typeLabels = { relay: i18n.t('modbus-ctrl-type-relay') || '继电器', pwm: 'PWM', pid: 'PID', motor: i18n.t('modbus-type-motor') || '电机' };
            var protLabels = ['Coil', 'Register'];
            var self = this;
            tbody.innerHTML = this._modbusDevices.map(function(dev, idx) {
                return self._renderModbusDeviceRow(dev, idx, typeLabels, protLabels);
            }).join('');
        },

        _updateDevice(idx, field, value) {
            if (this._modbusDevices && this._modbusDevices[idx]) {
                this._modbusDevices[idx][field] = value;
                if (field === 'deviceType') {
                    if (idx === this._activeDeviceIdx) {
                        this._activateDevice(idx);
                    } else {
                        this._renderAllDevices();
                    }
                }
            }
        },

        _updateActiveDeviceExt(field, value) {
            if (this._activeDeviceIdx < 0 || !this._modbusDevices) return;
            var dev = this._modbusDevices[this._activeDeviceIdx];
            if (!dev) return;
            if (field.indexOf('pidAddrs.') === 0) {
                var arrIdx = parseInt(field.split('.')[1]);
                if (!dev.pidAddrs) dev.pidAddrs = [0, 1, 2, 3, 4, 5];
                dev.pidAddrs[arrIdx] = value;
            } else {
                dev[field] = value;
            }
        },

        addModbusDevice() {
            if (!this._modbusDevices) this._modbusDevices = [];
            if (this._modbusDevices.length >= 8) {
                Notification.warning(i18n.t('modbus-device-max-reached') || '最多8个子设备');
                return;
            }
            this._openEditModal(-1);
        },

        _removeDevice(idx) {
            if (!this._modbusDevices) return;
            var msg = i18n.t('modbus-device-delete-confirm') || '确定要删除此设备？';
            if (!confirm(msg)) return;
            var devKey = 'dev_' + idx;
            delete this._deviceCoilCache[devKey];
            delete this._devicePwmCache[devKey];
            delete this._devicePidCache[devKey];
            this._modbusDevices.splice(idx, 1);
            if (this._activeDeviceIdx === idx) {
                this._activeDeviceIdx = -1;
            } else if (this._activeDeviceIdx > idx) {
                this._activeDeviceIdx--;
            }
            this._renderAllDevices();
        },

        // ========== 编辑弹窗 ==========

        _openEditModal(idx) {
            this._editingDeviceIdx = idx;
            var dev = (idx >= 0 && this._modbusDevices && this._modbusDevices[idx])
                ? this._modbusDevices[idx] : null;
            var modal = document.getElementById('modbus-device-edit-modal');
            if (!modal) return;
            document.getElementById('mdev-edit-name').value = dev ? (dev.name || '') : ((i18n.t('modbus-ctrl-device-default-name') || '设备') + ((this._modbusDevices ? this._modbusDevices.length : 0) + 1));
            document.getElementById('mdev-edit-sensorid').value = dev ? (dev.sensorId || '') : '';
            document.getElementById('mdev-edit-type').value = dev ? (dev.deviceType || 'relay') : 'relay';
            document.getElementById('mdev-edit-addr').value = dev ? (dev.slaveAddress || 1) : 1;
            document.getElementById('mdev-edit-ch').value = String(dev ? (dev.channelCount || 2) : 2);
            document.getElementById('mdev-edit-base').value = dev ? (dev.coilBase || 0) : 0;
            document.getElementById('mdev-edit-protocol').value = String(dev ? (dev.controlProtocol || 0) : 0);
            document.getElementById('mdev-edit-nc').value = dev ? (dev.ncMode ? 'true' : 'false') : 'true';
            document.getElementById('mdev-edit-enabled').checked = dev ? (dev.enabled !== false) : true;
            document.getElementById('mdev-edit-batch-reg').value = dev ? (dev.batchRegister || 0) : 0;
            document.getElementById('mdev-edit-batch-type').value = String(dev ? (dev.batchRegType || 0) : 0);
            document.getElementById('mdev-edit-pwm-reg-base').value = dev ? (dev.pwmRegBase || 0) : 0;
            document.getElementById('mdev-edit-pwm-resolution').value = String(dev ? (dev.pwmResolution || 8) : 8);
            var pidA = dev ? (dev.pidAddrs || [0,1,2,3,4,5]) : [0,1,2,3,4,5];
            var pidFields = ['pv','sv','out','p','i','d'];
            for (var pi = 0; pi < pidFields.length; pi++) {
                var pe = document.getElementById('mdev-edit-pid-' + pidFields[pi]);
                if (pe) pe.value = pidA[pi] || pi;
            }
            document.getElementById('mdev-edit-pid-decimals').value = String(dev ? (dev.pidDecimals || 1) : 1);
            var motorRegs = dev ? (dev.motorRegs || [0,1,2,5,7]) : [0,1,2,5,7];
            var motorFields = ['fwd','rev','stop','speed','pulse'];
            for (var mi = 0; mi < motorFields.length; mi++) {
                var me = document.getElementById('mdev-edit-motor-' + motorFields[mi]);
                if (me) me.value = motorRegs[mi] != null ? motorRegs[mi] : [0,1,2,5,7][mi];
            }
            document.getElementById('mdev-edit-motor-decimals').value = String(dev ? (dev.motorDecimals || 0) : 0);
            this._onEditTypeChange();
            var title = document.getElementById('modbus-edit-modal-title');
            if (title) title.textContent = (idx >= 0)
                ? (i18n.t('modbus-device-edit-title') || '编辑子设备')
                : (i18n.t('modbus-device-add') || '添加设备');
            AppState.showModal(modal);
        },

        _onEditTypeChange() {
            var type = document.getElementById('mdev-edit-type').value;
            var pwmSec = document.getElementById('mdev-edit-pwm-section');
            var pidSec = document.getElementById('mdev-edit-pid-section');
            var motorSec = document.getElementById('mdev-edit-motor-section');
            var relaySec = document.getElementById('mdev-edit-relay-section');
            if (pwmSec) pwmSec.classList.remove('fb-hidden');
            if (pidSec) pidSec.classList.remove('fb-hidden');
            if (motorSec) motorSec.classList.remove('fb-hidden');
            if (relaySec) relaySec.classList.remove('fb-hidden');
            if (pwmSec) AppState.toggleVisible(pwmSec, type === 'pwm');
            if (pidSec) AppState.toggleVisible(pidSec, type === 'pid');
            if (motorSec) AppState.toggleVisible(motorSec, type === 'motor');
            if (relaySec) AppState.toggleVisible(relaySec, type === 'relay');
        },

        _closeEditModal() {
            var modal = document.getElementById('modbus-device-edit-modal');
            if (modal) AppState.hideModal(modal);
            this._editingDeviceIdx = -1;
        },

        _saveEditModal() {
            var idx = this._editingDeviceIdx;
            var isNew = (idx < 0);
            if (isNew) {
                if (!this._modbusDevices || !Array.isArray(this._modbusDevices)) {
                    this._modbusDevices = [];
                }
                if (this._modbusDevices.length >= 8) {
                    Notification.warning(i18n.t('modbus-device-max-reached') || '最多8个子设备');
                    return;
                }
                this._modbusDevices.push({});
                idx = this._modbusDevices.length - 1;
            }
            var dev = this._modbusDevices[idx];
            if (!dev) {
                console.error('_saveEditModal: dev is undefined, idx=', idx, 'devices=', this._modbusDevices);
                return;
            }
            dev.name = document.getElementById('mdev-edit-name').value || 'Device';
            dev.sensorId = (document.getElementById('mdev-edit-sensorid').value || '').trim();
            dev.deviceType = document.getElementById('mdev-edit-type').value || 'relay';
            dev.slaveAddress = parseInt(document.getElementById('mdev-edit-addr').value) || 1;
            dev.channelCount = parseInt(document.getElementById('mdev-edit-ch').value) || 2;
            dev.coilBase = parseInt(document.getElementById('mdev-edit-base').value) || 0;
            dev.controlProtocol = parseInt(document.getElementById('mdev-edit-protocol').value) || 0;
            dev.ncMode = (document.getElementById('mdev-edit-nc').value === 'true');
            dev.enabled = document.getElementById('mdev-edit-enabled').checked;
            dev.batchRegister = parseInt(document.getElementById('mdev-edit-batch-reg').value) || 0;
            dev.batchRegType = parseInt(document.getElementById('mdev-edit-batch-type').value) || 0;
            dev.pwmRegBase = parseInt(document.getElementById('mdev-edit-pwm-reg-base').value) || 0;
            dev.pwmResolution = parseInt(document.getElementById('mdev-edit-pwm-resolution').value) || 8;
            var pidFields = ['pv','sv','out','p','i','d'];
            dev.pidAddrs = [];
            for (var pi = 0; pi < pidFields.length; pi++) {
                dev.pidAddrs.push(parseInt(document.getElementById('mdev-edit-pid-' + pidFields[pi]).value) || pi);
            }
            dev.pidDecimals = parseInt(document.getElementById('mdev-edit-pid-decimals').value) || 1;
            dev.motorRegs = [];
            var motorFields = ['fwd','rev','stop','speed','pulse'];
            for (var mi = 0; mi < motorFields.length; mi++) {
                dev.motorRegs.push(parseInt(document.getElementById('mdev-edit-motor-' + motorFields[mi]).value) || [0,1,2,5,7][mi]);
            }
            dev.motorDecimals = parseInt(document.getElementById('mdev-edit-motor-decimals').value) || 0;
            this._renderAllDevices();
            this._closeEditModal();
        },

        // ========== UART 外设 ==========

        async _loadUartPeripherals(selectedId) {
            const select = document.getElementById('rtu-peripheral-id');
            if (!select) return;
            try {
                const res = await apiGet('/api/peripherals?pageSize=50');
                if (!res || !res.success) return;
                this._uartPeripherals = (res.data || []).filter(p => p.type === 1 && p.enabled);
                select.innerHTML = '<option value="" disabled>' + i18n.t('rtu-peripheral-placeholder') + '</option>';
                if (this._uartPeripherals.length === 0) {
                    select.innerHTML += '<option value="" disabled>' + i18n.t('rtu-no-uart-peripherals') + '</option>';
                    return;
                }
                this._uartPeripherals.forEach(p => {
                    const pinsText = p.pins && p.pins.length >= 2 ? ' (RX:' + p.pins[0] + ', TX:' + p.pins[1] + ')' : '';
                    const opt = document.createElement('option');
                    opt.value = p.id;
                    opt.textContent = p.name + pinsText;
                    select.appendChild(opt);
                });
                if (selectedId) {
                    select.value = selectedId;
                    this.onRtuPeripheralChange(selectedId);
                }
            } catch (e) { console.error('Failed to load UART peripherals:', e); }
        },

        onRtuPeripheralChange(peripheralId) {
            const infoDiv = document.getElementById('rtu-peripheral-info');
            if (!infoDiv || !peripheralId) { if (infoDiv) AppState.hideElement(infoDiv); return; }
            const periph = (this._uartPeripherals || []).find(p => p.id === peripheralId);
            if (!periph) { AppState.hideElement(infoDiv); return; }
            apiGet('/api/peripherals/?id=' + peripheralId).then(res => {
                if (!res || !res.success) return;
                const data = res.data;
                const baudRate = data.params?.baudRate || i18n.t('unknown') || '未知';
                const pins = data.pins || [];
                infoDiv.textContent = 'RX: GPIO' + pins[0] + ', TX: GPIO' + pins[1] + ', ' + i18n.t('uart-baudrate-label') + ': ' + baudRate;
                AppState.showElement(infoDiv);
            }).catch(() => {
                const pins = periph.pins || [];
                infoDiv.textContent = 'RX: GPIO' + pins[0] + ', TX: GPIO' + pins[1];
                AppState.showElement(infoDiv);
            });
        }
    });
})();
