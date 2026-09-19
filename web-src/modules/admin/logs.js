(function() {
    AppState.registerModule('logs', {
        _logFileCount: null,
        _logCurrentSize: null,
        _logCurrentTruncated: false,
        _logListLoadSeq: 0,
        _logContentLoadSeq: 0,

        _formatLogSize(bytes) {
            const value = Number(bytes || 0);
            if (value < 1024) return value + ' B';
            if (value < 1024 * 1024) return (value / 1024).toFixed(value < 10 * 1024 ? 1 : 0) + ' KB';
            return (value / (1024 * 1024)).toFixed(1) + ' MB';
        },

        _updateLogToolbarSummary(errorText) {
            const summary = document.getElementById('log-meta');
            if (!summary) return;
            summary.classList.toggle('is-warning', !!errorText);
            if (errorText) {
                summary.textContent = errorText;
                return;
            }
            if (this._logFileCount === null && this._logCurrentSize === null) {
                summary.textContent = '加载中...';
                return;
            }
            const fileCount = this._logFileCount === null ? 0 : this._logFileCount;
            const size = this._logCurrentSize === null ? '--' : this._formatLogSize(this._logCurrentSize);
            summary.textContent = fileCount + ' 个文件 · 当前 ' + size +
                (this._logCurrentTruncated ? ' · 已截断' : '');
        },

        _setLogBusy(kind, busy) {
            const buttonId = kind === 'list' ? 'log-refresh-list-btn' : 'refresh-logs-btn';
            const button = document.getElementById(buttonId);
            if (button) {
                button.disabled = busy;
                button.setAttribute('aria-busy', busy ? 'true' : 'false');
            }
            const content = document.getElementById('log-content');
            if (kind === 'content' && content) {
                content.setAttribute('aria-busy', busy ? 'true' : 'false');
            }
        },

        setupLogsEvents() {
            if (this._logsEventsBound) return;
            this._logsEventsBound = true;

            const refreshListBtn = document.getElementById('log-refresh-list-btn');
            if (refreshListBtn) refreshListBtn.addEventListener('click', () => {
                this.loadLogFileList({ noCache: true }).then(() => this.loadLogs({ noCache: true }));
            });

            const refreshBtn = document.getElementById('refresh-logs-btn');
            if (refreshBtn) refreshBtn.addEventListener('click', () => this.loadLogs({ noCache: true }));

            const clearBtn = document.getElementById('clear-logs-btn');
            if (clearBtn) clearBtn.addEventListener('click', () => this.clearLogs());

            const select = document.getElementById('log-file-select');
            if (select) select.addEventListener('change', () => this.loadLogs({ noCache: true }));
        },

        loadLogsPage(options) {
            // 清空日志按钮
            const clearBtn = document.getElementById('clear-logs-btn');
            if (clearBtn) {
                clearBtn.style.display = '';
            }

            this.setupLogsEvents();
            this._logFileCount = null;
            this._logCurrentSize = null;
            this._logCurrentTruncated = false;
            this._updateLogToolbarSummary();
            return this.loadLogFileList(options).then(() => {
                if (this.currentPage === 'logs') return this.loadLogs(options);
                return null;
            });
        },

        loadLogFileList(options) {
            const getter = options && options.noCache && typeof window.apiGetFresh === 'function' ? window.apiGetFresh : apiGet;
            const loadSeq = ++this._logListLoadSeq;
            this._setLogBusy('list', true);
            return getter('/api/logs/list')
                .then((res) => {
                    if (loadSeq !== this._logListLoadSeq || this.currentPage !== 'logs') return;
                    const select = document.getElementById('log-file-select');
                    if (!select) return;
                    const current = select.value || 'system.log';
                    const files = res && res.success && Array.isArray(res.data) ? res.data : [];
                    this._logFileCount = files.length;
                    select.innerHTML = '';
                    if (!files.length) {
                        const opt = document.createElement('option');
                        opt.value = 'system.log';
                        opt.textContent = 'system.log';
                        select.appendChild(opt);
                        this._updateLogToolbarSummary();
                        return;
                    }
                    files.forEach((file) => {
                        const opt = document.createElement('option');
                        opt.value = file.name || 'system.log';
                        opt.textContent = (file.name || 'system.log') + (file.size ? ' (' + file.size + 'B)' : '');
                        select.appendChild(opt);
                    });
                    if (Array.from(select.options).some((opt) => opt.value === current)) {
                        select.value = current;
                    }
                    this._updateLogToolbarSummary();
                })
                .catch(err => {
                    if (loadSeq !== this._logListLoadSeq) return;
                    if (typeof window.isPageAbortError === 'function' && window.isPageAbortError(err)) return;
                    if (typeof window.apiNotifyError === 'function') {
                        window.apiNotifyError(err, '日志文件列表加载失败', '日志管理');
                    } else {
                        Notification.warning('日志文件列表加载失败');
                    }
                    this._updateLogToolbarSummary('日志文件列表加载失败');
                })
                .finally(() => {
                    if (loadSeq === this._logListLoadSeq) this._setLogBusy('list', false);
                });
        },

        loadLogs(options) {
            const content = document.getElementById('log-content');
            const meta = document.getElementById('log-meta');
            const select = document.getElementById('log-file-select');
            const file = select && select.value ? select.value : 'system.log';
            const getter = options && options.noCache && typeof window.apiGetFresh === 'function' ? window.apiGetFresh : apiGet;
            const loadSeq = ++this._logContentLoadSeq;
            this._setLogBusy('content', true);
            if (content) content.textContent = '日志加载中...';

            return getter('/api/logs', { file, tail: 180, limit: 4096 })
                .then((res) => {
                    if (loadSeq !== this._logContentLoadSeq || this.currentPage !== 'logs') return;
                    if (!res || !res.success) throw new Error(res && (res.error || res.message) || 'load failed');
                    const data = res.data || {};
                    if (content) content.textContent = data.content || '暂无日志';
                    this._logCurrentSize = Number(data.size || 0);
                    this._logCurrentTruncated = data.truncated === true;
                    this._updateLogToolbarSummary();
                })
                .catch((err) => {
                    if (loadSeq !== this._logContentLoadSeq) return;
                    if (typeof window.isPageAbortError === 'function' && window.isPageAbortError(err)) return;
                    if (content) content.textContent = '日志加载失败：' + (err && err.message ? err.message : '');
                    if (meta) this._updateLogToolbarSummary('日志接口不可用或当前构建未启用日志查看');
                    if (typeof window.apiNotifyError === 'function') {
                        window.apiNotifyError(err, '日志加载失败', '日志管理');
                    }
                })
                .finally(() => {
                    if (loadSeq === this._logContentLoadSeq) this._setLogBusy('content', false);
                });
        },

        clearLogs() {
            const select = document.getElementById('log-file-select');
            const file = select && select.value ? select.value : 'system.log';
            if (!confirm('确认清空日志文件 ' + file + '？')) return;
            apiPost('/api/logs/clear', { file })
                .then((res) => {
                    if (res && res.success) {
                        Notification.success('日志已清空');
                        this.loadLogs({ noCache: true });
                    } else {
                        Notification.error(res && (res.error || res.message) || '清空日志失败');
                    }
                })
                .catch(err => {
                    if (typeof window.isPageAbortError === 'function' && window.isPageAbortError(err)) return;
                    if (typeof window.apiNotifyError === 'function') {
                        window.apiNotifyError(err, '清空日志失败', '日志管理');
                    } else {
                        Notification.error('清空日志失败');
                    }
                });
        }
    });

    if (typeof AppState.setupLogsEvents === 'function') {
        AppState.setupLogsEvents();
    }
    if (typeof AppState.onPageHide === 'function') {
        AppState.onPageHide('logs', function() {
            if (this.currentPage !== 'logs') return;
            this._logListLoadSeq++;
            this._logContentLoadSeq++;
        });
    }
})();
