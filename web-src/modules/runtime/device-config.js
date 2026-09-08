/**
 * 设备配置模块
 * 包含设备基本信息、NTP配置、重启/恢复出厂、配置导入导出
 */
(function() {
    const MAX_CONFIG_TRANSFER_BYTES = 128 * 1024;
    const DEFAULT_NTP_SERVER1 = 'http://iot.fastbee.cn/prod-api/iot/tool/ntp';
    const DEFAULT_NTP_SERVER2 = 'cn.pool.ntp.org';
    // 弱网适配：URL 编码后 body ≈ 原始 3 倍，1.5KB → ~4.5KB body（约 3-4 个 TCP 包），
    // 退化链路下跨包丢失率远低于 4KB 分片（~12KB body）；后端上限 16KB/分片
    const CONFIG_IMPORT_CHUNK_BYTES = 1536;
    const CONFIG_IMPORT_CHUNK_RETRIES = 3;  // 单分片失败重试次数（退化链路下跨包丢失可重试恢复）
    const MAX_CONFIG_BUNDLE_BYTES = 512 * 1024;
    function t(key, fallback) {
        // 保留已有调用点，但界面始终返回中文硬编码。
        return fallback || key;
    }

    function isExpectedAbort(err) {
        return !!(err && (
            err.name === 'AbortError' ||
            err.code === 'ABORT_ERR' ||
            err._pageAborted ||
            (typeof window !== 'undefined' && typeof window.isPageAbortError === 'function' && window.isPageAbortError(err))
        ));
    }

    function isTransientNetworkError(err) {
        if (!err) return false;
        const message = String(err.message || err);
        return /timed out|timeout|network|fetch failed|load failed|ERR_CONNECTION/i.test(message);
    }

    function restoreTimeRefreshButton(btn) {
        if (!btn) return;
        btn.disabled = false;
        btn.innerHTML = '刷新';
        btn.title = '';
    }
    const CONFIG_TRANSFER_LABELS = {
        'all': '全部配置包',
        'device.json': '设备信息',
        'features.json': '功能配置',
        'network.json': '网络配置',
        'peripherals.json': '外设配置',
        'periph_exec.json': '外设执行',
        'protocol.json': '通信协议',
        'users.json': '用户配置',
        'auth.json': '安全认证',
        'rule_scripts.json': '规则脚本'
    };

    AppState.registerModule('device-config', {

        // ============ 事件绑定 ============
        setupDeviceConfigEvents() {
            // 设备基本信息表单提交
            const deviceBasicForm = document.getElementById('device-basic-form');
            if (deviceBasicForm) {
                deviceBasicForm.addEventListener('submit', (e) => {
                    e.preventDefault();
                    this.saveDeviceBasic();
                });
            }

            // NTP表单提交
            const deviceNtpForm = document.getElementById('device-ntp-form');
            if (deviceNtpForm) {
                deviceNtpForm.addEventListener('submit', (e) => {
                    e.preventDefault();
                    this.saveDeviceNTP();
                });
            }

            // 时间刷新按钮
            const devTimeRefreshBtn = document.getElementById('dev-time-refresh-btn');
            if (devTimeRefreshBtn) devTimeRefreshBtn.addEventListener('click', () => this.syncDeviceTime());

            // 重启按钮
            const devRestartBtn = document.getElementById('dev-restart-btn');
            if (devRestartBtn) devRestartBtn.addEventListener('click', () => this.restartDevice());

            // 恢复出厂设置按钮
            const devFactoryBtn = document.getElementById('dev-factory-btn');
            if (devFactoryBtn) devFactoryBtn.addEventListener('click', () => this.factoryReset());

            // 固件升级
            const otaFileInput = document.getElementById('dev-ota-file');
            if (otaFileInput) otaFileInput.addEventListener('change', () => this.handleOTAFileSelect());
            const otaUploadBtn = document.getElementById('dev-ota-upload-btn');
            if (otaUploadBtn) otaUploadBtn.addEventListener('click', () => this.startOTAUpload());

            // 4MB 单 app 分区无法执行 A/B OTA，内联兜底面板也要在入口处明确禁用。
            if (typeof apiGet === 'function') {
                apiGet('/api/system/capabilities').then((res) => {
                    const caps = (res && res.success && res.data) ? res.data : null;
                    if (!caps || caps.ota !== false) return;
                    const hint = document.getElementById('dev-ota-capability-hint');
                    if (hint) {
                        hint.textContent = '当前固件为 4MB 单应用分区，不支持 A/B OTA；请使用 8MB/16MB A/B 固件。';
                        hint.classList.remove('is-hidden');
                    }
                    if (otaFileInput) otaFileInput.disabled = true;
                    if (otaUploadBtn) otaUploadBtn.disabled = true;
                }).catch(() => { /* 能力接口不可用时保留原有入口 */ });
            }

            const devModeToggleBtn = document.getElementById('dev-mode-toggle-btn');
            if (devModeToggleBtn) devModeToggleBtn.addEventListener('click', () => this.toggleDeveloperMode());

            const configExportBtn = document.getElementById('dev-config-export-btn');
            if (configExportBtn) configExportBtn.addEventListener('click', () => this.exportDeviceConfigBundle());

            const configImportBtn = document.getElementById('dev-config-import-btn');
            if (configImportBtn) configImportBtn.addEventListener('click', () => this.importDeviceConfigBundle());

            const configImportFile = document.getElementById('dev-config-import-file');
            if (configImportFile) {
                configImportFile.addEventListener('change', (e) => {
                    const files = e.target.files ? Array.from(e.target.files) : [];
                    this._setConfigTransferStatus(files.length ? (t('dc-selected-count', '已选择：') + files.map(file => file.name).join('、')) : '--');
                });
            }

            // 安全策略表单提交
            const deviceSecurityForm = document.getElementById('device-security-form');
            if (deviceSecurityForm) {
                deviceSecurityForm.addEventListener('submit', (e) => {
                    e.preventDefault();
                    this.saveSecurityPolicy();
                });
            }

            // 懒加载完整 OTA 升级分片（在线升级/本地上传）；
            // compact 配置不含该分片，加载失败时保留内联精简面板作为兜底
            this._loadOtaFragment();

        },

        // ============ 设备配置 ============

        loadDeviceConfig(options) {
            options = options || {};
            var self = this;
            var batchGetter = null;
            if (options.noCache === true && typeof apiBatchGetFresh === 'function') {
                batchGetter = apiBatchGetFresh;
            } else if (typeof apiBatchGet === 'function') {
                batchGetter = apiBatchGet;
            }
            var getter = batchGetter || (options.noCache === true && typeof apiGetFresh === 'function' ? apiGetFresh : apiGet);
            var configRequest = getter('/api/device/config');
            configRequest
                .then(res => {
                    if (!res || !res.success) return;
                    const d = res.data || {};
                    this._setValue('dev-id', d.deviceId || '');
                    this._setValue('dev-product-number', d.productNumber !== undefined ? String(d.productNumber) : '0');
                    this._setValue('dev-user-id', d.userId || '');
                    this._setValue('dev-name', d.deviceName || '');
                    const desc = document.getElementById('dev-description');
                    if (desc) desc.value = d.description || '';
                    this._setValue('dev-ntp-enable', this._toBoolean(d.enableNTP, true) ? '1' : '0');
                    this._setValue('dev-ntp-server1', d.ntpServer1 || DEFAULT_NTP_SERVER1);
                    this._setValue('dev-ntp-server2', d.ntpServer2 || DEFAULT_NTP_SERVER2);
                    this._setValue('dev-timezone', d.timezone || 'CST-8');
                    this._setValue('dev-sync-interval', d.syncInterval !== undefined ? String(d.syncInterval) : '3600');
                    this._setValue('dev-cache-duration', d.cacheDuration !== undefined ? String(d.cacheDuration) : '86400');
                    this._setValue('dev-log-level', d.logLevel || 'INFO');
                    this.setDeveloperModeState(d.developerModeEnabled !== false);
                    this._renderDeveloperModeState();
                    // 安全策略字段
                    this._setValue('dev-sec-max-attempts', d.maxLoginAttempts !== undefined ? String(d.maxLoginAttempts) : '5');
                    this._setValue('dev-sec-lockout-time', d.loginLockoutTime !== undefined ? String(Math.round(d.loginLockoutTime / 1000)) : '300');
                    this._setValue('dev-sec-min-pwd-len', d.minPasswordLength !== undefined ? String(d.minPasswordLength) : '6');
                    this._setValue('dev-sec-require-strong', d.requireStrongPasswords ? '1' : '0');
                })
                .catch(err => console.error('Load device config failed:', err));
            setTimeout(function() {
                self._loadDeviceHardwareInfo({ noCache: options.noCache === true });
            }, options.deferHardware === false ? 0 : 120);
        },

        _loadDeviceHardwareInfo(options) {
            options = options || {};
            // 兼容后端返回结构差异：res 可能是 {success, data} 或直接是 data（如 batch 子响应）
            var batchGetter = null;
            if (options.noCache === true && typeof apiBatchGetFresh === 'function') {
                batchGetter = apiBatchGetFresh;
            } else if (typeof apiBatchGet === 'function') {
                batchGetter = apiBatchGet;
            }
            var getter = batchGetter || (options.noCache === true && typeof apiGetFresh === 'function' ? apiGetFresh : apiGet);
            var infoRequest = getter('/api/system/info');
            infoRequest
                .then(res => {
                    if (!res) {
                        console.warn('[device-config] /api/system/info empty response');
                        return;
                    }
                    // 优先取 res.data；若无则把 res 自身视作 data
                    const d = (res && typeof res === 'object' && res.data) ? res.data : res;
                    const dev = d.device || d.hardware || d;
                    const fw = d.firmware || {};
                    const memory = d.memory || {};
                    const flash = d.flash || {};
                    const set = (id, val) => { const el = document.getElementById(id); if (el) el.textContent = (val !== undefined && val !== null && val !== '') ? val : '--'; };
                    set('dev-sys-chip', dev.chipModel || dev.model || d.chipModel);
                    const cpuFreq = dev.cpuFreqMHz !== undefined ? dev.cpuFreqMHz : d.cpuFreqMHz;
                    const freeHeap = dev.freeHeap !== undefined ? dev.freeHeap : (memory.heapFree !== undefined ? memory.heapFree : d.freeHeap);
                    const flashSize = dev.flashSize !== undefined ? dev.flashSize : (flash.total !== undefined ? flash.total : d.flashSize);
                    set('dev-sys-cpu', cpuFreq !== undefined ? cpuFreq + ' MHz' : '--');
                    set('dev-sys-heap', freeHeap !== undefined ? Math.round(freeHeap / 1024) + ' KB' : '--');
                    set('dev-sys-flash', flashSize !== undefined ? Math.round(flashSize / 1024 / 1024) + ' MB' : '--');
                    set('dev-sys-sdk', dev.sdkVersion || d.sdkVersion);
                    set('dev-sys-fw', fw.version || dev.firmwareVersion || '--');
                })
                .catch(err => {
                    // 页面切换导致的请求中止属预期行为，静默返回
                    if (typeof window.isPageAbortError === 'function' && window.isPageAbortError(err)) return;
                    console.warn('[device-config] /api/system/info failed:', err && (err.status || err.message || err));
                });
        },

        saveDeviceBasic() {
            const deviceIdInput = document.getElementById('dev-id');
            let deviceId = deviceIdInput?.value?.trim() || '';

            const productNumberVal = document.getElementById('dev-product-number')?.value;
            const config = {
                deviceId: deviceId,
                deviceName: document.getElementById('dev-name')?.value || '',
                productNumber: productNumberVal !== undefined && productNumberVal !== '' ? parseInt(productNumberVal, 10) : 0,
                userId: document.getElementById('dev-user-id')?.value || '',
                description: document.getElementById('dev-description')?.value || '',
                ntpServer1: document.getElementById('dev-ntp-server1')?.value || DEFAULT_NTP_SERVER1,
                ntpServer2: document.getElementById('dev-ntp-server2')?.value || DEFAULT_NTP_SERVER2,
                timezone: document.getElementById('dev-timezone')?.value || 'CST-8',
                enableNTP: document.getElementById('dev-ntp-enable')?.value || '1',
                syncInterval: document.getElementById('dev-sync-interval')?.value || '3600',
                cacheDuration: parseInt(document.getElementById('dev-cache-duration')?.value || '86400'),
                logLevel: document.getElementById('dev-log-level')?.value || 'INFO',
            };
            apiPut('/api/device/config', config)
                .then(res => {
                    if (res && res.success) {
                        // 后端可能自动生成了新的 deviceId（FBE+MAC），立即回填输入框
                        if (res.data && res.data.deviceId && deviceIdInput) {
                            deviceIdInput.value = res.data.deviceId;
                        }
                        if (typeof window.apiInvalidateCache === 'function') {
                            window.apiInvalidateCache('/api/device/config');
                        }
                        this._showMessage('dev-basic-success', true);
                        Notification.success(t('dc-save-success', '设备信息保存成功'), t('dc-label-device', '设备信息'));
                    } else {
                        Notification.error(res?.error || t('common-save-fail', '保存失败'), t('dc-label-device', '设备信息'));
                    }
                })
                .catch(() => Notification.error(t('common-save-fail', '保存失败'), t('dc-label-device', '设备信息')));
        },

        saveCacheDuration() {
            const duration = parseInt(document.getElementById('dev-cache-duration')?.value || '86400');
            apiPut('/api/device/config', { cacheDuration: duration })
                .then(res => {
                    if (res && res.success) {
                        Notification.success(t('dc-cache-save-success', '缓存时间设置已保存'));
                    } else {
                        Notification.error(res?.error || t('common-save-fail', '保存失败'));
                    }
                })
                .catch(() => Notification.error(t('common-save-fail', '保存失败')));
        },

        clearBrowserCache: function() {
            try {
                // 1. 清除 Service Worker 缓存
                if (window.caches) {
                    caches.keys().then(function(names) {
                        names.forEach(function(name) { caches.delete(name); });
                    }).catch(function() {});
                }
                // 注销 Service Worker
                if (navigator.serviceWorker) {
                    navigator.serviceWorker.getRegistrations().then(function(regs) {
                        regs.forEach(function(reg) { reg.unregister(); });
                    }).catch(function() {});
                }

                // 2. 收集页面上所有已加载的 JS/CSS 资源 URL（去重）
                var urlMap = {};
                document.querySelectorAll('script[src]').forEach(function(el) {
                    if (el.src) urlMap[el.src] = true;
                });
                document.querySelectorAll('link[rel="stylesheet"]').forEach(function(el) {
                    if (el.href) urlMap[el.href] = true;
                });
                // 补充可能已被浏览器缓存的按需加载模块
                var origin = window.location.origin;
                var modules = [
                    'dashboard',
                    'device-config',
                    'device-control',
                    'network',
                    'peripherals',
                    'periph-exec',
                    'protocol'
                ];
                modules.forEach(function(m) { urlMap[origin + '/js/modules/' + m + '.js'] = true; });
                var urls = Object.keys(urlMap);

                // 3. 使用 fetch cache:'reload' 强制浏览器重新从服务器获取并更新 HTTP 缓存
                //    分批执行（每次2个并发），避免 ESP32 连接过多
                var idx = 0;
                Notification.success(
                    t('dc-cache-clearing', '正在清除缓存并刷新资源...')
                );

                function fetchBatch() {
                    if (idx >= urls.length) {
                        // 所有 JS/CSS 缓存已更新，最后刷新 HTML 页面本身
                        window.location.reload();
                        return;
                    }
                    var batch = urls.slice(idx, idx + 2);
                    idx += 2;
                    Promise.all(batch.map(function(url) {
                        return fetch(url, { cache: 'reload' }).catch(function() {});
                    })).then(fetchBatch);
                }
                fetchBatch();
            } catch (e) {
                console.error(t('dc-cache-clear-fail', '清除缓存失败:'), e);
                window.location.reload(true);
            }
        },

        saveDeviceNTP() {
            const config = {
                ntpServer1: document.getElementById('dev-ntp-server1')?.value || DEFAULT_NTP_SERVER1,
                ntpServer2: document.getElementById('dev-ntp-server2')?.value || DEFAULT_NTP_SERVER2,
                timezone: document.getElementById('dev-timezone')?.value || 'CST-8',
                enableNTP: document.getElementById('dev-ntp-enable')?.value || '1',
                syncInterval: document.getElementById('dev-sync-interval')?.value || '3600',
            };
            apiPut('/api/device/config', config)
                .then(res => {
                    if (res && res.success) {
                        if (typeof window.apiInvalidateCache === 'function') {
                            window.apiInvalidateCache('/api/device/config');
                        }
                        this._showMessage('dev-ntp-success', true);
                        Notification.success(t('dc-ntp-save-success', 'NTP配置保存成功'));
                        if (config.enableNTP === '1' || config.enableNTP === true) {
                            this.syncDeviceTime({ silent: true });
                        } else {
                            this.loadDeviceTime({ noCache: true });
                        }
                    } else {
                        Notification.error(res?.error || t('common-save-fail', '保存失败'));
                    }
                })
                .catch(() => Notification.error(t('common-save-fail', '保存失败')));
        },

        saveSecurityPolicy() {
            const lockoutSeconds = parseInt(document.getElementById('dev-sec-lockout-time')?.value || '300');
            const config = {
                maxLoginAttempts: parseInt(document.getElementById('dev-sec-max-attempts')?.value || '5'),
                loginLockoutTime: lockoutSeconds * 1000,
                minPasswordLength: parseInt(document.getElementById('dev-sec-min-pwd-len')?.value || '6'),
                requireStrongPasswords: document.getElementById('dev-sec-require-strong')?.value === '1',
            };
            apiPut('/api/device/config', config)
                .then(res => {
                    if (res && res.success) {
                        if (typeof window.apiInvalidateCache === 'function') {
                            window.apiInvalidateCache('/api/device/config');
                        }
                        this._showMessage('dev-sec-success', true);
                        Notification.success(t('dc-security-save-success', '安全策略保存成功'), t('dc-label-device', '设备信息'));
                    } else {
                        Notification.error(res?.error || t('common-save-fail', '保存失败'));
                    }
                })
                .catch(() => Notification.error(t('common-save-fail', '保存失败')));
        },

        loadDeviceTime(options) {
            options = options || {};
            var self = this;
            const btn = document.getElementById('dev-time-refresh-btn');
            if (btn) { btn.disabled = true; btn.innerHTML = t('common-refreshing', '刷新中...'); }

            var batchGetter = null;
            if (options.noCache === true && typeof apiBatchGetFresh === 'function') {
                batchGetter = apiBatchGetFresh;
            } else if (typeof apiBatchGet === 'function') {
                batchGetter = apiBatchGet;
            }

            if (batchGetter) {
                Promise.all([
                    batchGetter('/api/device/time'),
                    batchGetter('/api/network/status')
                ])
                    .then(([timeRes, netRes]) => {
                        const timeData = (timeRes && timeRes.success) ? timeRes.data || {} : {};
                        const netData = (netRes && netRes.success) ? netRes.data || {} : {};
                        const internetAvailable = netData.internetAvailable === true;
                        self._renderDeviceTime(timeData, internetAvailable);
                    })
                    .catch(err => {
                        if (isExpectedAbort(err) || isTransientNetworkError(err)) {
                            restoreTimeRefreshButton(btn);
                            return;
                        }
                        console.error('Load device time failed:', err);
                        restoreTimeRefreshButton(btn);
                    });
                return;
            }

            var getter = (options.noCache === true && typeof apiGetFresh === 'function') ? apiGetFresh : apiGet;
            getter('/api/device/time')
                .then(timeRes => {
                    return getter('/api/network/status')
                        .catch(() => null)
                        .then(netRes => [timeRes, netRes]);
                })
                .then(([timeRes, netRes]) => {
                    const timeData = (timeRes && timeRes.success) ? timeRes.data || {} : {};
                    const netData = (netRes && netRes.success) ? netRes.data || {} : {};
                    const internetAvailable = netData.internetAvailable === true;
                    self._renderDeviceTime(timeData, internetAvailable);
                })
                .catch(err => {
                    if (isExpectedAbort(err) || isTransientNetworkError(err)) {
                        restoreTimeRefreshButton(btn);
                        return;
                    }
                    console.error('Load device time failed:', err);
                    restoreTimeRefreshButton(btn);
                });
        },

        syncDeviceTime(options) {
            options = options || {};
            if (this._syncInProgress) return;
            this._syncInProgress = true;
            const btn = document.getElementById('dev-time-refresh-btn');
            if (btn) { btn.disabled = true; btn.innerHTML = t('common-refreshing', '刷新中...'); }
            apiPost('/api/device/time/sync', {})
                .then(res => {
                    const data = (res && res.data) ? res.data : {};
                    const synced = !!(res && res.success && data.synced);
                    if (!res || !res.success || !synced) {
                        if (!options.silent) Notification.warning(
                            (res && res.error) || t('dc-ntp-sync-fail', 'NTP同步失败'), 'NTP');
                        this.loadDeviceTime({ noCache: true });
                        return;
                    }
                    this._renderDeviceTime(data, data.internetAvailable !== false);
                    if (!options.silent) Notification.success(t('dc-ntp-sync-success', 'NTP同步成功'), 'NTP');
                })
                .catch(err => {
                    if (isExpectedAbort(err) || isTransientNetworkError(err)) {
                        restoreTimeRefreshButton(btn);
                        if (!options.silent) Notification.error(t('dc-ntp-sync-fail', 'NTP同步失败'), 'NTP');
                        return;
                    }
                    console.error('Sync device time failed:', err);
                    if (!options.silent) Notification.error(t('dc-ntp-sync-fail', 'NTP同步失败'), 'NTP');
                    this.loadDeviceTime({ noCache: true });
                })
                .finally(() => {
                    this._syncInProgress = false;
                });
        },

        _renderDeviceTime(d, internetAvailable = true) {
            const setEl = (id, val) => { const el = document.getElementById(id); if (el) el.textContent = val || '--'; };
            const setHtml = (id, html) => { const el = document.getElementById(id); if (el) el.innerHTML = html; };

            setEl('dev-time-datetime', d.datetime);

            if (!internetAvailable) {
                setHtml('dev-time-synced', '<span class="badge badge-warning">' + t('dc-ntp-not-synced', '未同步') + '</span>');
            } else if (d.synced) {
                setHtml('dev-time-synced', '<span class="badge badge-success">' + t('dc-ntp-synced', '已同步') + '</span>');
            } else {
                setHtml('dev-time-synced', '<span class="badge badge-warning">' + t('dc-ntp-not-synced', '未同步') + '</span>');
            }

            if (d.uptime !== undefined) {
                const ms = d.uptime;
                const h = Math.floor(ms / 3600000);
                const m = Math.floor((ms % 3600000) / 60000);
                const s = Math.floor((ms % 60000) / 1000);
                setEl('dev-time-uptime', h + t('dc-ntp-time-format', '时') + ' ' + m + t('dc-ntp-time-format-m', '分') + ' ' + s + t('dc-ntp-time-format-s', '秒'));
            }

            const btn = document.getElementById('dev-time-refresh-btn');
            if (btn) {
                // 保持按钮可用：网络探测结果可能尚未刷新，后端同步接口会返回准确错误。
                btn.disabled = false;
                btn.innerHTML = t('common-refresh', '刷新');
                btn.title = internetAvailable ? '' : t('dc-no-network-hint', '网络状态未确认，将尝试进行NTP同步');
            }
        },

        _toBoolean(value, fallback) {
            if (value === undefined || value === null || value === '') return fallback;
            if (typeof value === 'string') return !['0', 'false', 'off', 'no'].includes(value.toLowerCase());
            return !!value;
        },

        restartDevice() {
            const delay = document.getElementById('dev-restart-delay')?.value || '3';
            const btn = document.getElementById('dev-restart-btn');
            if (!confirm(t('dc-reboot-confirm', '确定要重启设备？将在') + ' ' + delay + ' ' + t('dc-reboot-confirm-suffix', '秒后重启。'))) return;
            if (btn) { btn.disabled = true; btn.innerHTML = t('dc-reboot-sending', '… 处理中...'); }

            apiRestart({ delay })
                .then(res => {
                    if (res && res.success) {
                        Notification.success(t('dc-reboot-scheduled', '设备将在') + delay + t('dc-reboot-scheduled-suffix', '秒后重启，请稍后刷新页面。'), t('dc-reboot-title', '设备重启'));
                    } else {
                        if (btn) { btn.disabled = false; btn.innerHTML = t('dc-reboot-now', '立即重启'); }
                        Notification.error(t('dc-reboot-fail', '重启指令发送失败'), t('dc-reboot-title', '设备重启'));
                    }
                })
                .catch(err => {
                    // 区分超时 vs 连接断开（设备重启导致连接断开属预期）
                    const isTimeout = err && err.name === 'AbortError';
                    const isConnectionClosed = err && (
                        err.name === 'TypeError' ||
                        (err.message && (err.message.includes('fetch') || err.message.includes('network')))
                    );
                    if (isTimeout) {
                        Notification.warning(t('dc-reboot-timeout', '重启请求超时，设备可能正在重启也可能未收到指令，请等待 30 秒后刷新页面'), t('dc-reboot-timeout-title', '重启超时'));
                    } else if (isConnectionClosed) {
                        Notification.success(t('dc-reboot-scheduled', '设备将在') + delay + t('dc-reboot-scheduled-suffix', '秒后重启，请稍后刷新页面。'), t('dc-reboot-title', '设备重启'));
                    } else {
                        if (btn) { btn.disabled = false; btn.innerHTML = t('dc-reboot-now', '立即重启'); }
                        Notification.error(t('dc-reboot-fail', '重启指令发送失败'), t('dc-reboot-title', '设备重启'));
                    }
                });
        },

        factoryReset() {
            const confirmInput = document.getElementById('dev-factory-confirm');
            const btn = document.getElementById('dev-factory-btn');
            const confirmValue = confirmInput?.value?.toUpperCase().trim();

            if (confirmValue !== 'RESET') {
                Notification.warning(t('dc-reset-confirm', '请输入 RESET 以确认操作'), t('dc-reset-btn', '恢复出厂设置'));
                if (confirmInput) confirmInput.focus();
                return;
            }

            if (!confirm(t('dc-reset-warning', '警告：此操作将清除所有用户配置和数据，恢复到出厂默认状态！'))) {
                if (confirmInput) confirmInput.value = '';
                return;
            }

            if (btn) { btn.disabled = true; btn.innerHTML = t('dc-reboot-sending', '… 处理中...'); }

            apiFactoryReset()
                .then(res => {
                    if (res && res.success) {
                        Notification.success(t('dc-reset-success', '恢复出厂设置成功，设备正在重启...'), t('dc-reset-btn', '恢复出厂设置'));
                    } else {
                        if (btn) { btn.disabled = false; btn.innerHTML = t('dc-reset-btn', '恢复出厂设置'); }
                        Notification.error(t('dc-reset-fail', '恢复出厂设置失败'), t('dc-reset-btn', '恢复出厂设置'));
                    }
                })
                .catch(err => {
                    // 区分超时 vs 连接断开（设备恢复出厂导致连接断开属预期）
                    const isTimeout = err && err.name === 'AbortError';
                    const isConnectionClosed = err && (
                        err.name === 'TypeError' ||
                        (err.message && (err.message.includes('fetch') || err.message.includes('network')))
                    );
                    if (isTimeout) {
                        Notification.warning(t('dc-reset-timeout', '恢复出厂请求超时，设备可能正在重启也可能未收到指令，请等待 30 秒后刷新页面'), t('dc-reset-timeout-title', '恢复出厂超时'));
                    } else if (isConnectionClosed) {
                        Notification.success(t('dc-reset-success', '恢复出厂设置成功，设备正在重启...'), t('dc-reset-btn', '恢复出厂设置'));
                    } else {
                        if (btn) { btn.disabled = false; btn.innerHTML = t('dc-reset-btn', '恢复出厂设置'); }
                        Notification.error(t('dc-reset-fail', '恢复出厂设置失败'), t('dc-reset-btn', '恢复出厂设置'));
                    }
                });
        },

        _renderDeveloperModeState() {
            const enabled = this.isDeveloperModeEnabled();
            const statusEl = document.getElementById('dev-mode-status');
            const btn = document.getElementById('dev-mode-toggle-btn');
            if (statusEl) {
                statusEl.textContent = enabled ? t('common-enabled', '已启用') : t('common-disabled', '已禁用');
                statusEl.classList.toggle('u-text-danger', !enabled);
            }
            if (btn) {
                btn.classList.remove('fb-btn-danger', 'fb-btn-success');
                btn.classList.add('fb-btn-warning');
                btn.innerHTML = enabled
                    ? '<span>' + t('dc-dev-mode-disable-btn', '禁用开发环境') + '</span>'
                    : '<span>' + t('dc-dev-mode-enable-btn', '启用开发环境') + '</span>';
            }
        },

        toggleDeveloperMode() {
            const enabled = this.isDeveloperModeEnabled();
            const passwordInput = document.getElementById('dev-mode-password');
            const btn = document.getElementById('dev-mode-toggle-btn');
            const password = passwordInput ? passwordInput.value : '';
            if (!password) {
                Notification.warning(t('dc-dev-mode-enter-pwd', '请输入登录密码'), t('dc-dev-mode-title', '开发环境功能'));
                if (passwordInput) passwordInput.focus();
                return;
            }
            const confirmText = enabled
                ? t('dc-dev-mode-disable-confirm', '确定要禁用开发环境功能吗？')
                : t('dc-dev-mode-enable-confirm', '确定要启用开发环境功能吗？');
            if (!confirm(confirmText)) return;

            if (btn) {
                btn.disabled = true;
                btn.innerHTML = '<span>' + t('common-processing', '处理中...') + '</span>';
            }
            apiPost('/api/device/developer-mode', {
                enabled: enabled ? 'false' : 'true',
                password: password
            }).then((res) => {
                if (res && res.success) {
                    const nextEnabled = res.data ? res.data.developerModeEnabled !== false : !enabled;
                    this.setDeveloperModeState(nextEnabled);
                    this._renderDeveloperModeState();
                    if (passwordInput) passwordInput.value = '';
                    Notification.success(
                        nextEnabled ? t('dc-dev-mode-enabled', '开发环境已启用') : t('dc-dev-mode-disabled', '开发环境已禁用'),
                        t('dc-dev-mode-title', '开发环境功能')
                    );
                } else {
                    Notification.error(res?.error || t('dc-dev-mode-fail', '开发环境设置失败'), t('dc-dev-mode-title', '开发环境功能'));
                }
            }).catch((err) => {
                if (typeof window.apiNotifyError === 'function') {
                    window.apiNotifyError(err, t('dc-dev-mode-fail', '开发环境设置失败'), t('dc-dev-mode-title', '开发环境功能'));
                } else {
                    Notification.error(t('dc-dev-mode-fail', '开发环境设置失败'), t('dc-dev-mode-title', '开发环境功能'));
                }
            }).finally(() => {
                if (btn) {
                    btn.disabled = false;
                    this._renderDeveloperModeState();
                }
            });
        },

        _setConfigTransferStatus(text) {
            const el = document.getElementById('dev-config-transfer-status');
            if (el) el.textContent = text || '--';
        },

        _configTransferFetch(url) {
            const headers = {};
            const token = localStorage.getItem('auth_token');
            if (token) headers.Authorization = 'Bearer ' + token;
            return fetch(url, {
                method: 'GET',
                headers,
                credentials: 'include',
                cache: 'no-store'
            });
        },

        _normalizeConfigFileName(name) {
            return String(name || '')
                .replace(/^fastbee-config-/, '')
                .replace(/\s+\(\d+\)(?=\.json$)/i, '');
        },

        /**
         * 根据 JSON 内容自动检测配置文件类型，返回规范文件名
         * 当文件名不是已知配置名时，用于 AI 生成配置等场景的自动识别
         */
        _detectConfigFileName(name, parsed) {
            // 已经是已知配置文件名，无需检测
            if (CONFIG_TRANSFER_LABELS[name]) return name;
            if (!parsed || typeof parsed !== 'object') return name;

            // 按内容特征匹配
            if (Array.isArray(parsed.peripherals)) return 'peripherals.json';
            if (Array.isArray(parsed.rules)) return 'periph_exec.json';
            if (parsed.staSSID !== undefined || parsed.apSSID !== undefined || parsed.mode !== undefined) return 'network.json';
            if (parsed.deviceName !== undefined || parsed.chipType !== undefined) return 'device.json';
            if (parsed.mqtt !== undefined || parsed.server !== undefined) return 'protocol.json';
            // 功能配置：功能页导出的包装格式（fastbee-feature-config）或设备原始
            // features.json；也兼容 feature-settings.json/functions.json 等友好命名
            if (parsed.type === 'fastbee-feature-config'
                || (parsed.features && typeof parsed.features === 'object' && !Array.isArray(parsed.features))) {
                return 'features.json';
            }
            return name;
        },

        _getConfigTransferLabel(name) {
            return CONFIG_TRANSFER_LABELS[name] || name || '--';
        },

        /**
         * 显示配置文件选择弹窗
         * @param {Array} items - [{name, size?}] 配置文件列表
         * @param {string} mode - 'export' 或 'import'
         * @returns {Promise<Array>} 用户选中的文件列表
         */
        _showConfigTransferModal(items, mode) {
            const self = this;
            return new Promise((resolve, reject) => {
                const titleEl = document.getElementById('config-transfer-modal-title');
                const listEl = document.getElementById('config-transfer-list');
                const selectAllEl = document.getElementById('config-transfer-select-all');
                const summaryEl = document.getElementById('config-transfer-summary');
                const confirmBtn = document.getElementById('config-transfer-confirm-btn');
                const cancelBtn = document.getElementById('config-transfer-cancel-btn');
                if (!listEl || !confirmBtn || !cancelBtn) { reject(new Error(t('dc-transfer-modal-missing', '弹窗元素不存在'))); return; }

                // 设置标题和确认按钮文字
                if (titleEl) titleEl.textContent = mode === 'export' ? t('dc-transfer-export-title', '选择导出的配置') : t('dc-transfer-import-title', '选择导入的配置');
                confirmBtn.textContent = mode === 'export' ? t('dc-transfer-export-confirm', '确认导出') : t('dc-transfer-import-confirm', '确认导入');

                // 渲染复选框列表
                listEl.innerHTML = '';
                items.forEach((item, idx) => {
                    const label = document.createElement('label');
                    label.className = 'fb-checkbox';
                    const cb = document.createElement('input');
                    cb.type = 'checkbox';
                    cb.checked = true;
                    cb.dataset.idx = idx;
                    const span = document.createElement('span');
                    const displayName = self._getConfigTransferLabel(item.name);
                    span.textContent = displayName + (item.size ? ' (' + (item.size > 1024 ? (item.size / 1024).toFixed(1) + ' KB' : item.size + ' B') + ')' : '');
                    label.appendChild(cb);
                    label.appendChild(span);
                    listEl.appendChild(label);
                });

                // 更新摘要
                function updateSummary() {
                    const checked = listEl.querySelectorAll('input[type="checkbox"]:checked').length;
                    if (summaryEl) summaryEl.textContent = checked + '/' + items.length + t('dc-transfer-item-count', ' 项');
                    confirmBtn.disabled = checked === 0;
                    if (selectAllEl) selectAllEl.checked = checked === items.length;
                }
                updateSummary();

                // 全选/取消全选
                function onSelectAll() {
                    const checked = selectAllEl.checked;
                    listEl.querySelectorAll('input[type="checkbox"]').forEach(cb => { cb.checked = checked; });
                    updateSummary();
                }
                if (selectAllEl) { selectAllEl.checked = true; selectAllEl.addEventListener('change', onSelectAll); }

                // 单项变化
                function onItemChange() { updateSummary(); }
                listEl.addEventListener('change', onItemChange);

                // 清理函数
                function cleanup() {
                    if (selectAllEl) selectAllEl.removeEventListener('change', onSelectAll);
                    listEl.removeEventListener('change', onItemChange);
                    confirmBtn.removeEventListener('click', onConfirm);
                    cancelBtn.removeEventListener('click', onCancel);
                    AppState.hideModal('config-transfer-modal');
                }

                function onConfirm() {
                    const selected = [];
                    listEl.querySelectorAll('input[type="checkbox"]:checked').forEach(cb => {
                        const idx = parseInt(cb.dataset.idx);
                        if (items[idx]) selected.push(items[idx]);
                    });
                    cleanup();
                    resolve(selected);
                }
                function onCancel() {
                    cleanup();
                    reject(new Error(t('dc-transfer-user-cancel', '用户取消')));
                }

                confirmBtn.addEventListener('click', onConfirm);
                cancelBtn.addEventListener('click', onCancel);
                AppState.showModal('config-transfer-modal');
            });
        },

        _configTransferByteLength(content) {
            let bytes = 0;
            for (let i = 0; i < content.length;) {
                const code = content.codePointAt(i);
                bytes += code <= 0x7F ? 1 : (code <= 0x7FF ? 2 : (code <= 0xFFFF ? 3 : 4));
                i += code > 0xFFFF ? 2 : 1;
            }
            return bytes;
        },

        _splitConfigTransferChunks(content, maxBytes) {
            const chunks = [];
            let start = 0;
            let bytes = 0;
            for (let i = 0; i < content.length;) {
                const code = content.codePointAt(i);
                const charLen = code > 0xFFFF ? 2 : 1;
                const charBytes = code <= 0x7F ? 1 : (code <= 0x7FF ? 2 : (code <= 0xFFFF ? 3 : 4));
                if (bytes > 0 && bytes + charBytes > maxBytes) {
                    chunks.push(content.slice(start, i));
                    start = i;
                    bytes = 0;
                }
                bytes += charBytes;
                i += charLen;
            }
            if (start < content.length) chunks.push(content.slice(start));
            return chunks;
        },

        _validateConfigTransferEntry(item) {
            const name = this._normalizeConfigFileName(item && item.name);
            const content = String((item && item.content) || '');
            if (!/^[A-Za-z0-9_.-]+\.json$/.test(name)) {
                throw new Error(t('dc-transfer-invalid-name', '配置文件名不正确：') + (item && item.name ? item.name : '--'));
            }
            if (!content || this._configTransferByteLength(content) > MAX_CONFIG_TRANSFER_BYTES) {
                throw new Error(t('dc-transfer-empty-or-large', '配置文件过大或为空：') + name);
            }
            try {
                JSON.parse(content);
            } catch (err) {
                throw new Error(t('dc-transfer-invalid-json', '配置文件不是有效 JSON：') + name);
            }
            return { name, content };
        },

        async _importConfigTransferEntry(item) {
            const entry = this._validateConfigTransferEntry(item);

            // 字段过滤容错：读取设备当前配置作为 schema，过滤导入文件中的多余字段
            let filteredContent = entry.content;
            if (typeof window.filterConfigFields === 'function') {
                try {
                    const url = new URL('/api/config/transfer/export', window.location.origin);
                    url.searchParams.set('name', entry.name);
                    const resp = await this._configTransferFetch(url.toString());
                    if (resp.ok) {
                        const currentText = await resp.text();
                        const currentObj = JSON.parse(currentText);
                        const importedObj = JSON.parse(entry.content);
                        const filtered = window.filterConfigFields(importedObj, currentObj);
                        filteredContent = JSON.stringify(filtered);
                    }
                } catch (filterErr) {
                    // 过滤失败不影响正常导入流程，继续使用原始内容
                    console.warn('[device-config] field filter skipped for', entry.name, filterErr);
                }
            }

            const chunks = this._splitConfigTransferChunks(filteredContent, CONFIG_IMPORT_CHUNK_BYTES);
            const total = chunks.length;
            let cleanedCount = 0;
            let i = 0;
            while (i < total) {
                const payload = {
                    name: entry.name,
                    index: String(i),
                    total: String(total),
                    chunk: chunks[i]
                };
                let res = null;
                let lastErrMsg = null;
                let attempt = 0;
                // 退化链路下跨包 body 丢失会使设备收到空分片（400），重试可恢复。
                // 幂等性：响应到达前超时/断连时设备可能已写入当前分片，重试前退回 index=0
                // 重新覆盖写（index=0 为覆盖语义），避免追加语义下内容重复。
                while (attempt <= CONFIG_IMPORT_CHUNK_RETRIES) {
                    try {
                        res = await apiPost('/api/config/transfer/import-chunk', payload, 30000);
                    } catch (reqErr) {
                        res = null;
                        const status = reqErr && reqErr.status;
                        lastErrMsg = (reqErr && reqErr.data && reqErr.data.error) || reqErr && reqErr.message || t('dc-transfer-request-fail', '请求失败');
                        // 4xx 明确拒绝（设备未写入）：仅 400 空分片可重试，其余直接失败
                        if (status !== undefined && status >= 400 && status < 500) {
                            if (status !== 400) throw new Error(lastErrMsg);
                        } else {
                            // 网络错误/超时/5xx：写入状态未知，退回首个分片重来
                            i = 0;
                            Object.assign(payload, { index: '0', chunk: chunks[0] });
                        }
                    }
                    if (res && res.success) break;
                    if (res && !res.success) lastErrMsg = res.error || lastErrMsg;
                    attempt++;
                    if (attempt <= CONFIG_IMPORT_CHUNK_RETRIES) {
                        await new Promise(r => setTimeout(r, 500 * attempt));
                    }
                }
                if (!res || !res.success) {
                    throw new Error(lastErrMsg || (t('dc-transfer-chunk-fail', '分片导入失败：') + entry.name));
                }
                // 最后一个分片可能包含清理信息
                if (res.data && res.data.cleaned) {
                    cleanedCount = res.data.cleaned;
                }
                i++;
            }
            if (cleanedCount > 0) {
                Notification.info(t('dc-transfer-cleaned-prefix', '已自动清理') + cleanedCount + t('dc-transfer-cleaned-suffix', ' 个残留配置文件'), t('dc-transfer-import-title-short', '配置导入'));
            }
            return { success: true, name: entry.name };
        },

        _readSelectedConfigEntries(selectedFiles) {
            return Promise.all(selectedFiles.map((file) => file.text().then((text) => ({ file, text }))))
                .then((items) => {
                    const entries = [];
                    items.forEach(({ file, text }) => {
                        let parsed = null;
                        try {
                            parsed = JSON.parse(text);
                        } catch (err) {
                            parsed = null;
                        }

                        if (parsed && parsed.type === 'fastbee-config-bundle' && Array.isArray(parsed.files)) {
                            parsed.files.forEach((item) => entries.push(item));
                            return;
                        }

                        // 自动检测：文件名不是已知配置名时，根据 JSON 内容推断正确的配置文件名
                        const normalizedName = this._normalizeConfigFileName(file.name);
                        const detectedName = this._detectConfigFileName(normalizedName, parsed);
                        if (detectedName !== normalizedName) {
                            console.info(`[device-config] Auto-detected config type: ${file.name} → ${detectedName}`);
                            // 在页面上显示自动重命名提示
                            this._setConfigTransferStatus(
                                t('dc-transfer-auto-detected', '已自动识别：') + file.name + ' → ' + this._getConfigTransferLabel(detectedName)
                            );
                        }

                        // JSON 格式预检查
                        if (!parsed || typeof parsed !== 'object') {
                            throw new Error(t('dc-transfer-invalid-json-file', '文件') + ' ' + file.name + ' ' + t('dc-transfer-invalid-json-format', '不是有效的 JSON 格式'));
                        }

                        entries.push({
                            name: detectedName,
                            content: text
                        });
                    });

                    return entries.map((item) => this._validateConfigTransferEntry(item));
                });
        },

        exportDeviceConfigBundle() {
            const btn = document.getElementById('dev-config-export-btn');
            if (btn) btn.disabled = true;
            this._setConfigTransferStatus(t('dc-transfer-reading-list', '正在读取配置列表...'));

            apiGetFresh('/api/config/transfer/list')
                .then((res) => {
                    const files = (res && res.success && res.data && res.data.files) ? res.data.files : [];
                    if (!files.length) throw new Error(t('dc-transfer-no-exportable', '没有可导出的配置文件'));
                    return this._showConfigTransferModal(files, 'export');
                })
                .then(async (selected) => {
                    if (!selected.length) throw new Error(t('dc-transfer-none-selected', '未选择任何配置文件'));
                    this._setConfigTransferStatus(t('dc-transfer-exporting', '正在导出...'));

                    const bundle = {
                        type: 'fastbee-config-bundle',
                        version: 1,
                        scope: selected.length === 1 ? selected[0].name : 'selected',
                        exportedAt: new Date().toISOString(),
                        files: []
                    };

                    for (let i = 0; i < selected.length; i++) {
                        const item = selected[i];
                        this._setConfigTransferStatus(t('dc-transfer-exporting', '正在导出') + ' ' + item.name + ' (' + (i + 1) + '/' + selected.length + ')...');
                        const url = new URL('/api/config/transfer/export', window.location.origin);
                        url.searchParams.set('name', item.name);
                        const resp = await this._configTransferFetch(url.toString());
                        if (!resp.ok) throw new Error(t('dc-transfer-export-fail', '导出失败：') + item.name);
                        const content = await resp.text();
                        if (this._configTransferByteLength(content) > MAX_CONFIG_TRANSFER_BYTES) {
                            throw new Error(t('dc-transfer-file-too-large', '配置文件过大：') + item.name);
                        }
                        bundle.files.push({ name: item.name, content });
                    }

                    // 单文件直接导出原始JSON，多文件导出bundle包
                    const exportContent = selected.length === 1
                        ? bundle.files[0].content
                        : JSON.stringify(bundle, null, 2);
                    const exportName = selected.length === 1
                        ? bundle.files[0].name
                        : 'fastbee-config-' + new Date().toISOString().replace(/[:.]/g, '-') + '.json';
                    const blob = new Blob([exportContent], { type: 'application/json' });
                    const link = document.createElement('a');
                    const objectUrl = URL.createObjectURL(blob);
                    link.href = objectUrl;
                    link.download = exportName;
                    document.body.appendChild(link);
                    link.click();
                    link.remove();
                    setTimeout(() => URL.revokeObjectURL(objectUrl), 3000);

                    this._setConfigTransferStatus(t('dc-transfer-export-done-prefix', '配置导出完成：') + bundle.files.length + t('dc-transfer-export-done-suffix', ' 个文件'));
                    Notification.success(t('dc-transfer-export-done', '配置导出完成'), t('dc-transfer-title', '配置导入/导出'));
                })
                .catch((err) => {
                    if (err && err.message === t('dc-transfer-user-cancel', '用户取消')) {
                        this._setConfigTransferStatus('--');
                        return;
                    }
                    console.error('[device-config] export config bundle failed:', err);
                    this._setConfigTransferStatus(t('dc-transfer-export-fail-status', '配置导出失败'));
                    Notification.error(err && err.message ? err.message : t('dc-transfer-export-fail-status', '配置导出失败'), t('dc-transfer-title', '配置导入/导出'));
                })
                .finally(() => {
                    if (btn) btn.disabled = false;
                });
        },

        importDeviceConfigBundle() {
            const input = document.getElementById('dev-config-import-file');
            const btn = document.getElementById('dev-config-import-btn');
            const selectedFiles = input && input.files ? Array.from(input.files) : [];
            if (!selectedFiles.length) {
                Notification.warning(t('dc-transfer-select-first', '请先选择配置文件或配置包'), t('dc-transfer-title', '配置导入/导出'));
                return;
            }
            const totalSize = selectedFiles.reduce((sum, file) => sum + (file.size || 0), 0);
            if (totalSize > MAX_CONFIG_BUNDLE_BYTES) {
                Notification.warning(t('dc-transfer-too-large', '配置文件总量过大，请分批导入'), t('dc-transfer-title', '配置导入/导出'));
                return;
            }

            if (btn) btn.disabled = true;
            this._setConfigTransferStatus(t('dc-transfer-parsing', '正在解析配置文件...'));

            this._readSelectedConfigEntries(selectedFiles)
                .then((files) => {
                    if (!files.length) throw new Error(t('dc-transfer-empty-bundle', '配置包内没有可导入的配置文件'));
                    // 弹窗让用户选择要导入的配置
                    return this._showConfigTransferModal(
                        files.map(f => ({ name: f.name, size: f.content ? f.content.length : 0 })),
                        'import'
                    ).then((selected) => {
                        // 只导入用户选中的文件
                        const selectedNames = selected.map(s => s.name);
                        return files.filter(f => selectedNames.includes(f.name));
                    });
                })
                .then(async (files) => {
                    if (!files.length) throw new Error(t('dc-transfer-none-selected', '未选择任何配置文件'));
                    for (let i = 0; i < files.length; i++) {
                        const item = files[i];
                        this._setConfigTransferStatus(t('dc-transfer-importing', '正在导入') + ' ' + item.name + ' (' + (i + 1) + '/' + files.length + ')...');
                        await this._importConfigTransferEntry(item);
                    }
                    this._setConfigTransferStatus(t('dc-transfer-import-done-prefix', '配置导入完成：') + files.length + t('dc-transfer-import-done-suffix', ' 个文件，建议重启设备使配置完全生效'));
                    Notification.success(t('dc-transfer-import-done', '配置导入完成，建议重启设备'), t('dc-transfer-title', '配置导入/导出'));
                    if (typeof window.apiInvalidateCache === 'function') {
                        window.apiInvalidateCache();
                    }
                })
                .catch((err) => {
                    if (err && err.message === t('dc-transfer-user-cancel', '用户取消')) {
                        this._setConfigTransferStatus('--');
                        return;
                    }
                    const errMsg = err && err.message ? err.message : t('dc-transfer-import-fail', '配置导入失败');
                    console.error('[device-config] import config bundle failed:', err);
                    this._setConfigTransferStatus(t('dc-transfer-import-fail-prefix', '配置导入失败：') + errMsg);
                    Notification.error(errMsg, t('dc-transfer-title', '配置导入/导出'));
                })
                .finally(() => {
                    if (btn) btn.disabled = false;
                });
        },

        // ============ 固件升级 (OTA) ============

        /**
         * 懒加载 device-ota 分片（完整 OTA 面板）
         * 成功后隐藏内联精简面板并绑定 device-ota 交互模块；
         * 失败（如 compact 配置未发布该分片）时清理容器错误提示，保留内联面板兜底
         */
        _loadOtaFragment() {
            if (typeof PageLoader === 'undefined') return;
            const container = document.getElementById('device-ota-container');
            if (!container) return;

            const self = this;
            PageLoader.loadFragment('device-ota-container', 'device-ota', function() {
                // 分片加载成功：隐藏内联兜底面板，按需加载交互模块
                const inline = document.getElementById('dev-ota-inline-panel');
                if (inline) inline.classList.add('is-hidden');
                self._loadModule('device-ota', function() {
                    if (typeof AppState.setupDeviceOta === 'function') {
                        AppState.setupDeviceOta();
                    }
                });
            });

            // 失败兜底：保留 PageLoader 的错误提示，避免 OTA tab 退化为只有标题的空白页
            setTimeout(function() {
                if (!document.getElementById('ota-url-form') && container) {
                    if (!container.textContent.trim()) {
                        container.innerHTML = '<div class="message message-error">OTA 升级内容加载失败，请刷新页面重试</div>';
                    }
                }
            }, 4000);
        },

        handleOTAFileSelect() {
            const input = document.getElementById('dev-ota-file');
            const uploadBtn = document.getElementById('dev-ota-upload-btn');
            const filenameEl = document.getElementById('dev-ota-filename');
            if (!input || !input.files || input.files.length === 0) return;

            const file = input.files[0];
            if (!file.name.endsWith('.bin')) {
                Notification.warning(t('dc-ota-select-bin', '请选择 .bin 固件文件'), t('dc-ota-title', '固件升级'));
                input.value = '';
                if (uploadBtn) uploadBtn.disabled = true;
                if (filenameEl) filenameEl.textContent = '';
                return;
            }
            if (filenameEl) filenameEl.textContent = file.name + ' (' + (file.size / 1024).toFixed(0) + ' KB)';
            if (uploadBtn) uploadBtn.disabled = false;
        },

        startOTAUpload() {
            const input = document.getElementById('dev-ota-file');
            const uploadBtn = document.getElementById('dev-ota-upload-btn');
            const progressWrap = document.getElementById('dev-ota-progress-wrap');
            const progressBar = document.getElementById('dev-ota-progress-bar');
            const progressText = document.getElementById('dev-ota-progress-text');
            if (!input || !input.files || input.files.length === 0) return;

            if (input.disabled) {
                Notification.warning('当前分区不支持 OTA 升级', t('dc-ota-title', '固件升级'));
                return;
            }

            const file = input.files[0];
            if (!confirm(t('dc-ota-confirm-upload', '确认上传固件「') + file.name + t('dc-ota-confirm-upload-suffix', '」并升级？升级过程中设备将重启，请勿断电。'))) return;

            if (uploadBtn) { uploadBtn.disabled = true; uploadBtn.innerHTML = t('dc-ota-uploading', '上传中...'); }
            if (progressWrap) progressWrap.classList.remove('is-hidden');
            if (progressBar) progressBar.style.width = '0%';
            if (progressText) progressText.textContent = '0%';

            // Step 1: 发起 OTA begin
            fetch('/api/ota/begin', { method: 'POST' })
                .then(res => res.json())
                .then(data => {
                    if (data && data.success === false) {
                        throw new Error(data.error || 'OTA begin failed');
                    }
                    // Step 2: 上传固件文件
                    const formData = new FormData();
                    formData.append('firmware', file, file.name);
                    return fetch('/api/ota/upload', { method: 'POST', body: formData });
                })
                .then(res => res.json())
                .then(data => {
                    if (data && data.success === false) {
                        throw new Error(data.error || 'OTA upload failed');
                    }
                    // 上传完成，开始轮询状态
                    if (progressText) progressText.textContent = t('dc-ota-writing', '固件写入中...');
                    this._pollOTAStatus();
                })
                .catch(err => {
                    if (typeof window.isPageAbortError === 'function' && window.isPageAbortError(err)) return;
                    if (uploadBtn) { uploadBtn.disabled = false; uploadBtn.innerHTML = t('dc-ota-upload-btn-text', '上传升级'); }
                    if (progressText) progressText.textContent = t('dc-ota-upload-fail-short', '上传失败: ') + (err.message || err);
                    Notification.error(t('dc-ota-upload-fail', '固件上传失败: ') + (err.message || err), t('dc-ota-title', '固件升级'));
                });
        },

        _pollOTAStatus() {
            const progressBar = document.getElementById('dev-ota-progress-bar');
            const progressText = document.getElementById('dev-ota-progress-text');
            const uploadBtn = document.getElementById('dev-ota-upload-btn');
            let pollCount = 0;
            const maxPolls = 120; // 最长轮询 120s

            const timer = setInterval(() => {
                pollCount++;
                if (pollCount > maxPolls) {
                    clearInterval(timer);
                    if (progressText) progressText.textContent = t('dc-ota-poll-timeout', '状态轮询超时，请手动刷新页面检查设备状态');
                    this._pollDeviceRecovery();
                    return;
                }

                fetch('/api/ota/status', { cache: 'no-store' })
                    .then(res => res.json())
                    .then(data => {
                        // /api/ota/status 返回 {success, data:{progress,...}} 包装结构
                        const body = (data && data.data) ? data.data : (data || {});
                        const progress = body.progress || 0;
                        if (progressBar) progressBar.style.width = progress + '%';
                        if (progressText) progressText.textContent = progress + '% - ' + (body.error || body.message || t('dc-ota-status-writing', '写入中'));

                        // 进度达到 100% 或 OTA 已完成（写入结束且不再进行中），开始轮询设备恢复
                        if (progress >= 100 || (body.in_progress === false && pollCount > 3 && body.current_size > 0)) {
                            clearInterval(timer);
                            if (progressText) progressText.textContent = t('dc-ota-write-done', '固件写入完成，设备正在重启...');
                            Notification.success(t('dc-ota-write-done-title', '固件写入完成，设备正在重启，请勿断电'), t('dc-ota-title', '固件升级'));
                            this._pollDeviceRecovery();
                        }
                    })
                    .catch(() => {
                        // 设备重启中，/api/ota/status 不可达，转为轮询设备恢复
                        clearInterval(timer);
                        if (progressText) progressText.textContent = t('dc-ota-rebooting', '设备正在重启...');
                        this._pollDeviceRecovery();
                    });
            }, 1000);
        },

        _pollDeviceRecovery() {
            const progressText = document.getElementById('dev-ota-progress-text');
            let pollCount = 0;
            const maxPolls = 90; // 最长等待 90s

            const timer = setInterval(() => {
                pollCount++;
                if (pollCount > maxPolls) {
                    clearInterval(timer);
                    if (progressText) progressText.textContent = t('dc-ota-recovery-timeout', '设备恢复超时，请手动刷新页面');
                    Notification.warning(t('dc-ota-recovery-timeout-title', '设备恢复超时，请手动刷新页面检查'), t('dc-ota-title', '固件升级'));
                    return;
                }

                fetch('/api/health', { cache: 'no-store' })
                    .then(res => {
                        if (res.ok) {
                            clearInterval(timer);
                            if (progressText) progressText.textContent = t('dc-ota-recovered', '设备已恢复，正在刷新页面...');
                            Notification.success(t('dc-ota-upgrade-done', '设备升级完成，正在刷新页面'), t('dc-ota-title', '固件升级'));
                            setTimeout(() => window.location.reload(), 1500);
                        }
                    })
                    .catch(() => {
                        // 设备仍在重启中，继续轮询
                        if (progressText && pollCount % 10 === 0) {
                            progressText.textContent = t('dc-ota-waiting-reboot', '等待设备重启恢复...') + ' (' + pollCount + 's)';
                        }
                    });
            }, 1000);
        },

    });

    // 自动绑定事件
    if (typeof AppState.setupDeviceConfigEvents === 'function') {
        AppState.setupDeviceConfigEvents();
    }
})();
