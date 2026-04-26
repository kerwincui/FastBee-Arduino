/**
 * 仪表板模块
 * 包含仪表板渲染、系统监控、网络状态
 */
(function() {
    AppState.registerModule('dashboard', {

        // ============ 事件绑定 ============
        setupDashboardEvents() {
            // 刷新监控按钮
            const monitorRefreshBtn = document.getElementById('monitor-refresh-btn');
            if (monitorRefreshBtn) monitorRefreshBtn.addEventListener('click', () => this.loadSystemMonitor());

            // 仪表盘网络状态刷新按钮
            const dashboardNetRefreshBtn = document.getElementById('dashboard-net-refresh-btn');
            if (dashboardNetRefreshBtn) dashboardNetRefreshBtn.addEventListener('click', () => this.loadNetworkStatus());
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
                            d.networkConnected ? i18n.t('dashboard-connected-prefix') + (d.ipAddress || '') : i18n.t('dashboard-disconnected')
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
                    set('sys-user-count', d.userCount || '—');
                    set('sys-sessions', d.activeSessions || '—');
                })
                .catch(() => {});
        },

        // ============ 系统监控 ============

        /**
         * 加载系统监控数据
         */
        loadSystemMonitor() {
            var self = this;
            // 使用 apiBatchGet 同时发起 system/info 和 network/status
            // 它们会在 50ms 窗口内被合并为单个 /api/batch 请求
            var infoPromise = apiBatchGet('/api/system/info');
            var netPromise = apiBatchGet('/api/network/status');

            infoPromise
                .then(res => {
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
                        `<span class="u-text-success">●</span> ${i18n.t('monitor-connected')} (${network.ssid || 'N/A'})` :
                        `<span class="u-text-danger">●</span> ${i18n.t('monitor-disconnected')}`;
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
                    this._setText('monitor-heap-percent', (memory.heapUsagePercent || 0) + '%');
                    this._setBar('monitor-heap-bar', memory.heapUsagePercent || 0);
                    this._setText('monitor-heap-used', this._formatBytes(memory.heapUsed || 0));
                    this._setText('monitor-heap-free', this._formatBytes(memory.heapFree || 0));
                    this._setText('monitor-heap-total', this._formatBytes(memory.heapTotal || 0));
                    this._setText('monitor-heap-min', this._formatBytes(memory.heapMinFree || 0));

                    // 文件系统
                    const fs = data.filesystem || {};
                    this._setText('monitor-fs-percent', (fs.usagePercent || 0) + '%');
                    this._setBar('monitor-fs-bar', fs.usagePercent || 0);
                    this._setText('monitor-fs-used', this._formatBytes(fs.used || 0));
                    this._setText('monitor-fs-free', this._formatBytes(fs.free || 0));
                    this._setText('monitor-fs-total', this._formatBytes(fs.total || 0));

                    // 用户统计
                    const users = data.users || {};
                    this._setText('monitor-user-total', users.total || 0);
                    this._setText('monitor-user-online', users.online || 0);
                    this._setText('monitor-user-sessions', users.activeSessions || 0);

                    // 网络状态通过独立的 promise 加载（已被批量合并）
                    netPromise.then(netRes => {
                        if (netRes && netRes.success) {
                            self._applyNetworkStatus(netRes);
                        }
                    }).catch(err => {
                        console.error('Load network status failed:', err);
                    });
                })
                .catch(err => {
                    console.error('Load system monitor failed:', err);
                });
        },

        /**
         * 加载并显示网络状态
         */
        loadNetworkStatus() {
            var self = this;
            const refreshBtn = document.getElementById('dashboard-net-refresh-btn');
            if (refreshBtn) {
                refreshBtn.disabled = true;
                refreshBtn.innerHTML = i18n.t('net-refreshing-html');
            }

            apiGet('/api/network/status')
                .then(res => {
                    if (!res || !res.success) {
                        Notification.error(i18n.t('net-status-load-fail'), i18n.t('net-status-title-msg'));
                        return;
                    }
                    self._applyNetworkStatus(res);
                })
                .catch(err => {
                    console.error('Load network status failed:', err);
                    Notification.error(i18n.t('net-status-load-fail'), i18n.t('net-status-title-msg'));
                })
                .finally(() => {
                    if (refreshBtn) {
                        refreshBtn.disabled = false;
                        refreshBtn.innerHTML = i18n.t('net-refresh-html');
                    }
                });
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
                connected:    `<span class="badge badge-success">${i18n.t('net-status-connected')}</span>`,
                disconnected: `<span class="badge badge-danger">${i18n.t('net-status-disconnected')}</span>`,
                connecting:   `<span class="badge badge-warning">${i18n.t('net-status-connecting')}</span>`,
                ap_mode:      `<span class="badge badge-primary">${i18n.t('net-status-ap')}</span>`,
                failed:       `<span class="badge badge-danger">${i18n.t('net-status-failed')}</span>`,
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
            setText('ns-ap-clients', d.apClientCount !== undefined ? d.apClientCount + i18n.t('net-ap-clients-unit') : '--');

            // 连接统计
            const modeLabel = {
                STA: i18n.t('net-mode-sta'),
                AP: i18n.t('net-mode-ap'),
                'AP+STA': i18n.t('net-mode-apsta')
            };
            setText('ns-mode', modeLabel[d.mode] || d.mode || '--');
            var actualDomain = d.mdnsDomain || d.customDomain;
            setText('ns-mdns', d.enableMDNS ? (actualDomain ? actualDomain + '.local' : i18n.t('net-mdns-enabled')) : i18n.t('net-mdns-disabled'));
            setText('ns-reconnect', d.reconnectAttempts !== undefined ? d.reconnectAttempts + i18n.t('net-reconnect-unit') : '--');
            setText('ns-tx-count', d.txCount !== undefined ? d.txCount + i18n.t('net-count-unit') : '--');
            setText('ns-rx-count', d.rxCount !== undefined ? d.rxCount + i18n.t('net-count-unit') : '--');
            setHtml('ns-internet', d.internetAvailable
                ? `<span class="badge badge-success">${i18n.t('net-accessible')}</span>`
                : `<span class="badge badge-danger">${i18n.t('net-inaccessible')}</span>`);
            setHtml('ns-conflict', d.conflictDetected
                ? `<span class="badge badge-danger">${i18n.t('net-conflict-yes')}</span>`
                : `<span class="badge badge-success">${i18n.t('net-no-conflict')}</span>`);
            setText('ns-uptime', d.uptimeFormatted || '--');
        }
    });

    // 自动绑定事件
    if (typeof AppState.setupDashboardEvents === 'function') {
        AppState.setupDashboardEvents();
    }
})();
