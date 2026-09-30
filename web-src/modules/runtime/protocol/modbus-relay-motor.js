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
            const deviceIdx = this._activeDeviceIdx;
            const clearUnknown = () => {
                this._coilStates = new Array(p.channelCount).fill(null);
                delete this._deviceCoilCache['dev_' + deviceIdx];
                this._renderCoilGrid();
            };
            try {
                const res = await apiGetSilent('/api/modbus/coil/status', {
                    slaveAddress: p.slaveAddress, channelCount: p.channelCount,
                    coilBase: p.coilBase, mode: p.relayMode
                });
                if (this._activeDeviceIdx !== deviceIdx) return;
                if (res && res.success === false && res.errorCode === 'BUSY') {
                    clearUnknown();
                    this._setCtrlModalOffline(true, '总线忙碌，等待刷新');
                    return;
                }
                if (res && res.success && res.data && Array.isArray(res.data.states) &&
                    res.data.states.length === p.channelCount &&
                    res.data.states.every(value => typeof value === 'boolean')) {
                    this._markCtrlModalOnline();
                    this._coilAutoRefreshErrors = 0;
                    this._coilStates = res.data.states;
                    this._deviceCoilCache['dev_' + deviceIdx] = this._coilStates.slice();
                    this._renderCoilGrid();
                    this._appendDebugLog(res.debug, p.relayMode === 'register' ? 'ReadRegisters FC03' : 'ReadCoils FC01');
                } else {
                    this._coilAutoRefreshErrors++;
                    this._stopAutoRefreshOnErrors();
                    clearUnknown();
                    this._setCtrlModalOffline(true, (res && res.error) || '状态读取失败');
                    this._appendDebugError((res && res.error) || 'Read status failed', res && res.debug);
                }
            } catch (e) {
                if (this._activeDeviceIdx !== deviceIdx) return;
                if (e && (e.status === 503 || e.status === 429)) {
                    clearUnknown();
                    this._setCtrlModalOffline(true, '服务忙碌，等待刷新');
                    return;
                }
                this._coilAutoRefreshErrors++;
                this._stopAutoRefreshOnErrors();
                clearUnknown();
                this._setCtrlModalOffline(true, '状态读取失败');
            }
        },

        _renderCoilGrid() {
            const grid = document.getElementById('coil-status-grid');
            if (!grid) return;
            const p = this._getCoilParams();
            const onText = '开';
            const offText = '关';
            let html = '';
            for (let i = 0; i < p.channelCount; i++) {
                const coilState = i < this._coilStates.length ? this._coilStates[i] : null;
                const known = typeof coilState === 'boolean';
                const isOn = known ? (p.ncMode ? !coilState : coilState) : null;
                const cls = !known ? 'coil-unknown' : (isOn ? 'coil-on' : 'coil-off');
                html += '<div class="coil-card ' + cls + '" data-ch="' + i + '">'
                      + '<div class="coil-ch">CH' + i + '</div>'
                      + '<div class="coil-st">' + (!known ? '--' : (isOn ? onText : offText)) + '</div>'
                      + '</div>';
            }
            grid.innerHTML = html;
        },

        async toggleCoil(ch) {
            if (this._isCtrlModalOffline()) return;
            const card = document.querySelector('.coil-card[data-ch="' + ch + '"]');
            if (card) card.classList.add('coil-loading');
            const p = this._getCoilParams();
            try {
                const res = await apiPostPriority('/api/modbus/coil/control', {
                    slaveAddress: p.slaveAddress, channel: ch, coilBase: p.coilBase,
                    action: 'toggle', mode: p.relayMode
                });
                if (res && res.success && res.data) {
                    if (ch < this._coilStates.length) this._coilStates[ch] = res.data.state;
                    this._renderCoilGrid();
                    Notification.success('操作成功');
                    this._appendDebugLog(res.debug, 'Toggle CH' + ch);
                } else {
                    Notification.error((res && res.error) || '操作失败');
                    this._appendDebugError('Toggle CH' + ch + ' failed', res && res.debug);
                }
            } catch (e) {
                Notification.error('操作失败');
            }
            if (card) card.classList.remove('coil-loading');
        },

        async batchCoil(action) {
            if (this._isCtrlModalOffline()) return;
            const p = this._getCoilParams();
            let modbusAction = action;
            if (p.ncMode) {
                if (action === 'allOn') modbusAction = 'allOff';
                else if (action === 'allOff') modbusAction = 'allOn';
            }
            try {
                const res = await apiPostPriority('/api/modbus/coil/batch', {
                    slaveAddress: p.slaveAddress, channelCount: p.channelCount,
                    coilBase: p.coilBase, action: modbusAction, mode: p.relayMode
                });
                if (res && res.success && res.data && res.data.states) {
                    this._coilStates = res.data.states;
                    this._renderCoilGrid();
                    Notification.success('操作成功');
                    this._appendDebugLog(res.debug, 'Batch: ' + action);
                } else {
                    Notification.error((res && res.error) || '操作失败');
                    this._appendDebugError('Batch ' + action + ' failed', res && res.debug);
                }
            } catch (e) {
                Notification.error('操作失败');
            }
        },

        async startCoilDelay() {
            if (this._isCtrlModalOffline()) return;
            const p = this._getCoilParams();
            const ch = parseInt(document.getElementById('modbus-ctrl-delay-ch')?.value || '0');
            const units = parseInt(document.getElementById('modbus-ctrl-delay-units')?.value || '50');
            if (units < 1 || units > 255) {
                Notification.warning('操作失败: 1-255 (x100ms)');
                return;
            }
            try {
                const params = {
                    slaveAddress: p.slaveAddress, channel: ch, delayBase: 0x0200,
                    delayUnits: units, ncMode: p.ncMode, coilBase: p.coilBase
                };
                const res = await apiPostPriority('/api/modbus/coil/delay', params);
                if (res && res.success) {
                    Notification.success('延时断开指令已发送');
                    this._appendDebugLog(res.debug, 'Delay CH' + ch + ' ' + units + 'x100ms' + (p.ncMode ? ' (NC)' : ''));
                    const refreshDelay = p.ncMode ? (units * 100 + 500) : 500;
                    setTimeout(() => this.refreshCoilStatus(), refreshDelay);
                } else {
                    Notification.error((res && res.error) || '操作失败');
                    this._appendDebugError('Delay CH' + ch + ' failed', res && res.debug);
                }
            } catch (e) {
                Notification.error('操作失败');
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
                this._scheduleCoilAutoRefresh();
            } else {
                this._stopCoilAutoRefresh();
            }
        },

        _getModbusControlRefreshInterval() {
            if (typeof apiGetPressureAwareInterval === 'function') {
                return apiGetPressureAwareInterval('modbusControl', 8000);
            }
            return 8000;
        },

        _scheduleCoilAutoRefresh() {
            var self = this;
            this._stopCoilAutoRefresh();
            this._coilAutoRefreshTimer = setTimeout(function() {
                self._coilAutoRefreshTimer = null;
                var autoRefreshEl = document.getElementById('modbus-ctrl-auto-refresh');
                if (!autoRefreshEl || !autoRefreshEl.checked) return;
                Promise.resolve(self.refreshCoilStatus()).finally(function() {
                    var nextAutoRefreshEl = document.getElementById('modbus-ctrl-auto-refresh');
                    if (nextAutoRefreshEl && nextAutoRefreshEl.checked) {
                        self._scheduleCoilAutoRefresh();
                    }
                });
            }, this._getModbusControlRefreshInterval());
        },

        _stopCoilAutoRefresh() {
            if (this._coilAutoRefreshTimer) {
                clearTimeout(this._coilAutoRefreshTimer);
                this._coilAutoRefreshTimer = null;
            }
        },

        _stopAutoRefreshOnErrors() {
            if (this._coilAutoRefreshErrors >= 3) {
                this._stopCoilAutoRefresh();
                var el = document.getElementById('modbus-ctrl-auto-refresh');
                if (el) el.checked = false;
                console.warn('[protocol] Coil auto-refresh stopped after', this._coilAutoRefreshErrors, 'consecutive errors');
            }
        },

        _stopPidAutoRefreshOnErrors() {
            if (this._pidAutoRefreshErrors >= 3) {
                if (typeof this._stopPidAutoRefresh === 'function') {
                    this._stopPidAutoRefresh();
                }
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
                    Notification.success('操作成功');
                    this._appendDebugLog(res.debug, 'ReadInputs FC02');
                } else {
                    if (display) display.innerHTML = '';
                    Notification.error((res && res.error) || '操作失败');
                    this._appendDebugError('ReadInputs failed', res && res.debug);
                }
            } catch (e) { Notification.error('操作失败'); }
        },

        // ========== 电机控制 ==========

        _protocolMotorViewCurrent(index, generation) {
            var modal = document.getElementById('modbus-device-ctrl-modal');
            return index === this._activeDeviceIdx && generation === (this._modbusCtrlGeneration || 0) &&
                (!this.currentPage || this.currentPage === 'protocol') && modal && modal.isConnected !== false &&
                (!modal.style || modal.style.display !== 'none');
        },

        _protocolMotorHasDraft() {
            return ['speed', 'pulse'].some(function(param) {
                var input = document.getElementById('modbus-ctrl-motor-' + param);
                return input && input.dataset.dirty === 'true';
            });
        },

        _startProtocolMotorSync() {
            var self = this, generation = this._modbusCtrlGeneration, index = this._activeDeviceIdx;
            async function refresh() {
                var failed = false;
                if (!self._protocolMotorViewCurrent(index, generation)) return;
                var p = self._getMotorDevParams();
                if (!p) return;
                var sequence = self._protocolMotorSequence;
                if (!document.hidden && !(self._protocolMotorPending && self._protocolMotorPending[String(p.slaveAddress)])) {
                    try {
                        var get = typeof apiGetSilentFresh === 'function' ? apiGetSilentFresh : apiGetSilent;
                        var res = await get('/api/modbus/motor/state', { slaveAddress: p.slaveAddress });
                        if (!self._protocolMotorViewCurrent(index, generation)) return;
                        if (!res || !res.success || !res.data) throw new Error('Motor state unavailable');
                        if (sequence === self._protocolMotorSequence) {
                            self._applyProtocolMotorState(res.data);
                            if (res.data.parametersKnown) self._markCtrlModalOnline();
                        }
                    } catch (_) {
                        if (!self._protocolMotorViewCurrent(index, generation)) return;
                        if (sequence === self._protocolMotorSequence) self._applyProtocolMotorState({ parametersKnown: false });
                        failed = true;
                    }
                }
                if (self._protocolMotorViewCurrent(index, generation))
                    self._protocolMotorSyncTimer = setTimeout(refresh, failed ? 10000 : 3000);
            }
            refresh();
        },

        _applyProtocolMotorState(data) {
            var p = this._getMotorDevParams();
            if (!p) return;
            if (data.travelToken !== undefined) {
                var previous = this._protocolMotorTravel;
                var checkbox = document.getElementById('motor-travel-stationary');
                if (checkbox && (!previous || previous.travelToken !== data.travelToken || previous.motorRef !== data.motorRef)) checkbox.checked = false;
                this._protocolMotorTravel = Object.assign({ index: this._activeDeviceIdx, generation: this._modbusCtrlGeneration || 0 }, data);
                var safety = document.getElementById('motor-travel-status');
                if (safety) safety.textContent = !data.softLimitEnabled ? '软限位未启用，行程尚未标定' :
                    !data.referenceKnown ? '软限位已配置，参考位置未确认' :
                    !data.stationaryConfirmed ? '运动结束未确认，停止仍可请求' :
                    '可能位置范围：' + data.possiblePositionMin + ' ~ ' + data.possiblePositionMax + ' 计数；无实际位置反馈';
            }
            var decimals = p.motorDecimals || 0, sf = Math.pow(10, decimals);
            ['speed', 'pulse'].forEach(function(param) {
                var value = data.parametersKnown === false ? '' : data[param] === undefined ? undefined :
                    param === 'speed' ? (data[param] / sf).toFixed(decimals) : String(data[param]);
                var current = document.getElementById('motor-cur-' + param);
                var input = document.getElementById('modbus-ctrl-motor-' + param);
                if (current && value !== undefined) current.textContent = value || '--';
                if (input && value !== undefined && input.dataset.dirty !== 'true' && document.activeElement !== input) input.value = value;
                if (input && param === 'speed') { input.max = String(65535 / sf); input.step = String(1 / sf); }
            });
            if (data.lastAcknowledgedAction !== undefined) this._protocolMotorAcknowledged = data.lastAcknowledgedAction;
            this._motorUnknownRunState(data.pending);
        },

        async _confirmMotorTravel(reference) {
            var data = this._protocolMotorTravel;
            var index = this._activeDeviceIdx, generation = this._modbusCtrlGeneration || 0;
            var checkbox = document.getElementById('motor-travel-stationary');
            var input = document.getElementById('motor-travel-reference');
            if (!data || data.index !== index || data.generation !== generation || !data.travelToken || !data.motorRef || !data.softLimitEnabled || data.pending) {
                Notification.error('请先完成行程配置，并等待最新保护状态'); return;
            }
            if (!checkbox || !checkbox.checked) { Notification.error('需要现场确认滑台静止、负载安全'); return; }
            var value = input ? input.value.trim() : '';
            if (reference && (!/^-?\d+$/.test(value) || !Number.isSafeInteger(Number(value)) || Number(value) < -2147483648 || Number(value) > 2147483647)) {
                Notification.error('请输入实测的整数控制器计数，不是毫米'); return;
            }
            checkbox.checked = false;
            this._protocolMotorTravel = null;
            try {
                var res = await apiPostPriority('/api/modbus/motor/control', {
                    action: reference ? 'confirmReference' : 'confirmStationary', motorRef: data.motorRef,
                    travelToken: data.travelToken, stationaryConfirmed: 'true', position: reference ? value : undefined
                });
                if (!this._protocolMotorViewCurrent(index, generation)) return;
                if (!res || !res.success) throw new Error(res && res.error || '确认失败');
                Notification.success('现场确认已记录；启动前仍需重新读取脉冲');
            } catch (error) {
                if (this._protocolMotorViewCurrent(index, generation)) Notification.error(error.message || '确认失败，请刷新保护状态');
            }
        },

        confirmMotorReference() { return this._confirmMotorTravel(true); },
        confirmMotorStationary() { return this._confirmMotorTravel(false); },

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

        async _protocolMotorRequest(params, action, value) {
            var self = this, index = this._activeDeviceIdx;
            var generation = this._modbusCtrlGeneration || 0;
            var current = function() { return self._protocolMotorViewCurrent(index, generation); };
            var pending = this._protocolMotorPending || (this._protocolMotorPending = {});
            var key = String(params.slaveAddress), isRead = action === 'readStatus';
            if (!isRead && action !== 'stop' && pending[key]) throw new Error('电机指令正在执行，请等待结果');
            if (value === undefined) value = 0;
            if (!Number.isInteger(value) || value < 0 || value > 65535) throw new Error('参数必须是 0 到 65535 的整数');
            this._protocolMotorSequence = (this._protocolMotorSequence || 0) + 1;
            var requestId = Date.now().toString(36) + '-' + Math.random().toString(36).slice(2) + '-' + this._protocolMotorSequence;
            if (!isRead) pending[key] = requestId;
            var started = Date.now(), operationId = 0;
            try {
                // Submit once. Poll only the operation ID; never retry a movement.
                var response = await apiPostSilentPriority('/api/modbus/motor/control', {
                    slaveAddress: params.slaveAddress, action: action, value: value, requestId: requestId
                });
                while (true) {
                    if (!current()) {
                        var closed = new Error('控制窗口已关闭，已提交指令不会重发');
                        closed.motorViewClosed = true; throw closed;
                    }
                    if (!response || response.success !== true) throw new Error(response && response.error || '电机请求失败');
                    var data = response.data || {};
                    if (!Number.isInteger(data.operationId) || data.operationId <= 0 || typeof data.pending !== 'boolean') throw new Error('缺少电机操作编号或状态');
                    if (operationId && data.operationId !== operationId) throw new Error('电机操作编号不匹配');
                    operationId = data.operationId;
                    if (!data.pending) {
                        if (data.success !== true) throw new Error(data.error || '电机未确认指令');
                        return response;
                    }
                    if (Date.now() - started >= 20000) throw new Error('指令结果尚未确认，请读取状态；不要重复启动');
                    await new Promise(function(resolve) { setTimeout(resolve, 400); });
                    if (!current()) {
                        var cancelled = new Error('控制窗口已关闭，已提交指令不会重发');
                        cancelled.motorViewClosed = true; throw cancelled;
                    }
                    try {
                        response = await apiPostSilentPriority('/api/modbus/motor/control', { action: 'operation', operationId: data.operationId });
                    } catch (error) {
                        if (error && (error.status === 503 || error.code === 503 || error.data && error.data.code === 503)) {
                            response = { success: true, data: { operationId: operationId, pending: true } }; continue;
                        }
                        if (error && (error.status === 404 || error.code === 404 || error.data && error.data.code === 404))
                            throw new Error('操作记录已失效，执行结果未知；请核对设备，勿重复启动');
                        throw error;
                    }
                }
            } catch (error) {
                if (!current()) {
                    var stale = new Error('控制窗口已关闭，已提交指令不会重发');
                    stale.motorViewClosed = true; throw stale;
                }
                throw error;
            } finally {
                if (pending[key] === requestId) delete pending[key];
            }
        },

        _motorUnknownRunState(pending) {
            var direction = document.getElementById('motor-cur-direction');
            var run = document.getElementById('modbus-ctrl-motor-run-status');
            var acknowledged = this._protocolMotorAcknowledged;
            if (direction) {
                direction.textContent = acknowledged === 'forward' ? '正转应答' : acknowledged === 'reverse' ? '反转应答' : acknowledged === 'stop' ? '停止应答' : '--';
                direction.title = '最近已应答指令，不代表当前实际运动';
            }
            if (run) { run.className = 'motor-run-badge motor-run-idle'; run.textContent = pending ? '指令处理中' : '运动未确认'; }
        },

        _motorRequestError(error) {
            if (error && error.motorViewClosed) return;
            Notification.error(error && error.message || '电机操作失败');
        },

        async motorForward() {
            if (this._isCtrlModalOffline()) return;
            if (this._protocolMotorHasDraft()) { Notification.warning('请先设置修改后的速度和脉冲'); return; }
            var p = this._getMotorDevParams();
            if (!p) return;
            try {
                var res = await this._protocolMotorRequest(p, 'forward');
                if (res && res.success) {
                    Notification.success('正转指令已发送');
                    this._appendDebugLog(res.debug, 'Motor Forward');
                    this._protocolMotorAcknowledged = 'forward';
                    this._motorUnknownRunState();
                } else {
                    Notification.error((res && res.error) || '操作失败');
                    this._appendDebugError('Motor Forward failed', res && res.debug);
                }
            } catch (e) { this._motorRequestError(e); }
        },

        async motorReverse() {
            if (this._isCtrlModalOffline()) return;
            if (this._protocolMotorHasDraft()) { Notification.warning('请先设置修改后的速度和脉冲'); return; }
            var p = this._getMotorDevParams();
            if (!p) return;
            try {
                var res = await this._protocolMotorRequest(p, 'reverse');
                if (res && res.success) {
                    Notification.success('反转指令已发送');
                    this._appendDebugLog(res.debug, 'Motor Reverse');
                    this._protocolMotorAcknowledged = 'reverse';
                    this._motorUnknownRunState();
                } else {
                    Notification.error((res && res.error) || '操作失败');
                    this._appendDebugError('Motor Reverse failed', res && res.debug);
                }
            } catch (e) { this._motorRequestError(e); }
        },

        async motorStop() {
            var p = this._getMotorDevParams();
            if (!p) return;
            try {
                var res = await this._protocolMotorRequest(p, 'stop');
                if (res && res.success) {
                    Notification.success('停止指令已发送');
                    this._appendDebugLog(res.debug, 'Motor Stop');
                    this._protocolMotorAcknowledged = 'stop';
                    this._motorUnknownRunState();
                } else {
                    Notification.error((res && res.error) || '操作失败');
                    this._appendDebugError('Motor Stop failed', res && res.debug);
                }
            } catch (e) { this._motorRequestError(e); }
        },

        async setMotorSpeed() {
            if (this._isCtrlModalOffline()) return;
            var p = this._getMotorDevParams();
            if (!p) return;
            var input = document.getElementById('modbus-ctrl-motor-speed').value.trim();
            var scaled = input ? Number(input) * Math.pow(10, p.motorDecimals) : NaN;
            var speed = Math.round(scaled);
            try {
                if (!Number.isFinite(scaled) || Math.abs(scaled - speed) > 1e-7) throw new Error('速度精度与电机配置不符');
                var res = await this._protocolMotorRequest(p, 'setSpeed', speed);
                if (res && res.success) {
                    Notification.success('速度设置成功');
                    var field = document.getElementById('modbus-ctrl-motor-speed');
                    field.dataset.dirty = field.value.trim() === input ? 'false' : 'true';
                    this._appendDebugLog(res.debug, 'Motor SetSpeed');
                } else {
                    Notification.error((res && res.error) || '操作失败');
                    this._appendDebugError('Motor SetSpeed failed', res && res.debug);
                }
            } catch (e) { this._motorRequestError(e); }
        },

        async setMotorPulse() {
            if (this._isCtrlModalOffline()) return;
            var p = this._getMotorDevParams();
            if (!p) return;
            var input = document.getElementById('modbus-ctrl-motor-pulse').value.trim();
            var pulse = input ? Number(input) : NaN;
            try {
                var res = await this._protocolMotorRequest(p, 'setPulse', pulse);
                if (res && res.success) {
                    Notification.success('脉冲数设置成功');
                    var field = document.getElementById('modbus-ctrl-motor-pulse');
                    field.dataset.dirty = field.value.trim() === input ? 'false' : 'true';
                    this._appendDebugLog(res.debug, 'Motor SetPulse');
                } else {
                    Notification.error((res && res.error) || '操作失败');
                    this._appendDebugError('Motor SetPulse failed', res && res.debug);
                }
            } catch (e) { this._motorRequestError(e); }
        },

        async refreshMotorStatus() {
            var p = this._getMotorDevParams();
            if (!p) return;
            try {
                var res = await this._protocolMotorRequest(p, 'readStatus');
                if (res && res.success && res.data) {
                    this._markCtrlModalOnline();
                    var data = res.data;
                    this._applyProtocolMotorState(data);
                    var sf = Math.pow(10, p.motorDecimals || 0);
                    var elSpeed = document.getElementById('motor-cur-speed');
                    var elPulse = document.getElementById('motor-cur-pulse');
                    var elCount = document.getElementById('motor-cur-count');
                    if (data.speed !== undefined && elSpeed) elSpeed.textContent = (data.speed / sf).toFixed(p.motorDecimals || 0);
                    if (data.pulse !== undefined && elPulse) elPulse.textContent = data.pulse;
                    this._motorUnknownRunState();
                    if (data.count !== undefined && elCount) elCount.textContent = data.count + ' 计数';
                    this._appendDebugLog(res.debug, 'Motor ReadStatus');
                } else {
                    this._appendDebugError('Motor ReadStatus failed', res && res.debug);
                    this._setCtrlModalOffline(true, (res && res.error) || '设备离线，当前控制不可操作');
                }
            } catch (e) {
                if (e && e.motorViewClosed) return;
                console.error('refreshMotorStatus failed:', e);
                ['motor-cur-speed', 'motor-cur-pulse'].forEach(function(id) {
                    var element = document.getElementById(id); if (element) element.textContent = '--';
                });
                this._motorUnknownRunState();
                this._setCtrlModalOffline(true, e && e.message || '电机状态读取失败');
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

    // A browser abort cannot undo a relay transaction already running on the
    // device. Admit only one relay read/write and never queue repeated clicks.
    AppState._runModbusRelayRequest = function(write, operation) {
        if (this._modbusRelayRequest) {
            if (write) Notification.warning('继电器请求处理中，请稍后再操作');
            return write ? Promise.resolve(false) : this._modbusRelayRequest;
        }
        var self = this;
        var pending = Promise.resolve().then(operation).finally(function() {
            if (self._modbusRelayRequest === pending) self._modbusRelayRequest = null;
        });
        this._modbusRelayRequest = pending;
        return pending;
    };

    [['refreshCoilStatus', false], ['toggleCoil', true],
     ['batchCoil', true], ['startCoilDelay', true]].forEach(function(entry) {
        var original = AppState[entry[0]];
        AppState[entry[0]] = function() {
            var self = this;
            var args = Array.prototype.slice.call(arguments);
            return this._runModbusRelayRequest(entry[1], function() {
                return original.apply(self, args);
            });
        };
    });
})();
