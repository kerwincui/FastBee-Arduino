/**
 * 全屏页面独立模块
 * 供 fullscreen.html 使用，不依赖 AppState 模块系统
 * 独立建立 SSE 连接、加载数据、自动刷新
 */
(function() {
    'use strict';

    var FS = {
        _sseSource: null,
        _refreshTimer: null,
        _monitorLoadPromise: null,
        _networkLoadPromise: null,
        _lastMonitorRefreshAt: 0,
        _lastNetworkRefreshAt: 0,
        REFRESH_INTERVAL: 30000,  // 30 秒自动刷新

        // ============ 初始化 ============
        init: function() {
            // 初始化 i18n
            if (typeof i18n !== 'undefined' && i18n.updatePageText) {
                i18n.updatePageText();
            }

            this.ensureWebRuntimePanel();

            // 绑定按钮事件
            var refreshBtn = document.getElementById('fullscreen-refresh-btn');
            if (refreshBtn) refreshBtn.addEventListener('click', function() { FS.loadAll({ noCache: true, force: true }); });

            var closeBtn = document.getElementById('fullscreen-close-btn');
            if (closeBtn) closeBtn.addEventListener('click', function() { FS.closeWindow(); });

            // 首次加载数据
            this.loadAll();

            // 建立 SSE 连接
            this._setupSSE();

            // 启动自动刷新
            this._setupAutoRefresh();
        },

        // ============ 数据加载 ============
        loadAll: function(options) {
            this.loadSystemMonitor(options);
            this.loadNetworkStatus(options);
            this._scheduleAutoRefresh();
        },

        loadSystemMonitor: function(options) {
            options = options || {};
            if (this._monitorLoadPromise) {
                return this._monitorLoadPromise;
            }

            var self = this;
            this.ensureWebRuntimePanel();
            var batchGetter = null;
            if (options.noCache === true && typeof apiBatchGetFresh === 'function') {
                batchGetter = apiBatchGetFresh;
            } else if (typeof apiBatchGet === 'function') {
                batchGetter = apiBatchGet;
            }

            var getter = batchGetter || (
                options.noCache === true && typeof apiGetFresh === 'function'
                    ? apiGetFresh
                    : ((typeof apiGet === 'function') ? apiGet : self._fetchJson.bind(self))
            );
            var runtimeGetter = options.noCache === true && typeof apiGetFresh === 'function'
                ? apiGetFresh
                : ((typeof apiGet === 'function') ? apiGet : self._fetchJson.bind(self));
            var infoPromise = getter('/api/system/info');

            var monitorPromise = infoPromise
                .then(function(res) {
                    if (!res || !res.success) return;
                    var data = res.data || {};

                    // 设备信息
                    var device = data.device || {};
                    self._setText('monitor-chip-model', device.chipModel || 'ESP32');
                    self._setText('monitor-cpu-freq', device.cpuFreqMHz || '--');
                    self._setText('monitor-sdk', device.sdkVersion || '--');

                    // 运行时间
                    var uptime = data.uptime || {};
                    self._setText('monitor-uptime', uptime.formatted || '--');

                    // 网络状态
                    var network = data.network || {};
                    var netStatus = network.connected
                        ? '<span class="u-text-success">●</span> ' + self._t('monitor-connected') + ' (' + (network.ssid || 'N/A') + ')'
                        : '<span class="u-text-danger">●</span> ' + self._t('monitor-disconnected');
                    self._setHtml('monitor-network-status', netStatus);
                    self._setText('monitor-ip', network.ipAddress || '--');

                    // Flash 存储
                    var flash = data.flash || {};
                    self._setText('monitor-flash-percent', (flash.usagePercent || 0) + '%');
                    self._setBar('monitor-flash-bar', flash.usagePercent || 0);
                    self._setText('monitor-flash-used', self._formatBytes(flash.used || 0));
                    self._setText('monitor-flash-free', self._formatBytes(flash.free || 0));
                    self._setText('monitor-flash-total', self._formatBytes(flash.total || 0));
                    self._setText('monitor-flash-sketch', self._formatBytes(flash.sketchSize || 0));

                    // 内存
                    var memory = data.memory || {};
                    self._setText('monitor-heap-percent', (memory.heapUsagePercent || 0) + '%');
                    self._setBar('monitor-heap-bar', memory.heapUsagePercent || 0);
                    self._setText('monitor-heap-used', self._formatBytes(memory.heapUsed || 0));
                    self._setText('monitor-heap-free', self._formatBytes(memory.heapFree || 0));
                    self._setText('monitor-heap-total', self._formatBytes(memory.heapTotal || 0));
                    self._setText('monitor-heap-min', self._formatBytes(memory.heapMinFree || 0));

                    // 文件系统
                    var fs = data.filesystem || {};
                    self._setText('monitor-fs-percent', (fs.usagePercent || 0) + '%');
                    self._setBar('monitor-fs-bar', fs.usagePercent || 0);
                    self._setText('monitor-fs-used', self._formatBytes(fs.used || 0));
                    self._setText('monitor-fs-free', self._formatBytes(fs.free || 0));
                    self._setText('monitor-fs-total', self._formatBytes(fs.total || 0));

                })
                .catch(function(err) {
                    console.error('[Fullscreen] Load system monitor failed:', err);
                });

            var runtimePromise = infoPromise
                .then(function(res) {
                    if (!res || !res.success) return null;
                    return runtimeGetter('/api/system/web-runtime');
                })
                .then(function(res) {
                    if (res && res.success) {
                        self._applyWebRuntime(res);
                    }
                })
                .catch(function(err) {
                    console.error('[Fullscreen] Load web runtime failed:', err);
                });

            this._monitorLoadPromise = Promise.allSettled([monitorPromise, runtimePromise]).finally(function() {
                self._monitorLoadPromise = null;
                self._lastMonitorRefreshAt = Date.now();
            });

            return this._monitorLoadPromise;
        },

        loadNetworkStatus: function(options) {
            options = options || {};
            if (this._networkLoadPromise) {
                return this._networkLoadPromise;
            }

            var self = this;
            var batchGetter = null;
            if (options.noCache === true && typeof apiBatchGetFresh === 'function') {
                batchGetter = apiBatchGetFresh;
            } else if (typeof apiBatchGet === 'function') {
                batchGetter = apiBatchGet;
            }

            var getter = batchGetter || (
                options.noCache === true && typeof apiGetFresh === 'function'
                    ? apiGetFresh
                    : ((typeof apiGet === 'function') ? apiGet : self._fetchJson.bind(self))
            );

            this._networkLoadPromise = getter('/api/network/status')
                .then(function(res) {
                    if (!res || !res.success) return;
                    self._applyNetworkStatus(res);
                })
                .catch(function(err) {
                    console.error('[Fullscreen] Load network status failed:', err);
                })
                .finally(function() {
                    self._networkLoadPromise = null;
                    self._lastNetworkRefreshAt = Date.now();
                });

            return this._networkLoadPromise;
        },

        // ============ 网络状态渲染 ============
        _applyNetworkStatus: function(res) {
            var d = res.data || {};
            var self = this;

            var setText = function(id, val) {
                var el = document.getElementById(id);
                if (el) el.textContent = (val !== undefined && val !== null && val !== '') ? val : '--';
            };
            var setHtml = function(id, html) {
                var el = document.getElementById(id);
                if (el) el.innerHTML = html;
            };

            // 状态徽章
            var statusMap = {
                connected:    '<span class="badge badge-success">' + self._t('net-status-connected') + '</span>',
                disconnected: '<span class="badge badge-danger">' + self._t('net-status-disconnected') + '</span>',
                connecting:   '<span class="badge badge-warning">' + self._t('net-status-connecting') + '</span>',
                ap_mode:      '<span class="badge badge-primary">' + self._t('net-status-ap') + '</span>',
                failed:       '<span class="badge badge-danger">' + self._t('net-status-failed') + '</span>'
            };
            setHtml('ns-status', statusMap[d.status] || '<span class="badge badge-info">' + (d.status || '--') + '</span>');

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
                var sec = d.connectedTime;
                var h = Math.floor(sec / 3600);
                var m = Math.floor((sec % 3600) / 60);
                var s = sec % 60;
                setText('ns-conn-time', h + 'h ' + m + 'm ' + s + 's');
            } else {
                setText('ns-conn-time', '--');
            }

            // RSSI
            var rssi = d.rssi;
            if (rssi !== undefined && rssi !== null && rssi !== '') {
                var pct = d.signalStrength || 0;
                var toneClass = pct >= 70 ? 'u-text-success' : pct >= 40 ? 'u-text-warning' : 'u-text-danger';
                setHtml('ns-rssi', '<span class="' + toneClass + '">' + rssi + ' dBm (' + pct + '%)</span>');
            } else {
                setText('ns-rssi', '--');
            }

            // AP 信息
            setText('ns-ap-ssid', d.apSSID);
            setText('ns-ap-ip', d.apIPAddress);
            setText('ns-ap-channel', d.apChannel !== undefined ? 'CH ' + d.apChannel : '--');
            setText('ns-ap-clients', d.apClientCount !== undefined ? d.apClientCount + self._t('net-ap-clients-unit') : '--');

            // 连接统计
            var modeLabel = {
                STA: self._t('net-mode-sta'),
                AP: self._t('net-mode-ap')
            };
            setText('ns-mode', modeLabel[d.mode] || d.mode || '--');
            var actualDomain = d.mdnsDomain || d.customDomain;
            setText('ns-mdns', d.enableMDNS ? (actualDomain ? actualDomain + '.local' : self._t('net-mdns-enabled')) : self._t('net-mdns-disabled'));
            setText('ns-reconnect', d.reconnectAttempts !== undefined ? d.reconnectAttempts + self._t('net-reconnect-unit') : '--');
            setText('ns-tx-count', d.txCount !== undefined ? d.txCount + self._t('net-count-unit') : '--');
            setText('ns-rx-count', d.rxCount !== undefined ? d.rxCount + self._t('net-count-unit') : '--');
            setHtml('ns-internet', d.internetAvailable
                ? '<span class="badge badge-success">' + self._t('net-accessible') + '</span>'
                : '<span class="badge badge-danger">' + self._t('net-inaccessible') + '</span>');
            setHtml('ns-conflict', d.conflictDetected
                ? '<span class="badge badge-danger">' + self._t('net-conflict-yes') + '</span>'
                : '<span class="badge badge-success">' + self._t('net-no-conflict') + '</span>');
            setText('ns-uptime', d.uptimeFormatted || '--');
        },

        // ============ SSE 连接 ============
        ensureWebRuntimePanel: function() {
            var networkGrid = document.getElementById('ns-status');
            networkGrid = networkGrid ? networkGrid.closest('.dashboard-network-grid') : null;
            var networkHeader = networkGrid ? networkGrid.previousElementSibling : null;
            if (window.WebRuntimeDiagnostics) {
                window.WebRuntimeDiagnostics.ensurePanel({
                    anchorEl: networkHeader,
                    t: this._t.bind(this)
                });
            }
        },

        _applyWebRuntime: function(res) {
            if (window.WebRuntimeDiagnostics) {
                window.WebRuntimeDiagnostics.apply({
                    t: this._t.bind(this),
                    setText: this._setText.bind(this),
                    setHtml: this._setHtml.bind(this),
                    formatBytes: this._formatBytes.bind(this)
                }, res);
            }
        },

        _getPressureState: function() {
            if (typeof apiGetPressureState === 'function') {
                return apiGetPressureState();
            }
            return { level: 'NORMAL', activeRank: 0 };
        },

        _getAutoRefreshIntervalMs: function() {
            var pressure = this._getPressureState();
            var rank = pressure && typeof pressure.activeRank === 'number' ? pressure.activeRank : 0;
            if (rank >= 3) return 120000;
            if (rank >= 2) return 60000;
            if (rank >= 1) return 45000;
            return this.REFRESH_INTERVAL;
        },

        _getEventRefreshIntervalMs: function(kind) {
            var pressure = this._getPressureState();
            var rank = pressure && typeof pressure.activeRank === 'number' ? pressure.activeRank : 0;

            if (kind === 'network') {
                if (rank >= 3) return 45000;
                if (rank >= 2) return 20000;
                return 8000;
            }

            if (rank >= 3) return 30000;
            if (rank >= 2) return 15000;
            return 6000;
        },

        _scheduleAutoRefresh: function() {
            var self = this;
            if (this._refreshTimer) {
                clearTimeout(this._refreshTimer);
            }
            this._refreshTimer = setTimeout(function() {
                self.loadAll();
            }, this._getAutoRefreshIntervalMs());
        },

        _requestMonitorRefresh: function(options) {
            options = options || {};
            if (options.force !== true && (Date.now() - this._lastMonitorRefreshAt) < this._getEventRefreshIntervalMs('monitor')) {
                return Promise.resolve(null);
            }
            return this.loadSystemMonitor(options);
        },

        _requestNetworkRefresh: function(options) {
            options = options || {};
            if (options.force !== true && (Date.now() - this._lastNetworkRefreshAt) < this._getEventRefreshIntervalMs('network')) {
                return Promise.resolve(null);
            }
            return this.loadNetworkStatus(options);
        },

        _setupSSE: function() {
            try {
                this._sseSource = new EventSource('/api/events');
                var self = this;

                this._sseSource.onopen = function() {
                    console.log('[Fullscreen] SSE connected');
                };

                this._sseSource.onerror = function() {
                    console.warn('[Fullscreen] SSE connection error');
                };

                // 监听心跳事件
                this._sseSource.addEventListener('heartbeat', function(e) {
                    // 心跳正常，连接活跃
                });

                // 监听 modbus 数据事件 - 触发数据刷新
                this._sseSource.addEventListener('modbus-data', function(e) {
                    // Modbus 数据更新时刷新监控数据
                    self._requestMonitorRefresh();
                });

                // 监听 MQTT 状态事件
                this._sseSource.addEventListener('mqtt-status', function(e) {
                    self._requestNetworkRefresh();
                });

                // 监听 Modbus 状态事件
                this._sseSource.addEventListener('modbus-status', function(e) {
                    self._requestMonitorRefresh();
                });
            } catch (e) {
                console.error('[Fullscreen] SSE setup failed:', e);
            }
        },

        // ============ 自动刷新 ============
        _setupAutoRefresh: function() {
            this._scheduleAutoRefresh();
        },

        // ============ 关闭窗口 ============
        closeWindow: function() {
            // 清理 SSE 连接
            if (this._sseSource) {
                this._sseSource.close();
                this._sseSource = null;
            }
            // 清理定时器
            if (this._refreshTimer) {
                clearTimeout(this._refreshTimer);
                this._refreshTimer = null;
            }
            // 尝试关闭窗口（仅对脚本打开的窗口有效）
            window.close();
            // 如果 window.close() 被浏览器阻止，显示提示
            setTimeout(function() {
                var msg = typeof i18n !== 'undefined' ? i18n.t('dashboard-close-hint') : '请手动关闭此标签页';
                alert(msg);
            }, 300);
        },

        // ============ 工具方法 ============
        _setText: function(id, val) {
            var el = document.getElementById(id);
            if (el) el.textContent = val;
        },

        _setHtml: function(id, html) {
            var el = document.getElementById(id);
            if (el) el.innerHTML = html;
        },

        _setBar: function(id, pct) {
            var el = document.getElementById(id);
            if (!el) return;
            pct = Math.max(0, Math.min(100, pct || 0));
            el.style.width = pct + '%';
            if (pct === 0) {
                el.classList.add('progress-fill-zero');
            } else {
                el.classList.remove('progress-fill-zero');
            }
        },

        _formatBytes: function(bytes) {
            if (bytes === 0) return '0 B';
            var k = 1024;
            var sizes = ['B', 'KB', 'MB', 'GB'];
            var i = Math.floor(Math.log(bytes) / Math.log(k));
            i = Math.min(i, sizes.length - 1);
            return parseFloat((bytes / Math.pow(k, i)).toFixed(1)) + ' ' + sizes[i];
        },

        _t: function(key) {
            if (typeof i18n !== 'undefined' && i18n.t) return i18n.t(key);
            return key;
        },

        /**
         * 降级 fetch（当 apiGet/apiBatchGet 不可用时）
         */
        _fetchJson: function(url) {
            return fetch(url)
                .then(function(res) { return res.json(); })
                .catch(function(err) {
                    console.error('[Fullscreen] Fetch failed:', url, err);
                    return null;
                });
        }
    };

    // 页面加载后自动初始化
    if (document.readyState === 'loading') {
        document.addEventListener('DOMContentLoaded', function() { FS.init(); });
    } else {
        FS.init();
    }
})();
