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
        
            // 页面可见性检测：隐藏时暂停轮询，恢复时立即刷新
            this._setupLogVisibilityHandler();
        },
        
        /**
         * 设置页面可见性检测处理器
         */
        _setupLogVisibilityHandler() {
            // 避免重复绑定
            if (this._logVisibilityHandlerBound) return;
            this._logVisibilityHandlerBound = true;
        
            document.addEventListener('visibilitychange', () => {
                if (document.hidden) {
                    // 页面不可见，暂停日志轮询
                    if (this._logAutoRefreshTimer) {
                        clearInterval(this._logAutoRefreshTimer);
                        this._logAutoRefreshTimer = null;
                        this._logPausedByVisibility = true;
                    }
                } else if (this._logPausedByVisibility) {
                    // 页面恢复可见，立即刷新一次并恢复轮询
                    this._logPausedByVisibility = false;
                    if (this.currentPage === 'logs') {
                        this.loadLogs();
                        // 检查自动刷新复选框状态，仅当用户已开启时才恢复
                        const autoRefreshCheckbox = document.getElementById('log-auto-refresh');
                        if (autoRefreshCheckbox && autoRefreshCheckbox.checked) {
                            this.startLogAutoRefresh();
                        }
                    }
                }
            });
        },

        // ============ 日志管理 ============

        /**
         * 加载日志文件列表
         */
        loadLogFileList() {
            const listContainer = document.getElementById("log-file-list");
            if (!listContainer) return;

            apiGet("/api/logs/list")
                .then(res => {
                    if (!res || !res.success) {
                        listContainer.innerHTML = '<div class="logs-state logs-state-error">' + escapeHtml(i18n.t("log-load-fail")) + '</div>';
                        return;
                    }

                    const files = res.data || [];
                    if (files.length === 0) {
                        listContainer.innerHTML = '<div class="logs-state">' + escapeHtml(i18n.t("log-empty")) + '</div>';
                        return;
                    }

                    files.sort((a, b) => {
                        if (a.name === "system.log") return -1;
                        if (b.name === "system.log") return 1;
                        return b.name.localeCompare(a.name);
                    });

                    let html = "";
                    files.forEach(file => {
                        const sizeStr = file.size < 1024 ? `${file.size} B` : `${(file.size / 1024).toFixed(1)} KB`;
                        const isActive = file.name === this._currentLogFile;
                        const safeName = typeof escapeHtml === "function" ? escapeHtml(file.name) : file.name;
                        const icon = file.current ? "&#9679;" : "&#9675;";
                        html += `<div class="logs-file-item${isActive ? " is-active" : ""}" data-file="${encodeURIComponent(file.name)}">
                            <span class="logs-file-icon${file.current ? " is-current" : ""}">${icon}</span>
                            <span class="logs-file-label">${safeName}</span>
                            <span class="logs-file-size">${sizeStr}</span>
                        </div>`;
                    });

                    listContainer.innerHTML = html;

                    listContainer.querySelectorAll(".logs-file-item").forEach(item => {
                        item.addEventListener("click", () => {
                            const fileName = decodeURIComponent(item.dataset.file || "");
                            if (!fileName) return;
                            this._currentLogFile = fileName;
                            const currentSpan = document.getElementById("current-log-file");
                            if (currentSpan) currentSpan.textContent = i18n.t("log-current-file-prefix") + fileName;
                            this.setExclusiveActive(listContainer, ".logs-file-item", item);
                            this.loadLogs(500, fileName);
                        });
                    });
                })
                .catch(err => {
                    console.error("Load log file list failed:", err);
                    listContainer.innerHTML = '<div class="logs-state logs-state-error">' + escapeHtml(i18n.t("log-load-fail")) + '</div>';
                });
        },

        /**
         * 加载日志内容
         * @param {number} maxLines 最大行数，默认500
         * @param {string} fileName 日志文件名，默认为当前选中的文件
         */
        loadLogs(maxLines = 500, fileName = null) {
            const container = document.getElementById("device-log-container");
            const infoSpan = document.getElementById("log-info");

            if (!container) return;

            const logFile = fileName || this._currentLogFile || "system.log";
            this._currentLogFile = logFile;

            const currentSpan = document.getElementById("current-log-file");
            if (currentSpan) currentSpan.textContent = i18n.t("log-current-file-prefix") + logFile;

            var self = this;
            var _renderContent = function(res) {
                if (!res || !res.success) {
                    container.innerHTML = '<div class="logs-state logs-state-error">' + escapeHtml(i18n.t("log-load-fail")) + '</div>';
                    return;
                }

                const data = res.data || {};
                const content = data.content || "";
                const fileSize = data.size || 0;
                const lineCount = data.lines || 0;
                const truncated = data.truncated || false;

                if (infoSpan) {
                    const sizeStr = fileSize < 1024 ? `${fileSize} B` :
                                   fileSize < 1024 * 1024 ? `${(fileSize / 1024).toFixed(1)} KB` :
                                   `${(fileSize / 1024 / 1024).toFixed(2)} MB`;
                    let infoText = `${lineCount}${i18n.t("log-line-unit")}${sizeStr}`;
                    if (truncated) infoText += i18n.t("log-truncated-suffix");
                    infoSpan.textContent = infoText;
                }

                if (!content || !content.trim()) {
                    container.innerHTML = '<div class="logs-state">' + escapeHtml(i18n.t("log-empty")) + '</div>';
                } else {
                    container.innerHTML = self._formatLogContent(content);
                    container.scrollTop = container.scrollHeight;
                }
            };

            apiGet("/api/logs", { lines: maxLines, file: logFile })
                .then(_renderContent)
                .catch(err => {
                    if (err && err._pageAborted) return;
                    // 首次失败时自动重试一次（ESP32 可能还在处理静态资源）
                    console.warn("Load logs failed, retrying...", err);
                    setTimeout(function() {
                        apiGet("/api/logs", { lines: maxLines, file: logFile })
                            .then(_renderContent)
                            .catch(err2 => {
                                console.error("Load logs retry failed:", err2);
                                container.innerHTML = '<div class="logs-state logs-state-error">' + escapeHtml(i18n.t("log-load-fail")) + '</div>';
                            });
                    }, 1000);
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
                const escaped = escapeHtml(line);

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
        startLogAutoRefresh(interval = 5000) {
            // 先停止已有的定时器
            this.stopLogAutoRefresh();

            // 启动新的定时器
            this._logAutoRefreshTimer = setInterval(() => {
                // 仅在日志页面激活时刷新
                if (this.currentPage === 'logs') {
                    this.loadLogs();
                }
            }, interval);

        },

        /**
         * 停止日志自动刷新
         */
        stopLogAutoRefresh() {
            if (this._logAutoRefreshTimer) {
                clearInterval(this._logAutoRefreshTimer);
                this._logAutoRefreshTimer = null;
            }
        }
    });

    // 自动绑定事件
    if (typeof AppState.setupLogsEvents === 'function') {
        AppState.setupLogsEvents();
    }
})();
