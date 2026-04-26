/**
 * protocol/modbus-relay-motor.js — 线圈控制、电机控制、离散输入、调试日志
 */
(function() {
    Object.assign(AppState, {

        // ========== 线圈控制 ==========

        _getCoilParams() {
            var dev = (this._activeDeviceIdx >= 0 && this._modbusDevices[this._activeDeviceIdx])
                ? this._modbusDevices[this._activeDeviceIdx] : {};
            return {
                slaveAddress: dev.slaveAddress || 1,
                channelCount: dev.channelCount || 8,
                coilBase: dev.coilBase || 0,
                ncMode: !!dev.ncMode,
                relayMode: (dev.controlProtocol === 1) ? 'register' : 'coil'
            };
        },

        async refreshCoilStatus() {
            const p = this._getCoilParams();
            try {
                const res = await apiGetSilent('/api/modbus/coil/status', {
                    slaveAddress: p.slaveAddress, channelCount: p.channelCount,
                    coilBase: p.coilBase, mode: p.relayMode
                });
                if (res && res.success && res.data && res.data.states) {
                    this._coilAutoRefreshErrors = 0;
                    this._coilStates = res.data.states;
                    var cacheKey = 'dev_' + this._activeDeviceIdx;
                    this._deviceCoilCache[cacheKey] = this._coilStates.slice();
                    this._renderCoilGrid();
                    this._appendDebugLog(res.debug, 'ReadCoils FC01');
                } else if (res && !res.success) {
                    this._coilAutoRefreshErrors++;
                    this._stopAutoRefreshOnErrors();
                    Notification.error(res.error || i18n.t('modbus-ctrl-fail'));
                    this._appendDebugError(res.error || 'ReadCoils failed', res.debug);
                } else {
                    if (this._coilStates.length === 0) {
                        for (var i = 0; i < p.channelCount; i++) this._coilStates.push(false);
                    }
                    this._renderCoilGrid();
                }
            } catch (e) {
                this._coilAutoRefreshErrors++;
                this._stopAutoRefreshOnErrors();
                if (this._coilStates.length === 0) {
                    for (var i = 0; i < p.channelCount; i++) this._coilStates.push(false);
                }
                this._renderCoilGrid();
            }
        },

        _renderCoilGrid() {
            const grid = document.getElementById('coil-status-grid');
            if (!grid) return;
            const p = this._getCoilParams();
            const onText = i18n.t('modbus-ctrl-status-on') || 'ON';
            const offText = i18n.t('modbus-ctrl-status-off') || 'OFF';
            let html = '';
            for (let i = 0; i < p.channelCount; i++) {
                const coilState = i < this._coilStates.length ? this._coilStates[i] : false;
                const isOn = p.ncMode ? !coilState : coilState;
                const cls = isOn ? 'coil-on' : 'coil-off';
                html += '<div class="coil-card ' + cls + '" data-ch="' + i + '">'
                      + '<div class="coil-ch">CH' + i + '</div>'
                      + '<div class="coil-st">' + (isOn ? onText : offText) + '</div>'
                      + '</div>';
            }
            grid.innerHTML = html;
        },

        async toggleCoil(ch) {
            const card = document.querySelector('.coil-card[data-ch="' + ch + '"]');
            if (card) card.classList.add('coil-loading');
            const p = this._getCoilParams();
            try {
                const res = await apiPost('/api/modbus/coil/control', {
                    slaveAddress: p.slaveAddress, channel: ch, coilBase: p.coilBase,
                    action: 'toggle', mode: p.relayMode
                });
                if (res && res.success && res.data) {
                    if (ch < this._coilStates.length) this._coilStates[ch] = res.data.state;
                    this._renderCoilGrid();
                    Notification.success(i18n.t('modbus-ctrl-success'));
                    this._appendDebugLog(res.debug, 'Toggle CH' + ch);
                } else {
                    Notification.error((res && res.error) || i18n.t('modbus-ctrl-fail'));
                    this._appendDebugError('Toggle CH' + ch + ' failed', res && res.debug);
                }
            } catch (e) {
                Notification.error(i18n.t('modbus-ctrl-fail'));
            }
            if (card) card.classList.remove('coil-loading');
        },

        async batchCoil(action) {
            const p = this._getCoilParams();
            let modbusAction = action;
            if (p.ncMode) {
                if (action === 'allOn') modbusAction = 'allOff';
                else if (action === 'allOff') modbusAction = 'allOn';
            }
            try {
                const res = await apiPost('/api/modbus/coil/batch', {
                    slaveAddress: p.slaveAddress, channelCount: p.channelCount,
                    coilBase: p.coilBase, action: modbusAction, mode: p.relayMode
                });
                if (res && res.success && res.data && res.data.states) {
                    this._coilStates = res.data.states;
                    this._renderCoilGrid();
                    Notification.success(i18n.t('modbus-ctrl-success'));
                    this._appendDebugLog(res.debug, 'Batch: ' + action);
                } else {
                    Notification.error((res && res.error) || i18n.t('modbus-ctrl-fail'));
                    this._appendDebugError('Batch ' + action + ' failed', res && res.debug);
                }
            } catch (e) {
                Notification.error(i18n.t('modbus-ctrl-fail'));
            }
        },

        async startCoilDelay() {
            const p = this._getCoilParams();
            const ch = parseInt(document.getElementById('modbus-ctrl-delay-ch')?.value || '0');
            const units = parseInt(document.getElementById('modbus-ctrl-delay-units')?.value || '50');
            if (units < 1 || units > 255) {
                Notification.warning(i18n.t('modbus-ctrl-fail') + ': 1-255 (x100ms)');
                return;
            }
            try {
                const params = {
                    slaveAddress: p.slaveAddress, channel: ch, delayBase: 0x0200,
                    delayUnits: units, ncMode: p.ncMode, coilBase: p.coilBase
                };
                const res = await apiPost('/api/modbus/coil/delay', params);
                if (res && res.success) {
                    Notification.success(i18n.t('modbus-ctrl-delay-ok'));
                    this._appendDebugLog(res.debug, 'Delay CH' + ch + ' ' + units + 'x100ms' + (p.ncMode ? ' (NC)' : ''));
                    const refreshDelay = p.ncMode ? (units * 100 + 500) : 500;
                    setTimeout(() => this.refreshCoilStatus(), refreshDelay);
                } else {
                    Notification.error((res && res.error) || i18n.t('modbus-ctrl-fail'));
                    this._appendDebugError('Delay CH' + ch + ' failed', res && res.debug);
                }
            } catch (e) {
                Notification.error(i18n.t('modbus-ctrl-fail'));
            }
        },

        onCoilChannelCountChange() {
            this._updateDelayChannelSelect();
            this._renderCoilGrid();
        },

        _updateDelayChannelSelect() {
            const sel = document.getElementById('modbus-ctrl-delay-ch');
            if (!sel) return;
            var dev = (this._activeDeviceIdx >= 0 && this._modbusDevices)
                ? this._modbusDevices[this._activeDeviceIdx] : null;
            const count = dev ? (dev.channelCount || 8) : 8;
            let html = '';
            for (let i = 0; i < count; i++) {
                html += '<option value="' + i + '">CH' + i + '</option>';
            }
            sel.innerHTML = html;
        },

        toggleCoilAutoRefresh() {
            const checked = document.getElementById('modbus-ctrl-auto-refresh')?.checked;
            if (checked) {
                this._coilAutoRefreshErrors = 0;
                this.refreshCoilStatus();
                this._coilAutoRefreshTimer = setInterval(() => this.refreshCoilStatus(), 8000);
            } else {
                if (this._coilAutoRefreshTimer) {
                    clearInterval(this._coilAutoRefreshTimer);
                    this._coilAutoRefreshTimer = null;
                }
            }
        },

        _stopAutoRefreshOnErrors() {
            if (this._coilAutoRefreshErrors >= 3 && this._coilAutoRefreshTimer) {
                clearInterval(this._coilAutoRefreshTimer);
                this._coilAutoRefreshTimer = null;
                var el = document.getElementById('modbus-ctrl-auto-refresh');
                if (el) el.checked = false;
                console.warn('[protocol] Coil auto-refresh stopped after', this._coilAutoRefreshErrors, 'consecutive errors');
            }
        },

        _stopPidAutoRefreshOnErrors() {
            if (this._pidAutoRefreshErrors >= 3 && this._pidAutoRefreshTimer) {
                clearInterval(this._pidAutoRefreshTimer);
                this._pidAutoRefreshTimer = null;
                var el = document.getElementById('modbus-ctrl-pid-auto-refresh');
                if (el) el.checked = false;
                console.warn('[protocol] PID auto-refresh stopped after', this._pidAutoRefreshErrors, 'consecutive errors');
            }
        },

        async readDiscreteInputs() {
            const slaveAddr = parseInt(document.getElementById('modbus-debug-slave-addr')?.value || '0');
            const inputCount = parseInt(document.getElementById('modbus-ctrl-input-count')?.value || '4');
            const inputBase = parseInt(document.getElementById('modbus-ctrl-input-base')?.value || '0');
            try {
                const res = await apiGet('/api/modbus/device/inputs', { slaveAddress: slaveAddr, inputCount: inputCount, inputBase: inputBase });
                const display = document.getElementById('modbus-ctrl-inputs-display');
                if (res && res.success && res.data && res.data.states) {
                    const states = res.data.states;
                    if (display) {
                        display.innerHTML = '';
                        var chipList = document.createElement('div');
                        chipList.className = 'protocol-chip-list';
                        for (var si = 0; si < states.length; si++) {
                            var chip = document.createElement('span');
                            chip.className = 'protocol-chip' + (states[si] ? ' is-on' : '');
                            chip.textContent = 'IN' + si + ':' + (states[si] ? 'ON' : 'OFF');
                            chipList.appendChild(chip);
                        }
                        display.appendChild(chipList);
                    }
                    Notification.success(i18n.t('modbus-ctrl-success'));
                    this._appendDebugLog(res.debug, 'ReadInputs FC02');
                } else {
                    if (display) display.innerHTML = '';
                    Notification.error((res && res.error) || i18n.t('modbus-ctrl-fail'));
                    this._appendDebugError('ReadInputs failed', res && res.debug);
                }
            } catch (e) { Notification.error(i18n.t('modbus-ctrl-fail')); }
        },

        // ========== 电机控制 ==========

        _getMotorDevParams() {
            var idx = this._activeDeviceIdx;
            if (idx < 0 || !this._modbusDevices || !this._modbusDevices[idx]) return null;
            var dev = this._modbusDevices[idx];
            return {
                slaveAddress: dev.slaveAddress || 1,
                motorRegs: dev.motorRegs || [0,1,2,5,7],
                motorDecimals: dev.motorDecimals || 0
            };
        },

        async motorForward() {
            var p = this._getMotorDevParams();
            if (!p) return;
            try {
                var res = await apiPostSilent('/api/modbus/motor/control', { slaveAddress: p.slaveAddress, action: 'forward' });
                if (res && res.success) {
                    Notification.success(i18n.t('modbus-motor-ctrl-forward-ok') || '正转指令已发送');
                    this._appendDebugLog(res.debug, 'Motor Forward');
                    document.getElementById('motor-cur-direction').textContent = '← ' + (i18n.t('modbus-motor-ctrl-forward') || '正转');
                    var elRun = document.getElementById('modbus-ctrl-motor-run-status');
                    if (elRun) { elRun.className = 'motor-run-badge motor-run-forward'; elRun.textContent = i18n.t('modbus-motor-dir-forward') || '正转中'; }
                } else {
                    Notification.error((res && res.error) || i18n.t('modbus-ctrl-fail'));
                    this._appendDebugError('Motor Forward failed', res && res.debug);
                }
            } catch (e) { Notification.error(i18n.t('modbus-ctrl-fail')); }
        },

        async motorReverse() {
            var p = this._getMotorDevParams();
            if (!p) return;
            try {
                var res = await apiPostSilent('/api/modbus/motor/control', { slaveAddress: p.slaveAddress, action: 'reverse' });
                if (res && res.success) {
                    Notification.success(i18n.t('modbus-motor-ctrl-reverse-ok') || '反转指令已发送');
                    this._appendDebugLog(res.debug, 'Motor Reverse');
                    document.getElementById('motor-cur-direction').textContent = '→ ' + (i18n.t('modbus-motor-ctrl-reverse') || '反转');
                    var elRun = document.getElementById('modbus-ctrl-motor-run-status');
                    if (elRun) { elRun.className = 'motor-run-badge motor-run-reverse'; elRun.textContent = i18n.t('modbus-motor-dir-reverse') || '反转中'; }
                } else {
                    Notification.error((res && res.error) || i18n.t('modbus-ctrl-fail'));
                    this._appendDebugError('Motor Reverse failed', res && res.debug);
                }
            } catch (e) { Notification.error(i18n.t('modbus-ctrl-fail')); }
        },

        async motorStop() {
            var p = this._getMotorDevParams();
            if (!p) return;
            try {
                var res = await apiPostSilent('/api/modbus/motor/control', { slaveAddress: p.slaveAddress, action: 'stop' });
                if (res && res.success) {
                    Notification.success(i18n.t('modbus-motor-ctrl-stop-ok') || '停止指令已发送');
                    this._appendDebugLog(res.debug, 'Motor Stop');
                    document.getElementById('motor-cur-direction').textContent = i18n.t('modbus-motor-status-stop') || '停止';
                    var elRun = document.getElementById('modbus-ctrl-motor-run-status');
                    if (elRun) { elRun.className = 'motor-run-badge motor-run-stopped'; elRun.textContent = i18n.t('modbus-motor-status-stop') || '停止'; }
                } else {
                    Notification.error((res && res.error) || i18n.t('modbus-ctrl-fail'));
                    this._appendDebugError('Motor Stop failed', res && res.debug);
                }
            } catch (e) { Notification.error(i18n.t('modbus-ctrl-fail')); }
        },

        async setMotorSpeed() {
            var p = this._getMotorDevParams();
            if (!p) return;
            var speed = parseInt(document.getElementById('modbus-ctrl-motor-speed').value) || 50;
            try {
                var res = await apiPostSilent('/api/modbus/motor/control', { slaveAddress: p.slaveAddress, action: 'setSpeed', value: speed });
                if (res && res.success) {
                    Notification.success(i18n.t('modbus-motor-ctrl-speed-ok') || '速度设置成功');
                    this._appendDebugLog(res.debug, 'Motor SetSpeed');
                } else {
                    Notification.error((res && res.error) || i18n.t('modbus-ctrl-fail'));
                    this._appendDebugError('Motor SetSpeed failed', res && res.debug);
                }
            } catch (e) { Notification.error(i18n.t('modbus-ctrl-fail')); }
        },

        async setMotorPulse() {
            var p = this._getMotorDevParams();
            if (!p) return;
            var pulse = parseInt(document.getElementById('modbus-ctrl-motor-pulse').value) || 1600;
            try {
                var res = await apiPostSilent('/api/modbus/motor/control', { slaveAddress: p.slaveAddress, action: 'setPulse', value: pulse });
                if (res && res.success) {
                    Notification.success(i18n.t('modbus-motor-ctrl-pulse-ok') || '脉冲数设置成功');
                    this._appendDebugLog(res.debug, 'Motor SetPulse');
                } else {
                    Notification.error((res && res.error) || i18n.t('modbus-ctrl-fail'));
                    this._appendDebugError('Motor SetPulse failed', res && res.debug);
                }
            } catch (e) { Notification.error(i18n.t('modbus-ctrl-fail')); }
        },

        async refreshMotorStatus() {
            var p = this._getMotorDevParams();
            if (!p) return;
            try {
                var res = await apiPostSilent('/api/modbus/motor/control', { slaveAddress: p.slaveAddress, action: 'readStatus' });
                if (res && res.success && res.data) {
                    var data = res.data;
                    var sf = Math.pow(10, p.motorDecimals || 0);
                    var elSpeed = document.getElementById('motor-cur-speed');
                    var elPulse = document.getElementById('motor-cur-pulse');
                    var elDir = document.getElementById('motor-cur-direction');
                    var elCount = document.getElementById('motor-cur-count');
                    var elRun = document.getElementById('modbus-ctrl-motor-run-status');
                    if (data.speed !== undefined && elSpeed) elSpeed.textContent = (data.speed / sf).toFixed(p.motorDecimals || 0);
                    if (data.pulse !== undefined && elPulse) elPulse.textContent = data.pulse;
                    if (elDir) {
                        if (data.direction === 'forward' || data.direction === 1) {
                            elDir.textContent = '← ' + (i18n.t('modbus-motor-ctrl-forward') || '正转');
                        } else if (data.direction === 'reverse' || data.direction === -1) {
                            elDir.textContent = '→ ' + (i18n.t('modbus-motor-ctrl-reverse') || '反转');
                        } else {
                            elDir.textContent = i18n.t('modbus-motor-status-stop') || '停止';
                        }
                    }
                    if (data.count !== undefined && elCount) elCount.textContent = data.count + ' ' + (i18n.t('modbus-motor-count') || '次');
                    if (elRun) {
                        var dir = data.direction || '';
                        elRun.className = 'motor-run-badge ' + (dir === 'forward' || dir === 1 ? 'motor-run-forward' : dir === 'reverse' || dir === -1 ? 'motor-run-reverse' : 'motor-run-stopped');
                        elRun.textContent = (dir === 'forward' || dir === 1) ? (i18n.t('modbus-motor-dir-forward') || '正转中') : (dir === 'reverse' || dir === -1) ? (i18n.t('modbus-motor-dir-reverse') || '反转中') : (i18n.t('modbus-motor-status-stop') || '停止');
                    }
                    this._appendDebugLog(res.debug, 'Motor ReadStatus');
                } else {
                    this._appendDebugError('Motor ReadStatus failed', res && res.debug);
                }
            } catch (e) {
                console.error('refreshMotorStatus failed:', e);
            }
        },

        // ========== 调试日志 ==========

        _appendDebugLog(debug, label) {
            var log = document.getElementById('modbus-debug-log');
            if (!log) return;
            if (!debug && !label) return;
            var time = new Date().toLocaleTimeString();
            var entry = document.createElement('div');
            entry.className = 'modbus-debug-entry';
            var html = '<div class="modbus-debug-time">[' + escapeHtml(time) + '] ' + escapeHtml(label || '') + '</div>';
            if (debug && debug.tx) html += '<div class="modbus-debug-tx">TX: ' + escapeHtml(debug.tx) + '</div>';
            if (debug && debug.rx) html += '<div class="modbus-debug-rx">RX: ' + escapeHtml(debug.rx) + '</div>';
            entry.innerHTML = html;
            log.appendChild(entry);
            log.scrollTop = log.scrollHeight;
            while (log.children.length > 100) log.removeChild(log.firstChild);
        },

        _appendDebugError(msg, debug) {
            var log = document.getElementById('modbus-debug-log');
            if (!log) return;
            var time = new Date().toLocaleTimeString();
            var entry = document.createElement('div');
            entry.className = 'modbus-debug-entry';
            var html = '<div class="modbus-debug-time">[' + escapeHtml(time) + ']</div>';
            html += '<div class="modbus-debug-err">' + escapeHtml(msg) + '</div>';
            if (debug && debug.tx) html += '<div class="modbus-debug-tx">TX: ' + escapeHtml(debug.tx) + '</div>';
            if (debug && debug.rx) html += '<div class="modbus-debug-rx">RX: ' + escapeHtml(debug.rx) + '</div>';
            entry.innerHTML = html;
            log.appendChild(entry);
            log.scrollTop = log.scrollHeight;
            while (log.children.length > 100) log.removeChild(log.firstChild);
        },

        clearModbusDebugLog() {
            var log = document.getElementById('modbus-debug-log');
            if (log) log.innerHTML = '';
        }
    });
})();
