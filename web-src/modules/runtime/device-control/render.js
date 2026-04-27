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
                case 'high': return { text: this._t('modbus-master-risk-high'), className: 'modbus-risk-high' };
                case 'medium': return { text: this._t('modbus-master-risk-medium'), className: 'modbus-risk-medium' };
                default: return { text: this._t('modbus-master-risk-low'), className: 'modbus-risk-low' };
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
                '<div class="dc-risk-title">' + this._t('device-control-modbus-health-title') + '</div>' +
                '<div class="dc-risk-main">' +
                '<span class="dc-risk-label">' + this._t('modbus-master-risk-level') + '</span>' +
                '<span class="modbus-risk-badge ' + riskMeta.className + '">' + escapeHtml(riskMeta.text) + '</span>' +
                '</div>' +
                '<div class="dc-risk-metrics">' +
                '<span class="dc-risk-metric"><span class="dc-risk-metric-label">' + this._t('modbus-master-enabled-tasks') + '</span><strong>' + escapeHtml(health.enabledTaskCount ?? 0) + '</strong></span>' +
                '<span class="dc-risk-metric"><span class="dc-risk-metric-label">' + this._t('modbus-master-min-interval') + '</span><strong>' + escapeHtml(health.minPollInterval ? (health.minPollInterval + 's') : '--') + '</strong></span>' +
                '<span class="dc-risk-metric"><span class="dc-risk-metric-label">' + this._t('modbus-master-last-poll-age') + '</span><strong>' + escapeHtml(this._formatDcAge(health.lastPollAgeSec)) + '</strong></span>' +
                '<span class="dc-risk-metric"><span class="dc-risk-metric-label">' + this._t('modbus-master-timeout-rate') + '</span><strong>' + escapeHtml(this._formatDcPercent(health.timeoutRate)) + '</strong></span>' +
                '</div>';
            if (warnings.length > 0) {
                var warnHtml = '<div class="dc-risk-warnings">';
                for (var wi = 0; wi < warnings.length; wi++) {
                    warnHtml += '<div class="dc-risk-warning"><i class="fas fa-exclamation-triangle"></i><span>' + escapeHtml(warnings[wi] || '') + '</span></div>';
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
            var hasMonitor = monitorGroups.length > 0;
            var monitorItems = [];
            if (!hasMonitor) {
                monitorItems = this._filterByActionType(data.modbus, [18]).concat(this._filterByActionType(data.sensor, [19]));
                hasMonitor = monitorItems.length > 0;
            }
            if (!hasMonitor) return '';
            var html = '<div class="dc-monitor-grid" data-dc-sort-key="monitor-data">';
            if (monitorGroups.length > 0) {
                for (var i = 0; i < monitorGroups.length; i++) {
                    var group = monitorGroups[i];
                    for (var j = 0; j < group.items.length; j++) html += this._renderMonitorCard(group.items[j], group.label);
                }
            } else {
                for (var i = 0; i < monitorItems.length; i++) html += this._renderMonitorCard(monitorItems[i]);
            }
            html += '<div class="dc-resize-handle" title="拖拽调整大小"></div></div>';
            return html;
        },

        _buildMonitorGroupsFromModbus: function() {
            var groups = [];
            if (!this._modbusStatus || !this._modbusStatus.tasks) return groups;
            var tasks = this._modbusStatus.tasks;
            for (var i = 0; i < tasks.length; i++) {
                var task = tasks[i];
                if (!task.enabled || !task.mappings || task.mappings.length === 0) continue;
                var group = { label: task.name || task.label || ('设备 ' + task.slaveAddress), items: [] };
                var cachedData = task.cachedData;
                var hasCache = cachedData && cachedData.values;
                for (var j = 0; j < task.mappings.length; j++) {
                    var mapping = task.mappings[j];
                    if (!mapping.sensorId) continue;
                    var item = { id: mapping.sensorId, name: mapping.sensorId, value: '--', unit: mapping.unit || this._getMonitorUnit(mapping.sensorId) };
                    if (hasCache && mapping.regOffset < cachedData.values.length) {
                        item.value = (cachedData.values[mapping.regOffset] * (mapping.scaleFactor || 1)).toFixed(mapping.decimalPlaces || 0);
                    }
                    group.items.push(item);
                }
                if (group.items.length > 0) groups.push(group);
            }
            return groups;
        },

        _renderMonitorCard: function(item, deviceLabel) {
            var name = escapeHtml(item.name || 'Unknown');
            var value = escapeHtml(item.value || item.lastValue || '--');
            var unit = escapeHtml(item.unit || '');
            var toneClass = this._getMonitorToneClass(item.name || '');
            var icon = this._getMonitorIcon(item.name || '');
            var html = '<div class="dc-monitor-card" data-id="' + escapeHtml(item.id) + '">';
            html += '<div class="dc-monitor-top"><span class="dc-monitor-label">' + name + '</span><span class="dc-monitor-icon ' + toneClass + '">' + icon + '</span></div>';
            if (deviceLabel) html += '<div class="dc-monitor-device">' + escapeHtml(deviceLabel) + '</div>';
            html += '<div class="dc-monitor-val-row"><span class="dc-monitor-value">' + value + '</span>';
            if (unit) html += '<span class="dc-monitor-unit">' + unit + '</span>';
            html += '</div></div>';
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
            return '';
        },

        // ============ 渲染控制操作区 ============
        _renderControlSection: function(data) {
            var html = '';
            var gpioItems = this._filterByActionType(data.gpio, [0, 1, 2, 3, 4, 5, 13, 14]);
            var modbusCtrlItems = this._filterByActionType(data.modbus, [16, 17]);
            var systemItems = this._filterByActionType(data.system, [6, 7, 8, 9, 10, 11, 12]);
            var scriptItems = this._filterByActionType(data.script, [15]);
            var sensorReadItems = this._filterByActionType(data.sensor, [19]);
            var otherItems = data.other || [];
            gpioItems.sort(function(a, b) { return (a.name || '').localeCompare(b.name || ''); });
            modbusCtrlItems.sort(function(a, b) { return (a.name || '').localeCompare(b.name || ''); });
            systemItems.sort(function(a, b) { return (a.name || '').localeCompare(b.name || ''); });
            scriptItems.sort(function(a, b) { return (a.name || '').localeCompare(b.name || ''); });
            sensorReadItems.sort(function(a, b) { return (a.name || '').localeCompare(b.name || ''); });
            otherItems.sort(function(a, b) { return (a.name || '').localeCompare(b.name || ''); });
            var hasModbusDevices = this._modbusDevices && this._modbusDevices.length > 0;
            var modbusDeviceList = [];
            if (hasModbusDevices) {
                var typeOrder = { relay: 0, pid: 1, pwm: 2, motor: 3 };
                for (var mi = 0; mi < this._modbusDevices.length; mi++) modbusDeviceList.push({ idx: mi, dev: this._modbusDevices[mi] });
                modbusDeviceList.sort(function(a, b) { return (typeOrder[a.dev.deviceType || 'relay'] || 0) - (typeOrder[b.dev.deviceType || 'relay'] || 0); });
            }
            var hasButtonControls = gpioItems.length > 0 || modbusCtrlItems.length > 0 || systemItems.length > 0 || scriptItems.length > 0 || sensorReadItems.length > 0 || otherItems.length > 0;
            var hasAnyControls = hasButtonControls || modbusDeviceList.length > 0;
            if (hasAnyControls) {
                html += '<div class="fb-card dc-header-card"><div class="fb-card-header fb-card-header-toolbar dc-header-card-inner">';
                html += '<h2 class="fb-card-title dc-header-title">' + this._t('device-control-dashboard') + '</h2>';
                html += '<div class="dc-header-actions">';
                var _sidChecked = localStorage.getItem('dc_show_sid') === '1';
                html += '<label style="display:inline-flex;align-items:center;gap:4px;font-size:12px;color:#666;margin-right:8px;cursor:pointer;user-select:none;"><input type="checkbox" id="dc-sid-toggle"' + (_sidChecked ? ' checked' : '') + ' style="margin:0;">\u663E\u793A\u7269\u6A21\u578B\u6807\u8BC6</label>';
                html += '<button class="dc-btn-sm dc-btn-zoom" id="dc-zoom-out-btn" title="Ctrl+-">-</button>';
                html += '<button class="dc-btn-sm dc-btn-zoom" id="dc-zoom-reset-btn" title="Ctrl+0">100%</button>';
                html += '<button class="dc-btn-sm dc-btn-zoom" id="dc-zoom-in-btn" title="Ctrl++">+</button>';
                html += '<button class="dc-btn-sm dc-btn-refresh" id="dc-refresh-btn">' + this._t('dashboard-refresh') + '</button>';
                html += '<button class="dc-btn-sm dc-btn-reset dc-layout-reset">' + this._t('device-control-reset-layout') + '</button>';
                html += '<button class="dc-btn-sm dc-btn-fullscreen" id="dc-fullscreen-btn" title="Ctrl+F">' + this._t('dashboard-fullscreen') + '</button>';
                html += '</div></div></div>';
                html += '<div class="dc-zoom-wrapper"><div class="dc-control-flow">';
                html += this._renderMonitorSection(data);
                if (gpioItems.length > 0) html += this._renderGpioGroup(gpioItems);
                if (modbusCtrlItems.length > 0) html += this._renderControlGroup('Modbus', modbusCtrlItems, 'modbus', false);
                if (scriptItems.length > 0) html += this._renderControlGroup('Script', scriptItems, 'script', false);
                if (sensorReadItems.length > 0) html += this._renderControlGroup('Sensor', sensorReadItems, 'sensor', false);
                if (otherItems.length > 0) html += this._renderControlGroup('Other', otherItems, 'other', false);
                if (systemItems.length > 0) html += this._renderSystemGroup(systemItems);
                if (modbusDeviceList.length > 0) html += this._renderModbusDevicePanels(modbusDeviceList);
                html += '</div></div>';
            } else {
                html += '<div class="dc-empty">' + this._t('device-control-no-action') + '</div>';
            }
            return html;
        },

        _renderSystemGroup: function(items) {
            var title = this._t('device-control-group-system') || 'System';
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
            var title = this._t('device-control-group-gpio') || 'GPIO';
            var html = '<div class="dc-control-group dc-group-card" data-dc-sort-key="gpio">';
            html += this._renderDcCardHeader('dc-card-badge--gpio', 'GPIO', title);
            html += '<div class="dc-gpio-list">';
            for (var i = 0; i < items.length; i++) {
                var item = items[i];
                var setAttr = item && item.hasSetMode ? ' data-has-set-mode="true"' : '';
                html += '<div class="dc-gpio-row"><span class="dc-gpio-name">' + escapeHtml(item.name) + '</span>';
                html += '<button class="dc-ctrl-btn dc-gpio" data-id="' + escapeHtml(item.id) + '" data-name="' + escapeHtml(item.name) + '"' + setAttr + '>' + this._t('device-control-execute') + '</button>';
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
            var typeLabel = this._t('modbus-type-' + dt) || dt;
            var ctrlLabel = (dev.name || '') || (this._t('dc-modbus-ctrl-' + dt) || (typeLabel + ' ' + this._t('device-control-action-section')));
            var typeClassMap = {relay: 'dc-card-badge--relay', pwm: 'dc-card-badge--pwm', pid: 'dc-card-badge--pid', motor: 'dc-card-badge--motor'};
            var html = '<div class="dc-modbus-device-panel" data-dev-idx="' + devIdx + '" data-dc-sort-key="modbus-' + dt + '-' + devIdx + '">';
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
            var states = this._dcCoilStates[devIdx] || new Array(channelCount).fill(false);
            var ncMode = !!dev.ncMode;
            var html = '<div class="dc-modbus-device-body">';
            html += this._renderDcCoilGrid(devIdx, channelCount, states, ncMode);
            html += this._renderDcRelayDelaySection(devIdx, dev);
            html += '<div class="dc-modbus-actions">';
            var relayBtns = [
                { className: 'dc-coil-batch dc-btn-sm dc-btn-on', devIdx: devIdx, action: 'allOn', label: this._t('modbus-ctrl-all-on') },
                { className: 'dc-coil-batch dc-btn-sm dc-btn-off', devIdx: devIdx, action: 'allOff', label: this._t('modbus-ctrl-all-off') },
                { className: 'dc-coil-batch dc-btn-sm dc-btn-toggle', devIdx: devIdx, action: 'allToggle', label: this._t('modbus-ctrl-all-toggle') }
            ];
            for (var ri = 0; ri < relayBtns.length; ri++) {
                html += '<span style="display:inline-flex;flex-direction:column;align-items:center;">';
                html += this._renderDcActionButton(relayBtns[ri]);
                if (ri === 0) html += this._renderSidTag(devIdx, '_all', '1=\u5168\u5f00, 0=\u5168\u5173');
                html += '</span>';
            }
            html += this._renderDcActionButton({ className: 'dc-coil-refresh dc-btn-sm dc-btn-refresh', devIdx: devIdx, label: this._t('modbus-ctrl-refresh') });
            html += '</div></div>';
            return html;
        },

        _renderDcRelayDelaySection: function(devIdx, dev) {
            var channelCount = dev.channelCount || 2;
            var html = '<div class="dc-relay-delay-section"><div class="dc-delay-controls">';
            html += '<span class="dc-delay-label">' + this._t('modbus-delay-title') + '</span>';
            html += '<select class="dc-delay-channel" data-dev="' + devIdx + '">';
            for (var ch = 0; ch < channelCount; ch++) html += '<option value="' + ch + '">CH' + ch + '</option>';
            html += '</select>';
            html += '<input type="number" class="dc-delay-input" min="1" max="255" value="50" data-dev="' + devIdx + '">';
            html += '<button class="dc-delay-start dc-btn-sm" data-dev="' + devIdx + '">' + this._t('modbus-delay-start') + '</button>';
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
                { className: 'dc-pwm-batch dc-btn-sm dc-btn-on', devIdx: devIdx, action: 'max', label: this._t('modbus-ctrl-pwm-set-all-max') },
                { className: 'dc-pwm-batch dc-btn-sm dc-btn-off', devIdx: devIdx, action: 'off', label: this._t('modbus-ctrl-pwm-set-all-off') }
            ];
            for (var pi = 0; pi < pwmBtns.length; pi++) {
                html += '<span style="display:inline-flex;flex-direction:column;align-items:center;">';
                html += this._renderDcActionButton(pwmBtns[pi]);
                if (pi === 0) html += this._renderSidTag(devIdx, '_all', '\u503c=\u5168\u90e8\u8bbe\u4e3a\u6b64\u503c');
                html += '</span>';
            }
            html += this._renderDcActionButton({ className: 'dc-pwm-refresh dc-btn-sm dc-btn-refresh', devIdx: devIdx, label: this._t('modbus-ctrl-pwm-refresh') });
            html += '</div></div>';
            return html;
        },

        _renderDcPidPanel: function(devIdx, dev) {
            var decimals = dev.pidDecimals || 1;
            var sf = Math.pow(10, decimals);
            if (!this._dcPidValues[devIdx]) this._dcPidValues[devIdx] = { pv: 0, sv: 0, out: 0, p: 0, i: 0, d: 0 };
            var html = '<div class="dc-modbus-device-body">';
            html += this._renderDcPidGrid(devIdx, sf, decimals);
            html += this._renderDcActionBar([{ className: 'dc-pid-refresh dc-btn-sm dc-btn-refresh', devIdx: devIdx, label: this._t('modbus-ctrl-pid-refresh') }]);
            html += '</div>';
            return html;
        },

        _renderDcMotorPanel: function(devIdx, dev) {
            var html = '<div class="dc-modbus-device-body">';
            html += this._renderDcMotorStatusCard(devIdx);
            html += this._renderDcMotorParamsSection(devIdx, dev);
            html += '<div class="dc-modbus-actions">';
            var motorBtns = [
                { className: 'dc-motor-action dc-btn-sm dc-btn-on', devIdx: devIdx, action: 'forward', label: this._t('modbus-motor-ctrl-forward') || '正转' },
                { className: 'dc-motor-action dc-btn-sm dc-btn-off', devIdx: devIdx, action: 'stop', label: this._t('modbus-motor-ctrl-stop') || '停止' },
                { className: 'dc-motor-action dc-btn-sm dc-btn-toggle', devIdx: devIdx, action: 'reverse', label: this._t('modbus-motor-ctrl-reverse') || '反转' }
            ];
            for (var mi = 0; mi < motorBtns.length; mi++) html += this._renderDcActionButton(motorBtns[mi]);
            html += '</div>';
            html += this._renderSidTag(devIdx, '_oper', '0=\u505c\u6b62, 1=\u6b63\u8f6c, 2=\u53cd\u8f6c');
            html += '</div>';
            return html;
        },

        _renderDcMotorStatusCard: function(devIdx) {
            var html = '<div class="motor-status-card"><div class="motor-status-header"><span>' + (this._t('modbus-motor-status') || '状态') + '</span><div class="motor-header-actions">';
            html += '<span id="dc-motor-run-' + devIdx + '" class="motor-run-badge motor-run-idle">--</span>';
            html += '<button class="dc-motor-refresh motor-refresh-btn" data-dev="' + devIdx + '" title="' + (this._t('modbus-ctrl-refresh') || '刷新') + '">&#x21bb;</button>';
            html += '</div></div><div class="motor-status-grid">';
            html += '<div class="motor-status-item"><span class="motor-status-label">' + (this._t('modbus-motor-speed') || '速度') + '</span><span class="motor-status-value" id="dc-motor-speed-' + devIdx + '">--</span><span class="motor-status-unit">rpm</span></div>';
            html += '<div class="motor-status-item"><span class="motor-status-label">' + (this._t('modbus-motor-pulse') || '脉冲数') + '</span><span class="motor-status-value" id="dc-motor-pulse-' + devIdx + '">--</span></div>';
            html += '<div class="motor-status-item"><span class="motor-status-label">' + (this._t('modbus-motor-dir') || '方向') + '</span><span class="motor-status-value" id="dc-motor-dir-' + devIdx + '">--</span></div>';
            html += '<div class="motor-status-item"><span class="motor-status-label">' + (this._t('modbus-motor-count') || '计数') + '</span><span class="motor-status-value" id="dc-motor-count-' + devIdx + '">--</span><span class="motor-status-unit">次</span></div>';
            html += '</div></div>';
            return html;
        },

        _renderDcMotorParamsSection: function(devIdx, dev) {
            var html = '<div class="motor-params-section">';
            html += '<div class="motor-param-row"><label>' + (this._t('modbus-motor-speed') || '速度') + '</label>';
            html += '<input type="number" class="motor-param-input " id="dc-motor-speed-in-' + devIdx + '" value="50" min="0">';
            html += this._renderDcActionButton({ className: 'dc-motor-set dc-btn-sm dc-btn-on', devIdx: devIdx, param: 'speed', label: this._t('modbus-motor-ctrl-set-speed') || '设置速度' });
            html += this._renderSidTag(devIdx, '_spd', '0~65535') + '</div>';
            html += '<div class="motor-param-row"><label>' + (this._t('modbus-motor-pulse') || '脉冲') + '</label>';
            html += '<input type="number" class="motor-param-input " id="dc-motor-pulse-in-' + devIdx + '" value="1600" min="0">';
            html += this._renderDcActionButton({ className: 'dc-motor-set dc-btn-sm dc-btn-on', devIdx: devIdx, param: 'pulse', label: this._t('modbus-motor-ctrl-set-pulse') || '设置脉冲' });
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
            var vis = localStorage.getItem('dc_show_sid') === '1' ? '' : 'display:none;';
            var text = escapeHtml(sensorId);
            if (valueDesc) text += '<br><span style="color:#b0b8c8;">' + escapeHtml(valueDesc) + '</span>';
            return '<span class="dc-sid-tag" style="' + vis + 'font-size:10px;color:#8a9bb5;text-align:center;line-height:1.2;margin-top:2px;">' + text + '</span>';
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
                case 9: return '1=OTA';
                case 10: return '1=AP\u914d\u7f51';
                case 11: return '1=BLE\u914d\u7f51';
                case 12: return '\u8c03\u7528\u5916\u8bbe';
                default: return '';
            }
        },

        _renderDcCoilGrid: function(devIdx, channelCount, states, ncMode) {
            var html = '<div class="dc-coil-grid" id="dc-coil-grid-' + devIdx + '">';
            var onText = this._t('modbus-ctrl-status-on') || 'ON';
            var offText = this._t('modbus-ctrl-status-off') || 'OFF';
            for (var ch = 0; ch < channelCount; ch++) {
                var isOn = ncMode ? !(ch < states.length ? states[ch] : false) : (ch < states.length ? states[ch] : false);
                html += this._renderDcCoilCard(devIdx, ch, isOn, onText, offText);
            }
            html += '</div>';
            return html;
        },

        _renderDcCoilCard: function(devIdx, ch, isOn, onText, offText) {
            return '<div class="dc-coil-card ' + (isOn ? 'dc-coil-on' : 'dc-coil-off') + '" data-dev="' + devIdx + '" data-ch="' + ch + '">' +
                '<div class="dc-coil-icon">' + AppState._DC_POWER_ICON_SVG + '</div>' +
                '<div class="dc-coil-ch">CH' + ch + '</div>' +
                '<div class="dc-coil-st">' + escapeHtml(isOn ? onText : offText) + '</div>' +
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
                { key: 'out', label: this._t('modbus-ctrl-pid-out-label'), value: this._formatDcScaledValue(values.out, scaleFactor, decimals, '%'), editable: false },
                { key: 'pv', label: this._t('modbus-ctrl-pid-pv-label'), value: this._formatDcScaledValue(values.pv, scaleFactor, decimals), editable: false },
                { key: 'sv', label: this._t('modbus-ctrl-pid-sv-label'), value: this._formatDcScaledValue(values.sv, scaleFactor, decimals), editable: true },
                { key: 'p', label: this._t('modbus-ctrl-pid-p-label'), value: this._formatDcScaledValue(values.p, scaleFactor, decimals), editable: true },
                { key: 'i', label: this._t('modbus-ctrl-pid-i-label'), value: this._formatDcScaledValue(values.i, scaleFactor, decimals), editable: true },
                { key: 'd', label: this._t('modbus-ctrl-pid-d-label'), value: this._formatDcScaledValue(values.d, scaleFactor, decimals), editable: true }
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
                html += this._renderDcActionButton({ className: 'dc-pid-set dc-btn-sm dc-btn-on', devIdx: devIdx, param: card.key, label: this._t('modbus-ctrl-pid-set') }) + '</div>';
            }
            html += this._renderSidTag(devIdx, '_' + card.key, card.editable ? '\u6574\u578b(int)' : '\u53ea\u8bfb') + '</div>';
            return html;
        },

        _renderDcLoadingPlaceholder: function(message) {
            return '<div class="dc-loading-placeholder" style="text-align:center;padding:30px 20px;color:#999;"><div style="font-size:24px;margin-bottom:10px;">⏳</div><div style="font-size:13px;">' + escapeHtml(message || '加载中...') + '</div></div>';
        },

        _renderControlGroup: function(titleKey, items, typeClass, isSystem) {
            var title = this._t('device-control-group-' + titleKey.toLowerCase()) || titleKey;
            var html = '<div class="dc-control-group" data-dc-sort-key="' + typeClass + '"><div class="dc-control-group-title">' + title + '</div><div class="dc-control-grid">';
            for (var i = 0; i < items.length; i++) html += this._renderControlButton(items[i], typeClass, isSystem);
            html += '</div><div class="dc-resize-handle" title="拖拽调整大小"></div></div>';
            return html;
        },

        _renderControlButton: function(item, typeClass, isSystem) {
            var dataAttrs = 'data-id="' + escapeHtml(item.id) + '" data-name="' + escapeHtml(item.name) + '"';
            if (item && item.hasSetMode) dataAttrs += ' data-has-set-mode="true"';
            if (isSystem) dataAttrs += ' data-system="true"';
            var sidTag = this._renderGenericSidTag(item.id, '');
            if (sidTag) {
                return '<span style="display:inline-flex;flex-direction:column;align-items:center;"><button class="dc-ctrl-btn dc-' + typeClass + '" ' + dataAttrs + '>' + escapeHtml(item.name) + '</button>' + sidTag + '</span>';
            }
            return '<button class="dc-ctrl-btn dc-' + typeClass + '" ' + dataAttrs + '>' + escapeHtml(item.name) + '</button>';
        }
    });
})();
