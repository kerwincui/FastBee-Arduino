/**
 * device-control/modbus-ctrl.js — Modbus 设备控制操作 (Coil/PWM/PID/Motor)
 */
(function() {
    'use strict';

    Object.assign(AppState, {

        // --- 继电器 Coil ---

        _dcGetCoilParams: function(devIdx) {
            var dev = this._modbusDevices[devIdx] || {};
            return {
                slaveAddress: dev.slaveAddress || 1,
                channelCount: dev.channelCount || 2,
                coilBase: dev.coilBase || 0,
                ncMode: !!dev.ncMode,
                relayMode: (dev.controlProtocol === 1) ? 'register' : 'coil'
            };
        },

        _dcCancelCoilRetry: function(devIdx) {
            var key = 'coilRetry' + devIdx;
            var timers = this._dcAutoRefreshTimers || {};
            if (timers[key]) clearTimeout(timers[key]);
            delete timers[key];
            if (this._dcCoilRetryDelays) delete this._dcCoilRetryDelays[devIdx];
        },

        _dcScheduleCoilRetry: function(devIdx) {
            var generation = this._dcLifecycleGeneration;
            if (!this._dcIsLifecycleActive(generation)) return;
            var timers = this._dcAutoRefreshTimers || (this._dcAutoRefreshTimers = {});
            var delays = this._dcCoilRetryDelays || (this._dcCoilRetryDelays = {});
            var key = 'coilRetry' + devIdx;
            if (timers[key]) return;
            var delay = delays[devIdx] || 2000;
            delays[devIdx] = Math.min(delay * 2, 60000);
            timers[key] = setTimeout(function() {
                delete timers[key];
                if (this._dcIsLifecycleActive(generation)) this._dcRefreshCoilStatus(devIdx);
            }.bind(this), delay);
        },

        _dcRefreshCoilStatus: function(devIdx) {
            var self = this;
            var generation = this._dcLifecycleGeneration;
            var p = this._dcGetCoilParams(devIdx);
            var reads = this._dcCoilReads || (this._dcCoilReads = {});
            if (reads[devIdx]) return reads[devIdx];
            var request = apiGetSilent('/api/modbus/coil/status', {
                slaveAddress: p.slaveAddress, channelCount: p.channelCount,
                coilBase: p.coilBase, mode: p.relayMode
            }).then(function(res) {
                if (!self._dcIsLifecycleActive(generation)) return;
                if (res && res.success === false && res.errorCode === 'BUSY') {
                    self._dcCoilStates[devIdx] = new Array(p.channelCount).fill(null);
                    self._dcUpdateAllCoilUI(devIdx);
                    self._dcUpdatePanelOnlineState(devIdx, null);
                    self._dcScheduleCoilRetry(devIdx);
                    return;
                }
                if (res && res.success && res.data && Array.isArray(res.data.states) &&
                    res.data.states.length === p.channelCount &&
                    res.data.states.every(function(value) { return typeof value === 'boolean'; })) {
                    self._dcCoilStates[devIdx] = res.data.states;
                    self._dcUpdateAllCoilUI(devIdx);
                    self._dcUpdatePanelOnlineState(devIdx, true);
                    self._dcCancelCoilRetry(devIdx);
                } else {
                    self._dcCoilStates[devIdx] = new Array(p.channelCount).fill(null);
                    self._dcUpdateAllCoilUI(devIdx);
                    self._dcUpdatePanelOnlineState(devIdx, false);
                    self._dcScheduleCoilRetry(devIdx);
                }
            }).catch(function(error) {
                if (!self._dcIsLifecycleActive(generation)) return;
                self._dcCoilStates[devIdx] = new Array(p.channelCount).fill(null);
                self._dcUpdateAllCoilUI(devIdx);
                var deferred = error && (error.status === 503 || error.status === 429);
                self._dcUpdatePanelOnlineState(devIdx, deferred ? null : false);
                if (!error || (error.status !== 401 && error.status !== 403)) self._dcScheduleCoilRetry(devIdx);
            }).finally(function() {
                if (reads[devIdx] === request) delete reads[devIdx];
            });
            reads[devIdx] = request;
            return request;
        },

        _dcRerenderCoilGrid: function(devIdx) {
            var grid = document.getElementById('dc-coil-grid-' + devIdx);
            if (!grid) return;
            var p = this._dcGetCoilParams(devIdx);
            var states = this._dcCoilStates[devIdx] || [];
            grid.outerHTML = this._renderDcCoilGrid(devIdx, p.channelCount, states, p.ncMode);
        },

        /**
         * 更新 Modbus 设备面板的在线/离线状态
         */
        _dcUpdatePanelOnlineState: function(devIdx, online) {
            this._dcDeviceOnline[devIdx] = online;
            var panel = document.querySelector('.dc-modbus-device-panel[data-dev-idx="' + devIdx + '"]');
            if (!panel) return;
            if (online === true) {
                panel.classList.remove('dc-offline');
                var badge = panel.querySelector('.dc-offline-badge');
                if (badge) badge.remove();
            } else {
                panel.classList.toggle('dc-offline', online === false);
                var header = panel.querySelector('.dc-card-header');
                if (header) {
                    var span = header.querySelector('.dc-offline-badge');
                    if (!span) {
                        span = document.createElement('span');
                        span.className = 'dc-offline-badge';
                        header.appendChild(span);
                    }
                    span.textContent = online === false ? '离线' : '忙碌，待刷新';
                }
            }
        },

        // 单通道增量更新
        _dcUpdateSingleCoilUI: function(devIdx, ch) {
            var dev = this._modbusDevices[devIdx];
            if (!dev) return false;
            var ncMode = dev.ncMode || false;
            var states = this._dcCoilStates[devIdx] || [];
            var coilState = ch < states.length ? states[ch] : null;
            var known = typeof coilState === 'boolean';
            var isOn = known ? (ncMode ? !coilState : coilState) : null;
            var card = document.querySelector('.dc-coil-card[data-dev="' + devIdx + '"][data-ch="' + ch + '"]');
            if (!card) { this._dcRerenderCoilGrid(devIdx); return false; }
            card.classList.remove('dc-coil-on', 'dc-coil-off', 'dc-coil-unknown');
            card.classList.add(!known ? 'dc-coil-unknown' : (isOn ? 'dc-coil-on' : 'dc-coil-off'));
            var stEl = card.querySelector('.dc-coil-st');
            if (stEl) stEl.textContent = !known ? '--' : (isOn ? 'ON' : 'OFF');
            return true;
        },

        // 全通道增量更新
        _dcUpdateAllCoilUI: function(devIdx) {
            var dev = this._modbusDevices[devIdx];
            if (!dev) return;
            var count = dev.channelCount || 8;
            var grid = document.getElementById('dc-coil-grid-' + devIdx);
            if (!grid) { this._dcRerenderCoilGrid(devIdx); return; }
            for (var ch = 0; ch < count; ch++) {
                this._dcUpdateSingleCoilUI(devIdx, ch);
            }
        },

        _dcToggleCoil: function(devIdx, ch, cardEl) {
            this._dcCancelInit(false);
            var self = this;
            var generation = this._dcLifecycleGeneration;
            var p = this._dcGetCoilParams(devIdx);

            // 乐观更新：立即反转本地状态并更新 UI
            var states = this._dcCoilStates[devIdx] || [];
            var prevState = typeof states[ch] === 'boolean' ? states[ch] : null;
            states[ch] = prevState === null ? null : !prevState;
            this._dcCoilStates[devIdx] = states;
            this._dcUpdateSingleCoilUI(devIdx, ch);

            if (cardEl) {
                cardEl.style.pointerEvents = 'none';
            }

            return apiPostPriority('/api/modbus/coil/control', {
                slaveAddress: p.slaveAddress, channel: ch, coilBase: p.coilBase,
                action: 'toggle', mode: p.relayMode
            }).then(function(res) {
                if (!self._dcIsLifecycleActive(generation)) return;
                if (res && res.success && res.data) {
                    var st = self._dcCoilStates[devIdx] || [];
                    if (ch < st.length && st[ch] !== res.data.state) {
                        st[ch] = res.data.state;
                        self._dcCoilStates[devIdx] = st;
                        self._dcUpdateSingleCoilUI(devIdx, ch);
                    }
                } else {
                    var st = self._dcCoilStates[devIdx] || [];
                    if (ch < st.length) st[ch] = prevState;
                    self._dcCoilStates[devIdx] = st;
                    self._dcUpdateSingleCoilUI(devIdx, ch);
                    Notification.error((res && res.error) || '操作失败');
                }
                if (cardEl) cardEl.style.pointerEvents = '';
                self._dcCoilPending[devIdx + '_' + ch] = false;
            }, function() {
                if (!self._dcIsLifecycleActive(generation)) return;
                var st = self._dcCoilStates[devIdx] || [];
                if (ch < st.length) st[ch] = prevState;
                self._dcCoilStates[devIdx] = st;
                self._dcUpdateSingleCoilUI(devIdx, ch);
                Notification.error('操作失败');
                if (cardEl) cardEl.style.pointerEvents = '';
                self._dcCoilPending[devIdx + '_' + ch] = false;
            });
        },

        _dcBatchCoil: function(devIdx, action, btnEl) {
            this._dcCancelInit(false);
            var self = this;
            var generation = this._dcLifecycleGeneration;
            var p = this._dcGetCoilParams(devIdx);
            var modbusAction = action;
            if (p.ncMode) {
                if (action === 'allOn') modbusAction = 'allOff';
                else if (action === 'allOff') modbusAction = 'allOn';
            }

            var prevStates = (this._dcCoilStates[devIdx] || []).slice();
            var targetVal;
            if (modbusAction === 'allOn') targetVal = true;
            else if (modbusAction === 'allOff') targetVal = false;
            else targetVal = null;
            var optimistic = prevStates.map(function(s) {
                return targetVal !== null ? targetVal : (typeof s === 'boolean' ? !s : null);
            });
            this._dcCoilStates[devIdx] = optimistic;
            this._dcUpdateAllCoilUI(devIdx);

            return apiPostPriority('/api/modbus/coil/batch', {
                slaveAddress: p.slaveAddress, channelCount: p.channelCount,
                coilBase: p.coilBase, action: modbusAction, mode: p.relayMode
            }).then(function(res) {
                if (!self._dcIsLifecycleActive(generation)) return;
                if (res && res.success && res.data && res.data.states) {
                    self._dcCoilStates[devIdx] = res.data.states;
                    self._dcUpdateAllCoilUI(devIdx);
                } else {
                    self._dcCoilStates[devIdx] = prevStates;
                    self._dcUpdateAllCoilUI(devIdx);
                    Notification.error((res && res.error) || '操作失败');
                }
                if (btnEl) btnEl.disabled = false;
                self._dcBatchPending = false;
            }, function() {
                if (!self._dcIsLifecycleActive(generation)) return;
                self._dcCoilStates[devIdx] = prevStates;
                self._dcUpdateAllCoilUI(devIdx);
                Notification.error('操作失败');
                if (btnEl) btnEl.disabled = false;
                self._dcBatchPending = false;
            });
        },

        _dcStartCoilDelay: function(devIdx, channel, delayUnits) {
            this._dcCancelInit(false);
            var self = this;
            var generation = this._dcLifecycleGeneration;
            var p = this._dcGetCoilParams(devIdx);
            var dev = this._modbusDevices[devIdx] || {};

            var params = {
                slaveAddress: p.slaveAddress,
                channel: channel,
                delayBase: 0x0200,
                delayUnits: delayUnits,
                ncMode: !!dev.ncMode,
                coilBase: p.coilBase,
                mode: p.relayMode
            };

            return apiPostPriority('/api/modbus/coil/delay', params).then(function(res) {
                if (!self._dcIsLifecycleActive(generation)) return;
                if (res && res.success) {
                    window.Notification && Notification.success(
                        '延时已启动 CH' + channel + ' ' + (delayUnits * 0.1).toFixed(1) + 's'
                    );
                } else {
                    window.Notification && Notification.error(
                        (res && res.error) || '延时启动失败'
                    );
                }
            }).catch(function() {
                if (!self._dcIsLifecycleActive(generation)) return;
                window.Notification && Notification.error('延时启动失败');
            });
        },

        // --- PWM ---

        _dcGetPwmParams: function(devIdx) {
            var dev = this._modbusDevices[devIdx] || {};
            var res = dev.pwmResolution || 8;
            return {
                slaveAddress: dev.slaveAddress || 1,
                channelCount: dev.channelCount || 4,
                regBase: dev.pwmRegBase || 0,
                resolution: res,
                maxValue: (1 << res) - 1
            };
        },

        _dcRefreshPwmStatus: function(devIdx) {
            var self = this;
            var generation = this._dcLifecycleGeneration;
            var p = this._dcGetPwmParams(devIdx);
            return apiGetSilent('/api/modbus/register/read', {
                slaveAddress: p.slaveAddress,
                startAddress: p.regBase,
                quantity: p.channelCount,
                functionCode: 3
            }).then(function(res) {
                if (!self._dcIsLifecycleActive(generation)) return;
                if (res && res.success && res.data && res.data.values) {
                    self._dcPwmStates[devIdx] = res.data.values;
                    self._dcRerenderPwmGrid(devIdx);
                    self._dcUpdatePanelOnlineState(devIdx, true);
                } else {
                    self._dcUpdatePanelOnlineState(devIdx, false);
                }
            }).catch(function() {
                if (!self._dcIsLifecycleActive(generation)) return;
                if (!self._dcPwmStates[devIdx]) {
                    self._dcPwmStates[devIdx] = new Array(p.channelCount).fill(0);
                    self._dcRerenderPwmGrid(devIdx);
                }
                self._dcUpdatePanelOnlineState(devIdx, false);
            });
        },

        _dcRerenderPwmGrid: function(devIdx) {
            var grid = document.getElementById('dc-pwm-grid-' + devIdx);
            if (!grid) return;
            var p = this._dcGetPwmParams(devIdx);
            var states = this._dcPwmStates[devIdx] || [];
            grid.outerHTML = this._renderDcPwmGrid(devIdx, p.channelCount, states, p.maxValue);
        },

        _dcOnPwmSliderInput: function(devIdx, ch, val) {
            var grid = document.getElementById('dc-pwm-grid-' + devIdx);
            if (!grid) return;
            var numInput = grid.querySelector('.dc-pwm-num[data-ch="' + ch + '"]');
            var pctSpan = grid.querySelector('.dc-pwm-pct[data-ch="' + ch + '"]');
            var p = this._dcGetPwmParams(devIdx);
            if (numInput) numInput.value = val;
            if (pctSpan) pctSpan.textContent = (p.maxValue > 0 ? Math.round(val / p.maxValue * 100) : 0) + '%';
        },

        _dcSetPwmChannel: function(devIdx, ch, value) {
            this._dcCancelInit();
            var self = this;
            var generation = this._dcLifecycleGeneration;
            var p = this._dcGetPwmParams(devIdx);
            value = Math.max(0, Math.min(value, p.maxValue));
            var states = (this._dcPwmStates[devIdx] || []).slice();
            var prevVal = ch < states.length ? states[ch] : 0;
            if (ch < states.length) states[ch] = value;
            this._dcPwmStates[devIdx] = states;
            this._dcRerenderPwmGrid(devIdx);
            apiPostPriority('/api/modbus/register/write', {
                slaveAddress: p.slaveAddress,
                registerAddress: p.regBase + ch,
                value: value
            }).then(function(res) {
                if (!self._dcIsLifecycleActive(generation)) return;
                if (res && res.success) {
                    Notification.success('操作成功');
                } else {
                    var s = (self._dcPwmStates[devIdx] || []).slice();
                    if (ch < s.length) s[ch] = prevVal;
                    self._dcPwmStates[devIdx] = s;
                    self._dcRerenderPwmGrid(devIdx);
                    Notification.error((res && res.error) || '操作失败');
                }
            }).catch(function() {
                if (!self._dcIsLifecycleActive(generation)) return;
                var s = (self._dcPwmStates[devIdx] || []).slice();
                if (ch < s.length) s[ch] = prevVal;
                self._dcPwmStates[devIdx] = s;
                self._dcRerenderPwmGrid(devIdx);
                Notification.error('操作失败');
            });
        },

        _dcBatchPwm: function(devIdx, action) {
            this._dcCancelInit();
            var self = this;
            var generation = this._dcLifecycleGeneration;
            var p = this._dcGetPwmParams(devIdx);
            var prevStates = (this._dcPwmStates[devIdx] || []).slice();
            var values = [];
            var fillVal = action === 'max' ? p.maxValue : 0;
            for (var i = 0; i < p.channelCount; i++) values.push(fillVal);
            this._dcPwmStates[devIdx] = values.slice();
            this._dcRerenderPwmGrid(devIdx);
            apiPostPriority('/api/modbus/register/batch-write', {
                slaveAddress: p.slaveAddress,
                startAddress: p.regBase,
                values: JSON.stringify(values)
            }).then(function(res) {
                if (!self._dcIsLifecycleActive(generation)) return;
                if (res && res.success) {
                    Notification.success('操作成功');
                } else {
                    self._dcPwmStates[devIdx] = prevStates;
                    self._dcRerenderPwmGrid(devIdx);
                    Notification.error((res && res.error) || '操作失败');
                }
            }).catch(function() {
                if (!self._dcIsLifecycleActive(generation)) return;
                self._dcPwmStates[devIdx] = prevStates;
                self._dcRerenderPwmGrid(devIdx);
                Notification.error('操作失败');
            });
        },

        // --- PID ---

        _dcGetPidParams: function(devIdx) {
            var dev = this._modbusDevices[devIdx] || {};
            var pidA = dev.pidAddrs || [0,1,2,3,4,5];
            var decimals = dev.pidDecimals || 1;
            return {
                slaveAddress: dev.slaveAddress || 1,
                pvAddr: pidA[0] || 0, svAddr: pidA[1] || 1, outAddr: pidA[2] || 2,
                pAddr: pidA[3] || 3, iAddr: pidA[4] || 4, dAddr: pidA[5] || 5,
                decimals: decimals,
                scaleFactor: Math.pow(10, decimals)
            };
        },

        _dcGetMotorParams: function(devIdx) {
            var dev = this._modbusDevices[devIdx] || {};
            return {
                slaveAddress: dev.slaveAddress || 1,
                motorRegs: dev.motorRegs || [0, 1, 2, 5, 7],
                motorDecimals: dev.motorDecimals || 0
            };
        },

        _dcRefreshPidStatus: function(devIdx) {
            var self = this;
            var generation = this._dcLifecycleGeneration;
            var p = this._dcGetPidParams(devIdx);
            if (!p.slaveAddress) return Promise.resolve();
            var addrs = [p.pvAddr, p.svAddr, p.outAddr, p.pAddr, p.iAddr, p.dAddr];
            var minAddr = Math.min.apply(null, addrs);
            var maxAddr = Math.max.apply(null, addrs);
            var quantity = maxAddr - minAddr + 1;
            if (quantity > 125) return Promise.resolve();
            return apiGetSilent('/api/modbus/register/read', {
                slaveAddress: p.slaveAddress, startAddress: minAddr,
                quantity: quantity, functionCode: 3
            }).then(function(res) {
                if (!self._dcIsLifecycleActive(generation)) return;
                if (res && res.success && res.data && res.data.values) {
                    var vals = res.data.values;
                    self._dcPidValues[devIdx] = {
                        pv: vals[p.pvAddr - minAddr], sv: vals[p.svAddr - minAddr],
                        out: vals[p.outAddr - minAddr], p: vals[p.pAddr - minAddr],
                        i: vals[p.iAddr - minAddr], d: vals[p.dAddr - minAddr]
                    };
                    self._dcRerenderPidGrid(devIdx);
                    self._dcUpdatePanelOnlineState(devIdx, true);
                } else {
                    self._dcUpdatePanelOnlineState(devIdx, false);
                }
            }).catch(function() {
                if (!self._dcIsLifecycleActive(generation)) return;
                if (!self._dcPidValues[devIdx]) {
                    self._dcPidValues[devIdx] = { pv: 0, sv: 0, out: 0, p: 0, i: 0, d: 0 };
                    self._dcRerenderPidGrid(devIdx);
                }
                self._dcUpdatePanelOnlineState(devIdx, false);
            });
        },

        _dcRerenderPidGrid: function(devIdx) {
            var grid = document.getElementById('dc-pid-grid-' + devIdx);
            if (!grid) return;
            var p = this._dcGetPidParams(devIdx);
            grid.outerHTML = this._renderDcPidGrid(devIdx, p.scaleFactor, p.decimals);
        },

        _dcSetPidParam: function(devIdx, paramName, displayValue) {
            this._dcCancelInit();
            var self = this;
            var generation = this._dcLifecycleGeneration;
            var p = this._dcGetPidParams(devIdx);
            var addrMap = { sv: p.svAddr, p: p.pAddr, i: p.iAddr, d: p.dAddr };
            var addr = addrMap[paramName];
            if (addr === undefined) return;
            var rawValue = Math.round(parseFloat(displayValue) * p.scaleFactor);
            if (isNaN(rawValue)) return;
            apiPostPriority('/api/modbus/register/write', {
                slaveAddress: p.slaveAddress, registerAddress: addr, value: rawValue
            }).then(function(res) {
                if (!self._dcIsLifecycleActive(generation)) return;
                if (res && res.success) {
                    var v = self._dcPidValues[devIdx] || {};
                    v[paramName] = rawValue;
                    self._dcPidValues[devIdx] = v;
                    self._dcRerenderPidGrid(devIdx);
                    Notification.success('操作成功');
                } else {
                    Notification.error((res && res.error) || '操作失败');
                }
            }).catch(function() {
                if (!self._dcIsLifecycleActive(generation)) return;
                Notification.error('操作失败');
            });
        },

        // --- Motor ---

        _dcStartMotorSync: function() {
            var self = this, generation = this._dcLifecycleGeneration;
            var token = this._dcMotorSyncToken = (this._dcMotorSyncToken || 0) + 1;
            async function refresh() {
                var failed = false;
                if (token !== self._dcMotorSyncToken || !self._dcIsLifecycleActive(generation)) return;
                for (var i = 0; !document.hidden && i < (self._modbusDevices || []).length; i++) {
                    if (token !== self._dcMotorSyncToken || !self._dcIsLifecycleActive(generation)) return;
                    if (self._modbusDevices[i].deviceType !== 'motor' || self._modbusDevices[i].enabled === false) continue;
                    var p = self._dcGetMotorParams(i);
                    if (self._dcMotorPending && self._dcMotorPending[String(p.slaveAddress)]) continue;
                    var sequence = self._dcMotorSequence;
                    try {
                        var get = typeof apiGetSilentFresh === 'function' ? apiGetSilentFresh : apiGetSilent;
                        var res = await get('/api/modbus/motor/state', { slaveAddress: p.slaveAddress });
                        if (token !== self._dcMotorSyncToken || !self._dcIsLifecycleActive(generation)) return;
                        if (sequence !== self._dcMotorSequence) continue;
                        if (!res || !res.success || !res.data) throw new Error('Motor state unavailable');
                        self._dcUpdateMotorRunUI(i, res.data, p.motorDecimals);
                    } catch (_) {
                        if (token !== self._dcMotorSyncToken || !self._dcIsLifecycleActive(generation)) return;
                        if (sequence !== self._dcMotorSequence) continue;
                        self._dcUpdateMotorRunUI(i, { parametersKnown: false }, p.motorDecimals);
                        failed = true;
                    }
                }
                if (token === self._dcMotorSyncToken && self._dcIsLifecycleActive(generation))
                    self._dcAutoRefreshTimers.motorState = setTimeout(refresh, failed ? 10000 : 3000);
            }
            refresh();
        },

        _dcMotorReport: function(devIdx, data, waiting) {
            window.renderModbusMotorReport(document.getElementById('dc-motor-report-' + devIdx), data, waiting, '页面');
        },

        _dcMotorRequest: function(devIdx, action, value) {
            var self = this, p = this._dcGetMotorParams(devIdx);
            var generation = this._dcLifecycleGeneration;
            var key = String(p.slaveAddress), isRead = action === 'readStatus';
            var pending = this._dcMotorPending || (this._dcMotorPending = {});
            if (!isRead && action !== 'stop' && pending[key]) return Promise.reject(new Error('电机指令正在发送'));
            this._dcMotorSequence = (this._dcMotorSequence || 0) + 1;
            var token = Date.now().toString(36) + '-' + Math.random().toString(36).slice(2) + '-' + this._dcMotorSequence;
            if (!isRead) pending[key] = token;
            function report(data, waiting) {
                if (!isRead && self._dcIsLifecycleActive(generation) && pending[key] === token)
                    self._dcMotorReport(devIdx, data, waiting);
            }
            report(null, true);
            var started = Date.now();
            var operationId = 0;
            var terminal = false;
            function completed(response) {
                if (!self._dcIsLifecycleActive(generation)) throw new Error('页面已关闭，已提交指令不会重发');
                if (!response || !response.success) throw new Error(response && response.error || '指令结果待确认');
                var data = response.data || {};
                if (data.operationId !== undefined) {
                    if (!Number.isInteger(data.operationId) || data.operationId <= 0 ||
                        (operationId && operationId !== data.operationId)) throw new Error('电机操作编号不匹配');
                    operationId = data.operationId;
                }
                if (!data.pending) {
                    terminal = true;
                    report(data);
                    // Older firmware returns success only at the envelope.
                    // Never mistake a failed asynchronous operation for that
                    // legacy response shape.
                    if (data.success === false || (data.operationId && data.success !== true)) throw new Error(data.error || '指令未完成');
                    return { success: true, data: data };
                }
                if (!operationId) throw new Error('缺少电机操作编号');
                if (Date.now() - started > 20000) throw new Error('指令结果待确认，请查看设备状态');
                return self._dcDelay(action === 'readStatus' ? 400 : 120, generation).then(function(active) {
                    if (!active) throw new Error('页面已关闭，已提交指令不会重发');
                    return apiPostSilentPriority('/api/modbus/motor/control', { action: 'operation', operationId: operationId }).catch(function(error) {
                        if (error && (error.status === 503 || error.code === 503 || error.data && error.data.code === 503))
                            return { success: true, data: { operationId: operationId, pending: true } };
                        if (error && (error.status === 404 || error.code === 404 || error.data && error.data.code === 404))
                            throw new Error('操作记录已失效，执行结果未知；请核对设备，勿重复启动');
                        throw error;
                    });
                }).then(completed);
            }
            return apiPostSilentPriority('/api/modbus/motor/control', {
                slaveAddress: p.slaveAddress, action: action, value: value || 0, requestId: token
            }).then(completed).catch(function(error) {
                if (!terminal) report(null);
                throw error;
            }).finally(function() {
                if (pending[key] === token) delete pending[key];
            });
        },

        _dcRefreshMotorStatus: function(devIdx) {
            var self = this, generation = this._dcLifecycleGeneration;
            var p = this._dcGetMotorParams(devIdx);
            if (this._dcMotorPending && this._dcMotorPending[String(p.slaveAddress)]) return Promise.resolve();
            return this._dcMotorRequest(devIdx, 'readStatus').then(function(res) {
                if (!self._dcIsLifecycleActive(generation)) return;
                self._dcUpdateMotorRunUI(devIdx, res.data, p.motorDecimals);
                self._dcUpdatePanelOnlineState(devIdx, true);
                return true;
            }).catch(function() {
                if (!self._dcIsLifecycleActive(generation)) return;
                self._dcUpdateMotorRunUI(devIdx, { parametersKnown: false }, p.motorDecimals);
                self._dcUpdatePanelOnlineState(devIdx, false);
                return false;
            });
        },

        _dcUpdateMotorRunUI: function(devIdx, data, decimals) {
            var sf = Math.pow(10, decimals || 0);
            var speed = document.getElementById('dc-motor-speed-' + devIdx);
            var pulse = document.getElementById('dc-motor-pulse-' + devIdx);
            var direction = document.getElementById('dc-motor-dir-' + devIdx);
            var run = document.getElementById('dc-motor-run-' + devIdx);
            if (speed && data.speed !== undefined) speed.textContent = (data.speed / sf).toFixed(decimals || 0);
            if (pulse && data.pulse !== undefined) pulse.textContent = data.pulse;
            if (data.parametersKnown === false && data.parametersCached !== true) {
                if (speed) speed.textContent = '--';
                if (pulse) pulse.textContent = '--';
            }
            ['speed', 'pulse'].forEach(function(param) {
                var input = document.getElementById('dc-motor-' + param + '-in-' + devIdx);
                if (input && input.dataset.dirty !== 'true' && document.activeElement !== input) {
                    if (data.parametersKnown === false && data.parametersCached !== true) input.value = '';
                    else if (data[param] !== undefined) input.value = param === 'speed' ? (data[param] / sf).toFixed(decimals || 0) : String(data[param]);
                }
            });
            var remembered = this._dcMotorAcknowledgements || (this._dcMotorAcknowledgements = {});
            [speed, pulse].forEach(function(el) { if (el) el.title = data.parametersFresh === false && data.parametersCached ? '上次读回值，执行前自动刷新' : ''; });
            if (data.lastAcknowledgedAction !== undefined) remembered[devIdx] = data.lastAcknowledgedAction;
            var acknowledged = remembered[devIdx];
            var label = acknowledged === 'forward' ? '正转应答' : acknowledged === 'reverse' ? '反转应答' : acknowledged === 'stop' ? '停止应答' : '--';
            if (direction) { direction.textContent = label; direction.title = '最近已应答指令，不代表当前实际运动'; }
            if (run) { run.className = 'motor-run-badge motor-run-idle'; run.textContent = data.pending ? '指令处理中' : '运动未确认'; }
            if (run && !data.pending && data.referenceKnown !== undefined) {
                run.textContent = !data.softLimitEnabled ? '点动模式' : !data.referenceKnown ? '参考位置未确认' :
                    data.positionTracking === 'commanded' ? '估算位置 ' + data.estimatedPosition :
                    data.travelReason === 'TRAVEL_RANGE_EXCEEDED' ? '行程预算已耗尽' : '软限位已启用';
                run.title = '无编码器反馈；位置模式与上下界位于通信协议的电机编辑面板';
            }
        },

        _dcMotorAction: function(devIdx, action) {
            this._dcCancelInit(false);
            var self = this, generation = this._dcLifecycleGeneration;
            if (action !== 'stop' && ['speed', 'pulse'].some(function(param) {
                var input = document.getElementById('dc-motor-' + param + '-in-' + devIdx);
                return input && input.dataset.dirty === 'true';
            })) { Notification.warning('请先设置修改后的速度和脉冲'); return Promise.resolve(false); }
            return this._dcMotorRequest(devIdx, action).then(function() {
                if (!self._dcIsLifecycleActive(generation)) return;
                var label = action === 'forward' ? '正转' : action === 'reverse' ? '反转' : '停止';
                Notification.success(label + '指令已发送，运行状态待确认');
                self._dcUpdateMotorRunUI(devIdx, { lastAcknowledgedAction: action }, 0);
            }).catch(function(error) {
                if (self._dcIsLifecycleActive(generation)) Notification.error(error.message || '指令结果待确认');
            });
        },

        _dcMotorSet: function(devIdx, param) {
            this._dcCancelInit(false);
            var self = this, generation = this._dcLifecycleGeneration;
            var input = document.getElementById('dc-motor-' + param + '-in-' + devIdx);
            if (!input || (param !== 'speed' && param !== 'pulse')) return Promise.resolve(false);
            var submittedText = input.value;
            var decimals = param === 'speed' ? this._dcGetMotorParams(devIdx).motorDecimals || 0 : 0;
            var scale = Math.pow(10, decimals), rawValue = Number(submittedText) * scale;
            var value = Math.round(rawValue);
            if (!submittedText.trim() || !Number.isFinite(rawValue) || Math.abs(value - rawValue) > 0.000001 || value < 0 || value > 65535) {
                Notification.error('请输入0到' + (65535 / scale) + '之间、步长为' + (1 / scale) + '的数值'); return Promise.resolve(false);
            }
            return this._dcMotorRequest(devIdx, param === 'speed' ? 'setSpeed' : 'setPulse', value).then(async function() {
                if (self._dcIsLifecycleActive(generation)) {
                    // Editing during a request creates a new unsent draft.
                    input.dataset.dirty = input.value === submittedText ? 'false' : 'true';
                    self._dcUpdateMotorRunUI(devIdx, { parametersKnown: false }, self._dcGetMotorParams(devIdx).motorDecimals);
                    var readback = await self._dcRefreshMotorStatus(devIdx);
                    if (self._dcIsLifecycleActive(generation)) {
                        if (readback) Notification.success(param === 'speed' ? '速度已读回' : '脉冲数已读回');
                        else Notification.warning('参数写入已应答，但读回失败');
                    }
                }
            }).catch(function(error) {
                if (self._dcIsLifecycleActive(generation)) Notification.error(error.message || '参数设置失败');
            });
        }
    });

    // A browser abort cannot undo a relay transaction already running on the
    // device. Admit only one relay read/write and never queue repeated clicks.
    AppState._runModbusRelayRequest = function(write, operation) {
        var self = this, generation = this._dcLifecycleGeneration;
        if (this._modbusRelayRequest) {
            if (write) Notification.warning('继电器请求处理中，请稍后再操作');
            // A new page must obtain its own readback after the old request
            // settles. Keep transactions serialized across page generations.
            if (!write && this._dcRelayRequestGeneration !== generation) {
                return this._modbusRelayRequest.catch(function() {}).then(function() {
                    if (!self._dcIsLifecycleActive(generation)) return;
                    return self._runModbusRelayRequest(false, operation);
                });
            }
            return write ? Promise.resolve(false) : this._modbusRelayRequest;
        }
        this._dcRelayRequestGeneration = generation;
        var pending = Promise.resolve().then(operation).finally(function() {
            if (self._modbusRelayRequest === pending) self._modbusRelayRequest = null;
        });
        this._modbusRelayRequest = pending;
        return pending;
    };

    [['_dcRefreshCoilStatus', false], ['_dcToggleCoil', true],
     ['_dcBatchCoil', true], ['_dcStartCoilDelay', true]].forEach(function(entry) {
        var original = AppState[entry[0]];
        AppState[entry[0]] = function() {
            var self = this;
            var args = Array.prototype.slice.call(arguments);
            return this._runModbusRelayRequest(entry[1], function() {
                return original.apply(self, args);
            }).finally(function() {
                // Release DOM/event admission even if the page generation changed.
                if (entry[0] === '_dcToggleCoil') {
                    self._dcCoilPending[args[0] + '_' + args[1]] = false;
                    if (args[2]) args[2].style.pointerEvents = '';
                } else if (entry[0] === '_dcBatchCoil') {
                    self._dcBatchPending = false;
                    if (args[2]) args[2].disabled = false;
                }
            });
        };
    });
})();
