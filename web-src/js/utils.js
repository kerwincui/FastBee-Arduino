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

    window.WebRuntimeDiagnostics = {
        ensurePanel: function(options) {
            options = options || {};
            if (document.getElementById('monitor-web-guard-level')) return;
            if (!options.anchorEl || !options.anchorEl.parentNode) return;

            var mount = document.createElement('div');
            mount.innerHTML = this._buildMarkup(options.t || function(key) { return key; });
            var fragment = document.createDocumentFragment();
            while (mount.firstChild) fragment.appendChild(mount.firstChild);
            options.anchorEl.parentNode.insertBefore(fragment, options.anchorEl);
        },

        apply: function(options, res) {
            options = options || {};
            if (!res || !res.data || typeof options.setText !== 'function' || typeof options.setHtml !== 'function') {
                return;
            }

            var t = options.t || function(key) { return key; };
            var formatBytesFn = options.formatBytes || window.formatBytes;
            var data = res.data || {};
            var memory = data.memory || {};
            var web = data.web || {};
            var sseStats = web.sseStats || {};
            var recovery = data.recovery || {};
            var nowMs = data.server && data.server.nowMs ? data.server.nowMs : 0;
            var guardLevel = String(memory.guardLevel || 'NORMAL').toUpperCase();

            if (typeof window.apiReportRuntimePressure === 'function') {
                window.apiReportRuntimePressure(data, 'web-runtime');
            }

            options.setHtml('monitor-web-guard-level', this._renderGuardBadge(guardLevel));
            options.setText('monitor-web-largest-block', formatBytesFn(memory.largestBlock || 0));
            options.setText('monitor-web-max-alloc', formatBytesFn(memory.maxAlloc || 0));
            options.setText('monitor-web-health', memory.healthReport || '--');
            options.setText('monitor-web-sse-clients', (web.sseClients || 0) + '/' + (web.sseMaxClients || 0));
            options.setText('monitor-web-sse-rejects', 'LM ' + (sseStats.rejectedLowMemory || 0) + ' | G ' + (sseStats.rejectedGuard || 0) + ' | C ' + (sseStats.rejectedCapacity || 0));
            options.setText('monitor-web-sse-skips', 'LM ' + (sseStats.skippedBroadcastLowMemory || 0) + ' | G ' + (sseStats.skippedBroadcastGuard || 0));
            options.setText(
                'monitor-web-last-reject',
                this._formatSummary(
                    sseStats.lastRejectReason,
                    (sseStats.lastRejectAtMs && nowMs >= sseStats.lastRejectAtMs) ? (nowMs - sseStats.lastRejectAtMs) : 0,
                    t
                )
            );
            options.setText('monitor-web-soft-restarts', recovery.softRestartCount || 0);
            options.setText('monitor-web-last-restart', this._formatSummary(recovery.lastSoftRestartReason, recovery.lastSoftRestartAgeMs, t));
            options.setText(
                'monitor-web-pressure',
                recovery.severePressureDurationMs > 0 ? this._formatAge(recovery.severePressureDurationMs, t) : t('dashboard-web-none')
            );
            options.setHtml('monitor-web-events', this._renderEvents(recovery.events || [], formatBytesFn, t));
        },

        _buildMarkup: function(t) {
            return '' +
                '<div class="dashboard-section-header">' +
                    '<div class="dashboard-section-title">' +
                        '<span class="dashboard-section-bar"></span>' +
                        '<span>' + t('dashboard-web-runtime') + '</span>' +
                    '</div>' +
                '</div>' +
                '<div class="dashboard-network-grid">' +
                    '<div class="info-card">' +
                        '<div class="info-card-header info-card-header-danger">' +
                            '<span class="icon-network"></span> <span>' + t('dashboard-web-guard') + '</span>' +
                        '</div>' +
                        '<div class="info-card-body">' +
                            '<table class="info-table"><tbody>' +
                                '<tr><td>' + t('dashboard-web-guard-level') + '</td><td id="monitor-web-guard-level">--</td></tr>' +
                                '<tr><td>' + t('dashboard-web-largest-block') + '</td><td id="monitor-web-largest-block">--</td></tr>' +
                                '<tr><td>' + t('dashboard-web-max-alloc') + '</td><td id="monitor-web-max-alloc">--</td></tr>' +
                                '<tr><td>' + t('dashboard-web-health') + '</td><td id="monitor-web-health">--</td></tr>' +
                            '</tbody></table>' +
                        '</div>' +
                    '</div>' +
                    '<div class="info-card">' +
                        '<div class="info-card-header info-card-header-primary">' +
                            '<span class="icon-network"></span> <span>' + t('dashboard-web-sse') + '</span>' +
                        '</div>' +
                        '<div class="info-card-body">' +
                            '<table class="info-table"><tbody>' +
                                '<tr><td>' + t('dashboard-web-clients') + '</td><td id="monitor-web-sse-clients">--</td></tr>' +
                                '<tr><td>' + t('dashboard-web-rejects') + '</td><td id="monitor-web-sse-rejects">--</td></tr>' +
                                '<tr><td>' + t('dashboard-web-skips') + '</td><td id="monitor-web-sse-skips">--</td></tr>' +
                                '<tr><td>' + t('dashboard-web-last-reject') + '</td><td id="monitor-web-last-reject">--</td></tr>' +
                            '</tbody></table>' +
                        '</div>' +
                    '</div>' +
                    '<div class="info-card">' +
                        '<div class="info-card-header info-card-header-success">' +
                            '<span class="icon-dashboard"></span> <span>' + t('dashboard-web-recovery') + '</span>' +
                        '</div>' +
                        '<div class="info-card-body">' +
                            '<table class="info-table"><tbody>' +
                                '<tr><td>' + t('dashboard-web-soft-restarts') + '</td><td id="monitor-web-soft-restarts">--</td></tr>' +
                                '<tr><td>' + t('dashboard-web-last-restart') + '</td><td id="monitor-web-last-restart">--</td></tr>' +
                                '<tr><td>' + t('dashboard-web-pressure') + '</td><td id="monitor-web-pressure">--</td></tr>' +
                            '</tbody></table>' +
                            '<div class="u-mt-8 u-fs-12 u-text-muted" id="monitor-web-events">--</div>' +
                        '</div>' +
                    '</div>' +
                '</div>';
        },

        _renderGuardBadge: function(level) {
            var toneMap = {
                NORMAL: 'success',
                WARN: 'warning',
                SEVERE: 'danger',
                CRITICAL: 'danger'
            };
            var tone = toneMap[level] || 'info';
            return '<span class="badge badge-' + tone + '">' + escapeHtml(level) + '</span>';
        },

        _formatSummary: function(reason, ageMs, t) {
            if (!reason && !ageMs) return t('dashboard-web-none');
            var parts = [];
            if (reason) parts.push(this._formatCode(reason, t));
            if (ageMs > 0) parts.push(this._formatAge(ageMs, t));
            return parts.join(' | ') || t('dashboard-web-none');
        },

        _formatCode: function(code, t) {
            if (!code) return t('dashboard-web-none');
            return String(code).replace(/_/g, ' ');
        },

        _formatAge: function(ms, t) {
            var value = Number(ms || 0);
            if (value <= 0) return t('dashboard-web-none');
            if (value < 1000) return value + 'ms';
            var sec = Math.round(value / 1000);
            if (sec < 60) return sec + 's';
            var min = Math.round(sec / 60);
            if (min < 60) return min + 'm';
            var hours = Math.round(min / 60);
            if (hours < 24) return hours + 'h';
            return Math.round(hours / 24) + 'd';
        },

        _renderEvents: function(events, formatBytesFn, t) {
            if (!Array.isArray(events) || events.length === 0) {
                return escapeHtml(t('dashboard-web-events') + ': ' + t('dashboard-web-none'));
            }
            var self = this;
            return events.slice().reverse().map(function(event) {
                var parts = [
                    escapeHtml(self._formatCode(event.type, t)),
                    escapeHtml(self._formatAge(event.ageMs, t)),
                    escapeHtml(formatBytesFn(event.largestBlock || 0))
                ];
                if (event.reason) {
                    parts.push(escapeHtml(self._formatCode(event.reason, t)));
                }
                return '<div>' + parts.join(' | ') + '</div>';
            }).join('');
        }
    };
})();
