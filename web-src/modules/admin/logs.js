(function() {
    AppState.registerModule('logs', {
        setupLogsEvents() {
            if (this._logsEventsBound) return;
            this._logsEventsBound = true;

            const refreshListBtn = document.getElementById('log-refresh-list-btn');
            if (refreshListBtn) refreshListBtn.addEventListener('click', () => this.loadLogFileList({ noCache: true }));

            const refreshBtn = document.getElementById('refresh-logs-btn');
            if (refreshBtn) refreshBtn.addEventListener('click', () => this.loadLogs({ noCache: true }));

            const clearBtn = document.getElementById('clear-logs-btn');
            if (clearBtn) clearBtn.addEventListener('click', () => this.clearLogs());

            const select = document.getElementById('log-file-select');
            if (select) select.addEventListener('change', () => this.loadLogs({ noCache: true }));
        },

        loadLogsPage(options) {
            // 权限控制：清空日志按钮（需要 system.view 权限）
            const clearBtn = document.getElementById('clear-logs-btn');
            if (clearBtn) {
                if (AppState.hasPermission('system.view')) {
                    clearBtn.style.display = '';
                } else {
                    clearBtn.style.display = 'none';
                }
            }

            this.setupLogsEvents();
            this.loadLogFileList(options);
            this.loadLogs(options);
        },

        loadLogFileList(options) {
            const getter = options && options.noCache && typeof window.apiGetFresh === 'function' ? window.apiGetFresh : apiGet;
            return getter('/api/logs/list')
                .then((res) => {
                    const select = document.getElementById('log-file-select');
                    if (!select) return;
                    const current = select.value || 'system.log';
                    const files = res && res.success && Array.isArray(res.data) ? res.data : [];
                    select.innerHTML = '';
                    if (!files.length) {
                        const opt = document.createElement('option');
                        opt.value = 'system.log';
                        opt.textContent = 'system.log';
                        select.appendChild(opt);
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
                })
                .catch(() => Notification.warning('日志文件列表加载失败'));
        },

        loadLogs(options) {
            const content = document.getElementById('log-content');
            const meta = document.getElementById('log-meta');
            const select = document.getElementById('log-file-select');
            const file = select && select.value ? select.value : 'system.log';
            const getter = options && options.noCache && typeof window.apiGetFresh === 'function' ? window.apiGetFresh : apiGet;
            if (content) content.textContent = '日志加载中...';

            return getter('/api/logs', { file, tail: 120, limit: 4096 })
                .then((res) => {
                    if (!res || !res.success) throw new Error(res && (res.error || res.message) || 'load failed');
                    const data = res.data || {};
                    if (content) content.textContent = data.content || '暂无日志';
                    if (meta) {
                        meta.textContent = `${file} | ${data.size || 0} bytes${data.truncated ? ' | 已截断显示' : ''}`;
                    }
                })
                .catch((err) => {
                    if (content) content.textContent = '日志加载失败：' + (err && err.message ? err.message : '');
                    if (meta) meta.textContent = '日志接口不可用或当前构建未启用日志查看';
                });
        },

        clearLogs() {
            if (!AppState.hasPermission('system.view')) {
                Notification.warning('没有操作权限', '权限不足');
                return;
            }

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
                .catch(() => Notification.error('清空日志失败'));
        }
    });

    if (typeof AppState.setupLogsEvents === 'function') {
        AppState.setupLogsEvents();
    }
})();
