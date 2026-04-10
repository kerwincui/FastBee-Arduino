/**
 * 设备控制模块 - 三区布局版本
 * 区域一：监测数据展示区
 * 区域二：设备图标区
 * 区域三：控制操作区
 */
(function() {
    'use strict';

    console.log('[device-control] Module script loading...');

    // MCU/芯片图标 SVG（内联，不依赖外部资源）
    var DEVICE_ICON_SVG = '<svg viewBox="0 0 80 80" width="80" height="80" fill="none" xmlns="http://www.w3.org/2000/svg">' +
        '<rect x="20" y="15" width="40" height="50" rx="3" stroke="currentColor" stroke-width="2.5" fill="none"/>' +
        '<rect x="26" y="21" width="28" height="38" rx="1" stroke="currentColor" stroke-width="1.5" fill="none"/>' +
        '<circle cx="40" cy="58" r="2" fill="currentColor"/>' +
        '<line x1="15" y1="22" x2="20" y2="22" stroke="currentColor" stroke-width="2"/>' +
        '<line x1="15" y1="28" x2="20" y2="28" stroke="currentColor" stroke-width="2"/>' +
        '<line x1="15" y1="34" x2="20" y2="34" stroke="currentColor" stroke-width="2"/>' +
        '<line x1="15" y1="40" x2="20" y2="40" stroke="currentColor" stroke-width="2"/>' +
        '<line x1="15" y1="46" x2="20" y2="46" stroke="currentColor" stroke-width="2"/>' +
        '<line x1="15" y1="52" x2="20" y2="52" stroke="currentColor" stroke-width="2"/>' +
        '<line x1="15" y1="58" x2="20" y2="58" stroke="currentColor" stroke-width="2"/>' +
        '<line x1="60" y1="22" x2="65" y2="22" stroke="currentColor" stroke-width="2"/>' +
        '<line x1="60" y1="28" x2="65" y2="28" stroke="currentColor" stroke-width="2"/>' +
        '<line x1="60" y1="34" x2="65" y2="34" stroke="currentColor" stroke-width="2"/>' +
        '<line x1="60" y1="40" x2="65" y2="40" stroke="currentColor" stroke-width="2"/>' +
        '<line x1="60" y1="46" x2="65" y2="46" stroke="currentColor" stroke-width="2"/>' +
        '<line x1="60" y1="52" x2="65" y2="52" stroke="currentColor" stroke-width="2"/>' +
        '<line x1="60" y1="58" x2="65" y2="58" stroke="currentColor" stroke-width="2"/>' +
        '<line x1="28" y1="10" x2="28" y2="15" stroke="currentColor" stroke-width="2"/>' +
        '<line x1="34" y1="10" x2="34" y2="15" stroke="currentColor" stroke-width="2"/>' +
        '<line x1="40" y1="10" x2="40" y2="15" stroke="currentColor" stroke-width="2"/>' +
        '<line x1="46" y1="10" x2="46" y2="15" stroke="currentColor" stroke-width="2"/>' +
        '<line x1="52" y1="10" x2="52" y2="15" stroke="currentColor" stroke-width="2"/>' +
        '<line x1="28" y1="65" x2="28" y2="70" stroke="currentColor" stroke-width="2"/>' +
        '<line x1="34" y1="65" x2="34" y2="70" stroke="currentColor" stroke-width="2"/>' +
        '<line x1="40" y1="65" x2="40" y2="70" stroke="currentColor" stroke-width="2"/>' +
        '<line x1="46" y1="65" x2="46" y2="70" stroke="currentColor" stroke-width="2"/>' +
        '<line x1="52" y1="65" x2="52" y2="70" stroke="currentColor" stroke-width="2"/>' +
        '</svg>';

    AppState.registerModule('device-control', {
        _controlData: null,
        _deviceName: 'FastBee Device',
        _eventsBound: false,

        // ============ 事件绑定 ============
        setupDeviceControlEvents: function() {
            console.log('[device-control] Setting up events...');
            var self = this;

            // 刷新按钮事件
            var refreshBtn = document.getElementById('dc-refresh-btn');
            if (refreshBtn && !this._eventsBound) {
                refreshBtn.addEventListener('click', function() {
                    self.loadDeviceControlPage();
                });
            }

            // 使用事件委托绑定控制按钮点击
            var content = document.getElementById('dc-content');
            if (content && !this._eventsBound) {
                content.addEventListener('click', function(e) {
                    var btn = e.target.closest('.dc-ctrl-btn');
                    if (!btn) return;

                    var ruleId = btn.getAttribute('data-id');
                    var isSystem = btn.getAttribute('data-system') === 'true';
                    var ruleName = btn.getAttribute('data-name') || '';

                    if (isSystem) {
                        if (confirm(self._t('device-control-confirm-system') + '\n' + ruleName)) {
                            self._executeRule(ruleId, btn);
                        }
                    } else {
                        self._executeRule(ruleId, btn);
                    }
                });
            }

            this._eventsBound = true;
            console.log('[device-control] Events bound successfully');
        },

        // ============ 加载控制面板 ============
        loadDeviceControlPage: function() {
            console.log('[device-control] loadDeviceControlPage called');

            // 确保事件绑定
            if (!this._eventsBound) {
                this.setupDeviceControlEvents();
            }

            var content = document.getElementById('dc-content');
            if (!content) {
                console.error('[device-control] Content element "dc-content" not found!');
                return;
            }

            content.innerHTML = '<div class="dc-empty">⏳ ' + (typeof i18n !== 'undefined' ? i18n.t('loading') : '加载中...') + '</div>';

            var self = this;

            // 先获取设备信息
            this._fetchDeviceInfo().then(function() {
                // 再获取控制数据
                console.log('[device-control] Fetching controls data...');
                return apiGet('/api/periph-exec/controls');
            }).then(function(res) {
                console.log('[device-control] API response:', JSON.stringify(res));

                if (!res) {
                    console.log('[device-control] No response, showing empty state');
                    content.innerHTML = self._renderEmptyState();
                    return;
                }

                if (res.success === false) {
                    console.log('[device-control] API returned failure:', res.error || res.message);
                    content.innerHTML = '<div class="dc-empty" style="color:var(--danger);">❌ ' + self._esc(res.error || res.message || '请求失败') + '</div>';
                    return;
                }

                // 获取数据 - 兼容多种格式
                var data = res.data || res;
                console.log('[device-control] Parsed data:', JSON.stringify(data));
                
                if (!data || typeof data !== 'object') {
                    console.log('[device-control] No valid data, showing empty state');
                    content.innerHTML = self._renderEmptyState();
                    return;
                }

                self._controlData = data;
                
                // 渲染面板，捕获任何错误
                try {
                    var html = self._renderControlPanel(data);
                    console.log('[device-control] Rendered HTML length:', html.length);
                    content.innerHTML = html;
                } catch (renderErr) {
                    console.error('[device-control] Render error:', renderErr);
                    content.innerHTML = '<div class="dc-empty" style="color:var(--danger);">❌ 渲染错误: ' + self._esc(renderErr.message || renderErr) + '</div>';
                }
            }).catch(function(err) {
                console.error('[device-control] API error:', err);
                var errMsg = '请求失败';
                if (err && err.data && err.data.error) {
                    errMsg = err.data.error;
                } else if (err && err.message) {
                    errMsg = err.message;
                }
                content.innerHTML = '<div class="dc-empty" style="color:var(--danger);">❌ ' + self._esc(errMsg) + '</div>';
            });
        },

        // ============ 获取设备信息 ============
        _fetchDeviceInfo: function() {
            var self = this;
            return apiGetSilent('/api/device/config').then(function(res) {
                if (res && res.success && res.data) {
                    self._deviceName = res.data.deviceName || res.data.name || 'FastBee Device';
                }
            }).catch(function() {
                // 忽略错误，使用默认名称
                self._deviceName = 'FastBee Device';
            });
        },

        // ============ 渲染控制面板（三区布局） ============
        _renderControlPanel: function(data) {
            console.log('[device-control] _renderControlPanel called with data:', data);

            data = data || {};
            var html = '';

            try {
                // === 区域一：监测数据展示区 ===
                html += this._renderMonitorSection(data);

                // === 区域二：设备图标区 ===
                html += this._renderDeviceIconSection();

                // === 区域三：控制操作区 ===
                html += this._renderControlSection(data);
            } catch (e) {
                console.error('[device-control] Error in _renderControlPanel:', e);
                html = '<div class="dc-empty" style="color:var(--danger);">❌ 渲染错误: ' + this._esc(e.message || e) + '</div>';
            }

            return html;
        },

        // ============ 渲染监测数据区 ============
        _renderMonitorSection: function(data) {
            // 采集数据项：modbus(actionType=18) + sensor(actionType=19)
            var modbusItems = this._filterByActionType(data.modbus, [18]);
            var sensorItems = this._filterByActionType(data.sensor, [19]);
            var monitorItems = modbusItems.concat(sensorItems);

            var html = '<div class="dc-section">';
            html += '<div class="dc-section-title">📊 ' + this._t('device-control-monitor-section') + '</div>';

            if (monitorItems.length > 0) {
                html += '<div class="dc-monitor-grid">';
                for (var i = 0; i < monitorItems.length; i++) {
                    html += this._renderMonitorCard(monitorItems[i]);
                }
                html += '</div>';
            } else {
                html += '<div class="dc-empty">' + this._t('device-control-no-monitor') + '</div>';
            }

            html += '</div>';
            return html;
        },

        // ============ 渲染监测数据卡片 ============
        _renderMonitorCard: function(item) {
            var name = this._esc(item.name || 'Unknown');
            var value = this._esc(item.value || item.lastValue || '--');
            var unit = this._esc(item.unit || '');

            return '<div class="dc-monitor-card" data-id="' + this._esc(item.id) + '">' +
                '<div class="dc-monitor-name">' + name + '</div>' +
                '<div class="dc-monitor-value">' + value + '</div>' +
                (unit ? '<div class="dc-monitor-unit">' + unit + '</div>' : '') +
                '</div>';
        },

        // ============ 渲染设备图标区 ============
        _renderDeviceIconSection: function() {
            var html = '<div class="dc-device-icon">';
            html += DEVICE_ICON_SVG;
            html += '<div class="dc-device-name">' + this._esc(this._deviceName) + '</div>';
            html += '</div>';
            return html;
        },

        // ============ 渲染控制操作区 ============
        _renderControlSection: function(data) {
            var html = '<div class="dc-section">';
            html += '<div class="dc-section-title">🎮 ' + this._t('device-control-action-section') + '</div>';

            // 分类获取控制项
            var gpioItems = this._filterByActionType(data.gpio, [0, 1, 2, 3, 4, 5, 13, 14]);
            var modbusCtrlItems = this._filterByActionType(data.modbus, [16, 17]);
            var systemItems = this._filterByActionType(data.system, [6, 7, 8, 9, 10, 11, 12]);
            var scriptItems = this._filterByActionType(data.script, [15]);
            var sensorReadItems = this._filterByActionType(data.sensor, [19]);
            var otherItems = data.other || [];

            // 按 name 属性排序
            gpioItems.sort(function(a, b) { return (a.name || '').localeCompare(b.name || ''); });
            modbusCtrlItems.sort(function(a, b) { return (a.name || '').localeCompare(b.name || ''); });
            systemItems.sort(function(a, b) { return (a.name || '').localeCompare(b.name || ''); });
            scriptItems.sort(function(a, b) { return (a.name || '').localeCompare(b.name || ''); });
            sensorReadItems.sort(function(a, b) { return (a.name || '').localeCompare(b.name || ''); });
            otherItems.sort(function(a, b) { return (a.name || '').localeCompare(b.name || ''); });

            var hasAnyControls = gpioItems.length > 0 || modbusCtrlItems.length > 0 ||
                systemItems.length > 0 || scriptItems.length > 0 ||
                sensorReadItems.length > 0 || otherItems.length > 0;

            if (hasAnyControls) {
                // GPIO 控制
                if (gpioItems.length > 0) {
                    html += this._renderControlGroup('GPIO', gpioItems, 'gpio', false);
                }
                // Modbus 控制
                if (modbusCtrlItems.length > 0) {
                    html += this._renderControlGroup('Modbus', modbusCtrlItems, 'modbus', false);
                }
                // 系统操作
                if (systemItems.length > 0) {
                    html += this._renderControlGroup('System', systemItems, 'system', true);
                }
                // 脚本执行
                if (scriptItems.length > 0) {
                    html += this._renderControlGroup('Script', scriptItems, 'script', false);
                }
                // 传感器读取
                if (sensorReadItems.length > 0) {
                    html += this._renderControlGroup('Sensor', sensorReadItems, 'sensor', false);
                }
                // 其他
                if (otherItems.length > 0) {
                    html += this._renderControlGroup('Other', otherItems, 'other', false);
                }
            } else {
                html += '<div class="dc-empty">' + this._t('device-control-no-action') + '</div>';
            }

            html += '</div>';
            return html;
        },

        // ============ 渲染控制分组 ============
        _renderControlGroup: function(titleKey, items, typeClass, isSystem) {
            var title = this._t('device-control-group-' + titleKey.toLowerCase()) || titleKey;
            var html = '<div class="dc-control-group">';
            html += '<div class="dc-control-group-title">' + title + '</div>';
            html += '<div class="dc-control-grid">';

            for (var i = 0; i < items.length; i++) {
                html += this._renderControlButton(items[i], typeClass, isSystem);
            }

            html += '</div></div>';
            return html;
        },

        // ============ 渲染控制按钮 ============
        _renderControlButton: function(item, typeClass, isSystem) {
            var btnClass = 'dc-ctrl-btn dc-' + typeClass;
            var dataAttrs = 'data-id="' + this._esc(item.id) + '"';
            if (isSystem) {
                dataAttrs += ' data-system="true" data-name="' + this._esc(item.name) + '"';
            }

            return '<button class="' + btnClass + '" ' + dataAttrs + '>' +
                this._esc(item.name) +
                '</button>';
        },

        // ============ 按 actionType 过滤 ============
        _filterByActionType: function(items, actionTypes) {
            if (!Array.isArray(items)) return [];
            if (!Array.isArray(actionTypes)) actionTypes = [actionTypes];

            var result = [];
            for (var i = 0; i < items.length; i++) {
                var item = items[i];
                if (actionTypes.indexOf(item.actionType) !== -1) {
                    result.push(item);
                }
            }
            return result;
        },

        // ============ 执行规则 ============
        _executeRule: function(ruleId, btn) {
            if (!ruleId) return;

            var originalText = btn.textContent;
            btn.textContent = this._t('device-control-executing');
            btn.disabled = true;
            btn.classList.add('dc-loading');

            var self = this;
            apiPost('/api/periph-exec/run?id=' + ruleId).then(function(res) {
                if (res && res.success) {
                    Notification.success(self._t('device-control-exec-success'));
                } else {
                    Notification.error(self._t('device-control-exec-fail'));
                }
            }).catch(function() {
                Notification.error(self._t('device-control-exec-fail'));
            }).then(function() {
                // finally 等效
                btn.textContent = originalText;
                btn.disabled = false;
                btn.classList.remove('dc-loading');
            });
        },

        // ============ 空状态 ============
        _renderEmptyState: function() {
            return '<div class="dc-section">' +
                '<div class="dc-section-title">📊 ' + this._t('device-control-monitor-section') + '</div>' +
                '<div class="dc-empty">' + this._t('device-control-no-monitor') + '</div>' +
                '</div>' +
                '<div class="dc-device-icon">' + DEVICE_ICON_SVG +
                '<div class="dc-device-name">' + this._esc(this._deviceName) + '</div>' +
                '</div>' +
                '<div class="dc-section">' +
                '<div class="dc-section-title">🎮 ' + this._t('device-control-action-section') + '</div>' +
                '<div class="dc-empty">' + this._t('device-control-no-action') + '</div>' +
                '</div>';
        },

        // ============ 安全翻译函数 ============
        _t: function(key) {
            if (typeof i18n !== 'undefined' && typeof i18n.t === 'function') {
                return i18n.t(key) || key;
            }
            return key;
        },

        // ============ HTML 转义 ============
        _esc: function(str) {
            if (str == null) return '';
            return String(str).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;').replace(/'/g, '&#39;');
        }
    });

    // 自动绑定事件（模块首次加载时）
    console.log('[device-control] Module loaded, checking setupDeviceControlEvents...');
    setTimeout(function() {
        if (typeof AppState.setupDeviceControlEvents === 'function') {
            AppState.setupDeviceControlEvents();
            console.log('[device-control] Events bound successfully');
        } else {
            console.warn('[device-control] setupDeviceControlEvents not found after registerModule');
        }
    }, 0);
})();
