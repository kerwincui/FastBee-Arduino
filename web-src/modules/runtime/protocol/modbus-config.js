/**
 * protocol/modbus-config.js — Modbus Master 任务、设备管理、编辑弹窗、寄存器映射
 */
(function() {
    // This lazy module must also work when protocol is the first page opened.
    const t = typeof window.t === 'function' ? window.t : (key, fallback) => fallback;
    Object.assign(AppState, {

        // Modbus-only table rendering is loaded with the RTU editor.
        _renderProtocolEmptyRow(colspan, text) {
            return '<tr><td colspan="' + colspan + '" class="u-empty-cell">' + (text || '') + '</td></tr>';
        },

        _renderProtocolStatus(enabled) {
            const isEnabled = enabled !== false;
            return '<span class="' + (isEnabled ? 'protocol-status-on' : 'protocol-status-off') + '">' +
                (isEnabled ? 'ON' : 'OFF') + '</span>';
        },

        _getProtocolBadgeTone(type) {
            const tones = {
                sensor: 'protocol-badge--sensor',
                relay: 'protocol-badge--relay',
                pwm: 'protocol-badge--pwm',
                pid: 'protocol-badge--pid'
            };
            return tones[type] || 'protocol-badge--sensor';
        },

        _renderProtocolBadge(type, label) {
            return '<span class="protocol-badge ' + this._getProtocolBadgeTone(type) + '">' +
                (label || type || '-') + '</span>';
        },

        _renderProtocolActionButton(label, tone, action, index, source) {
            var attrs = 'data-protocol-action="' + action + '"';
            if (index !== undefined && index !== null) attrs += ' data-index="' + index + '"';
            if (source) attrs += ' data-source="' + source + '"';
            var locked = action !== 'open-control-modal' && !this.isDeveloperModeEnabled();
            if (locked) attrs += ' disabled title="' + escapeHtml(this.getDeveloperModeDisabledText()) + '"';
            return '<button type="button" class="fb-btn fb-btn-sm protocol-action-btn protocol-action-btn--' + tone +
                (locked ? ' dev-mode-locked' : '') + '" ' + attrs + '>' + label + '</button>';
        },

        _renderProtocolActionCell(buttons) {
            return '<td class="protocol-action-cell">' + buttons.join('') + '</td>';
        },

        _getProtocolControlDeviceInfo(device) {
            var deviceType = device.deviceType || 'relay';
            var info = (device.channelCount || 2) + 'ch';
            if (deviceType === 'relay') {
                info += ' ' + (device.controlProtocol === 1 ? 'Reg' : 'Coil') + '@' + (device.coilBase || 0);
            } else if (deviceType === 'pwm') {
                info += ' Reg@' + (device.pwmRegBase || 0) + ' ' + (device.pwmResolution || 8) + 'bit';
            } else if (deviceType === 'pid') {
                info += ' PV@' + ((device.pidAddrs && device.pidAddrs[0]) || 0);
            }
            if (device.sensorId) info += ' id:' + device.sensorId;
            return info;
        },

        // ========== 设备行渲染 ==========

        _renderAllDeviceSensorRow(task, index, fcNames, typeLabels) {
            var label = task.name || task.label || ('Slave ' + (task.slaveAddress || 1));
            var info = (fcNames[task.functionCode] || 'FC03') + ' @' + (task.startAddress || 0) + ' x' + (task.quantity || 10);
            var mappingCount = (task.mappings && task.mappings.length) || 0;
            if (mappingCount > 0) info += ' [' + mappingCount + ' ' + t('proto-mapping-count', '映射') + ']';
            // 采集任务始终允许打开寄存器映射配置：
            // - JSON 模式：映射用于解析寄存器为物模型属性
            // - 透传模式：映射仍可用于本地缓存展示（device-control/render.js 依赖 mappings）
            var actions = [
                this._renderProtocolActionButton(t('proto-btn-edit', '编辑'), 'primary', 'edit-device', index, 'sensor'),
                this._renderProtocolActionButton(t('proto-btn-mapping', '映射'), 'warning', 'open-mapping', index),
                this._renderProtocolActionButton(t('proto-btn-delete', '删除'), 'danger', 'delete-device', index, 'sensor')
            ];
            return '<tr>' +
                '<td>' + escapeHtml(label) + '</td>' +
                '<td>' + this._renderProtocolBadge('sensor', typeLabels.sensor) + '</td>' +
                '<td>' + (task.slaveAddress || 1) + '</td>' +
                '<td><small>' + escapeHtml(info) + '</small></td>' +
                '<td>' + this._renderProtocolStatus(task.enabled) + '</td>' +
                this._renderProtocolActionCell(actions) +
                '</tr>';
        },

        _renderAllDeviceControlRow(device, index, typeLabels) {
            var deviceType = device.deviceType || 'relay';
            return '<tr>' +
                '<td>' + escapeHtml(device.name || '-') + '</td>' +
                '<td>' + this._renderProtocolBadge(deviceType, typeLabels[deviceType] || deviceType) + '</td>' +
                '<td>' + (device.slaveAddress || 1) + '</td>' +
                '<td><small>' + escapeHtml(this._getProtocolControlDeviceInfo(device)) + '</small></td>' +
                '<td>' + this._renderProtocolStatus(device.enabled) + '</td>' +
                this._renderProtocolActionCell([
                    this._renderProtocolActionButton(t('proto-btn-edit', '编辑'), 'primary', 'edit-device', index, 'control'),
                    this._renderProtocolActionButton(t('proto-btn-control', '控制'), 'warning', 'open-control-modal', index),
                    this._renderProtocolActionButton(t('proto-btn-delete', '删除'), 'danger', 'delete-device', index, 'control')
                ]) +
                '</tr>';
        },

        _renderModbusDeviceRow(device, index, typeLabels, protocolLabels) {
            var deviceType = device.deviceType || 'relay';
            var controlProtocol = device.controlProtocol || 0;
            return '<tr>' +
                '<td>' + escapeHtml(device.name || '-') + '</td>' +
                '<td>' + this._renderProtocolBadge(deviceType, typeLabels[deviceType] || deviceType) + '</td>' +
                '<td>' + (device.slaveAddress || 1) + '</td>' +
                '<td>' + (device.channelCount || 2) + '</td>' +
                '<td>' + (device.coilBase || 0) + '</td>' +
                '<td>' + (protocolLabels[controlProtocol] || 'Coil') + '</td>' +
                '<td>' + (device.ncMode ? 'ON' : '-') + '</td>' +
                this._renderProtocolActionCell([
                    this._renderProtocolActionButton(t('proto-btn-edit', '编辑'), 'primary', 'open-edit-modal', index),
                    this._renderProtocolActionButton(t('proto-btn-control', '控制'), 'warning', 'open-control-modal', index),
                    this._renderProtocolActionButton(t('proto-btn-delete', '删除'), 'danger', 'remove-device', index)
                ]) +
                '</tr>';
        },


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
            this._renderAllDevices();
        },

        _renderAllDevices() {
            var tbody = document.getElementById('all-devices-body');
            if (!tbody) return;
            this.applyDeveloperModeState();
            var tasks = this._masterTasks || [];
            var devices = this._modbusDevices || [];
            var hasControlDevices = devices.some(device => this._isModbusControlDevice(device));
            if (tasks.length === 0 && !hasControlDevices) {
                tbody.innerHTML = this._renderProtocolEmptyRow(6, '暂无子设备');
                return;
            }
            var fcNames = {1: 'FC01', 2: 'FC02', 3: 'FC03', 4: 'FC04'};
            var typeLabels = {
                sensor: '采集',
                relay: '继电器',
                pwm: 'PWM',
                pid: 'PID',
                motor: '电机'
            };
            var rows = '';
            for (var i = 0; i < tasks.length; i++) {
                rows += this._renderAllDeviceSensorRow(tasks[i], i, fcNames, typeLabels);
            }
            // Keep persisted indices: execution rules address devices as modbus:N.
            for (var j = 0; j < devices.length; j++) {
                if (!this._isModbusControlDevice(devices[j])) continue;
                rows += this._renderAllDeviceControlRow(devices[j], j, typeLabels);
            }
            tbody.innerHTML = rows;
            this.applyDeveloperModeState(tbody);
        },

        _editDevice(source, idx) {
            if (!this.guardDeveloperModeAction()) return;
            if (source === 'sensor') {
                this._openTaskEditModal(idx);
            } else {
                this._openEditModal(idx);
            }
        },

        _isModbusControlDevice(device) {
            return !!device && ['relay', 'pwm', 'pid', 'motor'].includes(device.deviceType || device.type || 'relay');
        },

        async _deleteDevice(source, idx) {
            if (!this.guardDeveloperModeAction() || this._modbusDeletePending) return false;
            var key = source === 'sensor' ? '_masterTasks' : '_modbusDevices';
            var original = this[key];
            if (!Array.isArray(original) || !Number.isInteger(idx) || idx < 0 || idx >= original.length) return false;
            if (!confirm('确定要删除此设备并保存配置？')) return false;
            var form = document.getElementById('modbus-rtu-form');
            var wasInert = form && form.inert;
            this._modbusDeletePending = true;
            if (form) form.inert = true;
            this[key] = original.slice();
            this[key].splice(idx, 1);
            try {
                var saved = await this.saveProtocolConfig('modbus-rtu-form');
                if (!saved) {
                    this[key] = original;
                    return false;
                }
                // All caches are indexed; a splice invalidates every following slot.
                this._deviceCoilCache = {};
                this._devicePwmCache = {};
                this._devicePidCache = {};
                this._activeDeviceIdx = -1;
                this._currentMappingTaskIdx = -1;
                return true;
            } catch (err) {
                this[key] = original;
                Notification.error(err.message || '删除保存失败，已恢复列表', 'Modbus 子设备');
                return false;
            } finally {
                this._modbusDeletePending = false;
                if (form) form.inert = wasInert;
                this._renderAllDevices();
            }
        },

        _showAddDeviceMenu() {
            if (!this.guardDeveloperModeAction()) return;
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
            if (!this.guardDeveloperModeAction()) return;
            var menu = document.getElementById('add-device-menu');
            if (menu) menu.classList.add('fb-hidden');
            if (!this._masterTasks) this._masterTasks = [];
            if (this._masterTasks.length >= 8) {
                Notification.warning('最多8个采集设备', 'Modbus 子设备');
                return;
            }
            this._openTaskEditModal(-1);
        },

        _addControlDevice() {
            if (!this.guardDeveloperModeAction()) return;
            var menu = document.getElementById('add-device-menu');
            if (menu) menu.classList.add('fb-hidden');
            if (!this._modbusDevices) this._modbusDevices = [];
            if (this._modbusDevices.length >= 8) {
                Notification.warning('最多8个控制设备', 'Modbus 子设备');
                return;
            }
            this._openEditModal(-1);
        },

        // ============ 轮询任务编辑弹窗 ============

        _openTaskEditModal(idx) {
            if (!this.guardDeveloperModeAction()) return;
            var modal = AppState.ensureModal('task-edit-modal');
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
            f('task-edit-enabled').checked = task.enabled !== false;
            var titleEl = modal.querySelector('.modal-header h3');
            if (titleEl) titleEl.textContent = idx < 0 ? '新建子设备' : '编辑子设备';
            AppState.showModal(modal);
        },

        _closeTaskEditModal() {
            var modal = document.getElementById('task-edit-modal');
            if (modal) AppState.hideModal(modal);
            this._editingTaskIdx = -1;
        },

        _saveTaskEditModal() {
            if (!this.guardDeveloperModeAction()) return;
            var f = function(id) { return document.getElementById(id); };
            var fc = parseInt(f('task-edit-fc').value) || 3;
            var fcToDeviceType = {1: 'coil', 2: 'discrete', 3: 'holding', 4: 'input'};
            var task = {
                slaveAddress: parseInt(f('task-edit-slave-addr').value) || 1,
                functionCode: fc,
                startAddress: parseInt(f('task-edit-start-addr').value) || 0,
                quantity: parseInt(f('task-edit-quantity').value) || 10,
                name: f('task-edit-name').value || '',
                deviceType: fcToDeviceType[fc] || 'holding',
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
            if (!this.guardDeveloperModeAction()) return;
            if (!this._masterTasks) this._masterTasks = [];
            if (this._masterTasks.length >= 8) {
                Notification.warning('最多8个轮询任务', 'Modbus 采集');
                return;
            }
            this._openTaskEditModal(-1);
        },

        removeMasterPollTask(idx) {
            return this._deleteDevice('sensor', idx);
        },

        // ============ 寄存器映射管理 ============

        openMappingModal(taskIdx) {
            if (!this.guardDeveloperModeAction()) return;
            if (!this._masterTasks || !this._masterTasks[taskIdx]) return;
            if (!AppState.ensureModal('mapping-modal')) return;
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
            if (!this.guardDeveloperModeAction()) return;
            if (this._currentMappingTaskIdx < 0) return;
            if (this._collectMappingValues() === false) return;
            this._masterTasks[this._currentMappingTaskIdx].mappings = this._currentMappings;
            this._renderAllDevices();
            this.closeMappingModal();
        },

        addMapping() {
            if (!this.guardDeveloperModeAction()) return;
            if (this._currentMappings.length >= 8) {
                Notification.warning('每任务最多8个映射');
                return;
            }
            if (this._collectMappingValues() === false) return;
            this._currentMappings.push({ regOffset: 0, dataType: 0, wordOrder: 0, scaleFactor: 0.1, decimalPlaces: 1, sensorId: '', unit: '' });
            this._renderMappingTable();
        },

        removeMapping(idx) {
            if (!this.guardDeveloperModeAction()) return;
            if (this._collectMappingValues() === false) return;
            this._currentMappings.splice(idx, 1);
            this._renderMappingTable();
        },

        _collectMappingValues() {
            const tbody = document.getElementById('mapping-table-body');
            if (!tbody) return;
            const rows = tbody.querySelectorAll('tr');
            const next = this._currentMappings.map(item => ({...item}));
            try {
                rows.forEach((row, idx) => {
                    if (idx >= next.length) return;
                    const inputs = row.querySelectorAll('input, select');
                    if (inputs.length < 7) throw new Error('映射表单不完整，请重新打开');
                    for (const position of [0,1,2,3,6]) if (!inputs[position].value.trim()) throw new Error('数值字段不能为空');
                    Object.assign(next[idx], {regOffset: Number(inputs[0].value), dataType: Number(inputs[1].value),
                        scaleFactor: Number(inputs[2].value), decimalPlaces: Number(inputs[3].value),
                        sensorId: inputs[4].value, unit: inputs[5].value, wordOrder: Number(inputs[6].value)});
                    const m = next[idx], quantity = this._masterTasks[this._currentMappingTaskIdx]?.quantity;
                    const length = value => new TextEncoder().encode(value).length;
                    if (!Number.isInteger(quantity) || quantity < 1 || quantity > 125 ||
                        window.decodeModbusMapping(Array(quantity).fill(0), m) === null ||
                        !Number.isInteger(m.decimalPlaces) || m.decimalPlaces < 0 || m.decimalPlaces > 6 ||
                        length(m.sensorId) > 15 || length(m.unit) > 7 || /[\x00-\x1f"\\]/.test(m.sensorId) || /[\x00-\x1f]/.test(m.unit))
                        throw new Error('映射越界：检查寄存器、类型、字序、倍率、0～6小数位及字段字节长度');
                });
                this._currentMappings = next;
                return true;
            } catch (error) {
                Notification.warning(error.message, 'Modbus 映射');
                return false;
            }
        },

        _renderMappingTable() {
            const tbody = document.getElementById('mapping-table-body');
            if (!tbody) return;
            const dtOpts = [
                {v: 0, t: 'uint16'}, {v: 1, t: 'int16'},
                {v: 2, t: 'uint32'}, {v: 3, t: 'int32'}, {v: 4, t: 'float32'}
            ];
            if (this._currentMappings.length === 0) {
                tbody.innerHTML = this._renderProtocolEmptyRow(8, '暂无映射');
                return;
            }
            tbody.innerHTML = this._currentMappings.map((m, idx) => {
                const dtSelect = dtOpts.map(o =>
                    '<option value="' + o.v + '"' + (m.dataType === o.v ? ' selected' : '') + '>' + o.t + '</option>'
                ).join('');
                return '<tr>' +
                    '<td><input type="number" class="protocol-mapping-num-sm" value="' + escapeHtml(m.regOffset ?? 0) + '" min="0" max="124"></td>' +
                    '<td><select class="protocol-mapping-select">' + dtSelect + '</select></td>' +
                    '<td><input type="number" class="protocol-mapping-num-md" value="' + escapeHtml(m.scaleFactor === undefined ? 1 : m.scaleFactor) + '" step="any"></td>' +
                    '<td><input type="number" class="protocol-mapping-num-sm" value="' + escapeHtml(m.decimalPlaces ?? 1) + '" min="0" max="6"></td>' +
                    '<td><input type="text" class="protocol-mapping-text" value="' + escapeHtml(m.sensorId || '') + '" maxlength="15" placeholder="temperature"></td>' +
                    '<td><input type="text" class="protocol-mapping-text protocol-mapping-unit" value="' + escapeHtml(m.unit || '') + '" maxlength="7" placeholder="°C"></td>' +
                    '<td><select><option value="0">高字在前</option><option value="1"' + (m.wordOrder === 1 ? ' selected' : '') + '>低字在前</option></select></td>' +
                    '<td><button type="button" class="fb-btn fb-btn-sm fb-btn-danger protocol-mapping-remove" data-index="' + idx + '">删除</button></td>' +
                '</tr>';
            }).join('');
            this.applyDeveloperModeState(tbody);
        },

        // ============ 设备加载与管理 ============

        _loadModbusDevices() {
            // The server owns this list, including an explicit empty array. Legacy
            // browser storage must never resurrect removed devices on this board
            // (or leak devices saved for another board on the same origin).
            var serverDevices = this._modbusDevices;
            if (!Array.isArray(serverDevices)) {
                var rtu = this._protocolConfig && this._protocolConfig.modbusRtu;
                serverDevices = rtu && rtu.master && rtu.master.devices;
            }
            this._modbusDevices = Array.isArray(serverDevices)
                ? JSON.parse(JSON.stringify(serverDevices)) : [];
            for (var i = 0; i < this._modbusDevices.length; i++) {
                if (!this._modbusDevices[i].deviceType) {
                    this._modbusDevices[i].deviceType = this._modbusDevices[i].type || 'relay';
                }
            }
            this._activeDeviceIdx = -1;
            this._renderAllDevices();
        },

        _refreshModbusDeviceList() {
            if (this._modbusDeletePending) return;
            var btn = document.getElementById('modbus-refresh-devices-btn');
            if (btn && btn.disabled) return;
            if (btn) {
                btn.disabled = true;
                btn.textContent = '加载中...';
            }
            this._protocolConfig = null;
            if (typeof window.apiInvalidateCache === 'function') {
                window.apiInvalidateCache('/api/protocol/config');
                window.apiInvalidateCache('/api/peripherals');
                window.apiInvalidateCache('/api/modbus/status');
            }
            this.loadProtocolConfig('modbus-rtu', { noCache: true })
                .then(function(res) {
                    if (res) {
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
                tbody.innerHTML = this._renderProtocolEmptyRow(8, '暂无子设备，点击"添加设备"创建');
                return;
            }
            var typeLabels = { relay: '继电器', pwm: 'PWM', pid: 'PID', motor: '电机' };
            var protLabels = ['Coil', 'Register'];
            var self = this;
            tbody.innerHTML = this._modbusDevices.map(function(dev, idx) {
                return self._isModbusControlDevice(dev)
                    ? self._renderModbusDeviceRow(dev, idx, typeLabels, protLabels) : '';
            }).join('');
            this.applyDeveloperModeState(tbody);
        },



        addModbusDevice() {
            if (!this.guardDeveloperModeAction()) return;
            if (!this._modbusDevices) this._modbusDevices = [];
            if (this._modbusDevices.length >= 8) {
                Notification.warning('最多支持8个子设备');
                return;
            }
            this._openEditModal(-1);
        },

        _removeDevice(idx) {
            return this._deleteDevice('control', idx);
        },

        // ========== 编辑弹窗 ==========

        _openEditModal(idx) {
            if (!this.guardDeveloperModeAction()) return;
            this._editingDeviceIdx = idx;
            var dev = (idx >= 0 && this._modbusDevices && this._modbusDevices[idx])
                ? this._modbusDevices[idx] : null;
            var modal = AppState.ensureModal('modbus-device-edit-modal');
            if (!modal) return;
            document.getElementById('mdev-edit-name').value = dev ? (dev.name || '') : ('设备' + ((this._modbusDevices ? this._modbusDevices.length : 0) + 1));
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
            document.getElementById('mdev-edit-motor-min-pos').value = dev ? (dev.motorMinPosition || 0) : 0;
            document.getElementById('mdev-edit-motor-max-pos').value = dev ? (dev.motorMaxPosition || 0) : 0;
            document.getElementById('mdev-edit-motor-limits').checked = !!(dev && dev.motorMaxPosition > dev.motorMinPosition);
            this._onMotorLimitToggle();
            var profile = dev && dev.motorProtocol;
            var protocolDefaults = { 'read-fc': 3, 'write-fc': 6, 'read-speed': motorRegs[3], 'read-pulse': motorRegs[4],
                'fwd-value': 1, 'rev-value': 1, 'stop-value': 1 };
            var protocolKeys = { 'read-fc':'readFunction', 'write-fc':'writeFunction', 'read-speed':'speedReadRegister',
                'read-pulse':'pulseReadRegister', 'fwd-value':'forwardValue', 'rev-value':'reverseValue', 'stop-value':'stopValue' };
            Object.keys(protocolDefaults).forEach(function(key) {
                document.getElementById('mdev-edit-motor-' + key).value = profile ? profile[protocolKeys[key]] : protocolDefaults[key];
            });
            ['mdev-edit-motor-advanced', 'mdev-edit-travel-section'].forEach(function(id) {
                var details = document.getElementById(id); if (details) details.open = false;
            });
            this._onEditTypeChange();
            var title = document.getElementById('modbus-edit-modal-title');
            if (title) title.textContent = (idx >= 0)
                ? '编辑子设备'
                : '添加设备';
            AppState.showModal(modal);
        },

        _onMotorLimitToggle() {
            var enabled = document.getElementById('mdev-edit-motor-limits').checked;
            var minimum = document.getElementById('mdev-edit-motor-min-pos');
            var maximum = document.getElementById('mdev-edit-motor-max-pos');
            if (enabled && Number(minimum.value) === Number(maximum.value)) { minimum.value = -400; maximum.value = 400; }
            var fields = document.getElementById('mdev-edit-motor-limit-fields');
            if (fields) AppState.toggleVisible(fields, enabled);
        },

        openMotorTravelSettings() {
            if (!this.guardDeveloperModeAction()) return;
            var index = this._activeDeviceIdx;
            var device = this._modbusDevices && this._modbusDevices[index];
            if (!device || device.deviceType !== 'motor') return;
            if (typeof this._closeCtrlModal === 'function') this._closeCtrlModal();
            this._openEditModal(index);
            var section = document.getElementById('mdev-edit-travel-section');
            if (section) { section.open = true; section.scrollIntoView({ block: 'start' }); }
        },

        _onEditTypeChange() {
            var type = document.getElementById('mdev-edit-type').value;
            ['mdev-edit-ch', 'mdev-edit-base', 'mdev-edit-protocol', 'mdev-edit-nc'].forEach(function(id) {
                var input = document.getElementById(id);
                var row = input && input.closest && input.closest('.fb-grid-2');
                if (row) AppState.toggleVisible(row, type !== 'motor');
            });
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

        async _saveEditModalToDevice() {
            if (this._modbusEditSavePending || !this.guardDeveloperModeAction()) return false;
            var original = JSON.parse(JSON.stringify(this._modbusDevices || []));
            if (this._saveEditModal(true) !== true) return false;
            var modal = document.getElementById('modbus-device-edit-modal');
            var wasInert = modal && modal.inert;
            this._modbusEditSavePending = true;
            if (modal) modal.inert = true;
            try {
                if (await this.saveProtocolConfig('modbus-rtu-form')) {
                    this._closeEditModal();
                    return true;
                }
                this._modbusDevices = original;
                return false;
            } catch (error) {
                this._modbusDevices = original;
                Notification.error(error.message || '设备配置保存失败');
                return false;
            } finally {
                this._modbusEditSavePending = false;
                if (modal) modal.inert = wasInert;
                this._renderAllDevices();
            }
        },

        _saveEditModal(keepOpen) {
            if (!this.guardDeveloperModeAction()) return;
            var motorValues = null;
            if (document.getElementById('mdev-edit-type').value === 'motor') {
                var limits = document.getElementById('mdev-edit-motor-limits').checked;
                var invalidMotor = false;
                var motorInteger = function(field, min, max) {
                    var raw = String(document.getElementById('mdev-edit-motor-' + field).value).trim();
                    var value = Number(raw);
                    if (!raw || !Number.isInteger(value) || value < min || value > max) invalidMotor = true;
                    return value;
                };
                motorValues = {
                    motorRegs: ['fwd','rev','stop','speed','pulse'].map(function(field) { return motorInteger(field, 0, 65535); }),
                    motorDecimals: motorInteger('decimals', 0, 6),
                    motorMinPosition: limits ? motorInteger('min-pos', -2147483648, 0) : 0,
                    motorMaxPosition: limits ? motorInteger('max-pos', 0, 2147483647) : 0
                };
                if (invalidMotor) { Notification.warning('请填写有效的电机寄存器、位置和步数整数'); return; }
                var previousMotor = this._modbusDevices && this._modbusDevices[this._editingDeviceIdx];
                var previousProfile = previousMotor && previousMotor.motorProtocol;
                var wasLimited = previousMotor && previousMotor.motorMaxPosition > previousMotor.motorMinPosition;
                var profile = { version:1, readFunction:motorInteger('read-fc', 3, 4),
                    writeFunction:motorInteger('write-fc', 6, 16),
                    speedReadRegister:motorInteger('read-speed', 0, 65535),
                    pulseReadRegister:motorInteger('read-pulse', 0, 65535),
                    forwardValue:motorInteger('fwd-value', 0, 65535),
                    reverseValue:motorInteger('rev-value', 0, 65535),
                    stopValue:motorInteger('stop-value', 0, 65535),
                    finitePulseMove:!previousProfile || previousProfile.finitePulseMove !== false };
                if (invalidMotor || [6,16].indexOf(profile.writeFunction) < 0) {
                    Notification.warning('请核对协议功能码、读回地址和动作值'); return;
                }
                // Preserve an implicit legacy mapping on ordinary edits. New
                // motors and advanced changes use the same explicit settings.
                var keepLegacy = previousMotor && previousMotor.deviceType === 'motor' && !previousProfile &&
                    (!limits || wasLimited) && profile.readFunction === 3 && profile.writeFunction === 6 &&
                    profile.speedReadRegister === motorValues.motorRegs[3] && profile.pulseReadRegister === motorValues.motorRegs[4] &&
                    profile.forwardValue === 1 && profile.reverseValue === 1 && profile.stopValue === 1;
                if (!keepLegacy) {
                    if (limits) {
                        if (previousMotor && previousMotor.motorMinPosition === motorValues.motorMinPosition &&
                            previousMotor.motorMaxPosition === motorValues.motorMaxPosition) {
                            if (previousProfile && previousProfile.positionTracking) profile.positionTracking = previousProfile.positionTracking;
                            if (previousProfile && Number.isInteger(previousProfile.startupPosition)) profile.startupPosition = previousProfile.startupPosition;
                        } else { profile.positionTracking = 'commanded'; profile.startupPosition = 0; }
                    }
                    var actionValues = [profile.forwardValue, profile.reverseValue, profile.stopValue];
                    var regs = motorValues.motorRegs;
                    for (var a = 0; a < 3; a++) {
                        for (var b = a + 1; b < 3; b++) {
                            if (regs[a] === regs[b] && actionValues[a] === actionValues[b]) {
                                Notification.warning('共用控制寄存器时，正转、反转和停止值必须不同'); return;
                            }
                        }
                        if (regs[a] === regs[3] || regs[a] === regs[4]) {
                            Notification.warning('参数寄存器不能与动作寄存器重复'); return;
                        }
                    }
                    if (regs[3] === regs[4]) { Notification.warning('速度和脉冲数的写入地址不能相同'); return; }
                    motorValues.motorProtocol = profile;
                }
                if (limits && motorValues.motorMaxPosition <= motorValues.motorMinPosition) {
                    Notification.warning('行程范围必须包含零点，且最大位置大于最小位置'); return;
                }
            }
            var idx = this._editingDeviceIdx;
            var isNew = (idx < 0);
            if (isNew) {
                if (!this._modbusDevices || !Array.isArray(this._modbusDevices)) {
                    this._modbusDevices = [];
                }
                if (this._modbusDevices.length >= 8) {
                    Notification.warning('最多支持8个子设备');
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
            if (motorValues) {
                Object.assign(dev, motorValues);
                if (!motorValues.motorProtocol) delete dev.motorProtocol;
                dev.channelCount = 1; dev.controlProtocol = 1;
                if (!dev.sensorId) dev.sensorId = 'motor_' + dev.slaveAddress;
                ['motorTravelDraft', 'motorCurrentPosition', 'motorMoveStep', 'motorLastPulse',
                    'coilBase', 'ncMode', 'batchRegister', 'batchRegType', 'pwmRegBase', 'pwmResolution',
                    'pidAddrs', 'pidDecimals'].forEach(function(key) { delete dev[key]; });
            }
            this._renderAllDevices();
            if (keepOpen !== true) this._closeEditModal();
            return true;
        },

        // ========== UART 外设 ==========

        async _loadUartPeripherals(selectedId, options) {
            const select = document.getElementById('rtu-peripheral-id');
            if (!select) return;
            try {
                const getter = (options && options.noCache === true && typeof apiGetFresh === 'function') ? apiGetFresh : apiGet;
                const res = await getter('/api/peripherals', {
                    pageSize: 100,
                    compact: 1,
                    enabledOnly: 1,
                    category: 'communication'
                });
                if (!res || !res.success) return;
                this._uartPeripherals = (res.data || []).filter(p => p.type === 1 && p.enabled);
                select.innerHTML = '<option value="" disabled>-- 请选择UART外设 --</option>';
                if (this._uartPeripherals.length === 0) {
                    select.innerHTML += '<option value="" disabled>未找到已启用的UART外设，请先在外设管理中配置</option>';
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
                    this.onRtuPeripheralChange(selectedId, options);
                }
            } catch (e) { console.error('Failed to load UART peripherals:', e); }
        },

        onRtuPeripheralChange(peripheralId, options) {
            const infoDiv = document.getElementById('rtu-peripheral-info');
            if (!infoDiv || !peripheralId) { if (infoDiv) AppState.hideElement(infoDiv); return; }
            const periph = (this._uartPeripherals || []).find(p => p.id === peripheralId);
            if (!periph) { AppState.hideElement(infoDiv); return; }
            const getter = (options && options.noCache === true && typeof apiGetFresh === 'function') ? apiGetFresh : apiGet;
            getter('/api/peripherals/', { id: peripheralId }).then(res => {
                if (!res || !res.success) return;
                const data = res.data;
                const baudRate = data.params?.baudRate || '未知';
                const pins = data.pins || [];
                infoDiv.textContent = 'RX: GPIO' + pins[0] + ', TX: GPIO' + pins[1] + ', 波特率: ' + baudRate;
                AppState.showElement(infoDiv);
            }).catch(() => {
                const pins = periph.pins || [];
                infoDiv.textContent = 'RX: GPIO' + pins[0] + ', TX: GPIO' + pins[1];
                AppState.showElement(infoDiv);
            });
        }
    });
})();
