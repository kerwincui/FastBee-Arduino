/**
 * 仪表板模块
 * 包含仪表板渲染、系统监控、网络状态
 */
(function() {
    AppState.registerModule('dashboard', {
        _dashboardMonitorLoadPromise: null,
        _dashboardNetworkLoadPromise: null,

        // ============ 事件绑定 ============
        setupDashboardEvents() {
            // 刷新监控按钮
            const monitorRefreshBtn = document.getElementById('monitor-refresh-btn');
            if (monitorRefreshBtn) monitorRefreshBtn.addEventListener('click', () => this.loadSystemMonitor({ noCache: true, loadRuntime: true, runtimeDelayMs: 200 }));

            // 仪表盘网络状态刷新按钮
            const dashboardNetRefreshBtn = document.getElementById('dashboard-net-refresh-btn');
            if (dashboardNetRefreshBtn) dashboardNetRefreshBtn.addEventListener('click', () => this.loadNetworkStatus({ noCache: true }));
        },

        bootDashboardPage() {
            this.loadSystemMonitor({ loadRuntime: false });
        },

        // ============ 仪表板（从 /api/system/status 加载实时数据）============
        renderDashboard() {
            apiBatchGet('/api/system/status')
                .then(res => {
                    if (!res || !res.success) return;
                    const d = res.data || {};
                    const freeHeap = d.freeHeap || 0;
                    const heapTotal = 327680; // ESP32典型 320KB
                    const memPct = Math.round((1 - freeHeap / heapTotal) * 100);

                    const set = (id, val) => { const el = document.getElementById(id); if (el) el.textContent = val; };
                    set('memory-usage', memPct + '%');
                    set('cpu-usage', d.uptime !== undefined ? '—' : '—');
                    set('storage-usage', '—');
                    set('temperature', '—°C');

                    // 在线设备数（暂用模拟，等待设备API）
                    const tbody = document.getElementById('device-table-body');
                    if (tbody && typeof this.renderEmptyTableRow === 'function') {
                        this.renderEmptyTableRow(
                            tbody,
                            4,
                            d.networkConnected ? '网络已连接 IP: ' + (d.ipAddress || '') : '网络未连接'
                        );
                    }
                })
                .catch(() => {
                    // 无法获取状态时显示占位内容
                    ['cpu-usage', 'memory-usage', 'storage-usage', 'temperature'].forEach(id => {
                        const el = document.getElementById(id); if (el) el.textContent = '—';
                    });
                });
        },

        // ============ 系统信息（兼容保留）============
        // @deprecated monitor 独立页面已移除；请使用 dashboard + loadSystemMonitor()
        loadSystemInfo() {
            apiGet('/api/system/info')
                .then(res => {
                    if (!res || !res.success) return;
                    const d = res.data || {};
                    // 若页面有系统信息展示元素则填充
                    const set = (id, val) => { const el = document.getElementById(id); if (el) el.textContent = val; };
                    set('sys-chip-model', d.chipModel || '—');
                    set('sys-cpu-freq', d.cpuFreqMHz ? d.cpuFreqMHz + ' MHz' : '—');
                    set('sys-free-heap', d.freeHeap ? Math.round(d.freeHeap / 1024) + ' KB' : '—');
                    set('sys-uptime', d.uptime ? Math.round(d.uptime / 1000) + ' s' : '—');
                    set('sys-sdk-version', d.sdkVersion || '—');
                })
                .catch(() => {});
        },

        // ============ 系统监控 ============

        /**
         * 加载系统监控数据
         */
        loadSystemMonitor(options) {
            options = options || {};
            if (this._dashboardMonitorLoadPromise) return this._dashboardMonitorLoadPromise;
            var self = this;
            var monitorSourcePromise;
            if (typeof apiBatchGetMany === 'function') {
                var batchMany = (options.noCache === true && typeof apiBatchGetManyFresh === 'function')
                    ? apiBatchGetManyFresh
                    : apiBatchGetMany;
                monitorSourcePromise = batchMany([
                    { url: '/api/system/info' },
                    { url: '/api/network/status' }
                ]).then(function(results) {
                    return {
                        info: results && results[0],
                        network: results && results[1]
                    };
                });
            } else {
                var batchGetter = null;
                if (options.noCache === true && typeof apiBatchGetFresh === 'function') {
                    batchGetter = apiBatchGetFresh;
                } else if (typeof apiBatchGet === 'function') {
                    batchGetter = apiBatchGet;
                }
                var getter = batchGetter || (options.noCache === true && typeof apiGetFresh === 'function' ? apiGetFresh : apiGet);
                // Fallback keeps the older batch-window path for full profile builds.
                var infoPromise = getter('/api/system/info');
                var netPromise = getter('/api/network/status');
                monitorSourcePromise = infoPromise.then(function(res) {
                    return netPromise
                        .then(function(netRes) {
                            return { info: res, network: netRes };
                        })
                        .catch(function(err) {
                            if (!(err && err._pageAborted)) {
                                console.error('Load network status failed:', err);
                            }
                            return { info: res, network: null };
                        });
                });
            }

            var monitorPromise = monitorSourcePromise
                .then(result => {
                    const res = result && result.info;
                    if (!res || !res.success) return;

                    const data = res.data || {};

                    // 设备信息
                    const device = data.device || {};
                    this._setText('monitor-chip-model', device.chipModel || 'ESP32');
                    this._setText('monitor-cpu-freq', device.cpuFreqMHz || '--');
                    this._setText('monitor-sdk', device.sdkVersion || '--');

                    // 运行时间
                    const uptime = data.uptime || {};
                    this._setText('monitor-uptime', uptime.formatted || '--');

                    // 网络状态
                    const network = data.network || {};
                    const netStatus = network.connected ?
                        `<span class="u-text-success">●</span> 已连接 (${network.ssid || 'N/A'})` :
                        `<span class="u-text-danger">●</span> 未连接`;
                    this._setHtml('monitor-network-status', netStatus);
                    this._setText('monitor-ip', network.ipAddress || '--');

                    // Flash 存储
                    const flash = data.flash || {};
                    this._setText('monitor-flash-percent', (flash.usagePercent || 0) + '%');
                    this._setBar('monitor-flash-bar', flash.usagePercent || 0);
                    this._setText('monitor-flash-used', this._formatBytes(flash.used || 0));
                    this._setText('monitor-flash-free', this._formatBytes(flash.free || 0));
                    this._setText('monitor-flash-total', this._formatBytes(flash.total || 0));
                    this._setText('monitor-flash-sketch', this._formatBytes(flash.sketchSize || 0));

                    // 内存
                    const memory = data.memory || {};
                    const memoryPercent = (memory.usagePercent !== undefined) ? memory.usagePercent : (memory.heapUsagePercent || 0);
                    const memoryUsed = (memory.used !== undefined) ? memory.used : (memory.heapUsed || 0);
                    const memoryFree = (memory.free !== undefined) ? memory.free : (memory.heapFree || 0);
                    const memoryTotal = (memory.total !== undefined) ? memory.total : (memory.heapTotal || 0);
                    this._setText('monitor-heap-percent', memoryPercent + '%');
                    this._setBar('monitor-heap-bar', memoryPercent);
                    this._setText('monitor-heap-used', this._formatBytes(memoryUsed));
                    this._setText('monitor-heap-free', this._formatBytes(memoryFree));
                    this._setText('monitor-heap-total', this._formatBytes(memoryTotal));
                    this._setText('monitor-heap-min', this._formatBytes(memory.heapMinFree || 0));

                    // 文件系统
                    const fs = data.filesystem || {};
                    this._setText('monitor-fs-percent', (fs.usagePercent || 0) + '%');
                    this._setBar('monitor-fs-bar', fs.usagePercent || 0);
                    this._setText('monitor-fs-used', this._formatBytes(fs.used || 0));
                    this._setText('monitor-fs-free', this._formatBytes(fs.free || 0));
                    this._setText('monitor-fs-total', this._formatBytes(fs.total || 0));

                    // Apply network status once the batched/fallback result is ready.
                    const netRes = result && result.network;
                    if (netRes && netRes.success) {
                        self._applyNetworkStatus(netRes);
                    }
                })
                .catch(err => {
                    if (!(err && err._pageAborted)) {
                        console.error('Load system monitor failed:', err);
                    }
                });

            if (options.loadRuntime === true) {
                // Web Runtime module removed
            }
            this._dashboardMonitorLoadPromise = monitorPromise.finally(function() {
                self._dashboardMonitorLoadPromise = null;
            });
            return this._dashboardMonitorLoadPromise;
        },

        /**
         * 加载并显示网络状态
         */
        loadNetworkStatus(options) {
            options = options || {};
            if (this._dashboardNetworkLoadPromise) return this._dashboardNetworkLoadPromise;
            var self = this;
            const refreshBtn = document.getElementById('dashboard-net-refresh-btn');
            if (refreshBtn) {
                refreshBtn.disabled = true;
                refreshBtn.innerHTML = '刷新中...';
            }

            var getter = (options.noCache === true && typeof apiGetFresh === 'function') ? apiGetFresh : apiGet;
            this._dashboardNetworkLoadPromise = getter('/api/network/status')
                .then(res => {
                    if (!res || !res.success) {
                        Notification.error('获取网络状态失败', '网络状态');
                        return;
                    }
                    self._applyNetworkStatus(res);
                })
                .catch(err => {
                    console.error('Load network status failed:', err);
                    Notification.error('获取网络状态失败', '网络状态');
                })
                .finally(() => {
                    if (refreshBtn) {
                        refreshBtn.disabled = false;
                        refreshBtn.innerHTML = '刷新';
                    }
                    this._dashboardNetworkLoadPromise = null;
                });
            return this._dashboardNetworkLoadPromise;
        },

        /**
         * 应用网络状态数据到 DOM（供批量和单独请求共用）
         */
        _applyNetworkStatus(res) {
            const d = res.data || {};

            const setText = (id, val) => {
                const el = document.getElementById(id);
                if (el) el.textContent = (val !== undefined && val !== null && val !== '') ? val : '--';
            };
            const setHtml = (id, html) => {
                const el = document.getElementById(id);
                if (el) el.innerHTML = html;
            };

            // 状态徽章
            const statusMap = {
                connected:    '<span class="badge badge-success">已连接</span>',
                disconnected: '<span class="badge badge-danger">未连接</span>',
                connecting:   '<span class="badge badge-warning">连接中...</span>',
                ap_mode:      '<span class="badge badge-primary">AP模式</span>',
                failed:       '<span class="badge badge-danger">连接失败</span>',
            };
            setHtml('ns-status', statusMap[d.status] || `<span class="badge badge-info">${d.status || '--'}</span>`);

            // STA 信息
            setText('ns-ssid', d.ssid);
            setText('ns-ip', d.ipAddress);
            setText('ns-gateway', d.gateway);
            setText('ns-subnet', d.subnet);
            setText('ns-dns', d.dnsServer);
            setText('ns-dns2', d.dnsServer2);
            setText('ns-mac', d.macAddress);
            // 连接时长
            if (d.connectedTime !== undefined && d.connectedTime > 0) {
                const sec = d.connectedTime;
                const h = Math.floor(sec / 3600);
                const m = Math.floor((sec % 3600) / 60);
                const s = sec % 60;
                setText('ns-conn-time', `${h}h ${m}m ${s}s`);
            } else {
                setText('ns-conn-time', '--');
            }
            // RSSI
            const rssi = d.rssi;
            if (rssi !== undefined && rssi !== null && rssi !== '') {
                const pct = d.signalStrength || 0;
                const toneClass = pct >= 70 ? 'u-text-success' : pct >= 40 ? 'u-text-warning' : 'u-text-danger';
                setHtml('ns-rssi', `<span class="${toneClass}">${rssi} dBm (${pct}%)</span>`);
            } else {
                setText('ns-rssi', '--');
            }

            // AP 信息
            setText('ns-ap-ssid', d.apSSID);
            setText('ns-ap-ip', d.apIPAddress);
            setText('ns-ap-channel', d.apChannel !== undefined ? `CH ${d.apChannel}` : '--');
            setText('ns-ap-clients', d.apClientCount !== undefined ? d.apClientCount + ' 台' : '--');

            // 连接统计
            const modeLabel = {
                STA: '仅客户端 (STA)',
                AP: '仅热点 (AP)'
            };
            setText('ns-mode', modeLabel[d.mode] || d.mode || '--');
            var actualDomain = d.mdnsDomain || d.customDomain;
            setText('ns-mdns', d.enableMDNS ? (actualDomain ? actualDomain + '.local' : '已启用') : '禁用');
            setText('ns-reconnect', d.reconnectAttempts !== undefined ? d.reconnectAttempts + ' 次' : '--');
            setText('ns-tx-count', d.txCount !== undefined ? d.txCount + ' 条' : '--');
            setText('ns-rx-count', d.rxCount !== undefined ? d.rxCount + ' 条' : '--');
            setHtml('ns-internet', d.internetAvailable
                ? '<span class="badge badge-success">可访问</span>'
                : '<span class="badge badge-danger">不可访问</span>');
            setHtml('ns-conflict', d.conflictDetected
                ? '<span class="badge badge-danger">已检测到冲突</span>'
                : '<span class="badge badge-success">无冲突</span>');
            setText('ns-uptime', d.uptimeFormatted || '--');
        },

    });

    // 自动绑定事件
    if (typeof AppState.setupDashboardEvents === 'function') {
        AppState.setupDashboardEvents();
    }
})();
