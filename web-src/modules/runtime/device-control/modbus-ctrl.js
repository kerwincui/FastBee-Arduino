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

        _dcRefreshCoilStatus: function(devIdx) {
            var self = this;
            var p = this._dcGetCoilParams(devIdx);
            return apiGetSilent('/api/modbus/coil/status', {
                slaveAddress: p.slaveAddress, channelCount: p.channelCount,
                coilBase: p.coilBase, mode: p.relayMode
            }).then(function(res) {
                if (res && res.success && res.data && res.data.states) {
                    self._dcCoilStates[devIdx] = res.data.states;
                    self._dcUpdateAllCoilUI(devIdx);
                    self._dcUpdatePanelOnlineState(devIdx, true);
                } else {
                    self._dcUpdatePanelOnlineState(devIdx, false);
                }
            }).catch(function() {
                if (!self._dcCoilStates[devIdx]) {
                    self._dcCoilStates[devIdx] = new Array(p.channelCount).fill(false);
                    self._dcRerenderCoilGrid(devIdx);
                }
                self._dcUpdatePanelOnlineState(devIdx, false);
            });
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
            if (online) {
                panel.classList.remove('dc-offline');
                var badge = panel.querySelector('.dc-offline-badge');
                if (badge) badge.remove();
            } else {
                panel.classList.add('dc-offline');
                var header = panel.querySelector('.dc-card-header');
                if (header && !header.querySelector('.dc-offline-badge')) {
                    var span = document.createElement('span');
                    span.className = 'dc-offline-badge';
                    span.textContent = this._t('device-control-offline') || '离线';
                    header.appendChild(span);
                }
            }
        },

        // 单通道增量更新
        _dcUpdateSingleCoilUI: function(devIdx, ch) {
            var dev = this._modbusDevices[devIdx];
            if (!dev) return false;
            var ncMode = dev.ncMode || false;
            var states = this._dcCoilStates[devIdx] || [];
            var coilState = ch < states.length ? states[ch] : false;
            var isOn = ncMode ? !coilState : coilState;
            var card = document.querySelector('.dc-coil-card[data-dev="' + devIdx + '"][data-ch="' + ch + '"]');
            if (!card) { this._dcRerenderCoilGrid(devIdx); return false; }
            card.classList.remove('dc-coil-on', 'dc-coil-off');
            card.classList.add(isOn ? 'dc-coil-on' : 'dc-coil-off');
            var stEl = card.querySelector('.dc-coil-st');
            if (stEl) stEl.textContent = isOn
                ? (this._t('modbus-ctrl-status-on') || 'ON')
                : (this._t('modbus-ctrl-status-off') || 'OFF');
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
            this._dcCancelInit();
            var self = this;
            var p = this._dcGetCoilParams(devIdx);

            // 乐观更新：立即反转本地状态并更新 UI
            var states = this._dcCoilStates[devIdx] || [];
            var prevState = (ch < states.length) ? states[ch] : false;
            if (ch < states.length) states[ch] = !prevState;
            this._dcCoilStates[devIdx] = states;
            this._dcUpdateSingleCoilUI(devIdx, ch);

            if (cardEl) {
                cardEl.style.pointerEvents = 'none';
            }

            apiPost('/api/modbus/coil/control', {
                slaveAddress: p.slaveAddress, channel: ch, coilBase: p.coilBase,
                action: 'toggle', mode: p.relayMode
            }).then(function(res) {
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
                    Notification.error((res && res.error) || self._t('modbus-ctrl-fail'));
                }
                if (cardEl) cardEl.style.pointerEvents = '';
                self._dcCoilPending[devIdx + '_' + ch] = false;
            }, function() {
                var st = self._dcCoilStates[devIdx] || [];
                if (ch < st.length) st[ch] = prevState;
                self._dcCoilStates[devIdx] = st;
                self._dcUpdateSingleCoilUI(devIdx, ch);
                Notification.error(self._t('modbus-ctrl-fail'));
                if (cardEl) cardEl.style.pointerEvents = '';
                self._dcCoilPending[devIdx + '_' + ch] = false;
            });
        },

        _dcBatchCoil: function(devIdx, action, btnEl) {
            this._dcCancelInit();
            var self = this;
            var p = this._dcGetCoilParams(devIdx);
            var modbusAction = action;
            if (p.ncMode) {
                if (action === 'allOn') modbusAction = 'allOff';
                else if (action === 'allOff') modbusAction = 'allOn';
            }

            var prevStates = (this._dcCoilStates[devIdx] || []).slice();
            var targetVal;
            if (action === 'allOn') targetVal = true;
            else if (action === 'allOff') targetVal = false;
            else targetVal = null;
            var optimistic = prevStates.map(function(s) {
                return targetVal !== null ? targetVal : !s;
            });
            this._dcCoilStates[devIdx] = optimistic;
            this._dcUpdateAllCoilUI(devIdx);

            apiPost('/api/modbus/coil/batch', {
                slaveAddress: p.slaveAddress, channelCount: p.channelCount,
                coilBase: p.coilBase, action: modbusAction, mode: p.relayMode
            }).then(function(res) {
                if (res && res.success && res.data && res.data.states) {
                    self._dcCoilStates[devIdx] = res.data.states;
                    self._dcUpdateAllCoilUI(devIdx);
                } else {
                    self._dcCoilStates[devIdx] = prevStates;
                    self._dcUpdateAllCoilUI(devIdx);
                    Notification.error((res && res.error) || self._t('modbus-ctrl-fail'));
                }
                if (btnEl) btnEl.disabled = false;
                self._dcBatchPending = false;
            }, function() {
                self._dcCoilStates[devIdx] = prevStates;
                self._dcUpdateAllCoilUI(devIdx);
                Notification.error(self._t('modbus-ctrl-fail'));
                if (btnEl) btnEl.disabled = false;
                self._dcBatchPending = false;
            });
        },

        _dcStartCoilDelay: function(devIdx, channel, delayUnits) {
            this._dcCancelInit();
            var self = this;
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

            apiPost('/api/modbus/coil/delay', params).then(function(res) {
                if (res && res.success) {
                    window.Notification && Notification.success(
                        self._t('modbus-delay-success') + ' CH' + channel + ' ' + (delayUnits * 0.1).toFixed(1) + 's'
                    );
                } else {
                    window.Notification && Notification.error(
                        (res && res.error) || self._t('modbus-delay-fail')
                    );
                }
            }).catch(function() {
                window.Notification && Notification.error(self._t('modbus-delay-fail'));
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
            var p = this._dcGetPwmParams(devIdx);
            return apiGetSilent('/api/modbus/register/read', {
                slaveAddress: p.slaveAddress,
                startAddress: p.regBase,
                quantity: p.channelCount,
                functionCode: 3
            }).then(function(res) {
                if (res && res.success && res.data && res.data.values) {
                    self._dcPwmStates[devIdx] = res.data.values;
                    self._dcRerenderPwmGrid(devIdx);
                    self._dcUpdatePanelOnlineState(devIdx, true);
                } else {
                    self._dcUpdatePanelOnlineState(devIdx, false);
                }
            }).catch(function() {
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
            var p = this._dcGetPwmParams(devIdx);
            value = Math.max(0, Math.min(value, p.maxValue));
            var states = (this._dcPwmStates[devIdx] || []).slice();
            var prevVal = ch < states.length ? states[ch] : 0;
            if (ch < states.length) states[ch] = value;
            this._dcPwmStates[devIdx] = states;
            this._dcRerenderPwmGrid(devIdx);
            apiPost('/api/modbus/register/write', {
                slaveAddress: p.slaveAddress,
                registerAddress: p.regBase + ch,
                value: value
            }).then(function(res) {
                if (res && res.success) {
                    Notification.success(self._t('modbus-ctrl-success'));
                } else {
                    var s = (self._dcPwmStates[devIdx] || []).slice();
                    if (ch < s.length) s[ch] = prevVal;
                    self._dcPwmStates[devIdx] = s;
                    self._dcRerenderPwmGrid(devIdx);
                    Notification.error((res && res.error) || self._t('modbus-ctrl-fail'));
                }
            }).catch(function() {
                var s = (self._dcPwmStates[devIdx] || []).slice();
                if (ch < s.length) s[ch] = prevVal;
                self._dcPwmStates[devIdx] = s;
                self._dcRerenderPwmGrid(devIdx);
                Notification.error(self._t('modbus-ctrl-fail'));
            });
        },

        _dcBatchPwm: function(devIdx, action) {
            this._dcCancelInit();
            var self = this;
            var p = this._dcGetPwmParams(devIdx);
            var prevStates = (this._dcPwmStates[devIdx] || []).slice();
            var values = [];
            var fillVal = action === 'max' ? p.maxValue : 0;
            for (var i = 0; i < p.channelCount; i++) values.push(fillVal);
            this._dcPwmStates[devIdx] = values.slice();
            this._dcRerenderPwmGrid(devIdx);
            apiPost('/api/modbus/register/batch-write', {
                slaveAddress: p.slaveAddress,
                startAddress: p.regBase,
                values: JSON.stringify(values)
            }).then(function(res) {
                if (res && res.success) {
                    Notification.success(self._t('modbus-ctrl-success'));
                } else {
                    self._dcPwmStates[devIdx] = prevStates;
                    self._dcRerenderPwmGrid(devIdx);
                    Notification.error((res && res.error) || self._t('modbus-ctrl-fail'));
                }
            }).catch(function() {
                self._dcPwmStates[devIdx] = prevStates;
                self._dcRerenderPwmGrid(devIdx);
                Notification.error(self._t('modbus-ctrl-fail'));
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
            var p = this._dcGetPidParams(devIdx);
            var addrMap = { sv: p.svAddr, p: p.pAddr, i: p.iAddr, d: p.dAddr };
            var addr = addrMap[paramName];
            if (addr === undefined) return;
            var rawValue = Math.round(parseFloat(displayValue) * p.scaleFactor);
            if (isNaN(rawValue)) return;
            apiPost('/api/modbus/register/write', {
                slaveAddress: p.slaveAddress, registerAddress: addr, value: rawValue
            }).then(function(res) {
                if (res && res.success) {
                    var v = self._dcPidValues[devIdx] || {};
                    v[paramName] = rawValue;
                    self._dcPidValues[devIdx] = v;
                    self._dcRerenderPidGrid(devIdx);
                    Notification.success(self._t('modbus-ctrl-success'));
                } else {
                    Notification.error((res && res.error) || self._t('modbus-ctrl-fail'));
                }
            }).catch(function() {
                Notification.error(self._t('modbus-ctrl-fail'));
            });
        },

        // --- Motor ---

        _dcRefreshMotorStatus: function(devIdx) {
            var self = this;
            var p = this._dcGetMotorParams(devIdx);
            return apiPostSilent('/api/modbus/motor/control', {
                slaveAddress: p.slaveAddress, action: 'readStatus'
            }).then(function(res) {
                if (res && res.success && res.data) {
                    self._dcUpdateMotorRunUI(devIdx, res.data, p.motorDecimals);
                    self._dcUpdatePanelOnlineState(devIdx, true);
                } else {
                    self._dcUpdatePanelOnlineState(devIdx, false);
                }
            }).catch(function() {
                self._dcUpdatePanelOnlineState(devIdx, false);
            });
        },

        _dcUpdateMotorRunUI: function(devIdx, data, decimals) {
            var sf = Math.pow(10, decimals || 0);
            var elSpeed = document.getElementById('dc-motor-speed-' + devIdx);
            var elPulse = document.getElementById('dc-motor-pulse-' + devIdx);
            var elDir = document.getElementById('dc-motor-dir-' + devIdx);
            var elCount = document.getElementById('dc-motor-count-' + devIdx);
            var elRun = document.getElementById('dc-motor-run-' + devIdx);
            if (data.speed !== undefined && elSpeed) elSpeed.textContent = (data.speed / sf).toFixed(decimals || 0);
            if (data.pulse !== undefined && elPulse) elPulse.textContent = data.pulse;
            if (elDir) {
                if (data.direction === 'forward' || data.direction === 1) {
                    elDir.textContent = '← ' + (this._t('modbus-motor-ctrl-forward') || '正转');
                } else if (data.direction === 'reverse' || data.direction === -1) {
                    elDir.textContent = '→ ' + (this._t('modbus-motor-ctrl-reverse') || '反转');
                } else {
                    elDir.textContent = this._t('modbus-motor-status-stop') || '停止';
                }
            }
            if (data.count !== undefined && elCount) elCount.textContent = data.count + ' ' + (this._t('modbus-motor-count') || '次');
            if (elRun) {
                var dir = data.direction || '';
                elRun.className = 'motor-run-badge ' + (dir === 'forward' || dir === 1 ? 'motor-run-forward' : dir === 'reverse' || dir === -1 ? 'motor-run-reverse' : 'motor-run-stopped');
                elRun.textContent = (dir === 'forward' || dir === 1) ? (this._t('modbus-motor-dir-forward') || '正转中') : (dir === 'reverse' || dir === -1) ? (this._t('modbus-motor-dir-reverse') || '反转中') : (this._t('modbus-motor-status-stop') || '停止');
            }
        },

        _dcMotorAction: function(devIdx, action) {
            this._dcCancelInit();
            var self = this;
            var p = this._dcGetMotorParams(devIdx);
            apiPostSilent('/api/modbus/motor/control', {
                slaveAddress: p.slaveAddress, action: action
            }).then(function(res) {
                if (res && res.success) {
                    Notification.success(self._t('modbus-motor-ctrl-' + action + '-ok') || (action === 'forward' ? '正转指令已发送' : action === 'reverse' ? '反转指令已发送' : '停止指令已发送'));
                    var elDir = document.getElementById('dc-motor-dir-' + devIdx);
                    var elRun = document.getElementById('dc-motor-run-' + devIdx);
                    if (elDir) {
                        if (action === 'forward') {
                            elDir.textContent = '← ' + (self._t('modbus-motor-ctrl-forward') || '正转');
                        } else if (action === 'reverse') {
                            elDir.textContent = '→ ' + (self._t('modbus-motor-ctrl-reverse') || '反转');
                        } else {
                            elDir.textContent = self._t('modbus-motor-status-stop') || '停止';
                        }
                    }
                    if (elRun) {
                        elRun.className = 'motor-run-badge ' + (action === 'forward' ? 'motor-run-forward' : action === 'reverse' ? 'motor-run-reverse' : 'motor-run-stopped');
                        elRun.textContent = (action === 'forward' ? (self._t('modbus-motor-dir-forward') || '正转中') : action === 'reverse' ? (self._t('modbus-motor-dir-reverse') || '反转中') : (self._t('modbus-motor-status-stop') || '停止'));
                    }
                } else {
                    Notification.error((res && res.error) || self._t('modbus-ctrl-fail'));
                }
            }).catch(function() {
                Notification.error(self._t('modbus-ctrl-fail'));
            });
        },

        _dcMotorSet: function(devIdx, param) {
            this._dcCancelInit();
            var self = this;
            var p = this._dcGetMotorParams(devIdx);
            var inputId = 'dc-motor-' + param + '-in-' + devIdx;
            var input = document.getElementById(inputId);
            var value = parseInt(input ? input.value : 0) || 0;
            var action = param === 'speed' ? 'setSpeed' : 'setPulse';
            apiPostSilent('/api/modbus/motor/control', {
                slaveAddress: p.slaveAddress, action: action, value: value
            }).then(function(res) {
                if (res && res.success) {
                    Notification.success(self._t('modbus-motor-ctrl-' + param + '-ok') || (param === 'speed' ? '速度设置成功' : '脉冲数设置成功'));
                } else {
                    Notification.error((res && res.error) || self._t('modbus-ctrl-fail'));
                }
            }).catch(function() {
                Notification.error(self._t('modbus-ctrl-fail'));
            });
        }
    });
})();
