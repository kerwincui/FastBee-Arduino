/**
 * device-control/core.js — 状态管理、事件处理、页面加载、规则执行
 */
(function() {
    'use strict';

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

    // 电源图标 SVG
    var POWER_ICON_SVG = '<svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round"><path d="M12 2v8"/><path d="M18.4 6.6a9 9 0 1 1-12.8 0"/></svg>';

    // 将 SVG 常量挂到 AppState 上，供其他子模块使用
    AppState._DC_DEVICE_ICON_SVG = DEVICE_ICON_SVG;
    AppState._DC_POWER_ICON_SVG = POWER_ICON_SVG;

    // 注意：使用 Object.assign 而非 registerModule，避免子模块加载时过早触发回调
    //       最终 registerModule 调用由入口 device-control.js 在所有子模块加载完成后执行
    Object.assign(AppState, {
        _controlData: null,
        _modbusStatus: null,
        _modbusDevices: [],
        _dcCoilStates: {},
        _dcCoilPending: {},
        _dcBatchPending: false,
        _dcPwmStates: {},
        _dcPidValues: {},
        _dcDeviceOnline: {},
        _dcAutoRefreshTimers: {},
        _sseConnection: null,
        _deviceName: 'FastBee Device',
        _eventsBound: false,
        _periphExecRunPromptState: null,

        _dcZoomLevel: 1,
        _dcMinZoom: 0.5,
        _dcMaxZoom: 2.0,
        _dcZoomStep: 0.1,

        // ============ 事件绑定 ============
        setupDeviceControlEvents: function() {
            var self = this;

            // 全屏状态变更监听
            document.addEventListener('fullscreenchange', function() { self._dcOnFullscreenChange(); });
            document.addEventListener('webkitfullscreenchange', function() { self._dcOnFullscreenChange(); });

            var content = document.getElementById('dc-content');
            if (content && !this._eventsBound) {
                // 滚轮缩放（Ctrl+滚轮）
                content.addEventListener('wheel', function(e) {
                    if (e.ctrlKey) {
                        e.preventDefault();
                        if (e.deltaY < 0) self._dcZoomIn();
                        else self._dcZoomOut();
                    }
                }, { passive: false });

                content.addEventListener('click', function(e) {
                    if (e.target.closest('.dc-layout-reset')) { self._dcResetLayout(); return; }
                    if (e.target.closest('#dc-refresh-btn')) { self.loadDeviceControlPage(); return; }
                    if (e.target.closest('#dc-fullscreen-btn')) { self._dcToggleFullscreen(); return; }
                    if (e.target.closest('#dc-zoom-out-btn')) { self._dcZoomOut(); return; }
                    if (e.target.closest('#dc-zoom-reset-btn')) { self._dcZoomReset(); return; }
                    if (e.target.closest('#dc-zoom-in-btn')) { self._dcZoomIn(); return; }
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

                content.addEventListener('click', function(e) {
                    var coilCard = e.target.closest('.dc-coil-card');
                    if (coilCard) {
                        var devIdx = parseInt(coilCard.getAttribute('data-dev'));
                        var ch = parseInt(coilCard.getAttribute('data-ch'));
                        var key = devIdx + '_' + ch;
                        if (self._dcCoilPending[key]) return;
                        self._dcCoilPending[key] = true;
                        self._dcToggleCoil(devIdx, ch, coilCard);
                        return;
                    }
                    var batchBtn = e.target.closest('.dc-coil-batch');
                    if (batchBtn) {
                        var action = batchBtn.getAttribute('data-action');
                        var dIdx = parseInt(batchBtn.getAttribute('data-dev'));
                        if (self._dcBatchPending) return;
                        self._dcBatchPending = true;
                        batchBtn.disabled = true;
                        self._dcBatchCoil(dIdx, action, batchBtn);
                        return;
                    }
                    var refreshCoilBtn = e.target.closest('.dc-coil-refresh');
                    if (refreshCoilBtn) { self._dcRefreshCoilStatus(parseInt(refreshCoilBtn.getAttribute('data-dev'))); return; }
                    var delayStartBtn = e.target.closest('.dc-delay-start');
                    if (delayStartBtn) {
                        var dIdx = parseInt(delayStartBtn.getAttribute('data-dev'));
                        var section = delayStartBtn.closest('.dc-relay-delay-section');
                        var channelSelect = section.querySelector('.dc-delay-channel');
                        var delayInput = section.querySelector('.dc-delay-input');
                        if (channelSelect && delayInput) {
                            var channel = parseInt(channelSelect.value);
                            var delayUnits = parseInt(delayInput.value);
                            if (delayUnits >= 1 && delayUnits <= 255) {
                                self._dcStartCoilDelay(dIdx, channel, delayUnits);
                            }
                        }
                        return;
                    }
                    var pwmBatchBtn = e.target.closest('.dc-pwm-batch');
                    if (pwmBatchBtn) { self._dcBatchPwm(parseInt(pwmBatchBtn.getAttribute('data-dev')), pwmBatchBtn.getAttribute('data-action')); return; }
                    var refreshPwmBtn = e.target.closest('.dc-pwm-refresh');
                    if (refreshPwmBtn) { self._dcRefreshPwmStatus(parseInt(refreshPwmBtn.getAttribute('data-dev'))); return; }
                    var pidSetBtn = e.target.closest('.dc-pid-set');
                    if (pidSetBtn) {
                        var pidDevIdx = parseInt(pidSetBtn.getAttribute('data-dev'));
                        var pidParam = pidSetBtn.getAttribute('data-param');
                        var pidInput = pidSetBtn.parentNode.querySelector('.dc-pid-input');
                        if (pidInput) self._dcSetPidParam(pidDevIdx, pidParam, pidInput.value);
                        return;
                    }
                    var refreshPidBtn = e.target.closest('.dc-pid-refresh');
                    if (refreshPidBtn) { self._dcRefreshPidStatus(parseInt(refreshPidBtn.getAttribute('data-dev'))); return; }
                    var motorActionBtn = e.target.closest('.dc-motor-action');
                    if (motorActionBtn) { self._dcMotorAction(parseInt(motorActionBtn.getAttribute('data-dev')), motorActionBtn.getAttribute('data-action')); return; }
                    var motorSetBtn = e.target.closest('.dc-motor-set');
                    if (motorSetBtn) { self._dcMotorSet(parseInt(motorSetBtn.getAttribute('data-dev')), motorSetBtn.getAttribute('data-param')); return; }
                    var refreshMotorBtn = e.target.closest('.dc-motor-refresh');
                    if (refreshMotorBtn) { self._dcRefreshMotorStatus(parseInt(refreshMotorBtn.getAttribute('data-dev'))); return; }
                });

                content.addEventListener('input', function(e) {
                    if (e.target.classList.contains('dc-pwm-slider')) {
                        self._dcOnPwmSliderInput(parseInt(e.target.getAttribute('data-dev')), parseInt(e.target.getAttribute('data-ch')), parseInt(e.target.value));
                    }
                });
                content.addEventListener('change', function(e) {
                    if (e.target.classList.contains('dc-pwm-slider')) {
                        self._dcSetPwmChannel(parseInt(e.target.getAttribute('data-dev')), parseInt(e.target.getAttribute('data-ch')), parseInt(e.target.value));
                    }
                    if (e.target.classList.contains('dc-pwm-num')) {
                        self._dcSetPwmChannel(parseInt(e.target.getAttribute('data-dev')), parseInt(e.target.getAttribute('data-ch')), parseInt(e.target.value) || 0);
                    }
                    if (e.target.id === 'dc-sid-toggle') {
                        var show = e.target.checked;
                        localStorage.setItem('dc_show_sid', show ? '1' : '0');
                        var tags = document.querySelectorAll('.dc-sid-tag');
                        for (var t = 0; t < tags.length; t++) tags[t].style.display = show ? '' : 'none';
                    }
                });
            }

            this._eventsBound = true;

            // 键盘快捷键（仅设备大屏页面激活时触发）
            document.addEventListener('keydown', function(e) {
                var page = document.getElementById('device-control-page');
                if (!page || !page.classList.contains('active')) return;
                if (e.ctrlKey && e.key === 'f') {
                    e.preventDefault();
                    self._dcToggleFullscreen();
                }
                if (e.ctrlKey && (e.key === '+' || e.key === '=')) {
                    e.preventDefault();
                    self._dcZoomIn();
                }
                if (e.ctrlKey && e.key === '-') {
                    e.preventDefault();
                    self._dcZoomOut();
                }
                if (e.ctrlKey && e.key === '0') {
                    e.preventDefault();
                    self._dcZoomReset();
                }
            });
        },

        // ============ Periph-exec 运行值弹窗 ============
        _ensurePeriphExecRunValueModal: function() {
            var modal = document.getElementById('periph-exec-run-value-modal');
            if (modal) return modal;
            if (!document.body) return null;
            modal = document.createElement('div');
            modal.id = 'periph-exec-run-value-modal';
            modal.className = 'modal is-hidden';
            modal.innerHTML = '' +
                '<div class="modal-content u-modal-content-700">' +
                    '<div class="modal-header">' +
                        '<h2 class="modal-title" id="periph-exec-run-value-modal-title"></h2>' +
                        '<button type="button" id="close-periph-exec-run-value-modal" class="modal-close-btn">&times;</button>' +
                    '</div>' +
                    '<div class="modal-body">' +
                        '<div class="fb-form">' +
                            '<div class="fb-form-group">' +
                                '<label for="periph-exec-run-value-input" id="periph-exec-run-value-label"></label>' +
                                '<input type="text" id="periph-exec-run-value-input" class="" maxlength="128" autocomplete="off" spellcheck="false">' +
                                '<small id="periph-exec-run-value-help" class="pe-help-text"></small>' +
                            '</div>' +
                            '<div id="periph-exec-run-value-error" class="message message-error is-hidden"></div>' +
                        '</div>' +
                    '</div>' +
                    '<div class="modal-footer">' +
                        '<button class="fb-btn" id="cancel-periph-exec-run-value-btn" type="button"></button>' +
                        '<button class="fb-btn fb-btn-primary" id="confirm-periph-exec-run-value-btn" type="button"></button>' +
                    '</div>' +
                '</div>';
            document.body.appendChild(modal);
            var self = this;
            var closePrompt = function() { self._resolvePeriphExecRunValuePrompt(null); };
            modal.addEventListener('click', function(event) { if (event.target === modal) closePrompt(); });
            var closeBtn = document.getElementById('close-periph-exec-run-value-modal');
            if (closeBtn) closeBtn.addEventListener('click', function(event) { event.preventDefault(); closePrompt(); });
            var cancelBtn = document.getElementById('cancel-periph-exec-run-value-btn');
            if (cancelBtn) cancelBtn.addEventListener('click', function() { closePrompt(); });
            var confirmBtn = document.getElementById('confirm-periph-exec-run-value-btn');
            if (confirmBtn) confirmBtn.addEventListener('click', function() { self._submitPeriphExecRunValuePrompt(); });
            var input = document.getElementById('periph-exec-run-value-input');
            if (input) input.addEventListener('keydown', function(event) { if (event.key === 'Enter') { event.preventDefault(); self._submitPeriphExecRunValuePrompt(); } });
            return modal;
        },

        _setPeriphExecRunValuePromptError: function(message) {
            var errorEl = document.getElementById('periph-exec-run-value-error');
            if (!errorEl) return;
            errorEl.textContent = message || '';
            if (message) this.showElement(errorEl, 'block');
            else this.hideElement(errorEl);
        },

        _resolvePeriphExecRunValuePrompt: function(value) {
            var pending = this._periphExecRunPromptState;
            this._periphExecRunPromptState = null;
            this._setPeriphExecRunValuePromptError('');
            var input = document.getElementById('periph-exec-run-value-input');
            if (input) input.value = '';
            this.hideModal('periph-exec-run-value-modal');
            if (pending && typeof pending.resolve === 'function') pending.resolve(value);
        },

        _submitPeriphExecRunValuePrompt: function() {
            var input = document.getElementById('periph-exec-run-value-input');
            if (!input) { this._resolvePeriphExecRunValuePrompt(null); return; }
            var value = String(input.value || '').trim();
            if (!value.length) {
                this._setPeriphExecRunValuePromptError(i18n.t('periph-exec-set-value-required') || '请输入要设置的值');
                input.focus();
                return;
            }
            this._resolvePeriphExecRunValuePrompt(value);
        },

        promptPeriphExecRunValue: function(options) {
            var opts = options || {};
            var modal = this._ensurePeriphExecRunValueModal();
            if (!modal) {
                var fallbackValue = prompt(i18n.t('periph-exec-set-value-prompt') || '请输入要设置的值:', opts.defaultValue != null ? String(opts.defaultValue) : '');
                if (fallbackValue === null) return Promise.resolve(null);
                var normalizedValue = String(fallbackValue || '').trim();
                if (!normalizedValue.length) { Notification.warning(i18n.t('periph-exec-set-value-required') || '请输入要设置的值', this._t('device-control-exec-fail')); return Promise.resolve(null); }
                return Promise.resolve(normalizedValue);
            }
            var titleEl = document.getElementById('periph-exec-run-value-modal-title');
            var labelEl = document.getElementById('periph-exec-run-value-label');
            var helpEl = document.getElementById('periph-exec-run-value-help');
            var input = document.getElementById('periph-exec-run-value-input');
            var cancelBtn = document.getElementById('cancel-periph-exec-run-value-btn');
            var confirmBtn = document.getElementById('confirm-periph-exec-run-value-btn');
            if (titleEl) titleEl.textContent = opts.title || i18n.t('periph-exec-set-value-title') || '输入执行值';
            if (labelEl) labelEl.textContent = opts.label || i18n.t('periph-exec-set-value-label') || '执行值';
            var helpText = opts.helpText || i18n.t('periph-exec-set-value-help') || '请输入本次执行要设置的值';
            if (opts.ruleName) helpText = opts.ruleName + ' - ' + helpText;
            if (helpEl) helpEl.textContent = helpText;
            if (cancelBtn) cancelBtn.textContent = i18n.t('cancel') || '取消';
            if (confirmBtn) confirmBtn.textContent = opts.confirmText || i18n.t('periph-exec-run-once') || '执行一次';
            if (input) { input.value = opts.defaultValue != null ? String(opts.defaultValue) : ''; input.placeholder = opts.placeholder || i18n.t('periph-exec-set-value-placeholder') || ''; }
            this._setPeriphExecRunValuePromptError('');
            if (this._periphExecRunPromptState && typeof this._periphExecRunPromptState.resolve === 'function') this._periphExecRunPromptState.resolve(null);
            return new Promise((resolve) => {
                this._periphExecRunPromptState = { resolve: resolve };
                this.showModal(modal);
                setTimeout(function() { if (input) { input.focus(); input.select(); } }, 0);
            });
        },

        // ============ 加载控制面板 ============
        loadDeviceControlPage: function() {
            this._modbusDevices = [];
            this._dcDeviceOnline = {};
            if (!this._eventsBound) this.setupDeviceControlEvents();
            var content = document.getElementById('dc-content');
            if (!content) { console.error('[device-control] Content element "dc-content" not found!'); return; }
            this._setContentState(content, 'loading');
            var self = this;
            this._dcStopAllAutoRefresh();
            this._fetchDeviceInfo().then(function() {
                return apiGet('/api/periph-exec/controls');
            }).then(function(controlsRes) {
                self._tempControlsRes = controlsRes;
                return apiGetSilent('/api/modbus/status').catch(function() { return null; });
            }).then(function(modbusRes) {
                self._tempModbusRes = modbusRes;
                return apiGetSilent('/api/protocol/config').catch(function() { return null; });
            }).then(function(protoRes) {
                var res = self._tempControlsRes;
                var modbusRes = self._tempModbusRes;
                delete self._tempControlsRes;
                delete self._tempModbusRes;
                if (modbusRes && modbusRes.success && modbusRes.data) self._modbusStatus = modbusRes.data;
                else self._modbusStatus = null;
                self._modbusDevices = [];
                if (protoRes && protoRes.success && protoRes.data) {
                    var rtu = protoRes.data.modbusRtu;
                    if (rtu && rtu.enabled && rtu.master && rtu.master.devices) {
                        self._modbusDevices = rtu.master.devices.filter(function(d) { return d.enabled !== false; });
                    }
                }
                if (!res) { self._setContentState(content, 'empty'); return; }
                if (res.success === false) { self._setContentState(content, 'error', res.error || res.message || '请求失败'); return; }
                var data = res.data || res;
                if (!data || typeof data !== 'object') { self._setContentState(content, 'empty'); return; }
                self._controlData = data;
                try {
                    var html = self._renderControlPanel(data);
                    content.innerHTML = html;
                    self._dcApplyZoom();
                    self._dcApplyLayout();
                    self._dcInitFreeLayout();
                    self._dcInitModbusDeviceStates();
                    if (self.currentPage === 'device-control') self._setupSSE();
                    if (self._modbusDevices.length === 0) {
                        setTimeout(function() {
                            if (self.currentPage !== 'device-control') return;
                            apiGetSilent('/api/protocol/config').then(function(retryRes) {
                                if (!retryRes || !retryRes.success || !retryRes.data) return;
                                var rtu = retryRes.data.modbusRtu;
                                if (rtu && rtu.enabled && rtu.master && rtu.master.devices) {
                                    var newDevices = rtu.master.devices.filter(function(d) { return d.enabled !== false; });
                                    if (newDevices.length > 0) { self._modbusDevices = newDevices; self.loadDeviceControlPage(); }
                                }
                            }).catch(function() {});
                        }, 2000);
                    }
                } catch (renderErr) {
                    console.error('[device-control] Render error:', renderErr);
                    self._setContentState(content, 'error', renderErr.message || renderErr);
                }
            }).catch(function(err) {
                var errMsg = '请求失败';
                if (err && err.data && err.data.error) errMsg = err.data.error;
                else if (err && err.message) errMsg = err.message;
                self._setContentState(content, 'error', errMsg);
            });
        },

        _fetchDeviceInfo: function() {
            var self = this;
            return apiGetSilent('/api/device/config').then(function(res) {
                if (res && res.success && res.data) self._deviceName = res.data.deviceName || res.data.name || 'FastBee Device';
            }).catch(function() { self._deviceName = 'FastBee Device'; });
        },

        // ============ 执行规则 ============
        _executeRule: function(ruleId, btn) {
            if (!ruleId) return;
            var self = this;
            var hasSetMode = btn && btn.getAttribute('data-has-set-mode') === 'true';
            var ruleName = btn ? (btn.getAttribute('data-name') || btn.textContent || '') : '';
            var doRun = function(value) {
                var originalText = btn.textContent;
                btn.textContent = self._t('device-control-executing');
                btn.disabled = true;
                btn.classList.add('dc-loading');
                var payload = { id: ruleId };
                if (value !== undefined && value !== '') payload.value = value;
                apiPost('/api/periph-exec/run', payload).then(function(res) {
                    if (res && res.success) Notification.success(self._t('device-control-exec-success'));
                    else Notification.error((res && (res.error || res.message)) || self._t('device-control-exec-fail'));
                }).catch(function() {
                    Notification.error(self._t('device-control-exec-fail'));
                }).then(function() {
                    btn.textContent = originalText;
                    btn.disabled = false;
                    btn.classList.remove('dc-loading');
                });
            };
            if (hasSetMode) {
                this.promptPeriphExecRunValue({ ruleName: ruleName || '' }).then(function(inputValue) {
                    if (inputValue === null) return;
                    doRun(inputValue);
                });
            } else {
                doRun('');
            }
        },

        // ============ Modbus 设备初始化 ============
        _dcModbusInitRunning: false,
        _dcInitCancelled: false,

        _dcCancelInit: function() {
            if (this._dcModbusInitRunning) {
                this._dcInitCancelled = true;

            }
        },

        _dcInitModbusDeviceStates: async function() {
            if (this._dcModbusInitRunning) return;
            this._dcModbusInitRunning = true;
            this._dcInitCancelled = false;
            var devices = this._modbusDevices || [];
            try {
                for (var i = 0; i < devices.length; i++) {
                    if (this.currentPage !== 'device-control') break;
                    if (this._dcInitCancelled) break;
                    var dt = devices[i].deviceType || 'relay';
                    if (dt === 'relay') await this._dcRefreshCoilStatus(i);
                    else if (dt === 'pwm') await this._dcRefreshPwmStatus(i);
                    else if (dt === 'pid') await this._dcRefreshPidStatus(i);
                    else if (dt === 'motor') await this._dcRefreshMotorStatus(i);
                    if (i < devices.length - 1) await new Promise(function(resolve) { setTimeout(resolve, 150); });
                }
            } finally {
                this._dcModbusInitRunning = false;
                this._dcInitCancelled = false;
            }
        },

        _dcStopAllAutoRefresh: function() {
            var timers = this._dcAutoRefreshTimers || {};
            this._closeSSE();
            for (var key in timers) {
                if (timers.hasOwnProperty(key) && timers[key]) {
                    clearInterval(timers[key]);
                    clearTimeout(timers[key]);
                }
            }
            this._dcAutoRefreshTimers = {};
        },

        // ============ 按 actionType 过滤 ============
        _filterByActionType: function(items, actionTypes) {
            if (!Array.isArray(items)) return [];
            if (!Array.isArray(actionTypes)) actionTypes = [actionTypes];
            var result = [];
            for (var i = 0; i < items.length; i++) {
                if (actionTypes.indexOf(items[i].actionType) !== -1) result.push(items[i]);
            }
            return result;
        },

        // ============ 内容状态渲染辅助方法 ============
        _setContentState: function(container, type, message) {
            if (type === 'empty') {
                container.innerHTML = this._renderEmptyState();
            } else if (type === 'loading') {
                var loadingText = message || (typeof i18n !== 'undefined' ? i18n.t('loading') : '加载中...');
                container.innerHTML = '<div class="dc-empty">' + escapeHtml(loadingText) + '</div>';
            } else if (type === 'error') {
                container.innerHTML = '<div class="dc-empty u-text-danger">' + escapeHtml(message || '请求失败') + '</div>';
            }
        },

        _renderEmptyState: function() {
            return '<div class="dc-device-banner">' + AppState._DC_DEVICE_ICON_SVG +
                '<div class="dc-device-banner-info">' +
                '<div class="dc-device-name">' + escapeHtml(this._deviceName) + '</div>' +
                '<div class="dc-device-status">' + this._t('device-control-online') + '</div>' +
                '</div></div>' +
                '<div class="dc-section">' +
                '<div class="dc-section-title">' + this._t('device-control-dashboard') + '</div>' +
                '<div class="dc-empty">' + this._t('device-control-no-monitor') + '</div>' +
                '</div>' +
                '<div class="dc-section">' +
                '<div class="dc-section-title">' + this._t('device-control-action-section') + '</div>' +
                '<div class="dc-empty">' + this._t('device-control-no-action') + '</div>' +
                '</div>';
        },

        // ============ 安全翻译函数 ============
        // _t 直接委托 i18n.t()，i18n.t() 已内置 key 回退逻辑
        _t: function(key) { return i18n.t(key); },

        // ============ 全屏功能 ============

        /**
         * 切换全屏模式
         */
        _dcToggleFullscreen: function() {
            var el = document.getElementById('app-container');
            if (!el) return;

            var isFullscreen = !!(document.fullscreenElement || document.webkitFullscreenElement);

            if (!isFullscreen) {
                // 进入全屏
                var req = el.requestFullscreen || el.webkitRequestFullscreen;
                if (req) {
                    req.call(el).catch(function(err) {
                        // Fullscreen API 不可用时，降级为 CSS-only 全屏
                        console.warn('[DeviceControl] Fullscreen API failed, using CSS fallback:', err);
                        el.classList.add('fullscreen-mode');
                        AppState._dcUpdateFullscreenBtn(true);
                    });
                } else {
                    el.classList.add('fullscreen-mode');
                    AppState._dcUpdateFullscreenBtn(true);
                }
            } else {
                // 退出全屏
                var exit = document.exitFullscreen || document.webkitExitFullscreen;
                if (exit) {
                    exit.call(document).catch(function() {});
                }
                el.classList.remove('fullscreen-mode');
                AppState._dcUpdateFullscreenBtn(false);
            }
        },

        /**
         * 全屏状态变更回调
         */
        _dcOnFullscreenChange: function() {
            var el = document.getElementById('app-container');
            var isFullscreen = !!(document.fullscreenElement || document.webkitFullscreenElement);

            if (isFullscreen) {
                if (el) el.classList.add('fullscreen-mode');
            } else {
                if (el) el.classList.remove('fullscreen-mode');
            }
            AppState._dcUpdateFullscreenBtn(isFullscreen);
        },

        _dcUpdateFullscreenBtn: function(isFullscreen) {
            var btn = document.getElementById('dc-fullscreen-btn');
            if (!btn) return;
            if (isFullscreen) {
                btn.textContent = i18n.t('dashboard-exit-fullscreen');
                btn.classList.add('dc-btn-active');
            } else {
                btn.textContent = i18n.t('dashboard-fullscreen');
                btn.classList.remove('dc-btn-active');
            }
        },

        // ============ 缩放功能 ============
        _dcGetZoom: function() {
            var z = parseFloat(localStorage.getItem('dc_zoom_level'));
            return isFinite(z) && z >= 0.5 && z <= 2.0 ? z : 1;
        },

        _dcSetZoom: function(zoom) {
            zoom = Math.max(0.5, Math.min(2.0, Math.round(zoom * 10) / 10));
            this._dcZoomLevel = zoom;
            try { localStorage.setItem('dc_zoom_level', String(zoom)); } catch(e) {}
            this._dcApplyZoom();
        },

        _dcApplyZoom: function() {
            var wrapper = document.querySelector('.dc-zoom-wrapper');
            if (!wrapper) return;
            var zoom = this._dcZoomLevel || this._dcGetZoom();
            this._dcZoomLevel = zoom;
            wrapper.style.setProperty('--dc-zoom', zoom);
            wrapper.style.transform = 'scale(' + zoom + ')';
            wrapper.style.transformOrigin = '0 0';
            var resetBtn = document.getElementById('dc-zoom-reset-btn');
            if (resetBtn) resetBtn.textContent = Math.round(zoom * 100) + '%';
        },

        _dcZoomIn: function() {
            this._dcSetZoom((this._dcZoomLevel || this._dcGetZoom()) + 0.1);
        },

        _dcZoomOut: function() {
            this._dcSetZoom((this._dcZoomLevel || this._dcGetZoom()) - 0.1);
        },

        _dcZoomReset: function() {
            this._dcSetZoom(1);
        }
    });

    // 事件绑定由入口文件 device-control.js 在所有子模块加载完成后统一调用
})();
