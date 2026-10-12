/**
 * 设备配置模块
 * 包含设备基本信息、NTP配置、重启/恢复出厂、配置导入导出
 */
(function() {
    const DEFAULT_NTP_SERVER1 = 'http://iot.fastbee.cn/prod-api/iot/tool/ntp';
    const DEFAULT_NTP_SERVER2 = 'cn.pool.ntp.org';
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
    AppState.registerModule('device-config', {

        _deviceConfigActive(epoch) {
            return epoch === (this._deviceConfigEpoch || 0) && (!this.currentPage || this.currentPage === 'device');
        },

        _deviceConfigButtons() {
            const root = document.getElementById('device-page');
            if (!root) return;
            root.querySelectorAll('#device-basic-form button[type=submit], #device-ntp-form button[type=submit], #device-security-form button[type=submit], [data-action="saveCacheDuration"]').forEach(btn => {
                btn.disabled = !this._deviceConfigReady || !!this._deviceConfigSavePending;
            });
        },

        _deviceConfigStatus(message) {
            const el = document.getElementById('device-config-load-status');
            if (el) el.textContent = message;
        },

        _deviceConfigSelect(id, value) {
            const el = document.getElementById(id);
            if (el && el.options && !Array.from(el.options).some(o => o.value === String(value))) {
                el.add(new Option(String(value), String(value)));
            }
            this._setValue(id, String(value));
        },

        _saveDeviceConfigPatch(formId, config, message, onSuccess) {
            if (!this._deviceConfigReady) {
                Notification.warning('配置尚未成功加载，请重新加载后再保存');
                return Promise.resolve(false);
            }
            if (this._deviceConfigSavePending) return Promise.resolve(false);
            const form = document.getElementById(formId);
            if (form && !form.reportValidity()) return Promise.resolve(false);
            const epoch = this._deviceConfigEpoch || 0;
            if (!this._deviceConfigActive(epoch)) return Promise.resolve(false);
            this._deviceConfigSavePending = true;
            this._deviceConfigButtons();
            return apiPut('/api/device/config', config).then(res => {
                if (!res || !res.success) throw new Error(res?.error || '保存失败');
                if (typeof window.apiInvalidateCache === 'function') {
                    window.apiInvalidateCache('/api/device/config');
                    if ('minPasswordLength' in config) window.apiInvalidateCache('/api/users');
                }
                if (!this._deviceConfigActive(epoch)) return false;
                if (onSuccess) onSuccess(res);
                Notification.success(message);
                return true;
            }).catch(err => {
                if (this._deviceConfigActive(epoch) && !isExpectedAbort(err)) Notification.error(err.message || '保存失败');
                return false;
            }).finally(() => {
                this._deviceConfigSavePending = false;
                this._deviceConfigButtons();
            });
        },

        // ============ 事件绑定 ============
        setupDeviceConfigEvents() {
            const root = document.getElementById('device-page');
            if (root && root._deviceConfigBound) return;
            if (root) root._deviceConfigBound = true;
            if (typeof this.onPageHide === 'function') this.onPageHide('device-config', function() {
                this._deviceConfigEpoch = (this._deviceConfigEpoch || 0) + 1;
                this._deviceConfigReady = false;
                clearTimeout(this._deviceHardwareTimer);
                this._deviceConfigButtons();
            });
            this._deviceConfigButtons();
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
            const planExportBtn = document.getElementById('dev-config-plan-export-btn');
            if (planExportBtn) planExportBtn.addEventListener('click', () => this.exportDeviceConfigBundle(true));

            const configImportBtn = document.getElementById('dev-config-import-btn');
            if (configImportBtn) configImportBtn.addEventListener('click', () => this.importDeviceConfigBundle());
            const boardMigrationBtn = document.getElementById('dev-config-board-migration-btn');
            if (boardMigrationBtn) boardMigrationBtn.addEventListener('click', () => this.importDeviceConfigBundle(true));
            const migrationStatusBtn = document.getElementById('dev-config-migration-status-btn');
            if (migrationStatusBtn) migrationStatusBtn.addEventListener('click', () => this.checkConfigMigration());
            const migrationDiscardBtn = document.getElementById('dev-config-migration-discard-btn');
            if (migrationDiscardBtn) migrationDiscardBtn.addEventListener('click', () => this.discardConfigMigration());

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

            // OTA 分片和交互模块只在用户首次打开 OTA 页签时加载。无 PSRAM
            // 设备进入“设备配置”首页时不再额外争用静态响应槽和 DRAM。
            const otaTab = document.querySelector('#device-page .config-tab[data-tab="dev-ota"]');
            if (otaTab && !otaTab._fastbeeOtaLazyBound) {
                otaTab._fastbeeOtaLazyBound = true;
                otaTab.addEventListener('click', () => this._loadOtaFragment());
            }

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
            const epoch = this._deviceConfigEpoch || 0;
            const sequence = this._deviceConfigLoadSequence = (this._deviceConfigLoadSequence || 0) + 1;
            this._deviceConfigReady = false;
            this._deviceConfigButtons();
            this._deviceConfigStatus('正在加载设备配置…');
            clearTimeout(this._deviceHardwareTimer);
            this._deviceHardwareTimer = setTimeout(() => {
                if (this._deviceConfigActive(epoch)) this._loadDeviceHardwareInfo({ noCache: options.noCache === true });
            }, options.deferHardware === false ? 0 : 120);
            var configRequest = getter('/api/device/config');
            return configRequest
                .then(res => {
                    if (!this._deviceConfigActive(epoch) || sequence !== this._deviceConfigLoadSequence) return;
                    if (!res || !res.success || !res.data) throw new Error(res?.error || '读取配置失败');
                    const d = res.data;
                    this._setValue('dev-id', d.deviceId || '');
                    this._setValue('dev-product-number', d.productNumber !== undefined ? String(d.productNumber) : '0');
                    this._setValue('dev-user-id', d.userId || '');
                    this._setValue('dev-name', d.deviceName || '');
                    const desc = document.getElementById('dev-description');
                    if (desc) desc.value = d.description || '';
                    this._setValue('dev-ntp-enable', this._toBoolean(d.enableNTP, true) ? '1' : '0');
                    this._setValue('dev-ntp-server1', d.ntpServer1 || DEFAULT_NTP_SERVER1);
                    this._setValue('dev-ntp-server2', d.ntpServer2 || DEFAULT_NTP_SERVER2);
                    this._deviceConfigSelect('dev-timezone', d.timezone || 'CST-8');
                    this._setValue('dev-sync-interval', d.syncInterval !== undefined ? String(d.syncInterval) : '3600');
                    this._deviceConfigSelect('dev-cache-duration', d.cacheDuration !== undefined ? d.cacheDuration : 86400);
                    this._setValue('dev-log-level', d.logLevel || 'INFO');
                    this.setDeveloperModeState(d.developerModeEnabled !== false);
                    this._renderDeveloperModeState();
                    // 安全策略字段
                    this._setValue('dev-sec-max-attempts', d.maxLoginAttempts !== undefined ? String(d.maxLoginAttempts) : '5');
                    this._setValue('dev-sec-lockout-time', d.loginLockoutTime !== undefined ? String(Math.round(d.loginLockoutTime / 1000)) : '300');
                    this._setValue('dev-sec-min-pwd-len', d.minPasswordLength !== undefined ? String(d.minPasswordLength) : '6');
                    this._setValue('dev-sec-require-strong', d.requireStrongPasswords ? '1' : '0');
                    this._deviceConfigReady = true;
                    this._deviceConfigStatus('配置已加载');
                })
                .catch(err => {
                    if (this._deviceConfigActive(epoch) && sequence === this._deviceConfigLoadSequence && !isExpectedAbort(err)) {
                        this._deviceConfigStatus('加载失败：' + (err.message || err) + '，请点击重新加载');
                    }
                }).finally(() => this._deviceConfigButtons());
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
            const epoch = this._deviceConfigEpoch || 0;
            var infoRequest = getter('/api/system/info');
            infoRequest
                .then(res => {
                    if (!this._deviceConfigActive(epoch)) return;
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
            const productNumber = Number(document.getElementById('dev-product-number')?.value || 0);
            if (!Number.isInteger(productNumber) || productNumber < 0 || productNumber > 2147483647) {
                Notification.warning('产品编号应为 0 至 2147483647 的整数');
                return;
            }
            return this._saveDeviceConfigPatch('device-basic-form', {
                deviceId: deviceIdInput?.value?.trim() || '',
                deviceName: document.getElementById('dev-name')?.value || '',
                productNumber,
                userId: document.getElementById('dev-user-id')?.value || '',
                description: document.getElementById('dev-description')?.value || '',
                logLevel: document.getElementById('dev-log-level')?.value || 'INFO'
            }, '设备信息保存成功', res => {
                if (res.data?.deviceId && deviceIdInput) deviceIdInput.value = res.data.deviceId;
            });
        },

        saveCacheDuration() {
            const duration = Number(document.getElementById('dev-cache-duration')?.value);
            if (!Number.isInteger(duration) || duration < 0 || duration > 31536000) {
                Notification.warning('缓存时间超出有效范围');
                return;
            }
            return this._saveDeviceConfigPatch('', { cacheDuration: duration }, '缓存时间设置已保存');
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
            const syncInterval = Number(document.getElementById('dev-sync-interval')?.value);
            if (!Number.isInteger(syncInterval) || syncInterval < 60 || syncInterval > 86400) {
                Notification.warning('同步间隔应为 60 至 86400 秒的整数');
                return;
            }
            const config = {
                ntpServer1: document.getElementById('dev-ntp-server1')?.value || DEFAULT_NTP_SERVER1,
                ntpServer2: document.getElementById('dev-ntp-server2')?.value || DEFAULT_NTP_SERVER2,
                timezone: document.getElementById('dev-timezone')?.value || 'CST-8',
                enableNTP: document.getElementById('dev-ntp-enable')?.value === '1',
                syncInterval
            };
            return this._saveDeviceConfigPatch('device-ntp-form', config, 'NTP配置保存成功', () => {
                if (config.enableNTP) this.syncDeviceTime({ silent: true });
                else this.loadDeviceTime({ noCache: true });
            });
        },

        saveSecurityPolicy() {
            const attempts = Number(document.getElementById('dev-sec-max-attempts')?.value);
            const seconds = Number(document.getElementById('dev-sec-lockout-time')?.value);
            const length = Number(document.getElementById('dev-sec-min-pwd-len')?.value);
            if (!Number.isInteger(attempts) || attempts < 1 || attempts > 10 ||
                !Number.isInteger(seconds) || seconds < 60 || seconds > 3600 ||
                !Number.isInteger(length) || length < 4 || length > 32) {
                Notification.warning('安全策略参数超出有效范围');
                return;
            }
            return this._saveDeviceConfigPatch('device-security-form', {
                maxLoginAttempts: attempts, loginLockoutTime: seconds * 1000, minPasswordLength: length,
                requireStrongPasswords: document.getElementById('dev-sec-require-strong')?.value === '1'
            }, '安全策略保存成功');
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
                    const synced = !!(res && res.success && data.synced && data.syncRequestSucceeded === true);
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

            setEl('dev-time-datetime', d.timeDisplayAvailable === false ? '' : d.datetime);
            const clock = document.getElementById('dev-time-datetime');
            if (clock) clock.title = d.activeTimezoneAvailable === true && typeof d.activeTimezone === 'string'
                ? '当前时区：' + d.activeTimezone : '当前时区未确认';

            // Connectivity is independent from the age/provenance of a clock.
            // Legacy firmware without quality fields cannot prove a sync event.
            const timeLabels = {
                rtc_unverified: t('dc-time-quality-rtc', 'RTC启动时间（未校准）'),
                synced_tls: t('dc-time-quality-tls', '已同步（TLS来源，非精度保证）'),
                synced_unverified: t('dc-time-quality-unverified', '已同步（来源未认证）'),
                stale: t('dc-time-quality-stale', '时间已失鲜，待重新同步'),
                clock_changed: t('dc-time-quality-changed', '时钟发生未跟踪变化'),
                invalid: t('dc-time-quality-invalid', '系统时间无效'),
                busy: t('dc-time-quality-busy', '时间状态暂不可用'),
                unknown: t('dc-time-quality-unknown', '时间来源未知')
            };
            let quality = typeof d.timeQuality === 'string' && Object.prototype.hasOwnProperty.call(timeLabels, d.timeQuality)
                ? d.timeQuality : 'unknown';
            const sourceLabels = { sntp: 'SNTP', http: 'HTTP', https: 'HTTPS', mqtt: 'MQTT', rtc: 'RTC' };
            const knownSource = typeof d.timeSource === 'string' && Object.prototype.hasOwnProperty.call(sourceLabels, d.timeSource);
            const freshSource = d.timeValid === true && knownSource &&
                Number.isSafeInteger(d.timeSyncAgeMs) && d.timeSyncAgeMs >= 0 && d.timeSyncAgeMs <= 86400000 &&
                Number.isInteger(d.timeSyncSequence) && d.timeSyncSequence > 0 && d.timeSyncSequence < 4294967295;
            const freshRecord = d.synced === true && d.timeSource !== 'rtc' && freshSource;
            const rtcRecord = d.synced === false && d.timeSource === 'rtc' &&
                d.timeSourceAuthenticated === false && freshSource;
            if ((quality === 'synced_tls' && (!freshRecord || d.timeSource !== 'https' || d.timeSourceAuthenticated !== true)) ||
                (quality === 'synced_unverified' && (!freshRecord || d.timeSourceAuthenticated !== false)) ||
                (quality === 'rtc_unverified' && !rtcRecord)) quality = 'unknown';
            const rtcBootLabels = {
                not_attempted: '尚未尝试', pending: '处理中', disabled: '未启用', not_compiled: '未编译',
                busy: '资源忙', skipped_valid: '保留已有时间', multiple_sources: '启动来源不唯一',
                unavailable: 'RTC不可用', invalid_clock: 'RTC日历无效', stopped: 'RTC已停走',
                invalid_offset: '缺少有效偏移', invalid_epoch: '换算时间超界',
                rejected: '写入失败或时钟状态变化', applied: '已采用，未经校准'
            };
            const bootStatus = Object.prototype.hasOwnProperty.call(rtcBootLabels, d.rtcBootStatus)
                ? d.rtcBootStatus : 'not_attempted';
            const bootFailure = ['busy', 'multiple_sources', 'unavailable', 'invalid_clock', 'stopped',
                'invalid_offset', 'invalid_epoch', 'rejected'].includes(bootStatus);
            const label = timeLabels[quality] + (bootFailure ? '；RTC启动：' + rtcBootLabels[bootStatus] : '');
            const badge = quality === 'synced_tls' ? 'badge-success' : 'badge-warning';
            setHtml('dev-time-synced', '<span class="badge ' + badge + '">' + label + '</span>');
            const timeStatus = document.getElementById('dev-time-synced');
            if (timeStatus) timeStatus.title = quality === 'synced_tls' || quality === 'synced_unverified'
                ? '来源：' + sourceLabels[d.timeSource] + '；距最近同步：' + Math.floor(d.timeSyncAgeMs / 1000) + '秒；平台与RTC精度未验证'
                : quality === 'rtc_unverified'
                    ? '来源：RTC；距启动采用：' + Math.floor(d.timeSyncAgeMs / 1000) + '秒；不是RTC校准年龄或精度证明'
                    : '没有可用于确认当前已同步的完整来源记录';
            if (timeStatus) timeStatus.title += '；RTC启动：' + rtcBootLabels[bootStatus];

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
            if (this._deviceCommandPending) return;
            const delay = document.getElementById('dev-restart-delay')?.value || '3';
            const btn = document.getElementById('dev-restart-btn');
            if (!confirm(t('dc-reboot-confirm', '确定要重启设备？将在') + ' ' + delay + ' ' + t('dc-reboot-confirm-suffix', '秒后重启。'))) return;
            if (btn) { btn.disabled = true; btn.innerHTML = t('dc-reboot-sending', '… 处理中...'); }

            this._deviceCommandPending = true;
            return apiRestart({ delay })
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
                        Notification.warning('连接已断开，无法确认设备是否收到指令；请核对设备状态后再操作');
                    } else {
                        if (btn) { btn.disabled = false; btn.innerHTML = t('dc-reboot-now', '立即重启'); }
                        Notification.error(t('dc-reboot-fail', '重启指令发送失败'), t('dc-reboot-title', '设备重启'));
                    }
                }).finally(() => {
                    this._deviceCommandPending = false;
                    if (btn) { btn.disabled = false; btn.textContent = '立即重启'; }
                });
        },

        factoryReset() {
            if (this._deviceCommandPending) return;
            const confirmInput = document.getElementById('dev-factory-confirm');
            const passwordInput = document.getElementById('dev-factory-password');
            const btn = document.getElementById('dev-factory-btn');
            const confirmValue = confirmInput?.value?.toUpperCase().trim();
            const password = passwordInput?.value || '';

            if (confirmValue !== 'RESET') {
                Notification.warning(t('dc-reset-confirm', '请输入 RESET 以确认操作'), t('dc-reset-btn', '恢复出厂设置'));
                if (confirmInput) confirmInput.focus();
                return;
            }

            if (!password) {
                Notification.warning(t('dc-reset-password-required', '请输入当前账户密码以重新认证'), t('dc-reset-btn', '恢复出厂设置'));
                if (passwordInput) passwordInput.focus();
                return;
            }

            if (!confirm(t('dc-reset-warning', '警告：此操作将清除所有用户配置和数据，恢复到出厂默认状态！'))) {
                if (confirmInput) confirmInput.value = '';
                return;
            }

            if (btn) { btn.disabled = true; btn.innerHTML = t('dc-reboot-sending', '… 处理中...'); }

            this._deviceCommandPending = true;
            return apiFactoryReset({ confirm: 'RESET', password: password })
                .then(res => {
                    if (passwordInput) passwordInput.value = '';
                    if (res && res.success) {
                        Notification.success(t('dc-reset-success', '恢复出厂设置成功，设备正在重启...'), t('dc-reset-btn', '恢复出厂设置'));
                    } else {
                        if (btn) { btn.disabled = false; btn.innerHTML = t('dc-reset-btn', '恢复出厂设置'); }
                        Notification.error(t('dc-reset-fail', '恢复出厂设置失败'), t('dc-reset-btn', '恢复出厂设置'));
                    }
                })
                .catch(err => {
                    if (passwordInput) passwordInput.value = '';
                    // 区分超时 vs 连接断开（设备恢复出厂导致连接断开属预期）
                    const isTimeout = err && err.name === 'AbortError';
                    const isConnectionClosed = err && (
                        err.name === 'TypeError' ||
                        (err.message && (err.message.includes('fetch') || err.message.includes('network')))
                    );
                    if (isTimeout) {
                        Notification.warning(t('dc-reset-timeout', '恢复出厂请求超时，设备可能正在重启也可能未收到指令，请等待 30 秒后刷新页面'), t('dc-reset-timeout-title', '恢复出厂超时'));
                    } else if (isConnectionClosed) {
                        Notification.warning('连接已断开，无法确认设备是否收到指令；请核对设备状态后再操作');
                    } else {
                        if (btn) { btn.disabled = false; btn.innerHTML = t('dc-reset-btn', '恢复出厂设置'); }
                        Notification.error(t('dc-reset-fail', '恢复出厂设置失败'), t('dc-reset-btn', '恢复出厂设置'));
                    }
                }).finally(() => {
                    this._deviceCommandPending = false;
                    if (btn) { btn.disabled = false; btn.textContent = '恢复出厂设置'; }
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
            if (this._deviceCommandPending) return;
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
            this._deviceCommandPending = true;
            return apiPost('/api/device/developer-mode', {
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
                this._deviceCommandPending = false;
                if (passwordInput) passwordInput.value = '';
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

        async _runConfigTransferAction(action, args) {
            if (this._configTransferLoading) return;
            const epoch = this._deviceConfigEpoch || 0;
            this._configTransferLoading = true;
            this._setConfigTransferStatus('正在加载配置迁移工具...');
            try {
                await new Promise((resolve, reject) => {
                    const timeout = setTimeout(() => reject(new Error('配置迁移工具加载超时，请重试')), 30000);
                    try {
                        this._loadModule('device-config-transfer', () => {
                            clearTimeout(timeout);
                            if (this._configTransferReady === true) resolve();
                            else reject(new Error('配置迁移工具不完整，请刷新页面'));
                        });
                    } catch (error) { clearTimeout(timeout); reject(error); }
                });
                if (this._deviceConfigActive(epoch)) return this[action](...args);
            } catch (error) {
                if (this._deviceConfigActive(epoch)) this._setConfigTransferStatus(error.message || '配置迁移工具加载失败');
            } finally { this._configTransferLoading = false; }
        },

        exportDeviceConfigBundle(reusable = false) {
            return this._runConfigTransferAction('exportDeviceConfigBundle', [reusable]);
        },

        importDeviceConfigBundle(boardMigration = false) {
            return this._runConfigTransferAction('importDeviceConfigBundle', [boardMigration]);
        },

        checkConfigMigration() {
            return this._runConfigTransferAction('checkConfigMigration', []);
        },

        discardConfigMigration() {
            return this._runConfigTransferAction('discardConfigMigration', []);
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
            if (this._otaFragmentLoading || document.getElementById('ota-url-form')) return;
            this._otaFragmentLoading = true;

            const self = this;
            Promise.resolve(PageLoader.loadFragment('device-ota-container', 'device-ota', function() {
                // 分片加载成功：隐藏内联兜底面板，按需加载交互模块
                const inline = document.getElementById('dev-ota-inline-panel');
                if (inline) inline.classList.add('is-hidden');
                self._loadModule('device-ota', function() {
                    if (typeof AppState.setupDeviceOta === 'function') {
                        AppState.setupDeviceOta();
                    }
                });
            })).catch(function() {
                // PageLoader retains the visible retry affordance.
            }).finally(function() {
                self._otaFragmentLoading = false;
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
