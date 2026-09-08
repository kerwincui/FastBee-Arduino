// ============================================================
// 设备 OTA 升级交互模块（配套 pages/fragments/device-ota.html 分片）
// 提供固件信息展示、在线升级（URL 下载）、本地文件上传、
// 进度轮询与设备重启恢复检测；无外网连接时引导本地上传
// ============================================================
(function () {
    'use strict';

    // 页面文案统一使用中文；保留此兼容助手供现有提示调用点使用。
    function t(key, fallback) {
        return fallback || key;
    }

    function getEl(id) {
        return document.getElementById(id);
    }

    function formatBytes(bytes) {
        if (bytes === null || bytes === undefined || isNaN(bytes)) return '--';
        if (bytes >= 1024 * 1024) return (bytes / 1024 / 1024).toFixed(2) + ' MB';
        if (bytes >= 1024) return (bytes / 1024).toFixed(1) + ' KB';
        return bytes + ' B';
    }

    AppState.registerModule('device-ota', {

        _otaPollTimer: null,
        _otaRecoveryTimer: null,
        _otaBusy: false,
        _otaNetworkOnline: false,
        _otaSupported: true,
        _otaFreeSketchSpace: 0,
        
        // WP-13/73: Page lifecycle management (timer cleanup on page hide)
        _pageShown: false,
        _onPageHide: function() { this._stopAllTimers(); },

        // ============ 初始化 ============

        setupDeviceOta() {
            const refreshBtn = getEl('ota-refresh-btn');
            if (refreshBtn) refreshBtn.addEventListener('click', () => this.refreshOtaInfo());

            const urlForm = getEl('ota-url-form');
            if (urlForm) urlForm.addEventListener('submit', (e) => {
                e.preventDefault();
                this.startOnlineUpgrade();
            });

            const uploadForm = getEl('ota-upload-form');
            if (uploadForm) uploadForm.addEventListener('submit', (e) => {
                e.preventDefault();
                this.startLocalUpload();
            });

            const fileInput = getEl('ota-file');
            if (fileInput) fileInput.addEventListener('change', () => this._handleFileSelect());

            // WP-73: Register page hide handler for timer cleanup
            if (typeof AppState !== 'undefined') {
                AppState.onPageHide('device-ota', this._onPageHide);
            }
            
            this.refreshOtaInfo();
        },

        // data-action="triggerOtaFileSelect" 由全局分发器调用
        triggerOtaFileSelect() {
            if (!this._otaSupported) return;
            const fileInput = getEl('ota-file');
            if (fileInput) fileInput.click();
        },

        // ============ 信息加载 ============

        /**
         * 刷新固件信息（版本/空间）、网络状态与 OTA 状态
         */
        refreshOtaInfo() {
            const getter = (typeof apiGetFresh === 'function') ? apiGetFresh : apiGet;
            if (!getter) return;

            // 4MB 单 app 镜像没有 ota_1，服务端会拒绝 A/B 升级；先探测
            // 能力并在页面上明确说明，避免用户操作到不存在的 OTA 路由。
            getter('/api/system/capabilities')
                .then(res => {
                    const caps = (res && res.success && res.data) ? res.data : null;
                    if (caps && typeof caps.ota === 'boolean') this._applyOtaCapability(caps.ota);
                })
                .catch(() => { /* 能力接口不可用时保留默认可用态 */ });

            // 固件信息：当前版本、Flash 总量、可升级空间
            getter('/api/system/info')
                .then(res => {
                    if (!res || !res.success || !res.data) return;
                    const dev = res.data.device || {};
                    const flash = res.data.flash || {};
                    const version = dev.firmwareVersion || '--';
                    const freeSketch = (flash.freeSketchSpace != null) ? flash.freeSketchSpace : flash.free;
                    this._otaFreeSketchSpace = freeSketch || 0;
                    this._setText('ota-current-version', version);
                    this._setText('ota-flash-size', formatBytes(flash.total));
                    this._setText('ota-free-space', formatBytes(freeSketch));
                })
                .catch(err => {
                    if (typeof window.isPageAbortError === 'function' && window.isPageAbortError(err)) return;
                    console.warn('[device-ota] system info failed:', err && err.message);
                });

            // 网络状态：决定在线升级入口可用性
            getter('/api/network/status')
                .then(res => {
                    const d = (res && res.success && res.data) ? res.data : null;
                    const status = d
                        ? (d.status || ((d.connected || d.networkConnected || d.ipAddress) ? 'connected' : 'disconnected'))
                        : 'disconnected';
                    this._applyNetworkState(status === 'connected');
                })
                .catch(err => {
                    if (typeof window.isPageAbortError === 'function' && window.isPageAbortError(err)) return;
                    // 状态获取失败按离线处理（仍可本地上传）
                    this._applyNetworkState(false);
                });

            // OTA 当前状态
            this._loadOtaStatus();
        },

        _loadOtaStatus() {
            const getter = (typeof apiGetFresh === 'function') ? apiGetFresh : apiGet;
            if (!getter) return;
            getter('/api/ota/status')
                .then(res => {
                    const body = (res && res.success && res.data) ? res.data : null;
                    if (body) this._updateOtaStatusUI(body);
                })
                .catch(err => {
                    if (typeof window.isPageAbortError === 'function' && window.isPageAbortError(err)) return;
                });
        },

        // ============ 网络状态联动 ============

        /**
         * 应用网络状态：无外网时显示提示并禁用在线升级入口（本地上传始终可用）
         */
        _applyNetworkState(online) {
            this._otaNetworkOnline = online;

            const badge = getEl('ota-network-badge');
            if (badge) {
                badge.className = 'status-badge ' + (online ? 'status-online' : 'status-offline');
                badge.textContent = online
                    ? t('dev-ota-network-online', '已联网')
                    : t('dev-ota-network-offline', '未联网');
            }

            const hint = getEl('ota-network-hint');
            if (hint) hint.classList.toggle('fb-hidden', online);

            const urlInput = getEl('ota-url');
            if (urlInput) urlInput.disabled = !online || !this._otaSupported;

            this._updateButtonStates();
        },

        _updateButtonStates() {
            const urlBtn = getEl('ota-url-btn');
            if (urlBtn) urlBtn.disabled = this._otaBusy || !this._otaNetworkOnline || !this._otaSupported;
            const uploadBtn = getEl('ota-upload-btn');
            if (uploadBtn) uploadBtn.disabled = this._otaBusy || !this._otaSupported;
        },

        _applyOtaCapability(available) {
            this._otaSupported = available !== false;
            const hint = getEl('ota-capability-hint');
            if (hint) {
                hint.classList.toggle('is-hidden', this._otaSupported);
                if (!this._otaSupported) {
                    hint.textContent = '当前固件为 4MB 单应用分区，不支持 A/B OTA；请使用 8MB/16MB A/B 固件。';
                }
            }
            const fileInput = getEl('ota-file');
            if (fileInput) fileInput.disabled = !this._otaSupported;
            const urlInput = getEl('ota-url');
            if (urlInput) urlInput.disabled = !this._otaSupported || !this._otaNetworkOnline;
            this._updateButtonStates();
        },

        // ============ 在线升级（URL 下载） ============

        startOnlineUpgrade() {
            if (this._otaBusy) return;
            if (!this._otaSupported) {
                this._showMessage('ota-online-message', false, '当前分区不支持 OTA 升级');
                return;
            }
            const input = getEl('ota-url');
            const url = input ? input.value.trim() : '';
            if (!/^https?:\/\/.+/i.test(url)) {
                this._showMessage('ota-online-message', false,
                    t('dev-ota-url-invalid', '请输入有效的固件下载地址（http/https）'));
                return;
            }
            if (!this._otaNetworkOnline) {
                this._showMessage('ota-online-message', false,
                    t('dev-ota-no-network-hint', '当前未检测到可用的网络连接，请先连接网络，或选择本地文件上传方式升级。'));
                return;
            }
            if (!confirm(t('dev-ota-confirm-online', '确定从该地址下载固件并升级？升级过程中请勿断电。'))) return;

            this._setBusy(true);
            this._resetProgress();

            apiPost('/api/ota/url', { url: url })
                .then(res => {
                    if (res && res.success) {
                        this._showMessage('ota-online-message', true,
                            t('dev-ota-download-started', '固件下载已开始，请关注升级进度'));
                        this._startPolling();
                    } else {
                        this._setBusy(false);
                        this._showMessage('ota-online-message', false,
                            (res && (res.error || res.message)) || t('dev-ota-start-failed', '启动升级失败'));
                    }
                })
                .catch(err => {
                    if (typeof window.isPageAbortError === 'function' && window.isPageAbortError(err)) return;
                    this._setBusy(false);
                    this._showMessage('ota-online-message', false,
                        (err && err.message) || t('dev-ota-start-failed', '启动升级失败'));
                });
        },

        // ============ 本地上传升级 ============

        _handleFileSelect() {
            const input = getEl('ota-file');
            const nameEl = getEl('ota-file-name');
            if (!input || !input.files || input.files.length === 0) {
                if (nameEl) nameEl.textContent = t('no-file-selected', '未选择任何文件');
                return;
            }
            const file = input.files[0];
            if (!file.name.toLowerCase().endsWith('.bin')) {
                this._showMessage('ota-upload-message', false,
                    t('dev-ota-file-invalid', '请选择 .bin 格式的固件文件'));
                input.value = '';
                if (nameEl) nameEl.textContent = t('no-file-selected', '未选择任何文件');
                return;
            }
            if (nameEl) nameEl.textContent = file.name + ' (' + formatBytes(file.size) + ')';
        },

        startLocalUpload() {
            if (this._otaBusy) return;
            if (!this._otaSupported) {
                this._showMessage('ota-upload-message', false, '当前分区不支持 OTA 升级');
                return;
            }
            const input = getEl('ota-file');
            if (!input || !input.files || input.files.length === 0) {
                this._showMessage('ota-upload-message', false,
                    t('dev-ota-file-empty', '请先选择固件文件'));
                return;
            }
            const file = input.files[0];
            if (!file.name.toLowerCase().endsWith('.bin')) {
                this._showMessage('ota-upload-message', false,
                    t('dev-ota-file-invalid', '请选择 .bin 格式的固件文件'));
                return;
            }
            // 固件大于可用升级空间时提前拦截（服务端 beginOTA 亦会校验）
            if (this._otaFreeSketchSpace > 0 && file.size > this._otaFreeSketchSpace) {
                this._showMessage('ota-upload-message', false,
                    t('dev-ota-file-too-large', '固件大小超出可用升级空间'));
                return;
            }
            if (!confirm(t('dev-ota-confirm-upload', '确认上传固件并升级？升级过程中设备将重启，请勿断电。'))) return;

            this._setBusy(true);
            this._resetProgress();

            const self = this;
            // Step 1: begin（携带 size 参数，服务端预留空间并执行内存门控）
            fetch('/api/ota/begin', {
                method: 'POST',
                headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
                body: 'size=' + file.size
            })
                .then(function (res) {
                    return res.json().catch(function () { return {}; }).then(function (data) {
                        if (!res.ok || (data && data.error) || data.success === false) {
                            throw new Error((data && data.error) || ('HTTP ' + res.status));
                        }
                        // Step 2: multipart 上传固件文件
                        const formData = new FormData();
                        formData.append('firmware', file, file.name);
                        return fetch('/api/ota/upload', { method: 'POST', body: formData });
                    });
                })
                .then(function (res) {
                    return res.json().catch(function () { return {}; }).then(function (data) {
                        if ((data && data.error) || data.success === false) {
                            throw new Error((data && data.error) || 'upload failed');
                        }
                        // final 分块响应：写入完成，设备将在 3 秒后重启
                        self._setProgress(100, t('dev-ota-writing-done', '固件写入完成'));
                        self._showMessage('ota-upload-message', true,
                            t('dev-ota-restarting', '升级完成，设备正在重启...'));
                        if (typeof Notification !== 'undefined' && Notification.success) {
                            Notification.success(t('dev-ota-restarting', '升级完成，设备正在重启...'), 'OTA');
                        }
                        self._pollDeviceRecovery();
                    });
                })
                .catch(function (err) {
                    if (typeof window.isPageAbortError === 'function' && window.isPageAbortError(err)) return;
                    self._setBusy(false);
                    self._showMessage('ota-upload-message', false,
                        t('dev-ota-upload-failed', '固件上传失败') + ': ' + (err.message || err));
                });
        },

        // ============ 进度轮询与恢复检测 ============

        _resetProgress() {
            const wrap = getEl('ota-progress-wrap');
            if (wrap) wrap.classList.remove('is-hidden');
            this._setProgress(0, '');
        },

        _setProgress(percent, text) {
            const bar = getEl('ota-progress-bar');
            const label = getEl('ota-progress-text');
            if (bar) bar.style.width = Math.min(100, Math.max(0, percent)) + '%';
            if (label) label.textContent = (text !== undefined && text !== '') ? (percent + '% - ' + text) : (percent + '%');
        },

        _updateProgressBar(body) {
            if (body.total_size > 0) {
                this._setProgress(body.progress || 0, formatBytes(body.current_size || 0) + ' / ' + formatBytes(body.total_size));
            } else if (body.current_size > 0) {
                // 大小未知：仅展示已写入量
                this._setProgress(body.progress || 0,
                    formatBytes(body.current_size) + t('dev-ota-size-unknown', '（总大小未知）'));
            } else {
                this._setProgress(body.progress || 0, '');
            }
        },

        _updateOtaStatusUI(body) {
            const badge = getEl('ota-status-badge');
            if (badge) {
                if (body.in_progress) {
                    badge.className = 'status-badge status-connecting';
                    badge.textContent = t('dev-ota-status-upgrading', '升级中');
                } else if (body.error) {
                    badge.className = 'status-badge status-offline';
                    badge.textContent = t('dev-ota-status-failed', '失败');
                } else if (body.progress >= 100) {
                    badge.className = 'status-badge status-online';
                    badge.textContent = t('dev-ota-status-done', '已完成');
                } else {
                    badge.className = 'status-badge status-online';
                    badge.textContent = t('dev-ota-ready', '就绪');
                }
            }
            this._updateProgressBar(body);
        },

        /**
         * 轮询 /api/ota/status（URL 下载型 OTA 的进度来源）
         */
        _startPolling() {
            this._stopPolling();
            const self = this;
            let failCount = 0;
            let sawProgress = false;
            let pollCount = 0;
            const maxPolls = 600; // 最长 10 分钟，与服务端下载 deadline 对齐

            this._otaPollTimer = setInterval(function () {
                pollCount++;
                if (pollCount > maxPolls) {
                    self._stopPolling();
                    self._setBusy(false);
                    self._showMessage('ota-online-message', false,
                        t('dev-ota-poll-timeout', '升级状态轮询超时，请刷新页面检查设备状态'));
                    return;
                }

                fetch('/api/ota/status', { cache: 'no-store' })
                    .then(function (res) { return res.json(); })
                    .then(function (payload) {
                        failCount = 0;
                        const body = (payload && payload.data) ? payload.data : (payload || {});
                        self._updateOtaStatusUI(body);

                        if (body.in_progress) {
                            sawProgress = true;
                            return;
                        }
                        // 不再进行中：完成或失败
                        if (body.progress >= 100 || (sawProgress && !body.error)) {
                            self._stopPolling();
                            self._showMessage('ota-online-message', true,
                                t('dev-ota-restarting', '升级完成，设备正在重启...'));
                            self._pollDeviceRecovery();
                        } else if (body.error) {
                            self._stopPolling();
                            self._setBusy(false);
                            self._showMessage('ota-online-message', false, body.error);
                        }
                    })
                    .catch(function () {
                        failCount++;
                        // 升级成功后设备重启会导致请求失败，连续失败判定为重启中
                        if (sawProgress && failCount >= 5) {
                            self._stopPolling();
                            self._showMessage('ota-online-message', true,
                                t('dev-ota-restarting', '升级完成，设备正在重启...'));
                            self._pollDeviceRecovery();
                        }
                    });
            }, 1000);
        },

        _stopPolling() {
            if (this._otaPollTimer) {
                clearInterval(this._otaPollTimer);
                this._otaPollTimer = null;
            }
        },
        
        // WP-73: Stop all timers (OTA polling + recovery monitoring)
        _stopAllTimers() {
            this._stopPolling();
            if (this._otaRecoveryTimer) {
                clearInterval(this._otaRecoveryTimer);
                this._otaRecoveryTimer = null;
            }
            console.debug('[device-ota] All timers stopped (WP-73)');
        },
        
        // WP-73: Cleanup when module unloads
        destroy() {
            this._stopAllTimers();
            this._pageShown = false;
        },

        /**
         * 轮询 /api/health 等待设备重启恢复，恢复后自动刷新页面
         */
        _pollDeviceRecovery() {
            this._stopPolling();
            if (this._otaRecoveryTimer) clearInterval(this._otaRecoveryTimer);
            const self = this;
            let pollCount = 0;
            const maxPolls = 90; // 最长等待 90s

            this._otaRecoveryTimer = setInterval(function () {
                pollCount++;
                if (pollCount > maxPolls) {
                    clearInterval(self._otaRecoveryTimer);
                    self._otaRecoveryTimer = null;
                    self._setBusy(false);
                    self._showMessage('ota-online-message', false,
                        t('dev-ota-recovery-timeout', '设备恢复超时，请手动刷新页面'));
                    return;
                }

                fetch('/api/health', { cache: 'no-store' })
                    .then(function (res) {
                        if (res.ok) {
                            clearInterval(self._otaRecoveryTimer);
                            self._otaRecoveryTimer = null;
                            self._setProgress(100, t('dev-ota-done', '升级完成'));
                            self._showMessage('ota-online-message', true,
                                t('dev-ota-reloading', '设备已恢复，正在刷新页面...'));
                            setTimeout(function () { window.location.reload(); }, 1500);
                        }
                    })
                    .catch(function () {
                        // 设备仍在重启中，继续轮询
                    });
            }, 1000);
        },

        // ============ 通用助手 ============

        _setBusy(busy) {
            this._otaBusy = busy;
            this._updateButtonStates();
        },

        _setText(id, text) {
            const el = getEl(id);
            if (el) el.textContent = text;
        },

        _showMessage(elId, ok, text) {
            const el = getEl(elId);
            if (!el) return;
            el.textContent = text;
            el.style.color = ok ? 'var(--success,#52c41a)' : 'var(--danger,#ff4d4f)';
            el.classList.remove('is-hidden');
            clearTimeout(el._hideTimer);
            el._hideTimer = setTimeout(function () {
                el.classList.add('is-hidden');
            }, 6000);
        }
    });
})();
