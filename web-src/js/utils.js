/**
 * FastBee UI 工具函数
 */
(function() {
    'use strict';

    // HTML 转义（统一替代各模块的 _esc() 函数）
    window.escapeHtml = function(text) {
        if (text == null) return '';
        var map = { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' };
        return String(text).replace(/[&<>"']/g, function(m) { return map[m]; });
    };

    // 安全设置文本内容
    window.safeText = function(el, text) {
        if (el) el.textContent = text != null ? String(text) : '';
    };

    // 安全设置 HTML（已转义）
    window.safeHtml = function(el, html) {
        if (el) el.innerHTML = html;
    };

    /**
     * 字节数格式化（自动选择 B/KB/MB/GB 单位）
     * @param {number} bytes - 字节数
     * @returns {string} 格式化后的字符串，如 "1.25 MB"
     */
    window.formatBytes = function(bytes) {
        if (!bytes || bytes === 0) return '0 B';
        var k = 1024;
        var sizes = ['B', 'KB', 'MB', 'GB'];
        var i = Math.floor(Math.log(bytes) / Math.log(k));
        if (i >= sizes.length) i = sizes.length - 1;
        return parseFloat((bytes / Math.pow(k, i)).toFixed(2)) + ' ' + sizes[i];
    };

    /**
     * 运行时间格式化（秒数转为 Xh Xm Xs）
     * @param {number} seconds - 秒数
     * @returns {string} 格式化后的字符串，如 "2h 15m 30s"
     */
    window.formatUptime = function(seconds) {
        if (seconds == null || seconds < 0) return '--';
        seconds = Math.floor(seconds);
        var h = Math.floor(seconds / 3600);
        var m = Math.floor((seconds % 3600) / 60);
        var s = seconds % 60;
        if (h > 0) return h + 'h ' + m + 'm ' + s + 's';
        if (m > 0) return m + 'm ' + s + 's';
        return s + 's';
    };

    /**
     * 防抖函数
     * @param {Function} fn - 需要防抖的函数
     * @param {number} delay - 延迟毫秒数（默认 300）
     * @returns {Function} 防抖后的函数
     */
    window.debounce = function(fn, delay) {
        var timer = null;
        delay = delay || 300;
        return function() {
            var ctx = this, args = arguments;
            if (timer) clearTimeout(timer);
            timer = setTimeout(function() {
                fn.apply(ctx, args);
            }, delay);
        };
    };

    /**
     * 数值限制在 [min, max] 范围内
     * @param {number} value - 输入值
     * @param {number} min - 最小值
     * @param {number} max - 最大值
     * @returns {number}
     */
    window.clamp = function(value, min, max) {
        return Math.max(min, Math.min(max, value));
    };

    /**
     * 深拷贝（基于 JSON 序列化，适用于纯数据对象）
     * @param {*} obj - 需要拷贝的对象
     * @returns {*} 深拷贝后的新对象
     */
    window.deepClone = function(obj) {
        if (obj == null || typeof obj !== 'object') return obj;
        return JSON.parse(JSON.stringify(obj));
    };


    // Installed by either lazy page; existing dialog/pending state stays on AppState.
    window.PeriphExecRunValuePrompt = {
        _manualOutputMessage: function(result) {
            var control = result?.control || result?.data?.control, reason = control?.reason;
            if (!reason || ['CONTROL_NOT_ATTEMPTED', 'CONTROL_INVALID', 'CONTROL_STATUS'].includes(reason))
                reason = result?.reason || result?.errorCode || result?.error || result?.message || '';
            var labels = {CONTROL_CONFLICT: '输出正在被其它来源占用',
                CONTROL_DEDICATED_OWNER: '输出由专用控制器持有，不能手动接管',
                CONTROL_REVIEW_STALE: '占用状态已变化，请重新核对',
                CONTROL_STOP_UNCONFIRMED: '旧输出停止尚未确认',
                CONTROL_OUTPUT_UNCONFIRMED: '输出结果未确认',
                CONTROL_SOURCE_STALE: '配置已变化，本次输出已拒绝'};
            var text = control?.attemptKnown === false ? '输出结果待确认，是否已执行尚未知' : labels[reason] || reason;
            if (Number.isInteger(control?.backendOperationId) && control.backendOperationId > 0)
                text += ' · 电机操作 ' + control.backendOperationId;
            if (Number.isInteger(control?.acknowledgedSteps) && control.acknowledgedSteps > 0)
                text += ' · 已应答 ' + control.acknowledgedSteps + ' 步';
            if (control?.targetId) text += ' · ' + control.targetId;
            var sources = {rule: '规则', mqtt: 'MQTT', http: '网页会话', screen: '屏幕卡片'};
            if (sources[control?.source]) text += ' · 当前来源：' + sources[control.source];
            if (Number.isFinite(control?.holdRemainingMs) && control.holdRemainingMs > 0)
                text += ' · 查询时剩余占用约' + Math.ceil(control.holdRemainingMs / 1000) + '秒';
            return (text || '执行结果未确认') + '；未验证物理负载，不会自动重试输出';
        },
        _observeManualOutput: async function(result, active) {
            if (!active()) return;
            var operation = result?.operationId, current = result;
            if (result?.success !== true) { Notification.error(this._manualOutputMessage(result)); return; }
            Notification.success('执行已提交；输出结果待确认');
            if (!Number.isInteger(operation) || operation < 1) return;
            for (var attempt = 0; attempt < 3; ++attempt) {
                if (!active()) return;
                if (['success', 'failed', 'rejected', 'cancelled', 'timed_out'].includes(current.state)) {
                    if (current.state !== 'success') Notification.error(this._manualOutputMessage(current));
                    else Notification.success('执行完成；本机结果不代表物理负载反馈');
                    return;
                }
                await new Promise(resolve => setTimeout(resolve, 400 * (attempt + 1)));
                if (!active()) return;
                try {
                    var response = await (typeof apiGetFresh === 'function' ? apiGetFresh : apiGet)(
                        '/api/periph-exec/results', {operationId: operation, limit: 1});
                    if (!active()) return;
                    if (response?.success !== true || !Array.isArray(response.data)) throw new Error('执行状态响应无效');
                    current = response.data.find(item => item.operationId === operation);
                    if (!current) throw new Error('执行记录尚未确认');
                } catch (error) {
                    if (active()) Notification.error('执行结果未确认；请查看执行记录，不会自动重试输出');
                    return;
                }
            }
            if (!active()) return;
            if (current.state === 'success') Notification.success('执行完成；本机结果不代表物理负载反馈');
            else if (['failed', 'rejected', 'cancelled', 'timed_out'].includes(current.state))
                Notification.error(this._manualOutputMessage(current));
            else Notification.error('执行尚未确认完成；请查看执行记录，不会自动重试输出');
        },
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
                this._setPeriphExecRunValuePromptError('请输入要设置的值');
                input.focus();
                return;
            }
            this._resolvePeriphExecRunValuePrompt(value);
        },

        _promptPeriphExecRunValue: async function(options, defaults) {
            var opts = options || {};
            var modal = this._ensurePeriphExecRunValueModal();
            if (!modal) {
                var fallbackValue = await FBConfirm({ title: '输入执行值', message: String(defaults.fallbackMessage), input: { label: '执行值', value: opts.defaultValue != null ? String(opts.defaultValue) : '' } });
                if (fallbackValue === null) return Promise.resolve(null);
                var normalizedValue = String(fallbackValue || '').trim();
                if (!normalizedValue.length) { Notification.warning('请输入要设置的值', defaults.warningTitle); return Promise.resolve(null); }
                return Promise.resolve(normalizedValue);
            }
            var titleEl = document.getElementById('periph-exec-run-value-modal-title');
            var labelEl = document.getElementById('periph-exec-run-value-label');
            var helpEl = document.getElementById('periph-exec-run-value-help');
            var input = document.getElementById('periph-exec-run-value-input');
            var cancelBtn = document.getElementById('cancel-periph-exec-run-value-btn');
            var confirmBtn = document.getElementById('confirm-periph-exec-run-value-btn');
            if (titleEl) titleEl.textContent = opts.title || '输入执行值';
            if (labelEl) labelEl.textContent = opts.label || '执行值';
            var helpText = opts.helpText || '请输入本次执行要设置的值，例如 PWM 占空比、PID 参数或通道值。';
            if (opts.ruleName) helpText = opts.ruleName + ' - ' + helpText;
            if (helpEl) helpEl.textContent = helpText;
            if (cancelBtn) cancelBtn.textContent = '取消';
            if (confirmBtn) confirmBtn.textContent = opts.confirmText || '执行一次';
            if (input) { input.value = opts.defaultValue != null ? String(opts.defaultValue) : ''; input.placeholder = opts.placeholder || defaults.placeholder; }
            this._setPeriphExecRunValuePromptError('');
            if (this._periphExecRunPromptState && typeof this._periphExecRunPromptState.resolve === 'function') this._periphExecRunPromptState.resolve(null);
            return new Promise((resolve) => {
                this._periphExecRunPromptState = { resolve: resolve };
                this.showModal(modal);
                setTimeout(function() { if (input) { input.focus(); input.select(); } }, 0);
            });
        },
    };

    // Shared with both protocol status and device-control views. Values arriving
    // through mapped SSE are already engineering values and bypass this codec.
    window.decodeModbusMapping = function(words, mapping) {
        if (!mapping || typeof mapping !== 'object') return null;
        var offset = mapping.regOffset === undefined ? 0 : mapping.regOffset;
        var type = mapping.dataType === undefined ? 0 : mapping.dataType;
        var order = mapping.wordOrder === undefined ? 0 : mapping.wordOrder;
        var scale = mapping.scaleFactor === undefined ? 1 : mapping.scaleFactor;
        var count = type >= 2 ? 2 : 1;
        if (!Array.isArray(words) || !Number.isInteger(offset) || offset < 0 ||
            !Number.isInteger(type) || type < 0 || type > 4 || ![0, 1].includes(order) ||
            (type < 2 && order) || typeof scale !== 'number' || !Number.isFinite(scale) ||
            Math.abs(scale) > 3.4028234663852886e38 || offset + count > words.length) return null;
        if (scale !== 0 && Math.fround(scale) === 0) return null;
        var raw = words[offset];
        if (!Number.isInteger(raw) || raw < 0 || raw > 65535) return null;
        if (type === 1 && raw >= 32768) raw -= 65536;
        if (count === 2) {
            var low = words[offset + 1];
            if (!Number.isInteger(low) || low < 0 || low > 65535) return null;
            raw = order ? low * 65536 + raw : raw * 65536 + low;
            if (type === 3 && raw >= 2147483648) raw -= 4294967296;
            if (type === 4) {
                var view = new DataView(new ArrayBuffer(4));
                view.setUint32(0, raw); raw = view.getFloat32(0);
            }
        }
        var value = raw * Math.fround(scale);
        return Number.isFinite(raw) && Number.isFinite(value) ? value : null;
    };
    window.formatModbusMapping = function(words, mapping) {
        if (!mapping || typeof mapping !== 'object') return '--';
        var decimals = mapping.decimalPlaces === undefined ? 1 : mapping.decimalPlaces;
        var value = window.decodeModbusMapping(words, mapping);
        return value !== null && Number.isInteger(decimals) && decimals >= 0 && decimals <= 6 ? value.toFixed(decimals) : '--';
    };


/**
     * 危险操作共享确认弹窗：标题（动作名+对象）+ 风险摘要 + 确认/取消。
     * 复用 AppState.showModal/hideModal 的 ARIA 与焦点陷阱能力（state-ui.js）。
     * options: { title, message, confirmText?, cancelText?, danger?,
     *            requireCheck?, checkLabel?, input?: { label, value?, password? } }
     * 无 input → Promise<boolean>；有 input → Promise<string|null>。
     */
    window.FBConfirm = function(options) {
        options = options || {};
        var title = String(options.title || '确认操作');
        var message = String(options.message || '');
        var confirmText = String(options.confirmText || '确认');
        var cancelText = String(options.cancelText || '取消');
        var danger = options.danger === true;
        var requireCheck = options.requireCheck === true;
        var checkLabel = String(options.checkLabel || '我已了解风险，确认继续');
        var input = options.input || null;

        return new Promise(function(resolve) {
            if (typeof AppState === 'undefined' || typeof AppState.showModal !== 'function') {
                resolve(input ? null : false);
                return;
            }
            var overlay = document.createElement('div');
            overlay.className = 'modal';
            overlay.setAttribute('role', 'dialog');
            overlay.setAttribute('aria-modal', 'true');
            overlay.setAttribute('aria-labelledby', 'fb-confirm-title');
            overlay.id = 'fb-confirm-modal';
            var safeText = function(s) { return String(s).replace(/[&<>"]/g, function(c) {
                return { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]; }); };
            var messageHtml = safeText(message).replace(/\n/g, '<br>');
            overlay.innerHTML =
                '<div class="modal-content">' +
                '  <div class="modal-header"><h2 class="modal-title" id="fb-confirm-title">' + safeText(title) + '</h2>' +
                '  <button type="button" class="modal-close-btn" data-fbc-close aria-label="关闭">×</button></div>' +
                '  <div class="modal-body"><p style="margin:0 0 10px;line-height:1.6;">' + messageHtml + '</p>' +
                (input ? '    <div class="fb-form-group"><label for="fb-confirm-input">' + safeText(String(input.label || '请输入')) + '</label>' +
                '    <input type="' + (input.password ? 'password' : 'text') + '" id="fb-confirm-input" class="" value="' +
                safeText(String(input.value || '')) + '"></div>' : '') +
                (requireCheck ? '    <label style="display:flex;align-items:center;gap:8px;margin-top:10px;font-size:13px;">' +
                '    <input type="checkbox" id="fb-confirm-check">' + safeText(checkLabel) + '</label>' : '') +
                '  </div>' +
                '  <div class="modal-footer">' +
                '    <button type="button" class="fb-btn" data-fbc-close>' + safeText(cancelText) + '</button>' +
                '    <button type="button" class="fb-btn ' + (danger ? 'fb-btn-danger' : 'fb-btn-primary') + '" id="fb-confirm-ok"' +
                (requireCheck ? ' disabled' : '') + '>' + safeText(confirmText) + '</button>' +
                '  </div>' +
                '</div>';
            document.body.appendChild(overlay);

            var settled = false;
            var done = function(result) {
                if (settled) return;
                settled = true;
                AppState.hideModal(overlay);
                overlay.remove();
                resolve(result);
            };
            var okBtn = overlay.querySelector('#fb-confirm-ok');
            var check = overlay.querySelector('#fb-confirm-check');
            if (check && okBtn) {
                check.addEventListener('change', function() { okBtn.disabled = !check.checked; });
            }
            overlay.querySelectorAll('[data-fbc-close]').forEach(function(el) {
                el.addEventListener('click', function() { done(input ? null : false); });
            });
            if (okBtn) {
                okBtn.addEventListener('click', function() {
                    if (input) {
                        var field = overlay.querySelector('#fb-confirm-input');
                        done(field ? field.value : '');
                    } else {
                        done(true);
                    }
                });
            }
            var field = overlay.querySelector('#fb-confirm-input');
            if (field) {
                field.addEventListener('keydown', function(e) {
                    if (e.key === 'Enter') { e.preventDefault(); done(field.value); }
                });
            }
            AppState.showModal(overlay);
            if (requireCheck && check) check.focus();
            else if (field) field.focus();
        });
    };

})();
