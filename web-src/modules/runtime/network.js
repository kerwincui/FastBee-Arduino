/**
 * 网络配置模块
 * 包含网络配置加载/保存、WiFi扫描、AP配置、高级配置
 */
(function() {
    AppState.registerModule('network', {
        _networkModeChangeTimer: null,
        _networkRefreshTimer: null,
        _networkSaveCountdownTimer: null,
        _networkBleMessageTimer: null,
        _networkStatusPollingTimer: null,
        _networkStatusPollingCountdown: null,
        _networkStatusPollingRemaining: 0,
        _networkDeferredTimers: [],

        // ============ 事件绑定 ============
        setupNetworkEvents() {
            const pageRoot = document.getElementById('network-page');
            if (!pageRoot || pageRoot.dataset.eventsBound === 'true') return;
            pageRoot.dataset.eventsBound = 'true';
            // 联网方式切换
            const networkType = document.getElementById('network-type');
            if (networkType) {
                networkType.addEventListener('change', (e) => this._onNetworkTypeChange(e.target.value));
            }

            const networkTypeSaveBtn = document.getElementById('network-type-save-btn');
            if (networkTypeSaveBtn) {
                networkTypeSaveBtn.addEventListener('click', () => this.saveActiveNetworkConfig());
            }

            // WiFi扫描按钮
            const wifiScanBtn = document.getElementById('wifi-scan-btn');
            if (wifiScanBtn) wifiScanBtn.addEventListener('click', () => this.scanWifiNetworks());

            // 网络配置表单提交；保持 DOM 绑定顺序及提交时的方法查找。
            [
                ['wifi-form', 'saveNetworkConfig'],
                ['ap-form', 'saveAPConfig'],
                ['advanced-form', 'saveAdvancedConfig'],
                ['ethernet-form', 'saveEthernetConfig'],
                ['cellular-form', 'saveCellularConfig']
            ].forEach(([id, method]) => {
                const form = document.getElementById(id);
                if (form) {
                    form.addEventListener('submit', (e) => {
                        e.preventDefault();
                        this[method]();
                    });
                }
            });

            // DHCP/静态IP切换
            const wifiDhcp = document.getElementById('wifi-dhcp');
            if (wifiDhcp) {
                wifiDhcp.addEventListener('change', (e) => {
                    this._toggleStaticIPFields(e.target.value === '1');
                });
            }

            // 自定义域名输入时更新预览
            const customDomain = document.getElementById('custom-domain');
            if (customDomain) {
                customDomain.addEventListener('input', () => {
                    this._updateMDNSUrlPreview();
                });
            }

            this.setupStatusRefresh();
            this.setupBleProvision();
            this._filterNetworkTypesByCapabilities();
        },

        _filterNetworkTypesByCapabilities() {
            var getter = (typeof apiGetSilentFresh === 'function') ? apiGetSilentFresh
                : (typeof apiGetFresh === 'function' ? apiGetFresh : (typeof apiGet === 'function' ? apiGet : null));
            if (!getter) return;
            getter('/api/system/capabilities').then(function(res) {
                if (!res || !res.success || !res.data) return;
                var select = document.getElementById('network-type');
                if (!select) return;
                if (!res.data.ethernet) {
                    var ethernet = select.querySelector('option[value="1"]');
                    if (ethernet) ethernet.remove();
                }
                if (!res.data.cellular) {
                    var cellular = select.querySelector('option[value="2"]');
                    if (cellular) cellular.remove();
                }
            }).catch(function() { /* Keep the visible options when capability probing fails. */ });
        },

        /**
         * 联网方式切换处理
         */
        _onNetworkTypeChange(value) {
            const panels = ['wifi-panel', 'ethernet-panel', 'cellular-panel'];
            panels.forEach(id => {
                const el = document.getElementById(id);
                if (el) this.hideElement(el);
            });

            const panelMap = { '0': 'wifi-panel', '1': 'ethernet-panel', '2': 'cellular-panel' };
            const target = document.getElementById(panelMap[value] || 'wifi-panel');
            if (target) this.showElement(target, 'block');

            // WiFi扫描按钮仅在WiFi模式下显示
            const wifiScanGroup = document.getElementById('wifi-scan-group');
            if (wifiScanGroup) {
                if (value === '0') {
                    this.showElement(wifiScanGroup, 'block');
                } else {
                    this.hideElement(wifiScanGroup);
                }
            }

            // IP配置部分：仅WiFi和以太网需要DHCP/静态IP配置
            const ipConfigSection = document.querySelector('#advanced-form > .fb-section-title');
            if (ipConfigSection) {
                const ipGrid = ipConfigSection.nextElementSibling;
                const showIpConfig = (value === '0' || value === '1');
                ipConfigSection.style.display = showIpConfig ? '' : 'none';
                if (ipGrid && ipGrid.classList.contains('config-form-grid')) {
                    ipGrid.style.display = showIpConfig ? '' : 'none';
                }
            }

            // 域名配置部分：4G模式下隐藏，WiFi/以太网通显示
            const domainSection = document.getElementById('domain-config-section');
            const domainUnavailable = document.getElementById('domain-unavailable-notice');
            const isDomainSupported = (value === '0' || value === '1');
            if (domainSection) {
                // 域名配置标题和表单网格（前两个子元素：h4 + grid）
                const domainTitle = domainSection.querySelector('.fb-section-title-mt');
                const domainGrid = domainSection.querySelector('.config-form-grid');
                if (isDomainSupported) {
                    // WiFi/以太网：显示域名配置表单，隐藏不可用提示
                    if (domainTitle) domainTitle.style.display = '';
                    if (domainGrid) domainGrid.style.display = '';
                    if (domainUnavailable) this.hideElement(domainUnavailable);
                } else {
                    // 4G：隐藏域名配置表单，显示不可用提示
                    if (domainTitle) domainTitle.style.display = 'none';
                    if (domainGrid) domainGrid.style.display = 'none';
                    if (domainUnavailable) this.showElement(domainUnavailable, 'block');
                }
            }

            // 连接设置部分：仅WiFi模式需要超时/重连配置
            const connSettings = document.querySelectorAll('#advance .fb-section-title-mt');
            // 域名配置在 #domain-config-section 中，连接设置在高级配置表单中。
            const connTitles = Array.from(connSettings).filter(el => !el.closest('#domain-config-section'));
            if (connTitles.length >= 1) {
                const connTitle = connTitles[0];
                const connGrid = connTitle.nextElementSibling;
                const showConnConfig = (value === '0');
                if (connTitle) connTitle.style.display = showConnConfig ? '' : 'none';
                if (connGrid && connGrid.classList.contains('config-form-grid')) {
                    connGrid.style.display = showConnConfig ? '' : 'none';
                }
            }

            const saveBtn = document.getElementById('network-type-save-btn');
            if (saveBtn) saveBtn.disabled = false;
            if (this._networkModeChangeTimer) clearTimeout(this._networkModeChangeTimer);
            this._networkModeChangeTimer = setTimeout(() => {
                this._networkModeChangeTimer = null;
                if (this.currentPage !== 'network') return;
                if (value === '2' || value === '1') {
                    // 4G或以太网：启动状态轮询（4G最长4分钟，以太网1分钟）
                    var pollDuration = (value === '2') ? 240000 : 60000;
                    this._startNetworkStatusPolling(pollDuration, null, null, null, 5000);
                } else {
                    this.loadNetworkStatus();
                }
            }, 300);
        },

        _renderWifiNote(text) {
            return String(text == null ? '' : text);
        },

        _setWifiModeNotice(message) {
            const noticeEl = document.getElementById('wifi-mode-notice');
            const noticeTextEl = document.getElementById('wifi-mode-notice-text');
            if (!noticeEl || !noticeTextEl) return;
            noticeTextEl.textContent = message || '';
            if (message) {
                this.showElement(noticeEl, 'block');
            } else {
                this.hideElement(noticeEl);
            }
        },

        _renderWifiScanState(message, detail) {
            let html = '<div class="wifi-scan-state is-error">' +
                '<div class="wifi-scan-state-text">' + this._escapeHtml(message) + '</div>';
            if (detail) {
                html += '<div class="wifi-scan-state-detail">' + this._escapeHtml(detail) + '</div>';
            }
            html += '</div>';
            return html;
        },

        _getWifiLockIcon(isEncrypted) {
            return isEncrypted
                ? '<span class="wifi-lock-icon-secure">🔒</span>'
                : '<span class="wifi-lock-icon-open">🔓</span>';
        },

        // SSID 可能含引号/尖括号，插入 HTML/属性前必须转义
        _escapeHtml(s) {
            return String(s == null ? '' : s)
                .replace(/&/g, '&amp;')
                .replace(/</g, '&lt;')
                .replace(/>/g, '&gt;')
                .replace(/"/g, '&quot;')
                .replace(/'/g, '&#39;');
        },

        _ensureWifiModalEvents(modal) {
            if (!modal || modal.dataset.bound === 'true') return;
            const closeBtn = document.getElementById('close-wifi-modal');
            if (closeBtn) {
                closeBtn.addEventListener('click', () => this.hideModal(modal));
            }
            modal.addEventListener('click', (e) => {
                if (e.target === modal) this.hideModal(modal);
            });
            modal.dataset.bound = 'true';
        },

        // ============ 网络配置 ============

        /**
         * 加载网络配置
         */
        loadNetworkConfig() {
            const seq = this._networkConfigSeq = (this._networkConfigSeq || 0) + 1;
            this._networkConfigLoaded = false;
            this._networkLoadState('网络配置加载中…', false);
            const getter = typeof apiGetFresh === 'function' ? apiGetFresh : apiGet;
            return getter('/api/network/config')
                .then(res => {
                    if (seq !== this._networkConfigSeq || this.currentPage !== 'network') return;
                    if (!res || !res.success || !res.data) throw new Error('加载网络配置失败');

                    const data = res.data || {};
                    const device = data.device || {};
                    const network = data.network || {};
                    const sta = data.sta || {};
                    const ap = data.ap || {};
                    const advanced = data.advanced || {};

                    // ========== 联网方式 ==========
                    const networkType = network.networkType !== undefined ? network.networkType.toString() : '0';
                    this._setValue('network-type', networkType);
                    this._onNetworkTypeChange(networkType);

                    // ========== 基本配置 ==========
                    this._setValue('wifi-mode', network.mode !== undefined ? network.mode.toString() : '2');

                    // STA 配置
                    this._setValue('wifi-ssid', sta.ssid || '');
                    this._setValue('wifi-password', sta.password || '');
                    this._setValue('wifi-security', sta.security || 'wpa2');

                    // ========== 热点配置 ==========
                    this._setValue('ap-ssid', ap.ssid || '');
                    this._setValue('ap-password', ap.password || '');
                    this._setValue('ap-ip', ap.ip || '192.168.4.1');
                    this._setValue('ap-channel', ap.channel !== undefined ? ap.channel.toString() : '1');
                    this._setValue('ap-hidden', ap.hidden ? '1' : '0');
                    this._setValue('ap-max-connections', ap.maxConnections !== undefined ? ap.maxConnections.toString() : '4');

                    // ========== 高级配置 ==========
                    const ipConfigType = network.ipConfigType !== undefined ? network.ipConfigType : 0;
                    this._setValue('wifi-dhcp', ipConfigType.toString());
                    this._toggleStaticIPFields(ipConfigType === 1);

                    // 静态IP配置
                    this._setValue('static-ip', sta.staticIP || '');
                    this._setValue('gateway', sta.gateway || '');
                    this._setValue('subnet', sta.subnet || '');
                    this._setValue('dns1', sta.dns1 || '');
                    this._setValue('dns2', sta.dns2 || '');

                    // 域名配置
                    this._setValue('custom-domain', network.customDomain || '');
                    this._updateMDNSUrlPreview();

                    // 连接设置
                    this._setValue('connect-timeout', advanced.connectTimeout !== undefined ? advanced.connectTimeout.toString() : '10000');
                    this._setValue('reconnect-interval', advanced.reconnectInterval !== undefined ? advanced.reconnectInterval.toString() : '5000');
                    this._setValue('max-reconnect-attempts', advanced.maxReconnectAttempts !== undefined ? advanced.maxReconnectAttempts.toString() : '5');
                    this._setValue('conflict-detection', advanced.conflictDetection !== undefined ? advanced.conflictDetection.toString() : '3');

                    // ========== 以太网配置 ==========
                    const eth = data.ethernet || {};
                    this._setValue('eth-mosi', eth.spiMosi !== undefined ? eth.spiMosi.toString() : '11');
                    this._setValue('eth-miso', eth.spiMiso !== undefined ? eth.spiMiso.toString() : '13');
                    this._setValue('eth-sck', eth.spiSck !== undefined ? eth.spiSck.toString() : '12');
                    this._setValue('eth-cs', eth.csPin !== undefined ? eth.csPin.toString() : '47');
                    this._setValue('eth-rst', eth.rstPin !== undefined ? eth.rstPin.toString() : '48');
                    this._setValue('eth-int', eth.intPin !== undefined ? eth.intPin.toString() : '14');

                    // ========== 4G 配置 ==========
                    const cell = data.cellular || {};
                    this._setValue('cell-tx', cell.txPin !== undefined ? cell.txPin.toString() : '39');
                    this._setValue('cell-rx', cell.rxPin !== undefined ? cell.rxPin.toString() : '40');
                    this._setValue('cell-pwr', cell.pwrPin !== undefined ? cell.pwrPin.toString() : '38');
                    this._setValue('cell-baud', cell.baudRate !== undefined ? cell.baudRate.toString() : '115200');
                    this._setValue('cell-apn', cell.apn || 'CMNET');
                    this._networkConfigLoaded = true;
                    this._networkLoadState('网络配置已加载', true);

                })
                .catch(err => {
                    if (seq !== this._networkConfigSeq || this.currentPage !== 'network') return;
                    this._networkLoadState('网络配置加载失败，请重新加载', false, true);
                    // 页面切换导致的请求中止属预期行为，静默返回避免误报
                    if (typeof window.isPageAbortError === 'function' && window.isPageAbortError(err)) return;
                    if (typeof window.apiNotifyError === 'function') {
                        window.apiNotifyError(err, '加载网络配置失败', '网络设置');
                    } else {
                        Notification.error('加载网络配置失败', '网络设置');
                    }
                });
        },

        /**
         * 切换静态IP字段显示/隐藏
         */
        _networkLoadState(message, ready, retry) {
            const root = document.getElementById('network-page');
            if (!root) return;
            root.querySelectorAll('form button[type="submit"], #network-type-save-btn').forEach(button => { button.disabled = !ready; });
            let status = document.getElementById('network-config-load-status');
            if (!status) {
                status = document.createElement('div');
                status.id = 'network-config-load-status';
                status.className = 'u-hint-bar';
                status.setAttribute('role', 'status');
                root.prepend(status);
            }
            status.textContent = message;
            if (retry) {
                const button = document.createElement('button');
                button.type = 'button'; button.className = 'fb-btn fb-btn-sm';
                button.textContent = '重新加载';
                button.addEventListener('click', () => this.loadNetworkConfig());
                status.appendChild(button);
            }
        },

        _networkCanSave(formId) {
            const form = document.getElementById(formId);
            return this._networkConfigLoaded === true && this.currentPage === 'network' &&
                form && !form.querySelector('button[type="submit"]')?.disabled;
        },

        _toggleStaticIPFields(show) {
            const fields = ['static-ip', 'gateway', 'subnet', 'dns1', 'dns2'];
            fields.forEach(id => {
                const el = document.getElementById(id);
                if (el) {
                    const group = el.closest('.fb-form-group');
                    if (group) {
                        if (show) {
                            this.showElement(group, 'block');
                        } else {
                            this.hideElement(group);
                        }
                    }
                }
            });
        },

        /**
         * 保存基本网络配置
         */
        saveActiveNetworkConfig() {
            const networkType = document.getElementById('network-type')?.value || '0';
            switch (networkType) {
                case '1':
                    return this.saveEthernetConfig();
                case '2':
                    return this.saveCellularConfig();
                default:
                    return this.saveNetworkConfig();
            }
        },

        _refreshNetworkStatusSoon(delayMs) {
            if (this._networkRefreshTimer) clearTimeout(this._networkRefreshTimer);
            this._networkRefreshTimer = setTimeout(() => {
                this._networkRefreshTimer = null;
                if (this.currentPage === 'network') this.loadNetworkStatus();
            }, delayMs || 800);
        },

        _stopNetworkPageWork() {
            this._networkConfigSeq = (this._networkConfigSeq || 0) + 1;
            this._networkConfigLoaded = false;
            this._stopNetworkStatusPolling();
            [
                '_networkModeChangeTimer',
                '_networkRefreshTimer',
                '_networkSaveCountdownTimer',
                '_networkBleMessageTimer'
            ].forEach((key) => {
                if (this[key]) clearTimeout(this[key]);
                this[key] = null;
            });
            this._networkDeferredTimers.forEach(function(timer) { clearTimeout(timer); });
            this._networkDeferredTimers = [];
        },

        _scheduleNetworkPageTask(callback, delayMs) {
            var self = this;
            var timer = setTimeout(function() {
                self._networkDeferredTimers = self._networkDeferredTimers.filter(function(item) {
                    return item !== timer;
                });
                if (self.currentPage === 'network') callback();
            }, delayMs);
            this._networkDeferredTimers.push(timer);
            return timer;
        },

        /**
         * 停止网络状态轮询
         */
        _stopNetworkStatusPolling() {
            if (this._networkStatusPollingTimer) {
                clearInterval(this._networkStatusPollingTimer);
                this._networkStatusPollingTimer = null;
            }
            if (this._networkStatusPollingCountdown) {
                clearInterval(this._networkStatusPollingCountdown);
                this._networkStatusPollingCountdown = null;
            }
            this._networkStatusPollingRemaining = 0;
        },

        /**
         * 启动网络状态自动轮询（4G/以太网模式保存配置后使用）
         * @param {number} durationMs - 轮询总持续时间（毫秒），默认 240000 (4分钟)
         * @param {HTMLButtonElement} btn - 保存按钮元素
         * @param {HTMLElement} btnTextEl - 按钮文本元素
         * @param {string} originalText - 按钮原始文本
         * @param {number} intervalMs - 轮询间隔（毫秒），默认 5000
         */
        _startNetworkStatusPolling(durationMs, btn, btnTextEl, originalText, intervalMs) {
            this._stopNetworkStatusPolling();
            durationMs = durationMs || 240000;
            intervalMs = intervalMs || 5000;
            var self = this;
            var elapsed = 0;
            var totalSec = Math.ceil(durationMs / 1000);
            this._networkStatusPollingRemaining = totalSec;

            // 倒计时：每秒更新按钮文字
            this._networkStatusPollingCountdown = setInterval(function() {
                self._networkStatusPollingRemaining--;
                var remaining = self._networkStatusPollingRemaining;
                if (remaining <= 0) {
                    self._stopNetworkStatusPolling();
                    if (btn) btn.disabled = false;
                    if (btnTextEl) btnTextEl.textContent = originalText;
                } else {
                    if (btnTextEl) btnTextEl.textContent = '网络连接中... (' + remaining + 's)';
                }
            }, 1000);

            // 轮询：每 intervalMs 毫秒刷新一次网络状态
            this._networkStatusPollingTimer = setInterval(function() {
                elapsed += intervalMs;
                self.loadNetworkStatus();
                // 检查状态面板是否已显示"已连接"
                var connected = self._isAnyStatusPanelConnected();
                if (connected || elapsed >= durationMs) {
                    self._stopNetworkStatusPolling();
                    if (btn) btn.disabled = false;
                    if (btnTextEl) btnTextEl.textContent = originalText;
                    if (connected) {
                        Notification.success('网络连接成功！', '网络设置');
                    }
                }
            }, intervalMs);

            // 首次立即刷新
            this._refreshNetworkStatusSoon(1000);
        },

        /**
         * 检测当前是否有任何状态面板显示"已连接"
         */
        _isAnyStatusPanelConnected() {
            var badges = ['wifi-status-badge', 'eth-status-badge', 'cell-status-badge'];
            for (var i = 0; i < badges.length; i++) {
                var el = document.getElementById(badges[i]);
                if (el && el.classList.contains('status-connected')) return true;
            }
            return false;
        },

        /**
         * 保存按钮倒计时禁用（网络重启后自动恢复）
         */
        _startSaveBtnCountdown(btn, btnTextEl, originalText, seconds, onTick) {
            if (this._networkSaveCountdownTimer) {
                clearInterval(this._networkSaveCountdownTimer);
                this._networkSaveCountdownTimer = null;
            }
            let countdown = seconds;
            this._networkSaveCountdownTimer = setInterval(() => {
                countdown--;
                if (countdown <= 0) {
                    clearInterval(this._networkSaveCountdownTimer);
                    this._networkSaveCountdownTimer = null;
                    if (btn) btn.disabled = false;
                    if (btnTextEl) btnTextEl.textContent = originalText;
                    if (typeof onTick === 'function') onTick(0);
                } else {
                    if (btnTextEl) btnTextEl.textContent = '网络模式切换中... (' + countdown + 's)';
                    if (typeof onTick === 'function') onTick(countdown);
                }
            }, 1000);
            this._refreshNetworkStatusSoon(3000);
        },

        saveNetworkConfig() {
            if (!this._networkCanSave('wifi-form')) return;
            const saveSeq = this._networkConfigSeq;
            const config = {
                networkType: document.getElementById('network-type')?.value || '0',
                mode: document.getElementById('wifi-mode')?.value || '2',
                staSSID: document.getElementById('wifi-ssid')?.value || '',
                staPassword: document.getElementById('wifi-password')?.value || '',
                staSecurity: document.getElementById('wifi-security')?.value || 'wpa'
            };

            const submitBtn = document.getElementById('wifi-save-btn');
            const submitBtnText = document.getElementById('wifi-save-btn-text');
            const originalText = submitBtnText?.textContent || '保存配置';

            if (submitBtn && submitBtnText) {
                submitBtn.disabled = true;
                submitBtnText.textContent = '网络模式切换中...';
            }

            this._showMessage('wifi-success', false);
            this._showMessage('wifi-error', false);

            const noticeTextEl = document.getElementById('wifi-mode-notice-text');
            this._setWifiModeNotice('');

            return apiPut('/api/network/config', config)
                .then(res => {
                    if (saveSeq !== this._networkConfigSeq || this.currentPage !== 'network') return;
                    if (res && res.success) {
                        this._showMessage('wifi-success', true);

                        const data = res.data || {};
                        let message = '网络设置保存成功！';
                        let noticeMessage = '网络模式切换提醒：';

                        if (data.restartRequired) {
                            message += '\n' + this._renderWifiNote('网络配置变更需要重启网络服务才能生效，请等待约10秒...');
                        }

                        const mode = data.mode;
                        const modeText = data.modeText || '';

                        if (mode === 0 || modeText === 'STA') {
                            if (data.mdnsDomain) {
                                const hint = '可通过域名 http://' + data.mdnsDomain + ' 访问Web服务';
                                message += '\n' + this._renderWifiNote(hint);
                                noticeMessage += '切换到STA模式后，可通过 http://' + data.mdnsDomain + ' 访问';
                            }
                        } else if (mode === 1 || modeText === 'AP') {
                            const hint = '请先连接热点 [' + (data.apSSID || 'fastbee-ap') + ']，然后访问 http://' + (data.apIP || '192.168.4.1') + ' 或 http://fastbee.local';
                            message += '\n' + this._renderWifiNote(hint);
                            noticeMessage += '切换到AP模式后，请连接热点 [' + (data.apSSID || 'fastbee-ap') + '] 访问 http://' + (data.apIP || '192.168.4.1') + ' 或 http://fastbee.local';
                        }

                        message += '\n' + this._renderWifiNote('如无法访问，请刷新页面或重新连接网络。');
                        this._setWifiModeNotice(noticeMessage);

                        Notification.show({
                            type: 'success',
                            title: '网络配置已保存',
                            message: message,
                            duration: 8000
                        });

                        this._startSaveBtnCountdown(
                            submitBtn,
                            submitBtnText,
                            originalText,
                            15,
                            (countdown) => {
                                if (!noticeTextEl) return;
                                noticeTextEl.textContent = countdown > 0
                                    ? noticeMessage + ' 按钮将在 ' + countdown + ' 秒后恢复可用。'
                                    : noticeMessage;
                            }
                        );

                    } else {
                        this._showMessage('wifi-error', true);
                        Notification.error(res?.error || '保存失败', '网络设置');
                        if (submitBtn && submitBtnText) {
                            submitBtn.disabled = false;
                            submitBtnText.textContent = originalText;
                        }
                    }
                })
                .catch(err => {
                    if (saveSeq !== this._networkConfigSeq || this.currentPage !== 'network') return;
                    console.error('Save network config failed:', err);
                    // 区分超时（请求可能未到达）vs 连接断开（设备主动重启网络）
                    const isTimeout = err && err.name === 'AbortError';
                    const isConnectionClosed = err && (
                        err.name === 'TypeError' ||
                        (err.message && (
                            err.message.includes('fetch') ||
                            err.message.includes('network') ||
                            err.message.includes('Failed to fetch')
                        ))
                    );

                    if (isTimeout) {
                        // 超时：请求可能未到达设备，不能报成功
                        Notification.show({
                            type: 'warning',
                            title: '保存超时',
                            message: '保存请求超时，未确认是否生效，请刷新页面核对当前配置',
                            duration: 8000
                        });
                        if (submitBtn && submitBtnText) {
                            submitBtn.disabled = false;
                            submitBtnText.textContent = originalText;
                        }
                    } else if (isConnectionClosed) {
                        // 连接被设备断开：网络配置已保存，设备正在重启网络服务
                        this._showMessage('wifi-success', true);
                        Notification.show({
                            type: 'success',
                            title: '网络配置已保存',
                            message: '网络设置保存成功！\n' + this._renderWifiNote('网络配置变更需要重启网络服务才能生效，请等待约10秒...'),
                            duration: 8000
                        });
                    } else {
                        this._showMessage('wifi-error', true);
                        Notification.error('保存失败', '网络设置');
                        if (submitBtn && submitBtnText) {
                            submitBtn.disabled = false;
                            submitBtnText.textContent = originalText;
                        }
                    }
                });
        },

        /**
         * 保存热点配置
         */
        saveAPConfig() {
            if (!this._networkCanSave('ap-form')) return;
            const saveSeq = this._networkConfigSeq;
            const config = {
                apSSID: document.getElementById('ap-ssid')?.value || '',
                apPassword: document.getElementById('ap-password')?.value || '',
                apIP: document.getElementById('ap-ip')?.value || '192.168.4.1',
                apChannel: document.getElementById('ap-channel')?.value || '1',
                apHidden: document.getElementById('ap-hidden')?.value || '0',
                apMaxConnections: document.getElementById('ap-max-connections')?.value || '4'
            };

            const submitBtn = document.querySelector('#ap-form button[type="submit"]');
            const originalText = submitBtn?.textContent || '保存配置';
            if (submitBtn) {
                submitBtn.disabled = true;
                submitBtn.textContent = '… 保存中...';
            }

            return apiPut('/api/network/config', config)
                .then(res => {
                    if (saveSeq !== this._networkConfigSeq || this.currentPage !== 'network') return;
                    if (res && res.success) {
                        this._showMessage('ap-success', true);
                        Notification.success('热点配置保存成功！', '网络设置');
                    } else {
                        Notification.error(res?.error || '保存失败', '网络设置');
                    }
                })
                .catch(err => {
                    if (saveSeq !== this._networkConfigSeq || this.currentPage !== 'network') return;
                    console.error('Save AP config failed:', err);
                    Notification.error('保存失败', '网络设置');
                })
                .finally(() => {
                    if (submitBtn) {
                        submitBtn.disabled = false;
                        submitBtn.textContent = originalText;
                    }
                });
        },

        /**
         * 更新 mDNS URL 预览文本
         */
        _updateMDNSUrlPreview() {
            const domain = document.getElementById('custom-domain')?.value || 'fastbee';
            const preview = document.getElementById('mdns-url-preview');
            if (preview) {
                preview.textContent = (domain || 'fastbee') + '.local';
            }
        },

        /**
         * 保存高级配置
         */
        saveAdvancedConfig() {
            if (!this._networkCanSave('advanced-form')) return;
            const saveSeq = this._networkConfigSeq;
            const config = {
                ipConfigType: document.getElementById('wifi-dhcp')?.value || '0',
                staticIP: document.getElementById('static-ip')?.value || '',
                gateway: document.getElementById('gateway')?.value || '',
                subnet: document.getElementById('subnet')?.value || '',
                dns1: document.getElementById('dns1')?.value || '',
                dns2: document.getElementById('dns2')?.value || '',
                customDomain: document.getElementById('custom-domain')?.value || '',
                connectTimeout: document.getElementById('connect-timeout')?.value || '10000',
                reconnectInterval: document.getElementById('reconnect-interval')?.value || '5000',
                maxReconnectAttempts: document.getElementById('max-reconnect-attempts')?.value || '5',
                conflictDetection: document.getElementById('conflict-detection')?.value || '3'
            };

            const submitBtn = document.querySelector('#advanced-form button[type="submit"]');
            const originalText = submitBtn?.textContent || '保存配置';
            if (submitBtn) {
                submitBtn.disabled = true;
                submitBtn.textContent = '… 保存中...';
            }

            return apiPut('/api/network/config', config)
                .then(res => {
                    if (saveSeq !== this._networkConfigSeq || this.currentPage !== 'network') return;
                    if (res && res.success) {
                        this._showMessage('advanced-success', true);

                        const data = res.data || {};
                        let message = '高级配置保存成功！';

                        if (data.restartRequired) {
                            message += '\n配置变更需要重启网络服务才能生效，请等待约10秒...';
                        }

                        Notification.show({
                            type: 'success',
                            title: '高级配置已保存',
                            message: message,
                            duration: 8000
                        });
                    } else {
                        Notification.error(res?.error || '保存失败', '网络设置');
                    }
                })
                .catch(err => {
                    if (saveSeq !== this._networkConfigSeq || this.currentPage !== 'network') return;
                    console.error('Save advanced config failed:', err);
                    Notification.error('保存失败', '网络设置');
                })
                .finally(() => {
                    if (submitBtn) {
                        submitBtn.disabled = false;
                        submitBtn.textContent = originalText;
                    }
                });
        },

        /**
         * 保存以太网配置
         */
        saveEthernetConfig() {
            if (!this._networkCanSave('ethernet-form')) return;
            const saveSeq = this._networkConfigSeq;
            const config = {
                networkType: '1',
                ethernet: {
                    spiMosi: parseInt(document.getElementById('eth-mosi')?.value || '11'),
                    spiMiso: parseInt(document.getElementById('eth-miso')?.value || '13'),
                    spiSck: parseInt(document.getElementById('eth-sck')?.value || '12'),
                    csPin: parseInt(document.getElementById('eth-cs')?.value || '47'),
                    rstPin: parseInt(document.getElementById('eth-rst')?.value || '48'),
                    intPin: parseInt(document.getElementById('eth-int')?.value || '14')
                }
            };

            const submitBtn = document.getElementById('ethernet-save-btn');
            const submitBtnText = document.getElementById('ethernet-save-btn-text');
            const originalText = submitBtnText?.textContent || '保存配置';
            if (submitBtn) submitBtn.disabled = true;
            if (submitBtnText) submitBtnText.textContent = '网络模式切换中...';

            apiPut('/api/network/config', config)
                .then(res => {
                    if (saveSeq !== this._networkConfigSeq || this.currentPage !== 'network') return;
                    if (res && res.success) {
                        const message = '网络设置保存成功！'
                            + '\n网络配置变更需要重启网络服务才能生效，请等待约10秒...'
                            + '\n以太网连接成功后，可通过以太网 IP 或 http://fastbee.local 访问；'
                            + '如连接失败，AP 热点将始终可用：http://192.168.4.1 或 http://fastbee.local';
                        Notification.show({
                            type: 'success',
                            title: '网络配置已保存',
                            message: message,
                            duration: 12000
                        });
                        this._startNetworkStatusPolling(60000, submitBtn, submitBtnText, originalText);
                    } else {
                        Notification.error(res?.error || '保存失败', '网络设置');
                        if (submitBtn) submitBtn.disabled = false;
                        if (submitBtnText) submitBtnText.textContent = originalText;
                    }
                })
                .catch(() => {
                    if (saveSeq !== this._networkConfigSeq || this.currentPage !== 'network') return;
                    Notification.error('保存失败', '网络设置');
                    if (submitBtn) submitBtn.disabled = false;
                    if (submitBtnText) submitBtnText.textContent = originalText;
                });
        },

        /**
         * 保存4G配置
         */
        saveCellularConfig() {
            if (!this._networkCanSave('cellular-form')) return;
            const saveSeq = this._networkConfigSeq;
            const config = {
                networkType: '2',
                cellular: {
                    txPin: parseInt(document.getElementById('cell-tx')?.value || '39'),
                    rxPin: parseInt(document.getElementById('cell-rx')?.value || '40'),
                    pwrPin: parseInt(document.getElementById('cell-pwr')?.value || '38'),
                    baudRate: parseInt(document.getElementById('cell-baud')?.value || '115200'),
                    apn: document.getElementById('cell-apn')?.value || 'CMNET'
                }
            };

            const submitBtn = document.getElementById('cellular-save-btn');
            const submitBtnText = document.getElementById('cellular-save-btn-text');
            const originalText = submitBtnText?.textContent || '保存配置';
            if (submitBtn) submitBtn.disabled = true;
            if (submitBtnText) submitBtnText.textContent = '网络模式切换中...';

            apiPut('/api/network/config', config)
                .then(res => {
                    if (saveSeq !== this._networkConfigSeq || this.currentPage !== 'network') return;
                    if (res && res.success) {
                        const message = '网络设置保存成功！'
                            + '\n网络配置变更需要重启网络服务才能生效，请等待约15秒...'
                            + '\n4G 连接后，可通过设备 AP 热点 http://192.168.4.1 或 http://fastbee.local 访问本系统';
                        Notification.show({
                            type: 'success',
                            title: '网络配置已保存',
                            message: message,
                            duration: 12000
                        });
                        this._startNetworkStatusPolling(240000, submitBtn, submitBtnText, originalText);
                    } else {
                        Notification.error(res?.error || '保存失败', '网络设置');
                        if (submitBtn) submitBtn.disabled = false;
                        if (submitBtnText) submitBtnText.textContent = originalText;
                    }
                })
                .catch(() => {
                    if (saveSeq !== this._networkConfigSeq || this.currentPage !== 'network') return;
                    Notification.error('保存失败', '网络设置');
                    if (submitBtn) submitBtn.disabled = false;
                    if (submitBtnText) submitBtnText.textContent = originalText;
                });
        },

        /**
         * 扫描 WiFi 网络
         */
        async scanWifiNetworks() {
            const scanBtn = document.getElementById('wifi-scan-btn');
            const modal = await this._ensureModalForAction('wifi-modal');
            const modalBody = document.getElementById('wifi-modal-body');

            if (!modal || !modalBody) return;

            this.showModal(modal);
            this._ensureWifiModalEvents(modal);
            modalBody.innerHTML = '<div class="wifi-scan-state"><div class="wifi-scan-state-text">正在扫描...</div></div>';

            if (scanBtn) {
                scanBtn.disabled = true;
                scanBtn.innerHTML = '… 扫描中...';
            }

            apiGet('/api/wifi/scan')
                .then(res => {
                    if (!res || !res.success) {
                        if (res && res.error === 'scan_busy') {
                            modalBody.innerHTML = '<div class="wifi-scan-state is-warning"><div class="wifi-scan-state-text">扫描正在进行中，请稍后再试</div></div>';
                        } else {
                            modalBody.innerHTML = this._renderWifiScanState(
                                '扫描失败，请稍后重试',
                                res?.error || 'Unknown error'
                            );
                        }
                        return;
                    }

                    // 后端返回 {networks:[...], success, count}；兼容旧版 data 字段
                    const networks = res.networks || res.data || [];

                    if (networks.length === 0) {
                        modalBody.innerHTML = '<div class="wifi-scan-state"><div class="wifi-scan-state-text">未找到WiFi网络</div></div>';
                        return;
                    }

                    networks.sort((a, b) => b.rssi - a.rssi);

                    // 加密类型显示标签映射（后端返回 "open"/"wpa"/"wpa2"/"wpa3" 等字符串）
                    const encryptLabels = { open: '开放', wep: 'WEP', wpa: 'WPA', wpa2: 'WPA2', wpa3: 'WPA3' };

                    let html = '<div class="wifi-grid">';
                    networks.forEach((net) => {
                        const signalClass = net.rssi > -50 ? 'strong' : net.rssi > -70 ? 'medium' : 'weak';
                        // 优先用 encryption 字符串；旧固件仅有 encrypted 布尔时兜底推导
                        const encryption = net.encryption || (net.encrypted ? 'wpa2' : 'open');
                        const isOpen = encryption === 'open';
                        const encryptIcon = this._getWifiLockIcon(!isOpen);
                        const encryptionEsc = this._escapeHtml(encryption);
                        const encLabel = this._escapeHtml(encryptLabels[encryption] || encryption);
                        const ssidEsc = this._escapeHtml(net.ssid);
                        const rssiEsc = this._escapeHtml(net.rssi);

                        html += `
                            <div class="wifi-grid-item" data-ssid="${ssidEsc}" data-encryption="${encryptionEsc}">
                                <div class="wifi-info">
                                    <div class="wifi-ssid">${ssidEsc}</div>
                                    <div class="wifi-meta">
                                        ${encryptIcon} ${encLabel}
                                    </div>
                                </div>
                                <div class="wifi-signal ${signalClass}">
                                    ${rssiEsc} dBm
                                </div>
                            </div>
                        `;
                    });
                    html += '</div>';

                    modalBody.innerHTML = html;

                    modalBody.querySelectorAll('.wifi-grid-item').forEach(item => {
                        item.addEventListener('click', (e) => {
                            const ssid = e.currentTarget.dataset.ssid;
                            const encryption = e.currentTarget.dataset.encryption;

                            const ssidInput = document.getElementById('wifi-ssid');
                            if (ssidInput) ssidInput.value = ssid;

                            // 根据扫描结果自动设置安全类型（open→none，其他直接使用字符串）
                            const securitySelect = document.getElementById('wifi-security');
                            if (securitySelect) {
                                const secValue = encryption === 'open' ? 'none' : encryption;
                                securitySelect.value = secValue;
                                // 如果设置失败（无匹配选项），回退到默认 wpa2
                                if (securitySelect.value !== secValue) securitySelect.value = 'wpa2';
                            }

                            const passwordInput = document.getElementById('wifi-password');
                            if (passwordInput) passwordInput.value = '';

                            this.hideModal(modal);
                            Notification.success(`已选择: ${ssid}`, 'WiFi');
                        });
                    });
                })
                .catch(err => {
                    console.error('WiFi scan failed:', err);
                    modalBody.innerHTML = this._renderWifiScanState('扫描失败，请稍后重试');
                })
                .finally(() => {
                    if (scanBtn) {
                        scanBtn.disabled = false;
                        scanBtn.innerHTML = '选择Wifi网络';
                    }
                });
        },

        // ============ 网络状态显示 ============

        /**
         * 加载并显示网络状态
         */
        loadNetworkStatus(retryCount) {
            retryCount = retryCount || 0;
            var getter = (typeof apiGetFresh === 'function') ? apiGetFresh : apiGet;
            return getter('/api/network/status')
                .then(res => {
                    if (!res || !res.success || !res.data) {
                        // 响应无效，重试一次
                        if (retryCount < 2) {
                            this._scheduleNetworkPageTask(() => this.loadNetworkStatus(retryCount + 1), 2000);
                        } else {
                            this._loadNetworkStatusFallback();
                        }
                        return;
                    }
                    this._updateNetworkStatus(res.data);
                })
                .catch(err => {
                    // 页面切换导致的请求中止属预期行为，静默返回（不重试、不误报 DNS 失败）
                    if (typeof window.isPageAbortError === 'function' && window.isPageAbortError(err)) return;
                    console.error('Failed to load network status:', err);
                    // DNS解析失败（如4G模式关闭WiFi后mDNS不可用）无需重试
                    if (this._isDnsError(err)) {
                        console.warn('[Network] DNS resolution failed, device may be unreachable via current hostname');
                        this._showNetworkStatusFailure('无法读取设备网络状态，请检查当前访问地址');
                        return;
                    }
                    // 其他网络错误时重试（可能是内存压力导致503）
                    if (retryCount < 2) {
                        this._scheduleNetworkPageTask(() => this.loadNetworkStatus(retryCount + 1), 3000);
                    } else {
                        this._loadNetworkStatusFallback();
                    }
                });
        },

        /**
         * 检测是否为DNS解析失败
         * 4G/以太网模式下mDNS可能不可用，重试无意义
         */
        _isDnsError(err) {
            if (!err) return false;
            // 页面导航中止不是 DNS 失败，由 isPageAbortError 分支处理
            if (err._pageAborted || err.name === 'AbortError') return false;
            // TypeError: Failed to fetch 且无 status 字段，通常是DNS/网络层错误
            if (err instanceof TypeError && !err.status) return true;
            return false;
        },

        _loadNetworkStatusFallback() {
            const getter = (typeof apiGetSilentFresh === 'function')
                ? apiGetSilentFresh
                : (typeof apiGetFresh === 'function' ? apiGetFresh : apiGet);
            getter('/api/system/status')
                .then(res => {
                    if (!res || !res.success || !res.data) {
                        this._showNetworkStatusFailure('网络状态暂时不可用，请稍后刷新');
                        return;
                    }
                    const d = res.data || {};
                    this._updateNetworkStatus({
                        status: d.networkConnected ? 'connected' : 'disconnected',
                        mode: 'STA',
                        deviceNetworkType: 0,
                        ssid: d.ssid || '',
                        ipAddress: d.ipAddress || '',
                        rssi: d.rssi || 0,
                        signalStrength: d.rssi ? Math.max(0, Math.min(100, Math.round((Number(d.rssi) + 100) * 2))) : 0,
                        connectedTime: d.uptime ? Math.floor(Number(d.uptime) / 1000) : 0
                    });
                })
                .catch(err => {
                    console.warn('Network status fallback failed:', err);
                    if (typeof window.isPageAbortError === 'function' && window.isPageAbortError(err)) return;
                    this._showNetworkStatusFailure('网络状态请求失败，请检查设备连接');
                });
        },

        _setNetworkStatusLoading() {
            const panel = document.querySelector('#network-status-card .status-panel:not(.fb-hidden)')
                || document.getElementById('wifi-status-panel');
            if (!panel) return;
            const badge = panel.querySelector('.status-badge');
            if (badge) {
                badge.className = 'status-badge status-detecting';
                badge.textContent = '检测中';
                badge.removeAttribute('title');
            }
            panel.querySelectorAll('.status-value').forEach(function(el) {
                el.textContent = '';
                el.classList.add('fb-skeleton-text');
                el.setAttribute('aria-label', '检测中');
            });
        },

        _showNetworkStatusFailure(reason) {
            const panel = document.querySelector('#network-status-card .status-panel:not(.fb-hidden)')
                || document.getElementById('wifi-status-panel');
            if (!panel) return;
            const badge = panel.querySelector('.status-badge');
            if (badge) {
                badge.className = 'status-badge status-disconnected';
                badge.textContent = '状态获取失败';
                badge.title = reason || '';
            }
            panel.querySelectorAll('.status-value').forEach(function(el) {
                el.classList.remove('fb-skeleton-text');
                el.removeAttribute('aria-label');
                if (!el.textContent) el.textContent = '--';
            });
        },

        /**
         * 更新网络状态显示
         */
        _updateNetworkStatus(data) {
            // 优先使用API返回的设备实际联网类型，若无则回退到选择框值
            const networkType = document.getElementById('network-type');
            const typeValue = (data.deviceNetworkType !== undefined)
                ? data.deviceNetworkType.toString()
                : (networkType ? networkType.value : '0');

            // 隐藏所有状态面板
            ['wifi-status-panel', 'ethernet-status-panel', 'cellular-status-panel'].forEach(id => {
                const el = document.getElementById(id);
                if (el) el.classList.add('fb-hidden');
            });

            // 根据联网方式显示对应面板
            switch(typeValue) {
                case '0': // WiFi
                    this._updateWifiStatus(data);
                    document.getElementById('wifi-status-panel')?.classList.remove('fb-hidden');
                    break;
                case '1': // Ethernet
                    this._updateEthernetStatus(data);
                    document.getElementById('ethernet-status-panel')?.classList.remove('fb-hidden');
                    break;
                case '2': // 4G
                    this._updateCellularStatus(data);
                    document.getElementById('cellular-status-panel')?.classList.remove('fb-hidden');
                    break;
            }

            const activePanel = document.querySelector('#network-status-card .status-panel:not(.fb-hidden)');
            if (activePanel) {
                activePanel.querySelectorAll('.fb-skeleton-text').forEach(function(el) {
                    el.classList.remove('fb-skeleton-text');
                    el.removeAttribute('aria-label');
                });
                const badge = activePanel.querySelector('.status-badge');
                if (badge) badge.removeAttribute('title');
            }
        },

        /**
         * 更新WiFi状态显示
         */
        _updateWifiStatus(data) {
            const statusBadge = document.getElementById('wifi-status-badge');
            const status = data.status || ((data.connected || data.networkConnected || data.ipAddress) ? 'connected' : 'disconnected');
            if (statusBadge) {
                statusBadge.className = 'status-badge';
                switch(status) {
                    case 'connected':
                        statusBadge.classList.add('status-connected');
                        statusBadge.textContent = '已连接';
                        break;
                    case 'connecting':
                        statusBadge.classList.add('status-connecting');
                        statusBadge.textContent = '连接中';
                        break;
                    case 'ap_mode':
                        statusBadge.classList.add('status-ap-mode');
                        statusBadge.textContent = 'AP模式';
                        break;
                    default:
                        statusBadge.classList.add('status-disconnected');
                        statusBadge.textContent = '未连接';
                }
            }

            // 网络模式翻译：后端返回 "STA"/"AP" → 前端显示 "STA 模式"/"AP 模式"
            const modeTextMap = {
                'STA': 'STA 模式',
                'AP': 'AP 模式',
                'unknown': '--'
            };
            document.getElementById('wifi-mode-display').textContent = modeTextMap[data.mode] || data.mode || '--';
            document.getElementById('wifi-ssid-display').textContent = data.ssid || data.staSSID || '--';
            document.getElementById('wifi-ip-display').textContent = data.ipAddress || '--';
            
            // 信号强度
            if (data.rssi !== undefined && data.rssi !== 0) {
                const signalPercent = data.signalStrength || 0;
                document.getElementById('wifi-signal-display').textContent = `${data.rssi} dBm (${signalPercent}%)`;
            } else {
                document.getElementById('wifi-signal-display').textContent = '--';
            }

            // 连接时长
            if (data.connectedTime) {
                const seconds = parseInt(data.connectedTime);
                const hours = Math.floor(seconds / 3600);
                const minutes = Math.floor((seconds % 3600) / 60);
                const secs = seconds % 60;
                document.getElementById('wifi-uptime-display').textContent = 
                    `${hours.toString().padStart(2, '0')}:${minutes.toString().padStart(2, '0')}:${secs.toString().padStart(2, '0')}`;
            } else {
                document.getElementById('wifi-uptime-display').textContent = '--';
            }
        },

        /**
         * 更新以太网状态显示
         */
        _updateEthernetStatus(data) {
            const statusBadge = document.getElementById('eth-status-badge');
            if (statusBadge) {
                statusBadge.className = 'status-badge';
                if (data.status === 'connected') {
                    statusBadge.classList.add('status-connected');
                    statusBadge.textContent = '已连接';
                } else {
                    statusBadge.classList.add('status-disconnected');
                    statusBadge.textContent = '未连接';
                }
            }

            document.getElementById('eth-ip-display').textContent = data.ipAddress || '--';
            document.getElementById('eth-mac-display').textContent = data.macAddress || '--';
            document.getElementById('eth-gateway-display').textContent = data.gateway || '--';
            
            if (data.connectedTime) {
                const seconds = parseInt(data.connectedTime);
                const hours = Math.floor(seconds / 3600);
                const minutes = Math.floor((seconds % 3600) / 60);
                const secs = seconds % 60;
                document.getElementById('eth-uptime-display').textContent = 
                    `${hours.toString().padStart(2, '0')}:${minutes.toString().padStart(2, '0')}:${secs.toString().padStart(2, '0')}`;
            } else {
                document.getElementById('eth-uptime-display').textContent = '--';
            }
            
            // 配置热点信息（混合模式）
            document.getElementById('eth-ap-ssid-display').textContent = data.apSSID || '--';
            document.getElementById('eth-ap-ip-display').textContent = data.apIPAddress || '--';
        },

        /**
         * 更新4G蜂窝状态显示
         */
        _updateCellularStatus(data) {
            const statusBadge = document.getElementById('cell-status-badge');
            if (statusBadge) {
                statusBadge.className = 'status-badge';
                if (data.status === 'connected') {
                    statusBadge.classList.add('status-connected');
                    statusBadge.textContent = '已连接';
                } else {
                    statusBadge.classList.add('status-disconnected');
                    statusBadge.textContent = '未连接';
                }
            }

            // SIM卡状态（后端需要添加此字段）
            document.getElementById('cell-sim-status').textContent = data.simStatus || (data.status === 'connected' ? '就绪' : '--');
            document.getElementById('cell-operator').textContent = data.operator || '--';
            
            // 信号强度
            if (data.rssi !== undefined && data.rssi !== 0) {
                const signalPercent = data.signalStrength || 0;
                document.getElementById('cell-signal').textContent = `${data.rssi} dBm (${signalPercent}%)`;
            } else {
                document.getElementById('cell-signal').textContent = '--';
            }

            document.getElementById('cell-ip-display').textContent = data.ipAddress || '--';
            document.getElementById('cell-apn-display').textContent = data.apn || '--';
            document.getElementById('cell-network-type').textContent = data.networkType || '--';
            
            if (data.connectedTime) {
                const seconds = parseInt(data.connectedTime);
                const hours = Math.floor(seconds / 3600);
                const minutes = Math.floor((seconds % 3600) / 60);
                const secs = seconds % 60;
                document.getElementById('cell-uptime-display').textContent = 
                    `${hours.toString().padStart(2, '0')}:${minutes.toString().padStart(2, '0')}:${secs.toString().padStart(2, '0')}`;
            } else {
                document.getElementById('cell-uptime-display').textContent = '--';
            }

            // ICCID (SIM卡号)
            document.getElementById('cell-iccid-display').textContent = data.iccId || data.iccid || '--';
            
            // IMEI (设备号)
            document.getElementById('cell-imei-display').textContent = data.imei || '--';
            
            // 配置热点信息（混合模式）
            document.getElementById('cell-ap-ssid-display').textContent = data.apSSID || '--';
            document.getElementById('cell-ap-ip-display').textContent = data.apIPAddress || '--';
        },

        /**
         * 初始化网络状态刷新按钮
         */
        setupStatusRefresh() {
            const refreshBtn = document.getElementById('refresh-status-btn');
            if (refreshBtn) {
                refreshBtn.addEventListener('click', () => {
                    refreshBtn.disabled = true;
                    this._setNetworkStatusLoading();
                    this.loadNetworkStatus();
                    this._scheduleNetworkPageTask(() => {
                        refreshBtn.disabled = false;
                    }, 1500);
                });
            }
        },

        // ============ 蓝牙配网（BLE Provisioning）============

        /**
         * 绑定蓝牙配网面板事件（页面加载时调用一次）
         */
        setupBleProvision() {
            const startBtn = document.getElementById('ble-prov-start-btn');
            if (startBtn) startBtn.addEventListener('click', () => this.startBleProvision());

            const stopBtn = document.getElementById('ble-prov-stop-btn');
            if (stopBtn) stopBtn.addEventListener('click', () => this.stopBleProvision());

            const refreshBtn = document.getElementById('ble-prov-refresh-btn');
            if (refreshBtn) {
                refreshBtn.addEventListener('click', () => {
                    refreshBtn.disabled = true;
                    this.loadBleProvisionStatus();
                    this._scheduleNetworkPageTask(() => { refreshBtn.disabled = false; }, 1500);
                });
            }

            // 首次进入页面时探测固件是否支持 BLE 配网（不支持则隐藏面板）
            this.loadBleProvisionStatus();
        },

        /**
         * 加载蓝牙配网状态（不支持时隐藏面板）
         */
        loadBleProvisionStatus() {
            const card = document.getElementById('ble-provision-card');
            if (!card) return;

            apiGet('/api/ble/provision/status')
                .then(res => {
                    if (!res || !res.success || !res.data || !res.data.supported) {
                        this.hideElement(card);
                        return;
                    }
                    this.showElement(card, 'block');
                    this._updateBleProvisionUI(res.data);
                })
                .catch(() => this.hideElement(card));
        },

        /**
         * 启动蓝牙配网广播
         */
        startBleProvision() {
            const btn = document.getElementById('ble-prov-start-btn');
            if (btn) btn.disabled = true;

            apiPost('/api/ble/provision/start')
                .then(res => {
                    this._showBleProvisionMessage(res && res.success, res && res.message);
                    this.loadBleProvisionStatus();
                })
                .catch(() => this._showBleProvisionMessage(false))
                .finally(() => { if (btn) btn.disabled = false; });
        },

        /**
         * 停止蓝牙配网并释放内存
         */
        stopBleProvision() {
            const btn = document.getElementById('ble-prov-stop-btn');
            if (btn) btn.disabled = true;

            apiPost('/api/ble/provision/stop')
                .then(res => {
                    this._showBleProvisionMessage(res && res.success, res && res.message);
                    this.loadBleProvisionStatus();
                })
                .catch(() => this._showBleProvisionMessage(false))
                .finally(() => { if (btn) btn.disabled = false; });
        },

        /**
         * 根据状态数据刷新蓝牙配网面板 UI
         */
        _updateBleProvisionUI(data) {
            const badge = document.getElementById('ble-prov-state-badge');
            if (badge) {
                // 状态 → 徽章样式 + i18n 键
                const stateMap = {
                    'idle':        { cls: 'status-disconnected', key: 'ble-prov-state-idle' },
                    'advertising': { cls: 'status-ap-mode',      key: 'ble-prov-state-advertising' },
                    'connected':   { cls: 'status-connected',    key: 'ble-prov-state-connected' },
                    'provisioned': { cls: 'status-connected',    key: 'ble-prov-state-provisioned' }
                };
                const info = stateMap[data.state] || stateMap['idle'];
                badge.className = 'status-badge ' + info.cls;
                badge.setAttribute('data-i18n', info.key);
                badge.textContent = (typeof i18n !== 'undefined' && i18n.t) ? i18n.t(info.key) : badge.textContent;
            }

            const setText = (id, text) => {
                const el = document.getElementById(id);
                if (el) el.textContent = text;
            };
            setText('ble-prov-adv-name', data.advName || '--');
            setText('ble-prov-connected', data.connected
                ? ((typeof i18n !== 'undefined' && i18n.t) ? i18n.t('ble-prov-yes') : '是')
                : ((typeof i18n !== 'undefined' && i18n.t) ? i18n.t('ble-prov-no') : '否'));
            setText('ble-prov-result', data.provisioned
                ? ((typeof i18n !== 'undefined' && i18n.t) ? i18n.t('ble-prov-result-ok') : '成功')
                : '--');

            // 运行中禁用启动按钮，未运行时禁用停止按钮
            const startBtn = document.getElementById('ble-prov-start-btn');
            const stopBtn = document.getElementById('ble-prov-stop-btn');
            if (startBtn) startBtn.disabled = !!data.running;
            if (stopBtn) stopBtn.disabled = !data.running;
        },

        /**
         * 显示蓝牙配网操作结果消息
         */
        _showBleProvisionMessage(ok, message) {
            const okEl = document.getElementById('ble-prov-success');
            const errEl = document.getElementById('ble-prov-error');
            const t = (typeof i18n !== 'undefined' && i18n.t) ? i18n.t.bind(i18n) : (k => k);

            if (okEl) {
                okEl.textContent = message || t('ble-prov-op-ok');
                okEl.classList.toggle('fb-hidden', !ok);
            }
            if (errEl) {
                errEl.textContent = message || t('ble-prov-op-fail');
                errEl.classList.toggle('fb-hidden', !!ok);
            }
            if (this._networkBleMessageTimer) clearTimeout(this._networkBleMessageTimer);
            this._networkBleMessageTimer = setTimeout(() => {
                this._networkBleMessageTimer = null;
                if (okEl) okEl.classList.add('fb-hidden');
                if (errEl) errEl.classList.add('fb-hidden');
            }, 5000);
        },

    });

    // 自动绑定事件
    if (typeof AppState.setupNetworkEvents === 'function') {
        AppState.setupNetworkEvents();
    }

    if (typeof AppState.onPageHide === 'function') {
        AppState.onPageHide('network', function() {
            if (this.currentPage === 'network' && typeof this._stopNetworkPageWork === 'function') {
                this._stopNetworkPageWork();
            }
        });
    }

})();
