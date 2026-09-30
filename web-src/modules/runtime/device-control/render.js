/**
 * device-control/render.js — 渲染控制面板、监测数据、设备面板、各类控件
 */
(function() {
    'use strict';

    Object.assign(AppState, {

        // ============ 渲染控制面板 ============
        _renderControlPanel: function(data) {
            data = data || {};
            var html = '';
            try {
                html += this._renderControlSection(data);
            } catch (e) {
                console.error('[device-control] Error in _renderControlPanel:', e);
                html = '<div class="dc-empty u-text-danger">渲染错误: ' + escapeHtml(e.message || e) + '</div>';
            }
            return html;
        },

        _shouldShowModbusHealthBanner: function(health) {
            if (!health) return false;
            var warnings = Array.isArray(health.warnings) ? health.warnings : [];
            var riskLevel = String(health.riskLevel || 'low').toLowerCase();
            return riskLevel !== 'low' || warnings.length > 0 || Number(health.enabledTaskCount || 0) > 0;
        },

        _getDcRiskMeta: function(level) {
            switch (String(level || 'low').toLowerCase()) {
                case 'high': return { text: '高', className: 'modbus-risk-high' };
                case 'medium': return { text: '中', className: 'modbus-risk-medium' };
                default: return { text: '低', className: 'modbus-risk-low' };
            }
        },

        _formatDcAge: function(ageSec) {
            var age = Number(ageSec || 0);
            if (!age) return '--';
            if (age < 60) return age + 's';
            if (age < 3600) return Math.floor(age / 60) + 'm';
            return Math.floor(age / 3600) + 'h';
        },

        _formatDcPercent: function(value) {
            var num = Number(value || 0);
            if (!isFinite(num)) num = 0;
            return (Math.round(num * 10) / 10) + '%';
        },

        _renderModbusHealthBanner: function() {
            var health = this._modbusStatus && this._modbusStatus.health;
            if (!this._shouldShowModbusHealthBanner(health)) return '';
            var riskMeta = this._getDcRiskMeta(health.riskLevel);
            var warnings = Array.isArray(health.warnings) ? health.warnings.slice(0, 3) : [];
            var html = '<div class="dc-risk-banner" data-dc-sort-key="health">' +
                '<div class="dc-risk-title">Modbus 轮询健康</div>' +
                '<div class="dc-risk-main">' +
                '<span class="dc-risk-label">风险等级</span>' +
                '<span class="modbus-risk-badge ' + riskMeta.className + '">' + escapeHtml(riskMeta.text) + '</span>' +
                '</div>' +
                '<div class="dc-risk-metrics">' +
                '<span class="dc-risk-metric"><span class="dc-risk-metric-label">启用任务</span><strong>' + escapeHtml(health.enabledTaskCount ?? 0) + '</strong></span>' +
                '<span class="dc-risk-metric"><span class="dc-risk-metric-label">最小间隔</span><strong>' + escapeHtml(health.minPollInterval ? (health.minPollInterval + 's') : '--') + '</strong></span>' +
                '<span class="dc-risk-metric"><span class="dc-risk-metric-label">最近轮询</span><strong>' + escapeHtml(this._formatDcAge(health.lastPollAgeSec)) + '</strong></span>' +
                '<span class="dc-risk-metric"><span class="dc-risk-metric-label">超时率</span><strong>' + escapeHtml(this._formatDcPercent(health.timeoutRate)) + '</strong></span>' +
                '</div>';
            if (warnings.length > 0) {
                var warnHtml = '<div class="dc-risk-warnings">';
                for (var wi = 0; wi < warnings.length; wi++) {
                    warnHtml += '<div class="dc-risk-warning"><span>' + escapeHtml(warnings[wi] || '') + '</span></div>';
                }
                html += warnHtml + '</div>';
            }
            html += '<div class="dc-resize-handle" title="拖拽调整大小"></div></div>';
            return html;
        },

        _renderMonitorSection: function(data) {
            return this._renderModbusHealthBanner() + this._renderMonitorDataCard(data);
        },

        _renderMonitorDataCard: function(data) {
            var monitorGroups = this._buildMonitorGroupsFromModbus();
            var sensorCards = [];
            // 按传感器唯一键(periphId_dataField)去重：多条规则可能读取同一传感器（如“数据采集上报”
            // 与“数码管轮显温湿度”都读 dht11 温湿度），不去重会导致监测卡片重复显示。
            // SSE 实时更新(modbus-ops.js _handleSSESensorData)按 data-id 定位唯一卡片，去重后更新才正确。
            var seenSensorKeys = {};
            // 从 sensor 规则的 sensors[] 数组构建监测卡片
            var sensorRules = this._dcControlRules(data);
            for (var i = 0; i < sensorRules.length; i++) {
                var rule = sensorRules[i];
                if (rule.sensors && rule.sensors.length > 0) {
                    for (var j = 0; j < rule.sensors.length; j++) {
                        var s = rule.sensors[j];
                        var cardKey = s.key;
                        if (seenSensorKeys[cardKey]) {
                            // 同键卡片已存在：若新条目带有真实值而旧条目仍为占位 '--'，则刷新数值
                            var existingCard = seenSensorKeys[cardKey];
                            if (this._dcMonitorValue(existingCard) === '--' && this._dcMonitorValue(s) !== '--') {
                                Object.assign(existingCard, {value: s.value, valid: s.valid, valueValid: s.valueValid, quality: s.quality});
                            }
                            continue;
                        }
                        var card = {
                            id: s.key,
                            name: s.label || s.key,
                            value: s.value, valid: s.valid, valueValid: s.valueValid, quality: s.quality,
                            unit: s.unit || this._getMonitorUnit(s.label || '')
                        };
                        seenSensorKeys[cardKey] = card;
                        sensorCards.push(card);
                    }
                }
            }
            // Execution-rule names are actions, not sampled measurements. Only
            // cache-backed protocol mappings and sensor channels become monitor cards.
            var hasMonitor = monitorGroups.length > 0 || sensorCards.length > 0;
            if (!hasMonitor) return '';
            var html = '<div class="dc-monitor-grid" data-dc-sort-key="monitor-data">';
            if (monitorGroups.length > 0) {
                for (var i = 0; i < monitorGroups.length; i++) {
                    var group = monitorGroups[i];
                    for (var j = 0; j < group.items.length; j++) html += this._renderMonitorCard(group.items[j], group.label);
                }
            }
            // 传感器卡片始终显示（与 Modbus 并列）
            for (var i = 0; i < sensorCards.length; i++) html += this._renderMonitorCard(sensorCards[i]);
            html += '<div class="dc-resize-handle" title="拖拽调整大小"></div></div>';
            return html;
        },

        _dcMonitorValue: function(sample) {
            if (!sample || sample.valid === false || sample.valueValid === false ||
                (sample.quality && !['valid', 'good', 'ok'].includes(String(sample.quality).toLowerCase()))) return '--';
            var value = sample.value;
            return (typeof value !== 'number' && typeof value !== 'string') || String(value).trim() === '' ||
                !Number.isFinite(Number(value)) ? '--' : String(value);
        },

        _dcMonitorPresentation: function(sample, id) {
            var measured = this._dcMonitorValue(sample);
            var valid = measured !== '--';
            var soilHumidity = id === 'ai_soil_humidity_soil_humidity' || id === 'ai_soil_humidity';
            return {value: !valid && soilHumidity ? '0' : measured, valid: valid,
                placeholder: !valid && soilHumidity,
                quality: sample.quality || (valid ? 'valid' : 'unavailable')};
        },

        _dcUpdateMonitorCard: function(card, sample) {
            var presentation = this._dcMonitorPresentation(sample, card.dataset.id);
            var value = card.querySelector('.dc-monitor-value');
            if (value) value.textContent = presentation.value;
            card.dataset.measurementValid = String(presentation.valid);
            card.dataset.valuePlaceholder = String(presentation.placeholder);
            card.dataset.quality = presentation.quality;
            var status = card.querySelector('.dc-monitor-status');
            if (status) status.hidden = !presentation.placeholder;
        },

        _dcMappedRegisterValue: function(task, mapping) {
            var sensorId = mapping.sensorId;
            if (task._dcMappedValues && Object.prototype.hasOwnProperty.call(task._dcMappedValues, sensorId)) {
                return this._dcMonitorValue(task._dcMappedValues[sensorId]);
            }
            var cache = task.cachedData;
            if (!cache || cache.valid === false || task.status && task.status !== 'ok' ||
                Number(task.lastError || cache.lastError || 0) !== 0) return '--';
            var words = cache.values, offset = Number(mapping.regOffset ?? 0), type = Number(mapping.dataType ?? 0);
            var count = type >= 2 && type <= 4 ? 2 : 1;
            if (!Array.isArray(words) || !Number.isInteger(offset) || offset < 0 || !Number.isInteger(type) || type < 0 || type > 4 ||
                offset + count > words.length) return '--';
            var raw = words[offset];
            if (!Number.isInteger(raw) || raw < 0 || raw > 65535) return '--';
            if (type === 1 && raw > 32767) raw -= 65536;
            if (count === 2) {
                var low = words[offset + 1];
                if (!Number.isInteger(low) || low < 0 || low > 65535) return '--';
                raw = raw * 65536 + low;
                if (type === 3 && raw > 2147483647) raw -= 4294967296;
                if (type === 4) {
                    var view = new DataView(new ArrayBuffer(4));
                    view.setUint32(0, raw); raw = view.getFloat32(0);
                }
            }
            var scale = mapping.scaleFactor === undefined ? 1 : mapping.scaleFactor;
            if (scale === null || scale === '' || !Number.isFinite(Number(scale))) return '--';
            var value = raw * Number(scale);
            return Number.isFinite(value) ? value.toFixed(Math.max(0, Math.min(6, Number(mapping.decimalPlaces) || 0))) : '--';
        },

        _buildMonitorGroupsFromModbus: function() {
            var groups = [];
            if (!this._modbusStatus || !this._modbusStatus.tasks) return groups;
            var tasks = this._modbusStatus.tasks;
            for (var i = 0; i < tasks.length; i++) {
                var task = tasks[i];
                if (!task.enabled || !task.mappings || task.mappings.length === 0) continue;
                var group = { label: task.name || task.label || ('设备 ' + task.slaveAddress), items: [] };
                for (var j = 0; j < task.mappings.length; j++) {
                    var mapping = task.mappings[j];
                    if (!mapping.sensorId) continue;
                    var item = { id: mapping.sensorId, name: mapping.sensorId,
                        value: this._dcMappedRegisterValue(task, mapping),
                        unit: mapping.unit || this._getMonitorUnit(mapping.sensorId) };
                    group.items.push(item);
                }
                if (group.items.length > 0) groups.push(group);
            }
            return groups;
        },

        _renderMonitorCard: function(item, deviceLabel) {
            var name = escapeHtml(item.name || 'Unknown');
            var presentation = this._dcMonitorPresentation(item, item.id);
            var value = escapeHtml(presentation.value);
            var unit = escapeHtml(item.unit || '');
            var toneClass = this._getMonitorToneClass(item.name || '');
            var icon = this._getMonitorIcon(item.name || '');
            var html = '<div class="dc-monitor-card" data-id="' + escapeHtml(item.id) + '" data-measurement-valid="' + presentation.valid + '" data-value-placeholder="' + presentation.placeholder + '" data-quality="' + escapeHtml(presentation.quality) + '">';
            html += '<div class="dc-monitor-top"><span class="dc-monitor-label">' + name + '</span><span class="dc-monitor-icon ' + toneClass + '">' + icon + '</span></div>';
            if (deviceLabel) html += '<div class="dc-monitor-device">' + escapeHtml(deviceLabel) + '</div>';
            html += '<div class="dc-monitor-val-row"><span class="dc-monitor-value">' + value + '</span>';
            if (unit) html += '<span class="dc-monitor-unit">' + unit + '</span>';
            html += '</div>';
            html += '<div class="dc-monitor-status dc-monitor-device u-text-warning"' + (presentation.placeholder ? '' : ' hidden') + '>无有效数据（占位显示）</div>';
            html += '</div>';
            return html;
        },

        _getMonitorIcon: function(name) {
            var n = name.toLowerCase();
            if (n.indexOf('hum') !== -1 || n.indexOf('湿度') !== -1) return '<svg viewBox="0 0 24 24" width="16" height="16" fill="currentColor"><path d="M12 2c-5.33 8-8 12.67-8 16a8 8 0 0016 0c0-3.33-2.67-8-8-16z"/></svg>';
            if (n.indexOf('temp') !== -1 || n.indexOf('温度') !== -1) return '<svg viewBox="0 0 24 24" width="16" height="16" fill="currentColor"><path d="M15 13V5a3 3 0 00-6 0v8a5 5 0 106 0zm-3-9a1 1 0 011 1v9.17a3 3 0 11-2 0V5a1 1 0 011-1z"/></svg>';
            if (n.indexOf('pm') !== -1 || n.indexOf('颗粒') !== -1) return '<svg viewBox="0 0 24 24" width="16" height="16" fill="currentColor"><path d="M17.5 18.25a1.25 1.25 0 110-2.5 1.25 1.25 0 010 2.5zm-5-3a1.75 1.75 0 110-3.5 1.75 1.75 0 010 3.5zm-5-4a1.5 1.5 0 110-3 1.5 1.5 0 010 3zm10-3a1 1 0 110-2 1 1 0 010 2zm-3 10a1 1 0 110-2 1 1 0 010 2zm-7 2a.75.75 0 110-1.5.75.75 0 010 1.5z"/></svg>';
            if (n.indexOf('co2') !== -1 || n.indexOf('二氧化碳') !== -1) return '<svg viewBox="0 0 24 24" width="16" height="16" fill="currentColor"><path d="M12 2C6.48 2 2 6.48 2 12s4.48 10 10 10 10-4.48 10-10S17.52 2 12 2zm-1 17.93c-3.95-.49-7-3.85-7-7.93 0-.62.08-1.21.21-1.79L9 15v1c0 1.1.9 2 2 2v1.93zm6.9-2.54c-.26-.81-1-1.39-1.9-1.39h-1v-3c0-.55-.45-1-1-1H8v-2h2c.55 0 1-.45 1-1V7h2c1.1 0 2-.9 2-2v-.41c2.93 1.19 5 4.06 5 7.41 0 2.08-.8 3.97-2.1 5.39z"/></svg>';
            if (n.indexOf('noise') !== -1 || n.indexOf('噪声') !== -1 || n.indexOf('噪音') !== -1) return '<svg viewBox="0 0 24 24" width="16" height="16" fill="currentColor"><path d="M3 9v6h4l5 5V4L7 9H3zm13.5 3c0-1.77-1.02-3.29-2.5-4.03v8.05c1.48-.73 2.5-2.25 2.5-4.02zM14 3.23v2.06c2.89.86 5 3.54 5 6.71s-2.11 5.85-5 6.71v2.06c4.01-.91 7-4.49 7-8.77s-2.99-7.86-7-8.77z"/></svg>';
            if (n.indexOf('press') !== -1 || n.indexOf('气压') !== -1) return '<svg viewBox="0 0 24 24" width="16" height="16" fill="currentColor"><path d="M12 2C6.48 2 2 6.48 2 12s4.48 10 10 10 10-4.48 10-10S17.52 2 12 2zm0 18c-4.41 0-8-3.59-8-8s3.59-8 8-8 8 3.59 8 8-3.59 8-8 8zm-1-13h2v6h-2zm0 8h2v2h-2z"/></svg>';
            if (n.indexOf('lux') !== -1 || n.indexOf('light') !== -1 || n.indexOf('光照') !== -1) return '<svg viewBox="0 0 24 24" width="16" height="16" fill="currentColor"><path d="M12 7c-2.76 0-5 2.24-5 5s2.24 5 5 5 5-2.24 5-5-2.24-5-5-5zm0-5l-1.5 3h3L12 2zm0 20l1.5-3h-3L12 22zm9-10l-3-1.5v3l3-1.5zM2 12l3 1.5v-3L2 12zm15.07-7.07l-3.54.71 2.83 2.83 .71-3.54zM6.93 19.07l3.54-.71-2.83-2.83-.71 3.54zm0-14.14l.71 3.54 2.83-2.83-3.54-.71zm10.14 14.14l-.71-3.54-2.83 2.83 3.54.71z"/></svg>';
            return '<svg viewBox="0 0 24 24" width="16" height="16" fill="currentColor"><path d="M12 2a10 10 0 100 20 10 10 0 000-20zm1 15h-2v-2h2v2zm0-4h-2V7h2v6z"/></svg>';
        },

        _getMonitorToneClass: function(name) {
            var n = name.toLowerCase();
            if (n.indexOf('temp') !== -1 || n.indexOf('温度') !== -1) return 'dc-monitor-icon-tone-temp';
            if (n.indexOf('hum') !== -1 || n.indexOf('湿度') !== -1) return 'dc-monitor-icon-tone-humidity';
            if (n.indexOf('pm') !== -1 || n.indexOf('颗粒') !== -1 || n.indexOf('粉尘') !== -1) return 'dc-monitor-icon-tone-particle';
            if (n.indexOf('co2') !== -1 || n.indexOf('二氧化碳') !== -1) return 'dc-monitor-icon-tone-co2';
            if (n.indexOf('noise') !== -1 || n.indexOf('噪声') !== -1 || n.indexOf('噪音') !== -1) return 'dc-monitor-icon-tone-noise';
            if (n.indexOf('press') !== -1 || n.indexOf('气压') !== -1) return 'dc-monitor-icon-tone-pressure';
            if (n.indexOf('light') !== -1 || n.indexOf('光照') !== -1 || n.indexOf('lux') !== -1) return 'dc-monitor-icon-tone-light';
            if (n.indexOf('wind') !== -1 || n.indexOf('风') !== -1) return 'dc-monitor-icon-tone-weather';
            if (n.indexOf('rain') !== -1 || n.indexOf('雨') !== -1) return 'dc-monitor-icon-tone-weather';
            return 'dc-monitor-icon-tone-default';
        },

        _getMonitorUnit: function(name) {
            var n = name.toLowerCase();
            if (n.indexOf('temp') !== -1 || n.indexOf('温度') !== -1) return '°C';
            if (n.indexOf('hum') !== -1 || n.indexOf('湿度') !== -1) return '%RH';
            if (n.indexOf('co2') !== -1 || n.indexOf('二氧化碳') !== -1) return 'ppm';
            if (n.indexOf('noise') !== -1 || n.indexOf('噪声') !== -1 || n.indexOf('噪音') !== -1) return 'dB';
            if (n.indexOf('press') !== -1 || n.indexOf('气压') !== -1) return 'kPa';
            if (n.indexOf('lux') !== -1 || n.indexOf('light') !== -1 || n.indexOf('光照') !== -1) return 'Lux';
            if (n.indexOf('pm') !== -1 || n.indexOf('颗粒') !== -1) return 'μg/m³';
            if (n.indexOf('wind') !== -1 || n.indexOf('风') !== -1) return 'm/s';
            // 工业电量单位推断（配置 unit 优先，此处仅兜底）
            if (n.indexOf('current') !== -1 || n.indexOf('电流') !== -1) return 'A';
            if (n.indexOf('voltage') !== -1 || n.indexOf('电压') !== -1) return 'V';
            if (n.indexOf('resist') !== -1 || n.indexOf('电阻') !== -1) return 'Ω';
            if (n.indexOf('power') !== -1 || n.indexOf('功率') !== -1) return 'W';
            if (n.indexOf('freq') !== -1 || n.indexOf('频率') !== -1) return 'Hz';
            if (n.indexOf('ntc') !== -1 || n.indexOf('热敏') !== -1) return '°C';
            return '';
        },

        // ============ 渲染控制操作区 ============
        _renderControlSection: function(data) {
            var html = '';
            var configuredModbusDevices = this._modbusConfiguredDevices || [];
            var hasModbusDevices = this._modbusDevices && this._modbusDevices.length > 0;
            var modbusDeviceList = [];
            if (hasModbusDevices) {
                var typeOrder = { relay: 0, pid: 1, pwm: 2, motor: 3 };
                for (var mi = 0; mi < this._modbusDevices.length; mi++) {
                    var activeDevice = this._modbusDevices[mi];
                    if (Object.prototype.hasOwnProperty.call(typeOrder, activeDevice.deviceType || 'relay')) {
                        modbusDeviceList.push({ idx: mi, dev: activeDevice });
                    }
                }
                modbusDeviceList.sort(function(a, b) { return (typeOrder[a.dev.deviceType || 'relay'] || 0) - (typeOrder[b.dev.deviceType || 'relay'] || 0); });
            }
            var executionGroups = this._dcGroupExecutionRules(data);
            var hasAnyControls = this._dcControlRules(data).length > 0 || modbusDeviceList.length > 0 ||
                configuredModbusDevices.length > 0 || this._buildMonitorGroupsFromModbus().length > 0;
            if (hasAnyControls) {
                html += '<div class="fb-card dc-header-card"><div class="fb-card-header fb-card-header-actions-only dc-header-card-inner">';
                html += '<div class="dc-header-actions">';
                var _sidChecked = localStorage.getItem('dc_show_sid') === '1';
                html += '<label class="dc-sid-toggle"><input type="checkbox" id="dc-sid-toggle"' + (_sidChecked ? ' checked' : '') + '>\u663E\u793A\u7269\u6A21\u578B\u6807\u8BC6</label>';
                html += '<button class="dc-btn-sm dc-btn-zoom" id="dc-zoom-out-btn" title="Ctrl+-">-</button>';
                html += '<button class="dc-btn-sm dc-btn-zoom" id="dc-zoom-reset-btn" title="Ctrl+0">100%</button>';
                html += '<button class="dc-btn-sm dc-btn-zoom" id="dc-zoom-in-btn" title="Ctrl++">+</button>';
                html += '<button class="dc-btn-sm dc-btn-refresh" id="dc-refresh-btn">刷新</button>';
                html += '<button class="dc-btn-sm dc-btn-reset dc-layout-reset">重置布局</button>';
                html += '<button class="dc-btn-sm dc-btn-fullscreen" id="dc-fullscreen-btn" title="Ctrl+F">全屏</button>';
                html += '</div></div></div>';
                html += '<div class="dc-zoom-wrapper"><div class="dc-control-flow">';
                html += this._renderMonitorSection(data);
                if (configuredModbusDevices.length > 0) html += this._renderModbusDeviceInventory(configuredModbusDevices);
                executionGroups.forEach(function(group, key) {
                    html += this._renderControlGroup(group.title, group.items, key);
                }, this);
                for (var di = 0; di < modbusDeviceList.length; di++) {
                    var entry = modbusDeviceList[di];
                    html += this._renderSingleModbusPanel(entry.idx, entry.dev);
                }
                html += '</div></div>';
            } else {
                html += '<div class="dc-empty">暂无控制操作</div>';
            }
            return html;
        },

        _renderSystemGroup: function(items) {
            var title = '系统操作';
            var html = '<div class="dc-control-group dc-group-card" data-dc-sort-key="system">';
            html += this._renderDcCardHeader('dc-card-badge--system', 'SYS', title);
            html += '<div class="dc-sys-grid">';
            for (var i = 0; i < items.length; i++) {
                var item = items[i];
                var setAttr = item && item.hasSetMode ? ' data-has-set-mode="true"' : '';
                html += '<button class="dc-ctrl-btn dc-sys-card" data-id="' + escapeHtml(item.id) + '" data-system="true" data-name="' + escapeHtml(item.name) + '"' + setAttr + '><div class="dc-sys-card-name">' + escapeHtml(item.name) + '</div></button>';
            }
            html += '</div><div class="dc-resize-handle" title="\u62d6\u62fd\u8c03\u6574\u5927\u5c0f"></div></div>';
            return html;
        },

        _renderGpioGroup: function(items) {
            var title = 'GPIO 控制';
            var html = '<div class="dc-control-group dc-group-card" data-dc-sort-key="gpio">';
            html += this._renderDcCardHeader('dc-card-badge--gpio', 'GPIO', title);
            html += '<div class="dc-gpio-list">';
            for (var i = 0; i < items.length; i++) {
                var item = items[i];
                var setAttr = item && item.hasSetMode ? ' data-has-set-mode="true"' : '';
                html += '<div class="dc-gpio-row"><span class="dc-gpio-name">' + escapeHtml(item.name) + '</span>';
                html += '<button class="dc-ctrl-btn dc-gpio" data-id="' + escapeHtml(item.id) + '" data-name="' + escapeHtml(item.name) + '"' + setAttr + '>执行</button>';
                html += '</div>';
            }
            html += '</div><div class="dc-resize-handle" title="\u62d6\u62fd\u8c03\u6574\u5927\u5c0f"></div></div>';
            return html;
        },

        _renderModbusDevicePanels: function(deviceList) {
            var html = '';
            for (var i = 0; i < (deviceList || []).length; i++) html += this._renderSingleModbusPanel(deviceList[i].idx, deviceList[i].dev);
            return html;
        },

        _renderModbusDeviceInventory: function(devices) {
            var list = devices || [];
            var enabledCount = list.filter(function(device) { return device && device.enabled !== false; }).length;
            var typeLabelMap = { relay: '继电器', pwm: 'PWM', pid: 'PID', motor: '电机', sensor: '采集', control: '控制' };
            var html = '<div class="dc-modbus-device-inventory" data-dc-sort-key="modbus-devices">';
            html += '<div class="dc-card-header"><span class="dc-card-badge dc-card-badge--modbus">MODBUS</span>';
            html += '<span class="dc-card-title">Modbus 子设备</span>';
            html += '<span class="dc-modbus-inventory-count">' + enabledCount + '/' + list.length + ' 启用</span></div>';
            html += '<div class="dc-modbus-inventory-list" role="list">';
            for (var i = 0; i < list.length; i++) {
                var dev = list[i] || {};
                var enabled = dev.enabled !== false;
                var type = dev.deviceType || 'relay';
                var name = dev.name || ('Modbus 设备 ' + (i + 1));
                html += '<div class="dc-modbus-inventory-row" role="listitem" data-modbus-enabled="' + enabled + '">';
                html += '<div class="dc-modbus-inventory-identity"><strong>' + escapeHtml(name) + '</strong>';
                if (dev.sensorId) html += '<span>' + escapeHtml(dev.sensorId) + '</span>';
                html += '</div><div class="dc-modbus-inventory-meta">';
                html += '<span>' + escapeHtml(typeLabelMap[type] || type) + '</span>';
                html += '<span>地址 ' + escapeHtml(dev.slaveAddress || 1) + '</span>';
                html += '<span>' + escapeHtml(dev.channelCount || 1) + ' 通道</span></div>';
                html += '<span class="dc-modbus-inventory-state ' + (enabled ? 'is-enabled' : 'is-disabled') + '">' + (enabled ? '已启用' : '未启用') + '</span>';
                html += '</div>';
            }
            html += '</div><div class="dc-resize-handle" title="拖拽调整大小"></div></div>';
            return html;
        },

        _buildPlatformIds: function(dev) {
            var sid = dev.sensorId;
            if (!sid) return [];
            var dt = dev.deviceType || 'relay';
            var ids = [];
            if (dt === 'relay' || dt === 'pwm') {
                var cc = dev.channelCount || 1;
                if (cc <= 1) ids.push(sid);
                else { for (var ch = 0; ch < cc; ch++) ids.push(sid + '_ch' + ch); }
                ids.push(sid + '_all');
            } else if (dt === 'motor') {
                ['fwd', 'rev', 'stop', 'spd', 'pls'].forEach(function(s) { ids.push(sid + '_' + s); });
            } else if (dt === 'pid') {
                ['sv', 'p', 'i', 'd'].forEach(function(s) { ids.push(sid + '_' + s); });
                ids.push(sid + '_pv (R/O)');
                ids.push(sid + '_out (R/O)');
            }
            return ids;
        },

        _renderSingleModbusPanel: function(devIdx, dev) {
            var dt = dev.deviceType || 'relay';
            var typeLabelMap = { relay: '继电器', pwm: 'PWM', pid: 'PID', motor: '电机', sensor: '采集', control: '控制设备' };
            var typeLabel = typeLabelMap[dt] || dt;
            var ctrlLabelMap = { relay: '继电器控制', pwm: 'PWM 控制', pid: 'PID调速', motor: '电机控制' };
            var ctrlLabel = (dev.name || '') || (ctrlLabelMap[dt] || (typeLabel + ' 控制操作'));
            var typeClassMap = {relay: 'dc-card-badge--relay', pwm: 'dc-card-badge--pwm', pid: 'dc-card-badge--pid', motor: 'dc-card-badge--motor'};
            var configIndex = Number.isInteger(dev._configIndex) ? dev._configIndex : devIdx;
            var html = '<div class="dc-modbus-device-panel" data-dev-idx="' + devIdx + '" data-dc-sort-key="modbus-' + dt + '-' + configIndex + '">';
            var metaHtml = 'Addr: ' + (dev.slaveAddress || 1);
            if (dev.sensorId) metaHtml += ' | ID: ' + dev.sensorId;
            html += this._renderDcCardHeader(typeClassMap[dt] || 'dc-card-badge--system', typeLabel, ctrlLabel, 'dc-modbus-device-addr', metaHtml);
            if (dt === 'relay') html += this._renderDcRelayPanel(devIdx, dev);
            else if (dt === 'pwm') html += this._renderDcPwmPanel(devIdx, dev);
            else if (dt === 'pid') html += this._renderDcPidPanel(devIdx, dev);
            else if (dt === 'motor') html += this._renderDcMotorPanel(devIdx, dev);
            html += '<div class="dc-resize-handle" title="拖拽调整大小"></div></div>';
            return html;
        },

        _renderDcRelayPanel: function(devIdx, dev) {
            var channelCount = dev.channelCount || 2;
            var states = this._dcCoilStates[devIdx] || [];
            var ncMode = !!dev.ncMode;
            var html = '<div class="dc-modbus-device-body">';
            html += this._renderDcCoilGrid(devIdx, channelCount, states, ncMode);
            html += this._renderDcRelayDelaySection(devIdx, dev);
            html += '<div class="dc-modbus-actions">';
            var relayBtns = [
                { className: 'dc-coil-batch dc-btn-sm dc-btn-on', devIdx: devIdx, action: 'allOn', label: '全部开启' },
                { className: 'dc-coil-batch dc-btn-sm dc-btn-off', devIdx: devIdx, action: 'allOff', label: '全部关闭' },
                { className: 'dc-coil-batch dc-btn-sm dc-btn-toggle', devIdx: devIdx, action: 'allToggle', label: '翻转' }
            ];
            for (var ri = 0; ri < relayBtns.length; ri++) {
                html += '<span class="dc-action-stack">';
                html += this._renderDcActionButton(relayBtns[ri]);
                if (ri === 0) html += this._renderSidTag(devIdx, '_all', '1=\u5168\u5f00, 0=\u5168\u5173');
                html += '</span>';
            }
            html += this._renderDcActionButton({ className: 'dc-coil-refresh dc-btn-sm dc-btn-refresh', devIdx: devIdx, label: '刷新状态' });
            html += '</div></div>';
            return html;
        },

        _renderDcRelayDelaySection: function(devIdx, dev) {
            var channelCount = dev.channelCount || 2;
            var html = '<div class="dc-relay-delay-section"><div class="dc-delay-controls">';
            html += '<span class="dc-delay-label">延时断开</span>';
            html += '<select class="dc-delay-channel" data-dev="' + devIdx + '">';
            for (var ch = 0; ch < channelCount; ch++) html += '<option value="' + ch + '">CH' + ch + '</option>';
            html += '</select>';
            html += '<input type="number" class="dc-delay-input" min="1" max="255" value="50" data-dev="' + devIdx + '">';
            html += '<button class="dc-delay-start dc-btn-sm" data-dev="' + devIdx + '">启动</button>';
            html += '</div></div>';
            return html;
        },

        _renderDcPwmPanel: function(devIdx, dev) {
            var channelCount = dev.channelCount || 4;
            var resolution = dev.pwmResolution || 8;
            var maxValue = (1 << resolution) - 1;
            var states = this._dcPwmStates[devIdx] || new Array(channelCount).fill(0);
            var html = '<div class="dc-modbus-device-body">';
            html += this._renderDcPwmGrid(devIdx, channelCount, states, maxValue);
            html += '<div class="dc-modbus-actions">';
            var pwmBtns = [
                { className: 'dc-pwm-batch dc-btn-sm dc-btn-on', devIdx: devIdx, action: 'max', label: '全部最大' },
                { className: 'dc-pwm-batch dc-btn-sm dc-btn-off', devIdx: devIdx, action: 'off', label: '全部关闭' }
            ];
            for (var pi = 0; pi < pwmBtns.length; pi++) {
                html += '<span class="dc-action-stack">';
                html += this._renderDcActionButton(pwmBtns[pi]);
                if (pi === 0) html += this._renderSidTag(devIdx, '_all', '\u503c=\u5168\u90e8\u8bbe\u4e3a\u6b64\u503c');
                html += '</span>';
            }
            html += this._renderDcActionButton({ className: 'dc-pwm-refresh dc-btn-sm dc-btn-refresh', devIdx: devIdx, label: '刷新状态' });
            html += '</div></div>';
            return html;
        },

        _renderDcPidPanel: function(devIdx, dev) {
            var decimals = dev.pidDecimals || 1;
            var sf = Math.pow(10, decimals);
            if (!this._dcPidValues[devIdx]) this._dcPidValues[devIdx] = { pv: 0, sv: 0, out: 0, p: 0, i: 0, d: 0 };
            var html = '<div class="dc-modbus-device-body">';
            html += this._renderDcPidGrid(devIdx, sf, decimals);
            html += this._renderDcActionBar([{ className: 'dc-pid-refresh dc-btn-sm dc-btn-refresh', devIdx: devIdx, label: '刷新数据' }]);
            html += '</div>';
            return html;
        },

        _renderDcMotorPanel: function(devIdx, dev) {
            var html = '<div class="dc-modbus-device-body">';
            html += this._renderDcMotorStatusCard(devIdx);
            html += this._renderDcMotorParamsSection(devIdx, dev);
            html += '<div class="dc-modbus-actions">';
            var motorBtns = [
                { className: 'dc-motor-action dc-btn-sm dc-btn-on', devIdx: devIdx, action: 'forward', label: '正转' },
                { className: 'dc-motor-action dc-btn-sm dc-btn-off', devIdx: devIdx, action: 'stop', label: '停止' },
                { className: 'dc-motor-action dc-btn-sm dc-btn-toggle', devIdx: devIdx, action: 'reverse', label: '反转' }
            ];
            for (var mi = 0; mi < motorBtns.length; mi++) html += this._renderDcActionButton(motorBtns[mi]);
            html += '</div>';
            html += this._renderSidTag(devIdx, '_oper', '0=\u505c\u6b62, 1=\u6b63\u8f6c, 2=\u53cd\u8f6c');
            html += '</div>';
            return html;
        },

        _renderDcMotorStatusCard: function(devIdx) {
            var html = '<div class="motor-status-card"><div class="motor-status-header"><span>状态</span><div class="motor-header-actions">';
            html += '<span id="dc-motor-run-' + devIdx + '" class="motor-run-badge motor-run-idle">--</span>';
            html += '<button class="dc-motor-refresh motor-refresh-btn" data-dev="' + devIdx + '" title="刷新">&#x21bb;</button>';
            html += '</div></div><div class="motor-status-grid">';
            html += '<div class="motor-status-item"><span class="motor-status-label">设定速度</span><span class="motor-status-value" id="dc-motor-speed-' + devIdx + '">--</span></div>';
            html += '<div class="motor-status-item"><span class="motor-status-label">单次脉冲</span><span class="motor-status-value" id="dc-motor-pulse-' + devIdx + '">--</span></div>';
            html += '<div class="motor-status-item"><span class="motor-status-label">最近指令</span><span class="motor-status-value" id="dc-motor-dir-' + devIdx + '">--</span></div>';
            html += '</div></div>';
            return html;
        },

        _renderDcMotorParamsSection: function(devIdx, dev) {
            var sf = Math.pow(10, dev.motorDecimals || 0);
            var html = '<div class="motor-params-section">';
            html += '<div class="motor-param-row"><label>设定速度</label>';
            html += '<input type="number" class="motor-param-input " id="dc-motor-speed-in-' + devIdx + '" value="" placeholder="未读取" min="0" max="' + (65535 / sf) + '" step="' + (1 / sf) + '">';
            html += this._renderDcActionButton({ className: 'dc-motor-set dc-btn-sm dc-btn-on', devIdx: devIdx, param: 'speed', label: '设置速度' });
            html += this._renderSidTag(devIdx, '_spd', '0~65535') + '</div>';
            html += '<div class="motor-param-row"><label>脉冲</label>';
            html += '<input type="number" class="motor-param-input " id="dc-motor-pulse-in-' + devIdx + '" value="" placeholder="未读取" min="0" max="65535">';
            html += this._renderDcActionButton({ className: 'dc-motor-set dc-btn-sm dc-btn-on', devIdx: devIdx, param: 'pulse', label: '设置脉冲' });
            html += this._renderSidTag(devIdx, '_pls', '0~65535') + '</div></div>';
            return html;
        },

        // ============ 通用渲染辅助 ============
        _renderDcCardHeader: function(badgeClass, badgeText, title, metaClass, metaText) {
            var html = '<div class="dc-card-header"><span class="dc-card-badge ' + badgeClass + '">' + escapeHtml(badgeText) + '</span><span class="dc-card-title">' + escapeHtml(title) + '</span>';
            if (metaText) html += '<span class="' + (metaClass || '') + '">' + escapeHtml(metaText) + '</span>';
            html += '</div>';
            return html;
        },

        _renderDcActionBar: function(buttons) {
            var html = '<div class="dc-modbus-actions">';
            for (var i = 0; i < buttons.length; i++) html += this._renderDcActionButton(buttons[i]);
            html += '</div>';
            return html;
        },

        _renderDcActionButton: function(config) {
            var attrs = 'data-dev="' + config.devIdx + '"';
            if (config.action) attrs += ' data-action="' + escapeHtml(config.action) + '"';
            if (config.param) attrs += ' data-param="' + escapeHtml(config.param) + '"';
            return '<button class="' + config.className + '" ' + attrs + '>' + escapeHtml(config.label || '') + '</button>';
        },

        _renderSidTag: function(devIdx, suffix, valueDesc) {
            var dev = (this._modbusDevices || [])[devIdx];
            if (!dev || !dev.sensorId) return '';
            var sid = dev.sensorId + suffix;
            return this._renderGenericSidTag(sid, valueDesc);
        },

        // 通用物模型标识渲染（适用于 GPIO/System/Script/Sensor 等任意控件）
        _renderGenericSidTag: function(sensorId, valueDesc) {
            if (!sensorId) return '';
            var hiddenClass = localStorage.getItem('dc_show_sid') === '1' ? '' : ' fb-hidden';
            var text = escapeHtml(sensorId);
            if (valueDesc) text += '<br><span class="dc-sid-detail">' + escapeHtml(valueDesc) + '</span>';
            return '<span class="dc-sid-tag' + hiddenClass + '">' + text + '</span>';
        },

        // GPIO actionType 对应的物模型值说明
        _getGpioActionDesc: function(actionType) {
            switch (actionType) {
                case 0: return '1=\u9ad8\u7535\u5e73';
                case 1: return '0=\u4f4e\u7535\u5e73';
                case 2: return '\u95ea\u70c1';
                case 3: return '\u547c\u5438\u706f';
                case 4: return '0~255';
                case 5: return '0~255';
                case 13: return '1=\u9ad8\u7535\u5e73(\u53cd)';
                case 14: return '0=\u4f4e\u7535\u5e73(\u53cd)';
                default: return '';
            }
        },

        // System actionType 对应的物模型值说明
        _getSystemActionDesc: function(actionType) {
            switch (actionType) {
                case 6: return '1=\u91cd\u542f';
                case 7: return '1=\u6062\u590d\u51fa\u5382';
                case 8: return '1=\u540c\u6b65\u65f6\u95f4';
                case 10: return '\u8c03\u7528\u5916\u8bbe';
                default: return '';
            }
        },

        _renderDcCoilGrid: function(devIdx, channelCount, states, ncMode) {
            var html = '<div class="dc-coil-grid" id="dc-coil-grid-' + devIdx + '">';
            var onText = 'ON';
            var offText = 'OFF';
            for (var ch = 0; ch < channelCount; ch++) {
                var known = typeof states[ch] === 'boolean';
                var isOn = known ? (ncMode ? !states[ch] : states[ch]) : null;
                html += this._renderDcCoilCard(devIdx, ch, isOn, onText, offText);
            }
            html += '</div>';
            return html;
        },

        _renderDcCoilCard: function(devIdx, ch, isOn, onText, offText) {
            return '<div class="dc-coil-card ' + (isOn === null ? 'dc-coil-unknown' : isOn ? 'dc-coil-on' : 'dc-coil-off') + '" data-dev="' + devIdx + '" data-ch="' + ch + '">' +
                '<div class="dc-coil-icon">' + AppState._DC_POWER_ICON_SVG + '</div>' +
                '<div class="dc-coil-ch">CH' + ch + '</div>' +
                '<div class="dc-coil-st">' + escapeHtml(isOn === null ? '--' : isOn ? onText : offText) + '</div>' +
                this._renderSidTag(devIdx, '_ch' + ch, '1=\u6253\u5f00, 0=\u5173\u95ed') + '</div>';
        },

        _renderDcPwmGrid: function(devIdx, channelCount, states, maxValue) {
            var html = '<div class="dc-pwm-list" id="dc-pwm-grid-' + devIdx + '">';
            for (var ch = 0; ch < channelCount; ch++) html += this._renderDcPwmRow(devIdx, ch, ch < states.length ? states[ch] : 0, maxValue);
            html += '</div>';
            return html;
        },

        _renderDcPwmRow: function(devIdx, ch, val, maxValue) {
            var pct = maxValue > 0 ? Math.round(val / maxValue * 100) : 0;
            return '<div class="dc-pwm-row"><span class="dc-pwm-ch">CH' + ch + '</span>' +
                '<input type="range" class="dc-pwm-slider" min="0" max="' + maxValue + '" value="' + val + '" data-dev="' + devIdx + '" data-ch="' + ch + '">' +
                '<span class="dc-pwm-pct" data-dev="' + devIdx + '" data-ch="' + ch + '">' + pct + '%</span>' +
                '<input type="number" class="dc-pwm-num" min="0" max="' + maxValue + '" value="' + val + '" data-dev="' + devIdx + '" data-ch="' + ch + '">' +
                this._renderSidTag(devIdx, '_ch' + ch, '0~' + maxValue) + '</div>';
        },

        _formatDcScaledValue: function(raw, scaleFactor, decimals, suffix) {
            if (raw === undefined || raw === null) return '--';
            return (raw / scaleFactor).toFixed(decimals) + (suffix || '');
        },

        _getDcPidCards: function(devIdx, scaleFactor, decimals) {
            var values = this._dcPidValues[devIdx] || {};
            return [
                { key: 'out', label: '输出 %', value: this._formatDcScaledValue(values.out, scaleFactor, decimals, '%'), editable: false },
                { key: 'pv', label: '过程值 PV', value: this._formatDcScaledValue(values.pv, scaleFactor, decimals), editable: false },
                { key: 'sv', label: '设定值 SV', value: this._formatDcScaledValue(values.sv, scaleFactor, decimals), editable: true },
                { key: 'p', label: 'P 比例', value: this._formatDcScaledValue(values.p, scaleFactor, decimals), editable: true },
                { key: 'i', label: 'I 积分', value: this._formatDcScaledValue(values.i, scaleFactor, decimals), editable: true },
                { key: 'd', label: 'D 微分', value: this._formatDcScaledValue(values.d, scaleFactor, decimals), editable: true }
            ];
        },

        _renderDcPidGrid: function(devIdx, scaleFactor, decimals) {
            var cards = this._getDcPidCards(devIdx, scaleFactor, decimals);
            var html = '<div class="dc-pid-grid" id="dc-pid-grid-' + devIdx + '">';
            for (var ci = 0; ci < cards.length; ci++) html += this._renderDcPidCard(devIdx, cards[ci], scaleFactor, decimals);
            html += '</div>';
            return html;
        },

        _renderDcPidCard: function(devIdx, card, scaleFactor, decimals) {
            var values = this._dcPidValues[devIdx] || {};
            var cls = 'dc-pid-card' + (card.big ? ' dc-pid-pv' : '') + (card.editable ? ' dc-pid-editable' : '');
            var html = '<div class="' + cls + '"><div class="dc-pid-label">' + escapeHtml(card.label) + '</div><div class="dc-pid-value">' + escapeHtml(card.value) + '</div>';
            if (card.editable) {
                var rawVal = (values[card.key] !== undefined && values[card.key] !== null) ? (values[card.key] / scaleFactor).toFixed(decimals) : '';
                html += '<div class="dc-pid-edit"><input type="number" class="dc-pid-input" step="' + (1 / scaleFactor) + '" value="' + rawVal + '" data-dev="' + devIdx + '" data-param="' + escapeHtml(card.key) + '">';
                html += this._renderDcActionButton({ className: 'dc-pid-set dc-btn-sm dc-btn-on', devIdx: devIdx, param: card.key, label: '设置' }) + '</div>';
            }
            html += this._renderSidTag(devIdx, '_' + card.key, card.editable ? '\u6574\u578b(int)' : '\u53ea\u8bfb') + '</div>';
            return html;
        },

        _renderDcLoadingPlaceholder: function(message) {
            return '<div class="fb-loading-placeholder"><div class="fb-loading-placeholder-icon">&#9203;</div><div class="fb-loading-placeholder-detail">' + escapeHtml(message || 'Loading...') + '</div></div>';
        },

        // Keep one entry per execution rule, including disabled automatic triggers.
        _dcControlRules: function(data) {
            var rules = new Map();
            ['gpio', 'modbus', 'system', 'script', 'sensor', 'other'].forEach(function(group) {
                (Array.isArray(data && data[group]) ? data[group] : []).forEach(function(rule) {
                    if (rule && !rules.has(rule.id)) rules.set(rule.id, Object.assign({ _dcGroup: group }, rule));
                });
            });
            return Array.from(rules.values());
        },

        _dcGroupExecutionRules: function(data) {
            var groups = new Map();
            var titles = { gpio: 'GPIO 控制', modbus: 'Modbus 执行', system: '系统操作',
                script: '脚本执行', sensor: '传感器采集', other: '其他外设操作' };
            var names = new Map();
            (this._modbusConfiguredDevices || []).forEach(function(d, i) {
                names.set('modbus:' + (Number.isInteger(d._configIndex) ? d._configIndex : i), d.name);
            });
            (this._modbusStatus && this._modbusStatus.tasks || []).forEach(function(t, i) {
                names.set('modbus-task:' + i, t.name || t.label);
            });
            this._dcControlRules(data).forEach(function(rule) {
                var targets = new Map();
                var addTarget = function(id, name) { if (id) targets.set(id, names.get(id) || name || targets.get(id) || id); };
                (rule.targets || []).forEach(function(t) { addTarget(t.id, t.name); });
                addTarget(rule.targetPeriphId, rule.targetPeriphName);
                (rule.sensors || []).forEach(function(s) { addTarget(s.periphId); });
                (rule.pollTaskIndex || []).forEach(function(index) { addTarget('modbus-task:' + index); });
                var target = targets.size === 1 ? Array.from(targets.keys())[0] : '';
                var sensorKeys = Array.from(new Set((rule.sensors || []).map(function(s) { return s.key; }).filter(Boolean)));
                // A combined rule executes as a whole, never silently as a single channel.
                var combined = targets.size > 1 || (!rule.targets && sensorKeys.length > 1);
                rule._dcTargetText = Array.from(targets.values()).join('、');
                // Execution templates stay in peer cards, never inside measurements
                // or native device controls. Keep existing group keys for saved layouts.
                var key = combined ? 'combined' : target ? 'peripheral-' + target : rule._dcGroup;
                var title = combined ? '联合采集与联动' : target ? targets.get(target) : titles[rule._dcGroup];
                if (!groups.has(key)) groups.set(key, { title: title, items: [] });
                groups.get(key).items.push(rule);
            });
            return groups;
        },

        _renderDcRuleActions: function(items) {
            return '<div class="dc-rule-actions">' + items.map(function(item) {
                return this._renderControlButton(item, item._dcGroup || 'other', item.hasSystemAction || item._dcGroup === 'system');
            }, this).join('') + '</div>';
        },

        _renderControlGroup: function(title, items, key) {
            return '<div class="dc-control-group dc-execution-card" data-dc-sort-key="' + escapeHtml(key) + '">' +
                '<div class="dc-control-group-title">执行一次 · ' + escapeHtml(title) + '</div>' +
                this._renderDcRuleActions(items) + '<div class="dc-resize-handle" title="拖拽调整大小"></div></div>';
        },

        _renderControlButton: function(item, typeClass, isSystem) {
            var dataAttrs = 'data-id="' + escapeHtml(item.id) + '" data-name="' + escapeHtml(item.name) + '"';
            if (item.hasSetMode) dataAttrs += ' data-has-set-mode="true"';
            if (isSystem) dataAttrs += ' data-system="true"';
            if (item.unavailableReason) dataAttrs += ' data-unavailable="true" title="' + escapeHtml(item.unavailableReason) + '"';
            var busy = this._dcRuleRuns && this._dcRuleRuns.has(item.id);
            if (item.unavailableReason || busy) dataAttrs += ' disabled';
            var details = [];
            if (item._dcTargetText) details.push(item._dcTargetText);
            if (item.actionCount > 1) details.push(item.actionCount + ' 个动作，执行整条规则');
            if (item.enabled === false) details.push('自动触发已停用，可手动执行');
            if (item.unavailableReason) details.push(item.unavailableReason);
            return '<div class="dc-rule-row"><div class="dc-rule-info"><span class="dc-rule-name">' +
                escapeHtml(item.name || item.id) + '</span><small>' + escapeHtml(details.join(' · ')) +
                '</small>' + this._renderGenericSidTag(item.id, '') + '</div><button class="dc-ctrl-btn dc-' +
                typeClass + '" ' + dataAttrs + ' aria-label="' + escapeHtml((item.name || item.id) + '：执行一次') + '">' +
                (busy ? '提交中...' : '执行一次') + '</button></div>';
        }
    });
})();
