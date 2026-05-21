/**
 * 设备配置模块
 * 包含设备基本信息、NTP配置、重启/恢复出厂、配置导入导出
 */
(function() {
    const MAX_CONFIG_TRANSFER_BYTES = 128 * 1024;
    const CONFIG_IMPORT_CHUNK_BYTES = 6 * 1024;
    const MAX_CONFIG_BUNDLE_BYTES = 512 * 1024;
    const CONFIG_TRANSFER_LABELS = {
        'all': '全部配置包',
        'device.json': '设备信息',
        'network.json': '网络配置',
        'peripherals.json': '外设配置',
        'periph_exec.json': '外设执行',
        'protocol.json': '通信协议',
        'users.json': '用户配置',
        'roles.json': '角色配置'
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

            const configExportBtn = document.getElementById('dev-config-export-btn');
            if (configExportBtn) configExportBtn.addEventListener('click', () => this.exportDeviceConfigBundle());

            const configImportBtn = document.getElementById('dev-config-import-btn');
            if (configImportBtn) configImportBtn.addEventListener('click', () => this.importDeviceConfigBundle());

            const configImportFile = document.getElementById('dev-config-import-file');
            if (configImportFile) {
                configImportFile.addEventListener('change', (e) => {
                    const files = e.target.files ? Array.from(e.target.files) : [];
                    this._setConfigTransferStatus(files.length ? ('已选择：' + files.map(file => file.name).join('、')) : '--');
                });
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
                    this._setValue('dev-ntp-enable', d.enableNTP ? '1' : '0');
                    this._setValue('dev-ntp-server1', d.ntpServer1 || 'https://iot.fastbee.cn/prod-api/iot/tool/ntp');
                    this._setValue('dev-ntp-server2', d.ntpServer2 || 'time.nist.gov');
                    this._setValue('dev-timezone', d.timezone || 'CST-8');
                    this._setValue('dev-sync-interval', d.syncInterval !== undefined ? String(d.syncInterval) : '3600');
                    this._setValue('dev-cache-duration', d.cacheDuration !== undefined ? String(d.cacheDuration) : '86400');
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
                    const dev = d.device || {};
                    const fw = d.firmware || {};
                    const set = (id, val) => { const el = document.getElementById(id); if (el) el.textContent = (val !== undefined && val !== null && val !== '') ? val : '--'; };
                    set('dev-sys-chip', dev.chipModel);
                    set('dev-sys-cpu', dev.cpuFreqMHz ? dev.cpuFreqMHz + ' MHz' : '--');
                    set('dev-sys-heap', dev.freeHeap ? Math.round(dev.freeHeap / 1024) + ' KB' : '--');
                    set('dev-sys-flash', dev.flashSize ? Math.round(dev.flashSize / 1024 / 1024) + ' MB' : '--');
                    set('dev-sys-sdk', dev.sdkVersion);
                    set('dev-sys-fw', fw.version || dev.firmwareVersion || '--');
                })
                .catch(err => {
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
                        // 后端可能自动生成了新的 deviceId（FBE+MAC），立即回填输入框
                        if (res.data && res.data.deviceId && deviceIdInput) {
                            deviceIdInput.value = res.data.deviceId;
                        }
                        if (typeof window.apiInvalidateCache === 'function') {
                            window.apiInvalidateCache('/api/device/config');
                        }
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
                        this.loadDeviceTime({ noCache: true });
                    } else {
                        Notification.error(res?.error || i18n.t('dev-save-fail'), i18n.t('dev-ntp-config-title'));
                    }
                })
                .catch(() => Notification.error(i18n.t('dev-save-fail'), i18n.t('dev-ntp-config-title')));
        },

        loadDeviceTime(options) {
            options = options || {};
            var self = this;
            const btn = document.getElementById('dev-time-refresh-btn');
            if (btn) { btn.disabled = true; btn.innerHTML = i18n.t('dev-refreshing-html'); }

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
                        console.error('Load device time failed:', err);
                        if (btn) {
                            btn.disabled = false;
                            btn.innerHTML = i18n.t('dev-refresh-html');
                            btn.title = '';
                        }
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
                    console.error('Load device time failed:', err);
                    if (btn) {
                        btn.disabled = false;
                        btn.innerHTML = i18n.t('dev-refresh-html');
                        btn.title = '';
                    }
                });
        },

        syncDeviceTime() {
            const btn = document.getElementById('dev-time-refresh-btn');
            if (btn) { btn.disabled = true; btn.innerHTML = i18n.t('dev-refreshing-html'); }
            apiPost('/api/device/time/sync', {})
                .then(res => {
                    if (!res || !res.success) {
                        Notification.warning(i18n.t('dev-time-sync-fail') || 'NTP同步失败', 'NTP');
                        this.loadDeviceTime({ noCache: true });
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
                    this.loadDeviceTime({ noCache: true });
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

        _getConfigTransferType() {
            const select = document.getElementById('dev-config-transfer-type');
            const value = select ? select.value : 'all';
            return CONFIG_TRANSFER_LABELS[value] ? value : 'all';
        },

        _getConfigTransferLabel(type) {
            return CONFIG_TRANSFER_LABELS[type] || type || '--';
        },

        _filterConfigTransferEntries(entries, type) {
            if (type === 'all') return entries;
            const filtered = entries.filter((entry) => this._normalizeConfigFileName(entry.name) === type);
            if (!filtered.length) {
                throw new Error('所选文件中没有 ' + this._getConfigTransferLabel(type) + ' 配置');
            }
            return filtered;
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
                throw new Error('配置文件名不正确：' + (item && item.name ? item.name : '--'));
            }
            if (!content || this._configTransferByteLength(content) > MAX_CONFIG_TRANSFER_BYTES) {
                throw new Error('配置文件过大或为空：' + name);
            }
            try {
                JSON.parse(content);
            } catch (err) {
                throw new Error('配置文件不是有效 JSON：' + name);
            }
            return { name, content };
        },

        async _importConfigTransferEntry(item) {
            const entry = this._validateConfigTransferEntry(item);
            const chunks = this._splitConfigTransferChunks(entry.content, CONFIG_IMPORT_CHUNK_BYTES);
            const total = chunks.length;
            for (let i = 0; i < total; i++) {
                const res = await apiPost('/api/config/transfer/import-chunk', {
                    name: entry.name,
                    index: String(i),
                    total: String(total),
                    chunk: chunks[i]
                }, 30000);
                if (!res || !res.success) {
                    throw new Error((res && res.error) || ('分片导入失败：' + entry.name));
                }
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

                        entries.push({
                            name: file.name,
                            content: text
                        });
                    });

                    return entries.map((item) => this._validateConfigTransferEntry(item));
                });
        },

        exportDeviceConfigBundle() {
            const btn = document.getElementById('dev-config-export-btn');
            const type = this._getConfigTransferType();
            if (btn) btn.disabled = true;
            this._setConfigTransferStatus('正在读取配置列表...');

            apiGetFresh('/api/config/transfer/list')
                .then(async (res) => {
                    let files = (res && res.success && res.data && res.data.files) ? res.data.files : [];
                    if (!files.length) throw new Error('没有可导出的配置文件');
                    if (type !== 'all') {
                        files = files.filter((item) => this._normalizeConfigFileName(item.name) === type);
                        if (!files.length) throw new Error('当前设备没有可导出的 ' + this._getConfigTransferLabel(type) + ' 配置');
                    }

                    const bundle = {
                        type: 'fastbee-config-bundle',
                        version: 1,
                        scope: type,
                        exportedAt: new Date().toISOString(),
                        files: []
                    };

                    for (let i = 0; i < files.length; i++) {
                        const item = files[i];
                        this._setConfigTransferStatus(`正在导出 ${item.name} (${i + 1}/${files.length})...`);
                        const url = new URL('/api/config/transfer/export', window.location.origin);
                        url.searchParams.set('name', item.name);
                        const resp = await this._configTransferFetch(url.toString());
                        if (!resp.ok) throw new Error(`导出失败：${item.name}`);
                        const content = await resp.text();
                        if (this._configTransferByteLength(content) > MAX_CONFIG_TRANSFER_BYTES) {
                            throw new Error(`配置文件过大：${item.name}`);
                        }
                        bundle.files.push({ name: item.name, content });
                    }

                    const exportContent = type === 'all'
                        ? JSON.stringify(bundle, null, 2)
                        : bundle.files[0].content;
                    const exportName = type === 'all'
                        ? 'fastbee-config-' + new Date().toISOString().replace(/[:.]/g, '-') + '.json'
                        : bundle.files[0].name;
                    const blob = new Blob([exportContent], { type: 'application/json' });
                    const link = document.createElement('a');
                    const objectUrl = URL.createObjectURL(blob);
                    link.href = objectUrl;
                    link.download = exportName;
                    document.body.appendChild(link);
                    link.click();
                    link.remove();
                    setTimeout(() => URL.revokeObjectURL(objectUrl), 3000);

                    this._setConfigTransferStatus(`配置导出完成：${this._getConfigTransferLabel(type)}，${bundle.files.length} 个文件`);
                    Notification.success('配置导出完成', '配置导入/导出');
                })
                .catch((err) => {
                    console.error('[device-config] export config bundle failed:', err);
                    this._setConfigTransferStatus('配置导出失败');
                    Notification.error(err && err.message ? err.message : '配置导出失败', '配置导入/导出');
                })
                .finally(() => {
                    if (btn) btn.disabled = false;
                });
        },

        importDeviceConfigBundle() {
            const input = document.getElementById('dev-config-import-file');
            const btn = document.getElementById('dev-config-import-btn');
            const type = this._getConfigTransferType();
            const selectedFiles = input && input.files ? Array.from(input.files) : [];
            if (!selectedFiles.length) {
                Notification.warning('请先选择配置文件或配置包', '配置导入/导出');
                return;
            }
            const totalSize = selectedFiles.reduce((sum, file) => sum + (file.size || 0), 0);
            if (totalSize > MAX_CONFIG_BUNDLE_BYTES) {
                Notification.warning('配置文件总量过大，请分批导入', '配置导入/导出');
                return;
            }
            if (!confirm('导入 ' + this._getConfigTransferLabel(type) + ' 会覆盖设备现有 /config 配置文件，是否继续？')) return;

            if (btn) btn.disabled = true;
            this._setConfigTransferStatus('正在读取配置文件...');

            this._readSelectedConfigEntries(selectedFiles)
                .then(async (files) => {
                    if (!files.length) throw new Error('配置包内没有可导入的配置文件');
                    files = this._filterConfigTransferEntries(files, type);
                    for (let i = 0; i < files.length; i++) {
                        const item = files[i];
                        this._setConfigTransferStatus(`正在导入 ${item.name} (${i + 1}/${files.length})...`);
                        await this._importConfigTransferEntry(item);
                    }
                    this._setConfigTransferStatus(`配置导入完成：${files.length} 个文件，建议重启设备使配置完全生效`);
                    Notification.success('配置导入完成，建议重启设备', '配置导入/导出');
                    if (typeof window.apiInvalidateCache === 'function') {
                        window.apiInvalidateCache();
                    }
                })
                .catch((err) => {
                    console.error('[device-config] import config bundle failed:', err);
                    this._setConfigTransferStatus('配置导入失败');
                    Notification.error(err && err.message ? err.message : '配置导入失败', '配置导入/导出');
                })
                .finally(() => {
                    if (btn) btn.disabled = false;
                });
        },

    });

    // 自动绑定事件
    if (typeof AppState.setupDeviceConfigEvents === 'function') {
        AppState.setupDeviceConfigEvents();
    }
})();
