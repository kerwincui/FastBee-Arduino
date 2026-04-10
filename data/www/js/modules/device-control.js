/**
 * 设备控制模块
 * 可视化操作面板，分为采集数据区和控制按钮区
 */
(function() {
    'use strict';
    
    console.log('[device-control] Module script loading...');
    
    AppState.registerModule('device-control', {
        _controlData: null,
        _eventsBound: false,

        // ============ 事件绑定 ============
        setupDeviceControlEvents() {
            console.log('[device-control] Setting up events...');
            var refreshBtn = document.getElementById('control-refresh-btn');
            if (refreshBtn) {
                // 移除旧的事件监听器（如果存在）
                var self = this;
                var newBtn = refreshBtn.cloneNode(true);
                refreshBtn.parentNode.replaceChild(newBtn, refreshBtn);
                newBtn.addEventListener('click', function() {
                    self.loadDeviceControlPage();
                });
                this._eventsBound = true;
                console.log('[device-control] Refresh button event bound');
            } else {
                console.warn('[device-control] Refresh button not found');
            }
        },

        // ============ 加载控制面板 ============
        loadDeviceControlPage() {
            console.log('[device-control] loadDeviceControlPage called');
            
            // 确保事件绑定
            if (!this._eventsBound) {
                this.setupDeviceControlEvents();
            }
            
            var panel = document.getElementById('device-control-panel');
            if (!panel) {
                console.error('[device-control] Panel element not found!');
                return;
            }
            panel.innerHTML = '<div class="dc-empty">⏳ ' + i18n.t('loading') + '</div>';

            var self = this;
            apiGet('/api/periph-exec/controls').then(function(res) {
                console.log('[device-control] API response:', res);
                // 检查响应有效性
                if (!res) {
                    console.log('[device-control] No response');
                    panel.innerHTML = self._renderEmpty();
                    return;
                }
                // 处理可能的错误响应
                if (res.success === false) {
                    console.log('[device-control] API returned failure:', res.error || res.message);
                    panel.innerHTML = '<div class="dc-empty" style="color:var(--danger);">❌ ' + (res.error || res.message || i18n.t('device-control-exec-fail')) + '</div>';
                    return;
                }
                // 获取数据 - 兼容多种格式
                var data = res.data || res;
                if (!data || typeof data !== 'object') {
                    console.log('[device-control] No valid data');
                    panel.innerHTML = self._renderEmpty();
                    return;
                }
                self._controlData = data;
                self._renderControlPanel(panel, data);
            }).catch(function(err) {
                console.error('[device-control] API error:', err);
                panel.innerHTML = '<div class="dc-empty" style="color:var(--danger);">❌ ' + i18n.t('device-control-exec-fail') + '</div>';
            });
        },

        // ============ 渲染控制面板 ============
        _renderControlPanel(panel, data) {
            console.log('[device-control] _renderControlPanel called with data:', data);
                    
            // 防御性数据处理
            data = data || {};
                    
            // 采集数据项：modbus + sensor
            var modbusItems = Array.isArray(data.modbus) ? data.modbus : [];
            var sensorItems = Array.isArray(data.sensor) ? data.sensor : [];
            var dataItems = modbusItems.concat(sensorItems);
                    
            // 控制按钮项：gpio + system + script + other
            var gpioItems = Array.isArray(data.gpio) ? data.gpio : [];
            var systemItems = Array.isArray(data.system) ? data.system : [];
            var scriptItems = Array.isArray(data.script) ? data.script : [];
            var otherItems = Array.isArray(data.other) ? data.other : [];
            var actionItems = gpioItems.concat(systemItems, scriptItems, otherItems);
        
            console.log('[device-control] dataItems:', dataItems.length, ', actionItems:', actionItems.length);
        
            var html = '';
        
            // === 采集数据区 ===
            html += '<div class="dc-section">';
            html += '<div class="dc-section-title">' + i18n.t('device-control-data-section') + '</div>';
            if (dataItems.length > 0) {
                html += '<div class="dc-data-grid">';
                for (var i = 0; i < dataItems.length; i++) {
                    html += this._renderDataItem(dataItems[i]);
                }
                html += '</div>';
            } else {
                html += '<div class="dc-empty">' + i18n.t('device-control-empty') + '</div>';
            }
            html += '</div>';
        
            // === 控制按钮区 ===
            html += '<div class="dc-section">';
            html += '<div class="dc-section-title">' + i18n.t('device-control-action-section') + '</div>';
            if (actionItems.length > 0) {
                html += '<div class="dc-action-grid">';
                for (var j = 0; j < actionItems.length; j++) {
                    html += this._renderActionButton(actionItems[j]);
                }
                html += '</div>';
            } else {
                html += '<div class="dc-empty">' + i18n.t('device-control-empty') + '</div>';
            }
            html += '</div>';
        
            panel.innerHTML = html;
            this._bindEvents(panel);
            console.log('[device-control] Panel rendered successfully');
        },

        // ============ 渲染数据项 ============
        _renderDataItem(item) {
            return '<div class="dc-data-item" data-id="' + this._esc(item.id) + '">'
                + '<span class="dc-data-name">' + this._esc(item.name) + '</span>'
                + '<span class="dc-data-val" id="dc-val-' + this._esc(item.id) + '">--</span>'
                + '</div>';
        },

        // ============ 渲染控制按钮 ============
        _renderActionButton(item) {
            // 判断是否为系统操作
            var isSystem = item.actionType === 2 || (item.group === 'system');
            var btnClass = isSystem ? 'dc-action-btn dc-btn-system' : 'dc-action-btn';
            var dataAttrs = 'data-id="' + this._esc(item.id) + '"';
            if (isSystem) {
                dataAttrs += ' data-system="true" data-name="' + this._esc(item.name) + '"';
            }
            return '<button class="' + btnClass + '" ' + dataAttrs + '>' + this._esc(item.name) + '</button>';
        },

        // ============ 绑定事件 ============
        _bindEvents(panel) {
            var self = this;
            panel.querySelectorAll('.dc-action-btn').forEach(function(btn) {
                btn.addEventListener('click', function() {
                    var ruleId = btn.dataset.id;
                    var isSystem = btn.dataset.system === 'true';
                    var ruleName = btn.dataset.name || '';
                    if (isSystem) {
                        if (confirm(i18n.t('device-control-confirm-system') + '\n' + ruleName)) {
                            self._executeRule(ruleId, btn);
                        }
                    } else {
                        self._executeRule(ruleId, btn);
                    }
                });
            });
        },

        // ============ 执行规则 ============
        _executeRule(ruleId, btn) {
            if (!ruleId) return;
            var originalText = btn.textContent;
            btn.textContent = i18n.t('device-control-executing');
            btn.disabled = true;

            apiPost('/api/periph-exec/run?id=' + ruleId).then(function(res) {
                if (res && res.success) {
                    Notification.success(i18n.t('device-control-exec-success'));
                } else {
                    Notification.error(i18n.t('device-control-exec-fail'));
                }
            }).catch(function() {
                Notification.error(i18n.t('device-control-exec-fail'));
            }).finally(function() {
                btn.textContent = originalText;
                btn.disabled = false;
            });
        },

        // ============ 空状态 ============
        _renderEmpty() {
            return '<div class="dc-section">'
                + '<div class="dc-section-title">' + i18n.t('device-control-data-section') + '</div>'
                + '<div class="dc-empty">' + i18n.t('device-control-empty') + '</div>'
                + '</div>'
                + '<div class="dc-section">'
                + '<div class="dc-section-title">' + i18n.t('device-control-action-section') + '</div>'
                + '<div class="dc-empty">' + i18n.t('device-control-empty') + '</div>'
                + '</div>';
        },

        // ============ HTML 转义 ============
        _esc(str) {
            if (!str) return '';
            var div = document.createElement('div');
            div.textContent = str;
            return div.innerHTML;
        }
    });

    // 自动绑定事件（模块首次加载时）
    console.log('[device-control] Module loaded, checking setupDeviceControlEvents...');
    // 使用 setTimeout 确保 registerModule 完全执行完毕
    setTimeout(function() {
        if (typeof AppState.setupDeviceControlEvents === 'function') {
            AppState.setupDeviceControlEvents();
            console.log('[device-control] Events bound successfully');
        } else {
            console.warn('[device-control] setupDeviceControlEvents not found after registerModule');
        }
    }, 0);
})();
