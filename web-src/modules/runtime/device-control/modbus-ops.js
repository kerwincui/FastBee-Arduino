/**
 * device-control/modbus-ops.js — SSE 连接管理、数据解析、增量 UI 更新
 */
(function() {
    'use strict';

    Object.assign(AppState, {

        // ============ SSE 连接管理 ============
        _setupSSE: function(keepConnection) {
            this._closeSSE(keepConnection);
            // 使用全局 SSE 连接而非创建独立的 EventSource
            var self = this;
            var generation = this._dcLifecycleGeneration;
            this._dcLastSensorEventAt = Date.now();
            this._dcLastModbusEventAt = Date.now();
            // 保存引用以便清理
            this._sseModbusHandler = function(e) {
                if (!self._dcIsLifecycleActive(generation)) return;
                try {
                    var data = JSON.parse(e.data);
                    self._handleSSEModbusData(data);
                    if (Array.isArray(data) && data.length) self._dcLastModbusEventAt = Date.now();
                } catch (err) {
                    console.warn('[SSE] 数据解析失败:', err);
                    self._dcScheduleSnapshotRecovery('数据流解析失败，正在恢复');
                }
            };
            this._sseSensorHandler = function(e) {
                if (!self._dcIsLifecycleActive(generation)) return;
                try {
                    var data = JSON.parse(e.data);
                    self._handleSSESensorData(data);
                    if (data && data.key) self._dcLastSensorEventAt = Date.now();
                } catch (err) {
                    console.warn('[SSE] 传感器数据解析失败:', err);
                    self._dcScheduleSnapshotRecovery('传感器数据解析失败，正在恢复');
                }
            };

            AppState.onSSEEvent('modbus-data', this._sseModbusHandler);
            AppState.onSSEEvent('sensor-data', this._sseSensorHandler);
            this._sseResyncHandler = function() {
                if (self._dcIsLifecycleActive(generation)) self._dcScheduleSnapshotRecovery('连接恢复中，正在核对最新状态');
            };
            AppState.onSSEEvent('resync', this._sseResyncHandler);
            var watch = function() {
                if (!self._dcIsLifecycleActive(generation)) return;
                var now = Date.now();
                var sampleNow = self._dcSampleNow();
                document.querySelectorAll('.dc-monitor-card[data-sample-expires-at]').forEach(function(card) {
                    var expires = Number(card.dataset.sampleExpiresAt);
                    if (!Number.isFinite(expires) || sampleNow >= expires) {
                        self._dcUpdateMonitorCard(card, {value: null, valid: false, quality: 'stale'});
                    }
                });
                var hasLocalSensors = document.querySelector('.dc-monitor-card[data-id^="ai_"]');
                var hasModbus = self._modbusStatus && (self._modbusStatus.tasks || []).length;
                if ((hasLocalSensors && now - self._dcLastSensorEventAt > 120000) ||
                    (hasModbus && now - self._dcLastModbusEventAt > 120000)) {
                    self._dcScheduleSnapshotRecovery('数据超过有效时限，正在恢复');
                }
                self._dcAutoRefreshTimers.dataWatch = setTimeout(watch, 1000);
            };
            this._dcAutoRefreshTimers = this._dcAutoRefreshTimers || {};
            this._dcAutoRefreshTimers.dataWatch = setTimeout(watch, 1000);
            AppState.connectSSE();
        },

        _closeSSE: function(keepConnection) {
            if (this._sseResyncHandler) {
                AppState.offSSEEvent('resync', this._sseResyncHandler);
                this._sseResyncHandler = null;
            }
            if (this._dcAutoRefreshTimers && this._dcAutoRefreshTimers.dataWatch) {
                clearTimeout(this._dcAutoRefreshTimers.dataWatch);
                delete this._dcAutoRefreshTimers.dataWatch;
            }
            if (this._sseModbusHandler) {
                AppState.offSSEEvent('modbus-data', this._sseModbusHandler);
                this._sseModbusHandler = null;
            }
            if (this._sseSensorHandler) {
                AppState.offSSEEvent('sensor-data', this._sseSensorHandler);
                this._sseSensorHandler = null;
            }
            if (!keepConnection) AppState.disconnectSSE();
            if (this._sseDebounceTimer) {
                clearTimeout(this._sseDebounceTimer);
                this._sseDebounceTimer = null;
            }
        },

        /**
         * 处理 SSE 传感器数据更新
         * data 格式: {key: "dht_01_temperature", label: "温度", value: "25.3", unit: "°C"}
         */
        _handleSSESensorData: function(data) {
            if (!this._dcIsLifecycleActive(this._dcLifecycleGeneration)) return;
            if (!data || !data.key) return;
            data = Object.assign({}, data, {_dcSampleReceivedAt: this._dcSampleNow()});
            // 查找对应的监测卡片并更新数值
            var card = document.querySelector('.dc-monitor-card[data-id="' + data.key + '"]');
            if (card) {
                var unitEl = card.querySelector('.dc-monitor-unit');
                this._dcUpdateMonitorCard(card, data);
                if (unitEl && typeof data.unit === 'string') unitEl.textContent = data.unit;
            }
            var rules = this._dcControlRules(this._controlData || {});
            for (var i = 0; i < rules.length; i++) {
                var sensors = rules[i].sensors || [];
                for (var j = 0; j < sensors.length; j++) if (sensors[j].key === data.key) {
                    Object.assign(sensors[j], data, {valid: data.valid, valueValid: data.valueValid, quality: data.quality,
                        ageMs: data.ageMs, maxAgeMs: data.maxAgeMs});
                }
            }
        },

        // ============ 根据 slaveAddress 查找设备索引 ============
        _findDevIdxBySlaveAddress: function(slaveAddress) {
            var devices = this._modbusDevices || [];
            for (var i = 0; i < devices.length; i++) {
                if (devices[i].slaveAddress === slaveAddress) {
                    return i;
                }
            }
            return -1;
        },

        // ============ 解析 sensorId 并提取设备信息 ============
        // 支持格式: sensorId_chN (多通道), sensorId (单通道)
        _parseSensorId: function(sensorId) {
            if (!sensorId) return null;
            var devices = this._modbusDevices || [];

            // 多通道格式: sensorId_chN (N为通道号)
            for (var i = 0; i < devices.length; i++) {
                if (!devices[i].sensorId) continue;
                var prefix = devices[i].sensorId + '_ch';
                if (sensorId.indexOf(prefix) === 0 && sensorId.length > prefix.length) {
                    var numPart = sensorId.substring(prefix.length);
                    if (numPart.length > 0 && numPart.length <= 3 && /^\d+$/.test(numPart)) {
                        var channel = parseInt(numPart, 10);
                        if (channel >= 0) {
                            return {
                                type: devices[i].deviceType || 'relay',
                                devIdx: i,
                                channel: channel,
                                slaveAddress: devices[i].slaveAddress
                            };
                        }
                    }
                }
            }

            // 精确匹配设备 sensorId (单通道设备)
            for (var j = 0; j < devices.length; j++) {
                if (devices[j].sensorId && devices[j].sensorId === sensorId) {
                    return {
                        type: devices[j].deviceType || 'relay',
                        devIdx: j,
                        channel: 0,
                        slaveAddress: devices[j].slaveAddress
                    };
                }
            }

            return null;
        },

        // ============ 直接更新继电器 UI ============
        _updateCoilUIFromSSE: function(devIdx, channel, value) {
            var dev = this._modbusDevices[devIdx];
            if (!dev) return false;

            var ncMode = dev.ncMode || false;
            // 修复：SSE 推送的 value 为字符串类型，"0" 在 JS 中是 truthy，
            // 必须先转为数字再判断布尔值
            var known = (typeof value === 'number' || typeof value === 'string') &&
                String(value).trim() !== '' && Number.isFinite(Number(value));
            var boolVal = known ? Number(value) !== 0 : null;
            var isOn = known ? (ncMode ? !boolVal : boolVal) : null;
            var onText = 'ON';
            var offText = 'OFF';

            // 更新本地状态缓存
            var states = this._dcCoilStates[devIdx] || [];
            if (channel >= 0 && channel < (dev.channelCount || 8)) {
                states[channel] = boolVal;
                this._dcCoilStates[devIdx] = states;
            }

            // 直接更新 DOM 元素
            var card = document.querySelector('.dc-coil-card[data-dev="' + devIdx + '"][data-ch="' + channel + '"]');
            if (!card) return false;

            // 更新样式类
            card.classList.remove('dc-coil-on', 'dc-coil-off', 'dc-coil-unknown');
            card.classList.add(isOn === null ? 'dc-coil-unknown' : isOn ? 'dc-coil-on' : 'dc-coil-off');

            // 更新状态文本
            var stEl = card.querySelector('.dc-coil-st');
            if (stEl) {
                stEl.textContent = isOn === null ? '--' : isOn ? onText : offText;
            }

            return true;
        },

        // ============ 直接更新 PWM UI ============
        _updatePwmUIFromSSE: function(devIdx, channel, value) {
            var dev = this._modbusDevices[devIdx];
            if (!dev) return false;

            var resolution = dev.pwmResolution || 8;
            var maxValue = (1 << resolution) - 1;
            var numValue = parseInt(value, 10);
            if (isNaN(numValue)) numValue = 0;
            numValue = Math.max(0, Math.min(numValue, maxValue));

            // 更新本地状态缓存
            var states = this._dcPwmStates[devIdx] || [];
            if (channel >= 0) {
                states[channel] = numValue;
                this._dcPwmStates[devIdx] = states;
            }

            // 直接更新 DOM 元素
            var grid = document.getElementById('dc-pwm-grid-' + devIdx);
            if (!grid) return false;

            var slider = grid.querySelector('.dc-pwm-slider[data-ch="' + channel + '"]');
            if (slider) slider.value = numValue;

            var numInput = grid.querySelector('.dc-pwm-num[data-ch="' + channel + '"]');
            if (numInput) numInput.value = numValue;

            var pctSpan = grid.querySelector('.dc-pwm-pct[data-ch="' + channel + '"]');
            if (pctSpan) {
                var pct = maxValue > 0 ? Math.round(numValue / maxValue * 100) : 0;
                pctSpan.textContent = pct + '%';
            }

            return true;
        },

        // ============ 直接更新 PID UI ============
        _updatePidUIFromSSE: function(devIdx, paramName, value) {
            var dev = this._modbusDevices[devIdx];
            if (!dev) return false;

            var decimals = dev.pidDecimals || 1;
            var scaleFactor = Math.pow(10, decimals);
            var numValue = parseFloat(value);
            if (isNaN(numValue)) numValue = 0;

            // SSE 数据已经是缩放后的值，需要转回原始值存储
            var rawValue = Math.round(numValue * scaleFactor);

            // 更新本地状态缓存
            var values = this._dcPidValues[devIdx] || {};
            values[paramName] = rawValue;
            this._dcPidValues[devIdx] = values;

            // 直接更新 DOM 元素
            var grid = document.getElementById('dc-pid-grid-' + devIdx);
            if (!grid) return false;

            // 找到对应的卡片并更新显示值
            var cards = grid.querySelectorAll('.dc-pid-card');
            for (var i = 0; i < cards.length; i++) {
                var card = cards[i];
                var labelEl = card.querySelector('.dc-pid-label');
                if (!labelEl) continue;

                // 根据标签文本判断是否匹配
                var labelText = (labelEl.textContent || '').toLowerCase();
                var paramLabels = {
                    'pv': '过程值 PV',
                    'sv': '设定值 SV',
                    'out': '输出 %',
                    'p': 'P 比例',
                    'i': 'I 积分',
                    'd': 'D 微分'
                };

                var expectedLabel = (paramLabels[paramName] || paramName).toLowerCase();
                if (labelText.indexOf(expectedLabel) !== -1 || labelText.indexOf(paramName) !== -1) {
                    // 更新显示值
                    var valueEl = card.querySelector('.dc-pid-value');
                    if (valueEl) {
                        valueEl.textContent = numValue.toFixed(decimals);
                    }
                    // 更新输入框值（如果是可编辑参数）
                    var inputEl = card.querySelector('.dc-pid-input');
                    if (inputEl) {
                        inputEl.value = numValue.toFixed(decimals);
                    }
                    break;
                }
            }

            return true;
        },

        // ============ Motor SSE UI 更新 ============
        _dcUpdateMotorUIFromSSE: function(devIdx, value) {
            var dev = this._modbusDevices[devIdx];
            if (!dev) return false;
            var elRun = document.getElementById('dc-motor-run-' + devIdx);
            var elDir = document.getElementById('dc-motor-dir-' + devIdx);
            if (!elRun && !elDir) return false;
            // These SSE values are command reports, not measured motion feedback.
            this._dcUpdateMotorRunUI(devIdx, {}, dev.motorDecimals || 0);
            return true;
        },

        // ============ SSE Modbus 数据处理主入口 ============
        _handleSSEModbusData: function(data) {
            if (!this._dcIsLifecycleActive(this._dcLifecycleGeneration)) return;
            var self = this;
            var tasks = this._modbusStatus && this._modbusStatus.tasks ? this._modbusStatus.tasks : [];
            var hasMonitorUpdate = false;
            var needsFullRefresh = false;

            if (!Array.isArray(data) || data.length === 0) {
                return;
            }

            for (var i = 0; i < data.length; i++) {
                var item = data[i] || {};
                var sensorId = item.sensorId || item.id;
                var value = item.value;
                var matched = false;

                if (!sensorId) {
                    continue;
                }

                // 尝试解析 sensorId 是否为 Modbus 控制设备
                var parsedInfo = this._parseSensorId(sensorId);
                if (parsedInfo) {
                    var devIdx = parsedInfo.devIdx;
                    if (devIdx >= 0) {
                        // SSE 收到数据说明设备在线，自动恢复离线状态
                        var sampleValid = this._dcMonitorValue(item) !== '--';
                        if (sampleValid && self._dcDeviceOnline && self._dcDeviceOnline[devIdx] !== true) {
                            self._dcUpdatePanelOnlineState(devIdx, true);
                        }
                        var deviceType = parsedInfo.type;
                        if (deviceType === 'relay') {
                            if (this._updateCoilUIFromSSE(devIdx, parsedInfo.channel, sampleValid ? value : null)) {
                                matched = true;
                                continue;
                            }
                        } else if (deviceType === 'pwm') {
                            if (this._updatePwmUIFromSSE(devIdx, parsedInfo.channel, value)) {
                                matched = true;
                                continue;
                            }
                        } else if (deviceType === 'pid') {
                            var pidParams = ['pv', 'sv', 'out', 'p', 'i', 'd'];
                            var paramName = pidParams[parsedInfo.channel] || 'pv';
                            if (this._updatePidUIFromSSE(devIdx, paramName, value)) {
                                matched = true;
                                continue;
                            }
                        } else if (deviceType === 'motor') {
                            if (this._dcUpdateMotorUIFromSSE(devIdx, value)) {
                                matched = true;
                                continue;
                            }
                        }
                    }
                    // 解析成功但未找到对应设备，触发全量刷新
                    if (!matched) {
                        needsFullRefresh = true;
                        continue;
                    }
                }

                // 原有逻辑：匹配监测数据卡片
                for (var ti = 0; ti < tasks.length; ti++) {
                    var task = tasks[ti];
                    var mappings = task && task.mappings ? task.mappings : [];
                    for (var mi = 0; mi < mappings.length; mi++) {
                        var mapping = mappings[mi];
                        if (mapping && mapping.sensorId === sensorId) {
                            matched = true;
                            // SSE values are already decoded/scaled. Keep them separate
                            // from raw register words (especially signed/float32 mappings).
                            task._dcMappedValues = task._dcMappedValues || {};
                            task._dcMappedValues[sensorId] = item;
                            break;
                        }
                    }
                    if (matched) {
                        break;
                    }
                }

                var valueEl = document.querySelector('.dc-monitor-card[data-id="' + sensorId + '"] .dc-monitor-value');
                if (valueEl) {
                    this._dcUpdateMonitorCard(valueEl.closest('.dc-monitor-card'), item);
                    hasMonitorUpdate = true;
                    matched = true;
                }

                // 仅当数据既不匹配监测卡片，也不匹配 coil/pwm/pid 时才触发全量刷新
                if (!matched) {
                    needsFullRefresh = true;
                }
            }

            if (needsFullRefresh) {
                this._dcAutoRefreshTimers = this._dcAutoRefreshTimers || {};
                if (this._dcAutoRefreshTimers.sseDebounce) {
                    clearTimeout(this._dcAutoRefreshTimers.sseDebounce);
                }
                this._dcAutoRefreshTimers.sseDebounce = setTimeout(function() {
                    self._dcAutoRefreshTimers.sseDebounce = null;
                    if (self.currentPage !== 'device-control') {
                        return;
                    }
                    self._dcInitModbusDeviceStates();
                }, 100);
            }

            if (!needsFullRefresh && !hasMonitorUpdate) {
                console.warn('[SSE] 未匹配到可更新的设备数据');
            }
        }
    });
})();
