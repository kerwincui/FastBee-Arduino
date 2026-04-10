/**
 * 日志管理模块
 * 包含日志文件列表、日志内容加载、自动刷新、清空
 */
(function() {
    AppState.registerModule('logs', {

        // ============ 事件绑定 ============
        setupLogsEvents() {
            // 刷新日志文件列表按钮
            const refreshLogListBtn = document.getElementById('log-refresh-list-btn');
            if (refreshLogListBtn) refreshLogListBtn.addEventListener('click', () => this.loadLogFileList());

            // 刷新日志按钮
            const refreshLogsBtn = document.getElementById('refresh-logs-btn');
            if (refreshLogsBtn) refreshLogsBtn.addEventListener('click', () => this.loadLogs());

            // 清空日志按钮
            const clearLogsBtn = document.getElementById('clear-logs-btn');
            if (clearLogsBtn) clearLogsBtn.addEventListener('click', () => this.clearLogs());

            // 自动刷新复选框
            const autoRefreshCheckbox = document.getElementById('log-auto-refresh');
            if (autoRefreshCheckbox) {
                autoRefreshCheckbox.addEventListener('change', (e) => {
                    if (e.target.checked) {
                        this.startLogAutoRefresh();
                    } else {
                        this.stopLogAutoRefresh();
                    }
                });
            }
        },

        // ============ 日志管理 ============

        /**
         * 加载日志文件列表
         */
        loadLogFileList() {
            const listContainer = document.getElementById('log-file-list');
            if (!listContainer) return;

            apiGet('/api/logs/list')
                .then(res => {
                    if (!res || !res.success) {
                        listContainer.innerHTML = '<div style="color: #f56c6c; padding: 10px;">' + i18n.t('log-load-fail-html') + '</div>';
                        return;
                    }

                    const files = res.data || [];
                    if (files.length === 0) {
                        listContainer.innerHTML = '<div style="color: #666; padding: 10px;">' + i18n.t('log-empty-html') + '</div>';
                        return;
                    }

                    // 按文件名排序，system.log 排最前面
                    files.sort((a, b) => {
                        if (a.name === 'system.log') return -1;
                        if (b.name === 'system.log') return 1;
                        return b.name.localeCompare(a.name); // 新文件在前
                    });

                    let html = '';
                    files.forEach(file => {
                        const sizeStr = file.size < 1024 ? `${file.size} B` : `${(file.size / 1024).toFixed(1)} KB`;
                        const isActive = file.name === this._currentLogFile;
                        const activeStyle = isActive ? 'background: #3a3a3a; color: #fff;' : 'color: #ccc;';
                        const icon = file.current ? '📄' : '📃';
                        html += `<div class="log-file-item" style="padding: 8px 10px; cursor: pointer; border-bottom: 1px solid #333; ${activeStyle}" data-file="${file.name}">
                            <span style="margin-right: 5px;">${icon}</span>
                            <span style="font-size: 12px;">${file.name}</span>
                            <span style="font-size: 11px; color: #888; float: right;">${sizeStr}</span>
                        </div>`;
                    });

                    listContainer.innerHTML = html;

                    // 绑定点击事件
                    listContainer.querySelectorAll('.log-file-item').forEach(item => {
                        item.addEventListener('click', () => {
                            const fileName = item.dataset.file;
                            this._currentLogFile = fileName;
                            // 更新当前文件显示
                            const currentSpan = document.getElementById('current-log-file');
                            if (currentSpan) currentSpan.textContent = i18n.t('log-current-file-prefix') + fileName;
                            // 更新选中状态
                            listContainer.querySelectorAll('.log-file-item').forEach(i => {
                                i.style.background = '';
                                i.style.color = '#ccc';
                            });
                            item.style.background = '#3a3a3a';
                            item.style.color = '#fff';
                            // 加载日志内容
                            this.loadLogs(500, fileName);
                        });

                        // 鼠标悬停效果
                        item.addEventListener('mouseenter', () => {
                            if (item.style.background !== '#3a3a3a') {
                                item.style.background = '#2a2a2a';
                            }
                        });
                        item.addEventListener('mouseleave', () => {
                            if (item.style.background === '#2a2a2a') {
                                item.style.background = '';
                            }
                        });
                    });
                })
                .catch(err => {
                    console.error('Load log file list failed:', err);
                    listContainer.innerHTML = '<div style="color: #f56c6c; padding: 10px;">' + i18n.t('log-load-fail-html') + '</div>';
                });
        },

        /**
         * 加载日志内容
         * @param {number} maxLines 最大行数，默认500
         * @param {string} fileName 日志文件名，默认为当前选中的文件
         */
        loadLogs(maxLines = 500, fileName = null) {
            const container = document.getElementById('device-log-container');
            const infoSpan = document.getElementById('log-info');

            if (!container) return;

            // 使用传入的文件名或当前选中的文件
            const logFile = fileName || this._currentLogFile || 'system.log';
            this._currentLogFile = logFile;

            apiGet('/api/logs', { lines: maxLines, file: logFile })
                .then(res => {
                    if (!res || !res.success) {
                        container.innerHTML = i18n.t('log-load-fail-html');
                        return;
                    }

                    const data = res.data || {};
                    const content = data.content || '';
                    const fileSize = data.size || 0;
                    const lineCount = data.lines || 0;
                    const truncated = data.truncated || false;

                    // 更新日志信息
                    if (infoSpan) {
                        const sizeStr = fileSize < 1024 ? `${fileSize} B` :
                                       fileSize < 1024 * 1024 ? `${(fileSize / 1024).toFixed(1)} KB` :
                                       `${(fileSize / 1024 / 1024).toFixed(2)} MB`;
                        let infoText = `${lineCount}${i18n.t('log-line-unit')}${sizeStr}`;
                        if (truncated) infoText += i18n.t('log-truncated-suffix');
                        infoSpan.textContent = infoText;
                    }

                    // 格式化并显示日志内容
                    if (!content || content.trim() === '') {
                        container.innerHTML = i18n.t('log-empty-html');
                    } else {
                        container.innerHTML = this._formatLogContent(content);
                        // 滚动到底部
                        container.scrollTop = container.scrollHeight;
                    }
                })
                .catch(err => {
                    console.error('Load logs failed:', err);
                    container.innerHTML = i18n.t('log-load-fail-html');
                });
        },

        /**
         * 格式化日志内容，添加颜色高亮
         */
        _formatLogContent(content) {
            const lines = content.split('\n');
            return lines.map(line => {
                // 跳过空行
                if (!line.trim()) return '';

                // 根据日志级别设置颜色
                let className = '';
                if (line.includes('[ERROR]') || line.includes('[E]')) {
                    className = 'log-error';
                } else if (line.includes('[WARN]') || line.includes('[W]')) {
                    className = 'log-warn';
                } else if (line.includes('[INFO]') || line.includes('[I]')) {
                    className = 'log-info';
                } else if (line.includes('[DEBUG]') || line.includes('[D]')) {
                    className = 'log-debug';
                }

                // HTML 转义
                const escaped = line
                    .replace(/&/g, '&amp;')
                    .replace(/</g, '&lt;')
                    .replace(/>/g, '&gt;');

                return `<div class="${className}">${escaped}</div>`;
            }).filter(line => line).join('');
        },

        /**
         * 清空日志
         */
        clearLogs() {
            if (!confirm(i18n.t('log-clear-confirm-msg'))) return;

            const btn = document.getElementById('clear-logs-btn');
            if (btn) {
                btn.disabled = true;
                btn.innerHTML = i18n.t('log-clearing-html');
            }

            apiPost('/api/logs/clear', {})
                .then(res => {
                    if (res && res.success) {
                        Notification.success(i18n.t('log-cleared-msg'), i18n.t('log-op-title'));
                        this.loadLogs();  // 重新加载
                    } else {
                        Notification.error((res && res.error) || i18n.t('log-op-fail'), i18n.t('log-op-fail'));
                    }
                })
                .catch(err => {
                    console.error('Clear logs failed:', err);
                    Notification.error(i18n.t('log-op-fail'), i18n.t('log-op-fail'));
                })
                .finally(() => {
                    if (btn) {
                        btn.disabled = false;
                        btn.innerHTML = i18n.t('log-clear-btn-html');
                    }
                });
        },

        /**
         * 启动日志自动刷新
         */
        startLogAutoRefresh(interval = 2000) {
            // 先停止已有的定时器
            this.stopLogAutoRefresh();

            // 启动新的定时器
            this._logAutoRefreshTimer = setInterval(() => {
                // 仅在日志页面激活时刷新
                if (this.currentPage === 'logs') {
                    this.loadLogs();
                }
            }, interval);

            console.log('[Logs] Auto refresh started with interval:', interval);
        },

        /**
         * 停止日志自动刷新
         */
        stopLogAutoRefresh() {
            if (this._logAutoRefreshTimer) {
                clearInterval(this._logAutoRefreshTimer);
                this._logAutoRefreshTimer = null;
                console.log('[Logs] Auto refresh stopped');
            }
        }
    });

    // 自动绑定事件
    if (typeof AppState.setupLogsEvents === 'function') {
        AppState.setupLogsEvents();
    }
})();
