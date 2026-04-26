/**
 * protocol/modbus-control.js — 控制弹窗、PWM/PID 控制
 */
(function() {
    Object.assign(AppState, {

        // ========== 控制弹窗 ==========

        _openCtrlModal(idx) {
            if (idx === undefined || idx === null || isNaN(idx)) {
                console.warn('[protocol] Invalid device index:', idx);
                Notification.error(i18n.t('modbus-device-not-found') || '设备未找到');
                return;
            }
            if (!this._modbusDevices || !this._modbusDevices[idx]) {
                console.warn('[protocol] Device not found at index', idx);
                Notification.error(i18n.t('modbus-device-not-found') || '设备未找到，请刷新列表');
                this._renderDeviceTable();
                this._renderAllDevices();
                return;
            }
            if (this._coilAutoRefreshTimer) {
                clearInterval(this._coilAutoRefreshTimer);
                this._coilAutoRefreshTimer = null;
            }
            if (this._pidAutoRefreshTimer) {
                clearInterval(this._pidAutoRefreshTimer);
                this._pidAutoRefreshTimer = null;
            }
            var autoEl = document.getElementById('modbus-ctrl-auto-refresh');
            if (autoEl) autoEl.checked = false;
            var pidAutoEl = document.getElementById('modbus-ctrl-pid-auto-refresh');
            if (pidAutoEl) pidAutoEl.checked = false;
            this._coilAutoRefreshErrors = 0;
            this._pidAutoRefreshErrors = 0;
            if (this._activeDeviceIdx >= 0) {
                var cacheKey = 'dev_' + this._activeDeviceIdx;
                if (this._coilStates.length > 0) this._deviceCoilCache[cacheKey] = this._coilStates.slice();
                if (this._pwmStates.length > 0) this._devicePwmCache[cacheKey] = this._pwmStates.slice();
                if (this._pidValues && Object.keys(this._pidValues).length > 0) {
                    this._devicePidCache[cacheKey] = JSON.parse(JSON.stringify(this._pidValues));
                }
            }
            this._activeDeviceIdx = idx;
            localStorage.setItem('modbus_active_device', String(idx));
            var dev = this._modbusDevices[idx];
            var type = dev.deviceType || 'relay';
            var title = document.getElementById('modbus-ctrl-modal-title');
            if (title) title.textContent = (dev.name || 'Device') + ' - ' + (i18n.t('modbus-device-ctrl-title') || '设备控制');
            var relayConfig = document.getElementById('modbus-relay-config');
            var relayPanel = document.getElementById('modbus-relay-panel');
            var pwmPanel = document.getElementById('modbus-pwm-panel');
            var pidPanel = document.getElementById('modbus-pid-panel');
            var motorPanel = document.getElementById('modbus-motor-panel');
            [relayConfig, relayPanel, pwmPanel, pidPanel, motorPanel].forEach(function(el) {
                if (el) el.classList.remove('fb-hidden');
            });
            if (relayConfig) AppState.toggleVisible(relayConfig, type === 'relay');
            if (relayPanel) AppState.toggleVisible(relayPanel, type === 'relay');
            if (pwmPanel) AppState.toggleVisible(pwmPanel, type === 'pwm');
            if (pidPanel) AppState.toggleVisible(pidPanel, type === 'pid');
            if (motorPanel) AppState.toggleVisible(motorPanel, type === 'motor');
            this._updateDelayChannelSelect();
            var newCacheKey = 'dev_' + idx;
            if (type === 'pwm') {
                if (this._devicePwmCache[newCacheKey]) {
                    this._pwmStates = this._devicePwmCache[newCacheKey].slice();
                    this._renderPwmGrid();
                } else {
                    this._pwmStates = [];
                    this._renderPwmGrid(true);
                    this.refreshPwmStatus();
                }
            } else if (type === 'pid') {
                if (this._devicePidCache[newCacheKey]) {
                    this._pidValues = JSON.parse(JSON.stringify(this._devicePidCache[newCacheKey]));
                    this._renderPidGrid();
                } else {
                    this._pidValues = {};
                    this._renderPidGrid(true);
                    this.refreshPidStatus();
                }
            } else if (type === 'motor') {
                this.refreshMotorStatus();
            } else {
                if (this._deviceCoilCache[newCacheKey]) {
                    this._coilStates = this._deviceCoilCache[newCacheKey].slice();
                    this._renderCoilGrid();
                } else {
                    this._coilStates = [];
                    this._renderCoilGrid();
                    this.refreshCoilStatus();
                }
            }
            var modal = document.getElementById('modbus-device-ctrl-modal');
            if (!modal) {
                console.error('[protocol] Modal element "modbus-device-ctrl-modal" not found in DOM');
                return;
            }
            AppState.showModal(modal);
        },

        _closeCtrlModal() {
            if (this._coilAutoRefreshTimer) {
                clearInterval(this._coilAutoRefreshTimer);
                this._coilAutoRefreshTimer = null;
            }
            var autoEl = document.getElementById('modbus-ctrl-auto-refresh');
            if (autoEl) autoEl.checked = false;
            var pidAutoEl = document.getElementById('modbus-ctrl-pid-auto-refresh');
            if (pidAutoEl) pidAutoEl.checked = false;
            var modal = document.getElementById('modbus-device-ctrl-modal');
            if (modal) AppState.hideModal(modal);
        },

        _activateDevice(idx) {
            this._openCtrlModal(idx);
        },

        onDeviceTypeChange() {
            if (this._activeDeviceIdx < 0 || !this._modbusDevices) return;
            var dev = this._modbusDevices[this._activeDeviceIdx];
            if (!dev) return;
            var type = dev.deviceType || 'relay';
            var types = ['relay', 'pwm', 'pid'];
            types.forEach(function(t) {
                var panel = document.getElementById('modbus-' + t + '-panel');
                var config = document.getElementById('modbus-' + t + '-config');
                if (panel) panel.classList.remove('fb-hidden');
                if (config) config.classList.remove('fb-hidden');
                if (panel) AppState.toggleVisible(panel, t === type);
                if (config) AppState.toggleVisible(config, t === type);
            });
        },

        onRelayModeChange() {
            if (this._activeDeviceIdx < 0 || !this._modbusDevices) return;
            var dev = this._modbusDevices[this._activeDeviceIdx];
            if (!dev) return;
            var mode = (dev.controlProtocol === 1) ? 'register' : 'coil';
            var hintEl = document.querySelector('#modbus-relay-config .field-hint');
            if (hintEl) {
                hintEl.textContent = (mode === 'register')
                    ? (i18n.t('modbus-ctrl-reg-base-hint') || '寄存器起始地址，如M88继电器从0x0008开始')
                    : (i18n.t('modbus-ctrl-coil-base-hint') || '线圈起始地址，默认0');
            }
        },

        // ========== PWM 控制 ==========

        _getPwmParams() {
            var dev = (this._activeDeviceIdx >= 0 && this._modbusDevices)
                ? this._modbusDevices[this._activeDeviceIdx] : null;
            var res = dev ? (dev.pwmResolution || 8) : 8;
            return {
                slaveAddress: dev ? (dev.slaveAddress || 1) : 1,
                channelCount: dev ? (dev.channelCount || 4) : 4,
                regBase: dev ? (dev.pwmRegBase || 0) : 0,
                resolution: res,
                maxValue: (1 << res) - 1
            };
        },

        _renderPwmGrid(loading) {
            var grid = document.getElementById('pwm-channel-grid');
            if (!grid) return;
            var p = this._getPwmParams();
            var html = '';
            if (loading) {
                html = '<div class="pwm-loading-placeholder" style="text-align:center;padding:30px;color:#999;">'
                    + '<div style="font-size:14px;margin-bottom:10px;">加载中...</div>'
                    + '<div style="font-size:12px;">正在获取 PWM 通道状态</div>'
                    + '</div>';
                grid.innerHTML = html;
                return;
            }
            for (var i = 0; i < p.channelCount; i++) {
                var val = i < this._pwmStates.length ? this._pwmStates[i] : 0;
                var pct = p.maxValue > 0 ? Math.round(val / p.maxValue * 100) : 0;
                html += '<div class="pwm-card">'
                    + '<div class="pwm-ch">CH' + i + '</div>'
                    + '<input type="range" class="pwm-slider" min="0" max="' + p.maxValue + '" value="' + val + '" data-ch="' + i + '">'
                    + '<div class="pwm-value-row">'
                    + '<input type="number" class="pwm-num-input" min="0" max="' + p.maxValue + '" value="' + val + '" data-ch="' + i + '">'
                    + '<span class="pwm-pct">' + pct + '%</span>'
                    + '</div></div>';
            }
            grid.innerHTML = html;
        },

        async refreshPwmStatus() {
            var p = this._getPwmParams();
            try {
                var res = await apiGetSilent('/api/modbus/register/read', {
                    slaveAddress: p.slaveAddress, startAddress: p.regBase,
                    quantity: p.channelCount, functionCode: 3
                });
                if (res && res.success && res.data && res.data.values) {
                    this._pwmStates = res.data.values;
                    var cacheKey = 'dev_' + this._activeDeviceIdx;
                    this._devicePwmCache[cacheKey] = this._pwmStates.slice();
                    this._renderPwmGrid();
                    this._appendDebugLog(res.debug, 'ReadRegs FC03');
                }
            } catch (e) {
                if (this._pwmStates.length === 0) {
                    var p2 = this._getPwmParams();
                    this._pwmStates = new Array(p2.channelCount).fill(0);
                }
                this._renderPwmGrid();
            }
        },

        async setPwmChannel(ch, value) {
            var p = this._getPwmParams();
            try {
                var res = await apiPost('/api/modbus/register/write', {
                    slaveAddress: p.slaveAddress, registerAddress: p.regBase + ch, value: value
                });
                if (res && res.success) {
                    if (ch < this._pwmStates.length) this._pwmStates[ch] = value;
                    this._appendDebugLog(res.debug, 'WriteReg CH' + ch + '=' + value);
                }
            } catch (e) {
                Notification.error(i18n.t('modbus-ctrl-fail'));
            }
        },

        async batchPwm(action) {
            var p = this._getPwmParams();
            var values = [];
            var fillVal = action === 'max' ? p.maxValue : 0;
            for (var i = 0; i < p.channelCount; i++) values.push(fillVal);
            try {
                var res = await apiPost('/api/modbus/register/batch-write', {
                    slaveAddress: p.slaveAddress, startAddress: p.regBase,
                    values: JSON.stringify(values)
                });
                if (res && res.success) {
                    this._pwmStates = values;
                    this._renderPwmGrid();
                    Notification.success(i18n.t('modbus-ctrl-success'));
                }
            } catch (e) {
                Notification.error(i18n.t('modbus-ctrl-fail'));
            }
        },

        _onPwmSliderInput(ch, val) {
            var numInput = document.querySelector('.pwm-num-input[data-ch="' + ch + '"]');
            var card = numInput ? numInput.closest('.pwm-card') : null;
            var pctSpan = card ? card.querySelector('.pwm-pct') : null;
            var p = this._getPwmParams();
            if (numInput) numInput.value = val;
            if (pctSpan) pctSpan.textContent = (p.maxValue > 0 ? Math.round(val / p.maxValue * 100) : 0) + '%';
        },

        _onPwmSliderChange(ch, val) {
            this.setPwmChannel(ch, parseInt(val));
        },

        _onPwmNumChange(ch, val) {
            var p = this._getPwmParams();
            val = Math.max(0, Math.min(parseInt(val) || 0, p.maxValue));
            var slider = document.querySelector('.pwm-slider[data-ch="' + ch + '"]');
            if (slider) slider.value = val;
            var card = slider ? slider.closest('.pwm-card') : null;
            var pctSpan = card ? card.querySelector('.pwm-pct') : null;
            if (pctSpan) pctSpan.textContent = (p.maxValue > 0 ? Math.round(val / p.maxValue * 100) : 0) + '%';
            this.setPwmChannel(ch, val);
        },

        // ========== PID 控制器 ==========

        _getPidParams() {
            var dev = (this._activeDeviceIdx >= 0 && this._modbusDevices)
                ? this._modbusDevices[this._activeDeviceIdx] : null;
            var pidA = dev ? (dev.pidAddrs || [0,1,2,3,4,5]) : [0,1,2,3,4,5];
            var decimals = dev ? (dev.pidDecimals || 1) : 1;
            return {
                slaveAddress: dev ? (dev.slaveAddress || 1) : 1,
                pvAddr: pidA[0] || 0, svAddr: pidA[1] || 1, outAddr: pidA[2] || 2,
                pAddr: pidA[3] || 3, iAddr: pidA[4] || 4, dAddr: pidA[5] || 5,
                decimals: decimals,
                scaleFactor: Math.pow(10, decimals)
            };
        },

        _renderPidGrid(loading) {
            var container = document.getElementById('pid-data-grid');
            if (!container) return;
            if (loading) {
                container.innerHTML = '<div class="pid-loading-placeholder" style="text-align:center;padding:30px;color:#999;">'
                    + '<div style="font-size:14px;margin-bottom:10px;">加载中...</div>'
                    + '<div style="font-size:12px;">正在获取 PID 数据</div></div>';
                return;
            }
            var p = this._getPidParams();
            var v = this._pidValues || {};
            var sf = p.scaleFactor;
            var fmtVal = function(raw, dec) {
                if (raw === undefined || raw === null) return '--';
                return (raw / sf).toFixed(dec);
            };
            var fmtPct = function(raw) {
                if (raw === undefined || raw === null) return '--';
                return (raw / sf).toFixed(p.decimals) + '%';
            };
            var cards = [
                { key: 'pv', label: i18n.t('modbus-ctrl-pid-pv-label') || '过程值 PV', value: fmtVal(v.pv, p.decimals), editable: false, big: true },
                { key: 'sv', label: i18n.t('modbus-ctrl-pid-sv-label') || '设定值 SV', value: fmtVal(v.sv, p.decimals), editable: true },
                { key: 'out', label: i18n.t('modbus-ctrl-pid-out-label') || '输出 %', value: fmtPct(v.out), editable: false },
                { key: 'p', label: i18n.t('modbus-ctrl-pid-p-label') || 'P 比例', value: fmtVal(v.p, p.decimals), editable: true },
                { key: 'i', label: i18n.t('modbus-ctrl-pid-i-label') || 'I 积分', value: fmtVal(v.i, p.decimals), editable: true },
                { key: 'd', label: i18n.t('modbus-ctrl-pid-d-label') || 'D 微分', value: fmtVal(v.d, p.decimals), editable: true }
            ];
            var html = '';
            for (var ci = 0; ci < cards.length; ci++) {
                var c = cards[ci];
                var cls = 'pid-card' + (c.big ? ' pid-pv-card' : '') + (c.editable ? ' pid-editable' : '');
                html += '<div class="' + cls + '">';
                html += '<div class="pid-card-label">' + c.label + '</div>';
                if (c.editable) {
                    var rawVal = (v[c.key] !== undefined && v[c.key] !== null) ? (v[c.key] / sf).toFixed(p.decimals) : '';
                    html += '<div class="pid-card-value">' + c.value + '</div>';
                    html += '<div class="pid-edit-row">';
                    html += '<input type="number" class="pid-input" step="' + (1 / sf) + '" value="' + rawVal + '" data-param="' + c.key + '">';
                    html += '<button type="button" class="fb-btn fb-btn-sm fb-btn-success pid-set-btn pid-set-btn-wide" data-param="' + c.key + '">'
                          + (i18n.t('modbus-ctrl-pid-set') || '设置') + '</button>';
                    html += '</div>';
                } else {
                    html += '<div class="pid-card-value">' + c.value + '</div>';
                }
                html += '</div>';
            }
            container.innerHTML = html;
        },

        refreshPidStatus() {
            var p = this._getPidParams();
            if (!p.slaveAddress) return;
            var addrs = [p.pvAddr, p.svAddr, p.outAddr, p.pAddr, p.iAddr, p.dAddr];
            var minAddr = Math.min.apply(null, addrs);
            var maxAddr = Math.max.apply(null, addrs);
            var quantity = maxAddr - minAddr + 1;
            if (quantity > 125) {
                Notification.warning('PID 寄存器地址跨度过大 (>' + 125 + ')');
                return;
            }
            var self = this;
            apiGetSilent('/api/modbus/register/read', {
                slaveAddress: p.slaveAddress, startAddress: minAddr, quantity: quantity, functionCode: 3
            }).then(function(res) {
                if (res && res.success && res.data && res.data.values) {
                    self._pidAutoRefreshErrors = 0;
                    var vals = res.data.values;
                    self._pidValues = {
                        pv: vals[p.pvAddr - minAddr], sv: vals[p.svAddr - minAddr],
                        out: vals[p.outAddr - minAddr], p: vals[p.pAddr - minAddr],
                        i: vals[p.iAddr - minAddr], d: vals[p.dAddr - minAddr]
                    };
                    var cacheKey = 'dev_' + self._activeDeviceIdx;
                    self._devicePidCache[cacheKey] = JSON.parse(JSON.stringify(self._pidValues));
                    self._renderPidGrid();
                } else {
                    self._pidAutoRefreshErrors++;
                    self._stopPidAutoRefreshOnErrors();
                }
                if (res && res.debug) {
                    self._appendDebugLog(res.debug.tx ? { tx: res.debug.tx, rx: res.debug.rx } : null, 'PID Read');
                }
            }).catch(function() {
                self._pidAutoRefreshErrors++;
                self._stopPidAutoRefreshOnErrors();
                if (!self._pidValues || Object.keys(self._pidValues).length === 0) {
                    self._pidValues = { pv: 0, sv: 0, out: 0, p: 0, i: 0, d: 0 };
                }
                self._renderPidGrid();
            });
        },

        setPidRegister(paramName, rawValue) {
            var p = this._getPidParams();
            var addrMap = { sv: p.svAddr, p: p.pAddr, i: p.iAddr, d: p.dAddr };
            var addr = addrMap[paramName];
            if (addr === undefined) return;
            var self = this;
            apiPost('/api/modbus/register/write', {
                slaveAddress: p.slaveAddress, registerAddress: addr, value: rawValue
            }).then(function(res) {
                if (res && res.success) {
                    self._pidValues[paramName] = rawValue;
                    var cacheKey = 'dev_' + self._activeDeviceIdx;
                    self._devicePidCache[cacheKey] = JSON.parse(JSON.stringify(self._pidValues));
                    self._renderPidGrid();
                }
                if (res && res.debug) {
                    self._appendDebugLog(res.debug.tx ? { tx: res.debug.tx, rx: res.debug.rx } : null, 'PID Write ' + paramName);
                }
            }).catch(function() {});
        },

        _onPidInputChange(paramName, displayValue) {
            var p = this._getPidParams();
            var val = parseFloat(displayValue);
            if (isNaN(val)) return;
            var rawValue = Math.round(val * p.scaleFactor);
            this.setPidRegister(paramName, rawValue);
        },

        _stopPidAutoRefresh() {
            if (this._pidAutoRefreshTimer) {
                clearInterval(this._pidAutoRefreshTimer);
                this._pidAutoRefreshTimer = null;
            }
        },

        togglePidAutoRefresh() {
            var checked = document.getElementById('modbus-ctrl-pid-auto-refresh')?.checked;
            if (checked) {
                this._pidAutoRefreshErrors = 0;
                this.refreshPidStatus();
                var self = this;
                this._pidAutoRefreshTimer = setInterval(function() { self.refreshPidStatus(); }, 8000);
            } else {
                this._stopPidAutoRefresh();
            }
        }
    });
})();
