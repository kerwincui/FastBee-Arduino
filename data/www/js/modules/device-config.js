/**
 * 设备配置模块
 * 包含设备基本信息、NTP配置、重启/恢复出厂、蓝牙配网、OTA升级
 */
(function() {
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

            // 蓝牙配网事件绑定
            const bleProvisionForm = document.getElementById('device-ble-provision-form');
            if (bleProvisionForm) bleProvisionForm.addEventListener('submit', (e) => { e.preventDefault(); this.saveBLEProvisionConfig(); });

            const bleProvisionRefreshBtn = document.getElementById('ble-provision-refresh-btn');
            if (bleProvisionRefreshBtn) bleProvisionRefreshBtn.addEventListener('click', () => this.loadBLEProvisionStatus());

            const bleProvisionStartBtn = document.getElementById('ble-provision-start-btn');
            if (bleProvisionStartBtn) bleProvisionStartBtn.addEventListener('click', () => this.startBLEProvision());

            const bleProvisionStopBtn = document.getElementById('ble-provision-stop-btn');
            if (bleProvisionStopBtn) bleProvisionStopBtn.addEventListener('click', () => this.stopBLEProvision());

            // OTA升级事件绑定
            const otaUrlForm = document.getElementById('ota-url-form');
            if (otaUrlForm) otaUrlForm.addEventListener('submit', (e) => { e.preventDefault(); this.startOtaUrl(); });

            const otaUploadForm = document.getElementById('ota-upload-form');
            if (otaUploadForm) otaUploadForm.addEventListener('submit', (e) => { e.preventDefault(); this.startOtaUpload(); });

            const otaRefreshBtn = document.getElementById('ota-refresh-btn');
            if (otaRefreshBtn) otaRefreshBtn.addEventListener('click', () => this.loadOtaStatus());

            // OTA文件选择更新文件名显示
            const otaFileInput = document.getElementById('ota-file');
            if (otaFileInput) {
                otaFileInput.addEventListener('change', (e) => {
                    const fileNameEl = document.getElementById('ota-file-name');
                    if (fileNameEl) {
                        const file = e.target.files?.[0];
                        fileNameEl.textContent = file ? file.name : i18n.t('no-file-selected');
                    }
                });
            }
        },

        // ============ 设备配置 ============

        loadDeviceConfig() {
            apiGet('/api/device/config')
                .then(res => {
                    if (!res || !res.success) return;
                    const d = res.data || {};
                    this._setValue('dev-id', d.deviceId || '');
                    this._setValue('dev-product-number', d.productNumber !== undefined ? String(d.productNumber) : '0');
                    this._setValue('dev-user-id', d.userId || '');
                    this._setValue('dev-name', d.deviceName || '');
                    const desc = document.getElementById('dev-description');
                    if (desc) desc.value = d.description || '';
                    this._setValue('dev-ntp-enable', d.enableNTP ? '1' : '0');
                    this._setValue('dev-ntp-server1', d.ntpServer1 || 'https://iot.fastbee.cn/prod-api/iot/tool/ntp');
                    this._setValue('dev-ntp-server2', d.ntpServer2 || 'time.nist.gov');
                    this._setValue('dev-timezone', d.timezone || 'CST-8');
                    this._setValue('dev-sync-interval', d.syncInterval !== undefined ? String(d.syncInterval) : '3600');
                    this._setValue('dev-cache-duration', d.cacheDuration !== undefined ? String(d.cacheDuration) : '86400');
                })
                .catch(err => console.error('Load device config failed:', err));
            // 同时加载硬件信息
            this._loadDeviceHardwareInfo();
        },

        _loadDeviceHardwareInfo() {
            apiGet('/api/system/info')
                .then(res => {
                    if (!res || !res.success) return;
                    const d = res.data || {};
                    const dev = d.device || {};
                    const fw = d.firmware || {};
                    const set = (id, val) => { const el = document.getElementById(id); if (el) el.textContent = val || '--'; };
                    set('dev-sys-chip', dev.chipModel);
                    set('dev-sys-cpu', dev.cpuFreqMHz ? dev.cpuFreqMHz + ' MHz' : '--');
                    set('dev-sys-heap', dev.freeHeap ? Math.round(dev.freeHeap / 1024) + ' KB' : '--');
                    set('dev-sys-flash', dev.flashSize ? Math.round(dev.flashSize / 1024 / 1024) + ' MB' : '--');
                    set('dev-sys-sdk', dev.sdkVersion);
                    set('dev-sys-fw', fw.version || dev.firmwareVersion || '--');
                })
                .catch(() => {});
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
                ntpServer1: document.getElementById('dev-ntp-server1')?.value || 'https://iot.fastbee.cn/prod-api/iot/tool/ntp',
                ntpServer2: document.getElementById('dev-ntp-server2')?.value || 'time.nist.gov',
                timezone: document.getElementById('dev-timezone')?.value || 'CST-8',
                enableNTP: document.getElementById('dev-ntp-enable')?.value || '1',
                syncInterval: document.getElementById('dev-sync-interval')?.value || '3600',
                cacheDuration: parseInt(document.getElementById('dev-cache-duration')?.value || '86400'),
            };
            apiPut('/api/device/config', config)
                .then(res => {
                    if (res && res.success) {
                        this._showMessage('dev-basic-success', true);
                        Notification.success(i18n.t('dev-save-basic-ok'), i18n.t('dev-config-title'));
                    } else {
                        Notification.error(res?.error || i18n.t('dev-save-fail'), i18n.t('dev-config-title'));
                    }
                })
                .catch(() => Notification.error(i18n.t('dev-save-fail'), i18n.t('dev-config-title')));
        },

        saveCacheDuration() {
            const duration = parseInt(document.getElementById('dev-cache-duration')?.value || '86400');
            apiPut('/api/device/config', { cacheDuration: duration })
                .then(res => {
                    if (res && res.success) {
                        Notification.success(i18n.t('dev-cache-save-ok'), i18n.t('dev-cache-title'));
                    } else {
                        Notification.error(res?.error || i18n.t('dev-save-fail'), i18n.t('dev-cache-title'));
                    }
                })
                .catch(() => Notification.error(i18n.t('dev-save-fail'), i18n.t('dev-cache-title')));
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
                var modules = ['dashboard', 'device-config', 'device-control', 'network',
                    'peripherals', 'periph-exec', 'protocol', 'i18n', 'i18n-en', 'admin-bundle'];
                modules.forEach(function(m) { urlMap[origin + '/js/modules/' + m + '.js'] = true; });
                var urls = Object.keys(urlMap);

                // 3. 使用 fetch cache:'reload' 强制浏览器重新从服务器获取并更新 HTTP 缓存
                //    分批执行（每次2个并发），避免 ESP32 连接过多
                var idx = 0;
                Notification.success(
                    (typeof i18n !== 'undefined' ? i18n.t('dev-cache-clear-ok') : '正在清除缓存并刷新资源...'),
                    (typeof i18n !== 'undefined' ? i18n.t('dev-cache-title') : '缓存管理')
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
                console.error('清除缓存失败:', e);
                window.location.reload(true);
            }
        },

        saveDeviceNTP() {
            const config = {
                ntpServer1: document.getElementById('dev-ntp-server1')?.value || 'https://iot.fastbee.cn/prod-api/iot/tool/ntp',
                ntpServer2: document.getElementById('dev-ntp-server2')?.value || 'time.nist.gov',
                timezone: document.getElementById('dev-timezone')?.value || 'CST-8',
                enableNTP: document.getElementById('dev-ntp-enable')?.value || '1',
                syncInterval: document.getElementById('dev-sync-interval')?.value || '3600',
            };
            apiPut('/api/device/config', config)
                .then(res => {
                    if (res && res.success) {
                        this._showMessage('dev-ntp-success', true);
                        Notification.success(i18n.t('dev-save-ntp-ok'), i18n.t('dev-config-title'));
                        this.loadDeviceTime();
                    } else {
                        Notification.error(res?.error || i18n.t('dev-save-fail'), i18n.t('dev-ntp-config-title'));
                    }
                })
                .catch(() => Notification.error(i18n.t('dev-save-fail'), i18n.t('dev-ntp-config-title')));
        },

        loadDeviceTime() {
            const btn = document.getElementById('dev-time-refresh-btn');
            if (btn) { btn.disabled = true; btn.innerHTML = i18n.t('dev-refreshing-html'); }

            Promise.all([
                apiGet('/api/device/time'),
                apiGet('/api/network/status')
            ])
                .then(([timeRes, netRes]) => {
                    const timeData = (timeRes && timeRes.success) ? timeRes.data || {} : {};
                    const netData = (netRes && netRes.success) ? netRes.data || {} : {};
                    const internetAvailable = netData.internetAvailable === true;
                    this._renderDeviceTime(timeData, internetAvailable);
                })
                .catch(err => console.error('Load device time failed:', err));
        },

        syncDeviceTime() {
            const btn = document.getElementById('dev-time-refresh-btn');
            if (btn) { btn.disabled = true; btn.innerHTML = i18n.t('dev-refreshing-html'); }
            apiPost('/api/device/time/sync', {})
                .then(res => {
                    if (!res || !res.success) {
                        Notification.warning(i18n.t('dev-time-sync-fail') || 'NTP同步失败', 'NTP');
                        this.loadDeviceTime();
                        return;
                    }
                    this._renderDeviceTime(res.data || {}, true);
                    if (res.data && res.data.synced) {
                        Notification.success(i18n.t('dev-time-sync-ok') || 'NTP同步成功', 'NTP');
                    }
                })
                .catch(err => {
                    console.error('Sync device time failed:', err);
                    Notification.error(i18n.t('dev-time-sync-fail') || 'NTP同步失败', 'NTP');
                    this.loadDeviceTime();
                });
        },

        _renderDeviceTime(d, internetAvailable = true) {
            const setEl = (id, val) => { const el = document.getElementById(id); if (el) el.textContent = val || '--'; };
            const setHtml = (id, html) => { const el = document.getElementById(id); if (el) el.innerHTML = html; };

            setEl('dev-time-datetime', d.datetime);

            if (!internetAvailable) {
                setHtml('dev-time-synced', i18n.t('dev-time-no-network-html'));
            } else if (d.synced) {
                setHtml('dev-time-synced', i18n.t('dev-time-synced-html'));
            } else {
                setHtml('dev-time-synced', i18n.t('dev-time-not-synced-html'));
            }

            if (d.uptime !== undefined) {
                const ms = d.uptime;
                const h = Math.floor(ms / 3600000);
                const m = Math.floor((ms % 3600000) / 60000);
                const s = Math.floor((ms % 60000) / 1000);
                setEl('dev-time-uptime', `${h}${i18n.t('dev-time-uptime-unit')}${m}${i18n.t('dev-time-uptime-min')}${s}${i18n.t('dev-time-uptime-sec')}`);
            }

            const btn = document.getElementById('dev-time-refresh-btn');
            if (btn) {
                if (internetAvailable) {
                    btn.disabled = false;
                    btn.innerHTML = i18n.t('dev-refresh-html');
                    btn.title = '';
                } else {
                    btn.disabled = true;
                    btn.innerHTML = i18n.t('dev-refresh-disabled-html');
                    btn.title = i18n.t('dev-time-no-network-tip');
                }
            }
        },

        restartDevice() {
            const delay = document.getElementById('dev-restart-delay')?.value || '3';
            const btn = document.getElementById('dev-restart-btn');
            if (!confirm(`${i18n.t('dev-restart-confirm-prefix')}${delay}${i18n.t('dev-restart-confirm-suffix')}`)) return;
            if (btn) { btn.disabled = true; btn.innerHTML = i18n.t('dev-restarting-html'); }

            apiRestart({ delay })
                .then(res => {
                    if (res && res.success) {
                        Notification.success(`${i18n.t('dev-restart-msg-prefix')}${delay}${i18n.t('dev-restart-msg-suffix')}`, i18n.t('dev-restart-title'));
                    } else {
                        if (btn) { btn.disabled = false; btn.innerHTML = i18n.t('dev-restart-btn-html'); }
                        Notification.error(i18n.t('dev-restart-fail'), i18n.t('dev-restart-title'));
                    }
                })
                .catch(err => {
                    const isConnectionClosed = err && (
                        err.name === 'AbortError' ||
                        err.name === 'TypeError' ||
                        (err.message && (err.message.includes('fetch') || err.message.includes('network')))
                    );
                    if (isConnectionClosed) {
                        Notification.success(`${i18n.t('dev-restart-msg-prefix')}${delay}${i18n.t('dev-restart-msg-suffix')}`, i18n.t('dev-restart-title'));
                    } else {
                        if (btn) { btn.disabled = false; btn.innerHTML = i18n.t('dev-restart-btn-html'); }
                        Notification.error(i18n.t('dev-restart-fail'), i18n.t('dev-restart-title'));
                    }
                });
        },

        factoryReset() {
            const confirmInput = document.getElementById('dev-factory-confirm');
            const btn = document.getElementById('dev-factory-btn');
            const confirmValue = confirmInput?.value?.toUpperCase().trim();

            if (confirmValue !== 'RESET') {
                Notification.warning(i18n.t('dev-sys-factory-confirm-error'), i18n.t('dev-sys-factory-title-msg'));
                if (confirmInput) confirmInput.focus();
                return;
            }

            if (!confirm(i18n.t('dev-sys-factory-warning'))) {
                if (confirmInput) confirmInput.value = '';
                return;
            }

            if (btn) { btn.disabled = true; btn.innerHTML = i18n.t('dev-sys-factory-processing'); }

            apiFactoryReset()
                .then(res => {
                    if (res && res.success) {
                        Notification.success(i18n.t('dev-sys-factory-success'), i18n.t('dev-sys-factory-title-msg'));
                    } else {
                        if (btn) { btn.disabled = false; btn.innerHTML = i18n.t('dev-sys-factory-btn-html'); }
                        Notification.error(i18n.t('dev-sys-factory-fail'), i18n.t('dev-sys-factory-title-msg'));
                    }
                })
                .catch(err => {
                    const isConnectionClosed = err && (
                        err.name === 'AbortError' ||
                        err.name === 'TypeError' ||
                        (err.message && (err.message.includes('fetch') || err.message.includes('network')))
                    );
                    if (isConnectionClosed) {
                        Notification.success(i18n.t('dev-sys-factory-success'), i18n.t('dev-sys-factory-title-msg'));
                    } else {
                        if (btn) { btn.disabled = false; btn.innerHTML = i18n.t('dev-sys-factory-btn-html'); }
                        Notification.error(i18n.t('dev-sys-factory-fail'), i18n.t('dev-sys-factory-title-msg'));
                    }
                });
        },

        // ============ 蓝牙配网 ============

        loadBLEProvisionStatus() {
            apiGet('/api/ble/provision/status')
                .then(res => {
                    if (!res || !res.success) return;
                    const d = res.data || {};

                    const badge = document.getElementById('ble-provision-status-badge');
                    const deviceName = document.getElementById('ble-provision-device-name');
                    const remainingWrap = document.getElementById('ble-provision-remaining-wrap');
                    const remaining = document.getElementById('ble-provision-remaining');
                    const startBtn = document.getElementById('ble-provision-start-btn');
                    const stopBtn = document.getElementById('ble-provision-stop-btn');

                    if (d.active) {
                        if (badge) { badge.className = 'status-badge status-online'; badge.textContent = i18n.t('ble-active'); }
                        if (deviceName) deviceName.textContent = d.deviceName || '--';
                        if (remainingWrap) remainingWrap.classList.remove('is-hidden');
                        if (remaining) remaining.textContent = (d.remainingTime || 0) + i18n.t('ble-remaining-unit');
                        if (startBtn) startBtn.classList.add('is-hidden');
                        if (stopBtn) { stopBtn.classList.remove('is-hidden'); stopBtn.disabled = false; }
                    } else {
                        if (badge) { badge.className = 'status-badge status-offline'; badge.textContent = i18n.t('ble-inactive'); }
                        if (deviceName) deviceName.textContent = '--';
                        if (remainingWrap) remainingWrap.classList.add('is-hidden');
                        if (startBtn) { startBtn.classList.remove('is-hidden'); startBtn.disabled = false; }
                        if (stopBtn) stopBtn.classList.add('is-hidden');
                    }
                })
                .catch(err => {
                    console.error('加载蓝牙配网状态失败:', err);
                });
        },

        loadBLEProvisionConfig() {
            apiGet('/api/ble/provision/config')
                .then(res => {
                    if (!res || !res.success) return;
                    const d = res.data || {};

                    this._setValue('ble-device-name', d.bleName || 'FBDevice');
                    this._setValue('ble-timeout', d.bleTimeout || 300);
                    this._setChecked('ble-auto-start', d.bleAutoStart || false);
                    this._setValue('ble-service-uuid', d.bleServiceUUID || '6E400001-B5A3-F393-E0A9-E50E24DCCA9F');
                    this._setValue('ble-rx-uuid', d.bleRxUUID || '6E400002-B5A3-F393-E0A9-E50E24DCCA9F');
                    this._setValue('ble-tx-uuid', d.bleTxUUID || '6E400003-B5A3-F393-E0A9-E50E24DCCA9F');
                })
                .catch(err => {
                    console.error('加载蓝牙配网配置失败:', err);
                });
        },

        saveBLEProvisionConfig() {
            const data = {
                bleName: document.getElementById('ble-device-name')?.value || 'FBDevice',
                bleTimeout: document.getElementById('ble-timeout')?.value || '300',
                bleAutoStart: document.getElementById('ble-auto-start')?.checked ? 'true' : 'false',
                bleServiceUUID: document.getElementById('ble-service-uuid')?.value || '6E400001-B5A3-F393-E0A9-E50E24DCCA9F',
                bleRxUUID: document.getElementById('ble-rx-uuid')?.value || '6E400002-B5A3-F393-E0A9-E50E24DCCA9F',
                bleTxUUID: document.getElementById('ble-tx-uuid')?.value || '6E400003-B5A3-F393-E0A9-E50E24DCCA9F'
            };

            apiPut('/api/ble/provision/config', data)
                .then(res => {
                    if (res && res.success) {
                        this._showMessage('ble-provision-success', true);
                        Notification.success(i18n.t('ble-save-ok'), i18n.t('ble-title'));
                    } else {
                        Notification.error(res?.message || i18n.t('dev-save-fail'), i18n.t('ble-title'));
                    }
                })
                .catch(err => {
                    Notification.error(i18n.t('ble-start-fail') + ': ' + (err.message || err), i18n.t('ble-title'));
                });
        },

        startBLEProvision() {
            const startBtn = document.getElementById('ble-provision-start-btn');
            if (startBtn) {
                startBtn.disabled = true;
                startBtn.innerHTML = i18n.t('ble-starting-html');
            }

            apiPost('/api/ble/provision/start', {})
                .then(res => {
                    if (res && res.success) {
                        Notification.success(i18n.t('ble-start-ok-prefix') + (res.data?.deviceName || ''), i18n.t('ble-title'));
                        this.loadBLEProvisionStatus();
                    } else {
                        Notification.error(res?.message || i18n.t('ble-start-fail'), i18n.t('ble-title'));
                    }
                })
                .catch(err => {
                    Notification.error(i18n.t('ble-start-fail') + ': ' + (err.message || err), i18n.t('ble-title'));
                })
                .finally(() => {
                    if (startBtn) startBtn.innerHTML = i18n.t('ble-start-html');
                });
        },

        stopBLEProvision() {
            const stopBtn = document.getElementById('ble-provision-stop-btn');
            if (stopBtn) {
                stopBtn.disabled = true;
                stopBtn.innerHTML = i18n.t('ble-stopping-html');
            }

            apiPost('/api/ble/provision/stop', {})
                .then(res => {
                    if (res && res.success) {
                        Notification.success(i18n.t('ble-stop-ok'), i18n.t('ble-title'));
                        this.loadBLEProvisionStatus();
                    } else {
                        Notification.error(res?.message || i18n.t('ble-stop-fail'), i18n.t('ble-title'));
                    }
                })
                .catch(err => {
                    Notification.error(i18n.t('ble-stop-fail') + ': ' + (err.message || err), i18n.t('ble-title'));
                })
                .finally(() => {
                    if (stopBtn) stopBtn.innerHTML = i18n.t('ble-stop-html');
                });
        },

        // ============ OTA升级 ============

        loadOtaStatus() {
            Promise.all([
                apiGet('/api/ota/status'),
                apiGet('/api/network/status'),
                apiGet('/api/system/info')
            ])
                .then(([otaRes, netRes, sysRes]) => {
                    const internetAvailable = (netRes && netRes.success && netRes.data) ? netRes.data.internetAvailable === true : false;

                    if (otaRes) {
                        const badge = document.getElementById('ota-status-badge');
                        const progressWrap = document.getElementById('ota-progress-wrap');
                        const progressBar = document.getElementById('ota-progress-bar');
                        const progressText = document.getElementById('ota-progress-text');

                        if (otaRes.status === 'OTA ready') {
                            if (badge) { badge.className = 'status-badge status-online'; badge.textContent = i18n.t('ota-ready'); }
                            if (progressWrap) progressWrap.classList.add('is-hidden');
                        } else if (otaRes.progress > 0 && otaRes.progress < 100) {
                            if (badge) { badge.className = 'status-badge status-warning'; badge.textContent = i18n.t('ota-in-progress'); }
                            if (progressWrap) progressWrap.classList.remove('is-hidden');
                            if (progressBar) progressBar.style.width = otaRes.progress + '%';
                            if (progressText) progressText.textContent = otaRes.progress + '%';
                        }
                    }

                    const urlBtn = document.getElementById('ota-url-btn');
                    const urlInput = document.getElementById('ota-url');
                    const urlHint = document.getElementById('ota-url-hint');
                    if (urlBtn) {
                        if (internetAvailable) {
                            urlBtn.disabled = false;
                            urlBtn.title = '';
                            if (urlInput) urlInput.disabled = false;
                            if (urlHint) urlHint.classList.add('is-hidden');
                        } else {
                            urlBtn.disabled = true;
                            urlBtn.title = i18n.t('ota-no-network-tip');
                            if (urlInput) urlInput.disabled = true;
                            if (urlHint) {
                                urlHint.classList.remove('is-hidden');
                                urlHint.innerHTML = `<span class="badge badge-danger">${i18n.t('ota-no-network-msg')}</span>`;
                            }
                        }
                    }

                    if (sysRes && sysRes.success) {
                        const d = sysRes.data || {};
                        this._setValue('ota-current-version', d.firmwareVersion || '--');
                        const flashSize = d.flashChipSize || 0;
                        const freeSketch = d.freeSketchSpace || 0;
                        const flashSizeEl = document.getElementById('ota-flash-size');
                        const freeSpaceEl = document.getElementById('ota-free-space');
                        if (flashSizeEl) flashSizeEl.textContent = flashSize > 0 ? (flashSize / 1024 / 1024).toFixed(2) + ' MB' : '--';
                        if (freeSpaceEl) freeSpaceEl.textContent = freeSketch > 0 ? (freeSketch / 1024).toFixed(0) + ' KB' : '--';
                    }
                })
                .catch(err => {
                    console.error('加载OTA状态失败:', err);
                });
        },

        startOtaUrl() {
            const url = document.getElementById('ota-url')?.value || '';
            if (!url) { Notification.error(i18n.t('ota-url-empty'), i18n.t('ota-title')); return; }
            if (!url.startsWith('http://') && !url.startsWith('https://')) { Notification.error(i18n.t('ota-url-invalid'), i18n.t('ota-title')); return; }

            const btn = document.getElementById('ota-url-btn');
            if (btn) { btn.disabled = true; btn.innerHTML = i18n.t('ota-downloading-html'); }

            const progressWrap = document.getElementById('ota-progress-wrap');
            if (progressWrap) progressWrap.classList.remove('is-hidden');

            apiPost('/api/ota/url', { url })
                .then(res => {
                    if (res && res.success) {
                        Notification.success(i18n.t('ota-start-ok'), i18n.t('ota-title'));
                        this._pollOtaProgress();
                    } else {
                        Notification.error(res?.message || i18n.t('ota-start-fail'), i18n.t('ota-title'));
                        if (progressWrap) progressWrap.classList.add('is-hidden');
                    }
                })
                .catch(err => {
                    Notification.error(i18n.t('ota-start-fail') + ': ' + (err.message || err), i18n.t('ota-title'));
                    if (progressWrap) progressWrap.classList.add('is-hidden');
                })
                .finally(() => {
                    if (btn) { btn.disabled = false; btn.innerHTML = i18n.t('ota-start-url-html'); }
                });
        },

        startOtaUpload() {
            const fileInput = document.getElementById('ota-file');
            const file = fileInput?.files?.[0];
            if (!file) { Notification.error(i18n.t('ota-file-empty'), i18n.t('ota-title')); return; }
            if (!file.name.endsWith('.bin')) { Notification.error(i18n.t('ota-file-invalid'), i18n.t('ota-title')); return; }

            const btn = document.getElementById('ota-upload-btn');
            if (btn) { btn.disabled = true; btn.innerHTML = i18n.t('ota-uploading-html'); }

            const progressWrap = document.getElementById('ota-progress-wrap');
            const progressBar = document.getElementById('ota-progress-bar');
            const progressText = document.getElementById('ota-progress-text');
            if (progressWrap) progressWrap.classList.remove('is-hidden');

            const formData = new FormData();
            formData.append('firmware', file);

            const xhr = new XMLHttpRequest();

            xhr.upload.addEventListener('progress', (e) => {
                if (e.lengthComputable) {
                    const percent = Math.round((e.loaded / e.total) * 100);
                    if (progressBar) progressBar.style.width = percent + '%';
                    if (progressText) progressText.textContent = percent + '%';
                }
            });

            xhr.addEventListener('load', () => {
                if (xhr.status === 200) {
                    try {
                        const res = JSON.parse(xhr.responseText);
                        if (res.success) {
                            Notification.success(i18n.t('ota-upload-ok'), i18n.t('ota-title'));
                            setTimeout(() => { window.location.reload(); }, 5000);
                        } else {
                            Notification.error(res.message || i18n.t('ota-upload-fail'), i18n.t('ota-title'));
                        }
                    } catch (e) {
                        Notification.success(i18n.t('ota-upload-ok2'), i18n.t('ota-title'));
                    }
                } else {
                    Notification.error(i18n.t('ota-upload-fail-prefix') + xhr.status, i18n.t('ota-title'));
                }
                if (btn) { btn.disabled = false; btn.innerHTML = i18n.t('ota-upload-btn-html'); }
            });

            xhr.addEventListener('error', () => {
                Notification.error(i18n.t('ota-upload-network-fail'), i18n.t('ota-title'));
                if (btn) { btn.disabled = false; btn.innerHTML = i18n.t('ota-upload-btn-html'); }
                if (progressWrap) progressWrap.classList.add('is-hidden');
            });

            xhr.open('POST', '/api/ota/upload');
            xhr.setRequestHeader('Authorization', 'Bearer ' + localStorage.getItem('token'));
            xhr.send(formData);
        },

        _pollOtaProgress() {
            const progressBar = document.getElementById('ota-progress-bar');
            const progressText = document.getElementById('ota-progress-text');
            const badge = document.getElementById('ota-status-badge');

            const poll = () => {
                apiGet('/api/ota/status')
                    .then(res => {
                        if (!res) return;
                        const progress = res.progress || 0;
                        if (progressBar) progressBar.style.width = progress + '%';
                        if (progressText) progressText.textContent = progress + '%';

                        if (progress < 100 && res.status !== 'OTA ready') {
                            if (badge) { badge.className = 'status-badge status-warning'; badge.textContent = i18n.t('ota-in-progress'); }
                            setTimeout(poll, 1000);
                        } else if (progress >= 100) {
                            if (badge) { badge.className = 'status-badge status-online'; badge.textContent = i18n.t('ota-done'); }
                            Notification.success(i18n.t('ota-complete-msg'), i18n.t('ota-title'));
                        }
                    })
                    .catch(err => {
                        console.error(i18n.t('ota-progress-fail'), err);
                    });
            };
            poll();
        }
    });

    // 自动绑定事件
    if (typeof AppState.setupDeviceConfigEvents === 'function') {
        AppState.setupDeviceConfigEvents();
    }
})();
