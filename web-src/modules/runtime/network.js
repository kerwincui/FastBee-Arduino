/**
 * 网络配置模块
 * 包含网络配置加载/保存、WiFi扫描、AP配置、高级配置
 */
(function() {
    AppState.registerModule('network', {

        // ============ 事件绑定 ============
        setupNetworkEvents() {
            // WiFi扫描按钮
            const wifiScanBtn = document.getElementById('wifi-scan-btn');
            if (wifiScanBtn) wifiScanBtn.addEventListener('click', () => this.scanWifiNetworks());

            // 基本配置表单提交
            const wifiForm = document.getElementById('wifi-form');
            if (wifiForm) {
                wifiForm.addEventListener('submit', (e) => {
                    e.preventDefault();
                    this.saveNetworkConfig();
                });
            }

            // 热点配置表单提交
            const apForm = document.getElementById('ap-form');
            if (apForm) {
                apForm.addEventListener('submit', (e) => {
                    e.preventDefault();
                    this.saveAPConfig();
                });
            }

            // 高级配置表单提交
            const advancedForm = document.getElementById('advanced-form');
            if (advancedForm) {
                advancedForm.addEventListener('submit', (e) => {
                    e.preventDefault();
                    this.saveAdvancedConfig();
                });
            }

            // DHCP/静态IP切换
            const wifiDhcp = document.getElementById('wifi-dhcp');
            if (wifiDhcp) {
                wifiDhcp.addEventListener('change', (e) => {
                    this._toggleStaticIPFields(e.target.value === '1');
                });
            }

        },

        _renderWifiNote(text) {
            return '<small class="wifi-note-inline">' + text + '</small>';
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
                '<i class="fas fa-exclamation-circle wifi-scan-state-icon"></i>' +
                '<div class="wifi-scan-state-text">' + message + '</div>';
            if (detail) {
                html += '<div class="wifi-scan-state-detail">' + detail + '</div>';
            }
            html += '</div>';
            return html;
        },

        _getWifiLockIcon(isEncrypted) {
            return isEncrypted
                ? '<i class="fas fa-lock wifi-lock-icon-secure"></i>'
                : '<i class="fas fa-lock-open wifi-lock-icon-open"></i>';
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
            apiGet('/api/network/config')
                .then(res => {
                    if (!res || !res.success) {
                        Notification.error(i18n.t('net-load-fail'), i18n.t('net-settings-title'));
                        return;
                    }

                    const data = res.data || {};
                    const device = data.device || {};
                    const network = data.network || {};
                    const sta = data.sta || {};
                    const ap = data.ap || {};
                    const advanced = data.advanced || {};

                    // ========== 基本配置 ==========
                    this._setValue('wifi-mode', network.mode !== undefined ? network.mode.toString() : '2');

                    // STA 配置
                    this._setValue('wifi-ssid', sta.ssid || '');
                    this._setValue('wifi-password', sta.password || '');
                    this._setValue('wifi-security', sta.security || 'wpa');

                    // ========== 热点配置 ==========
                    this._setValue('ap-ssid', ap.ssid || '');
                    this._setValue('ap-password', ap.password || '');
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
                    this._setValue('enable-mdns', network.enableMDNS ? '1' : '0');
                    this._setValue('custom-domain', network.customDomain || '');

                    // 连接设置
                    this._setValue('connect-timeout', advanced.connectTimeout !== undefined ? advanced.connectTimeout.toString() : '10000');
                    this._setValue('reconnect-interval', advanced.reconnectInterval !== undefined ? advanced.reconnectInterval.toString() : '5000');
                    this._setValue('max-reconnect-attempts', advanced.maxReconnectAttempts !== undefined ? advanced.maxReconnectAttempts.toString() : '5');
                    this._setValue('conflict-detection', advanced.conflictDetection !== undefined ? advanced.conflictDetection.toString() : '3');
                })
                .catch(err => {
                    console.error('Load network config failed:', err);
                    Notification.error(i18n.t('net-load-fail'), i18n.t('net-settings-title'));
                });
        },

        /**
         * 切换静态IP字段显示/隐藏
         */
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
        saveNetworkConfig() {
            const config = {
                mode: document.getElementById('wifi-mode')?.value || '2',
                staSSID: document.getElementById('wifi-ssid')?.value || '',
                staPassword: document.getElementById('wifi-password')?.value || '',
                staSecurity: document.getElementById('wifi-security')?.value || 'wpa'
            };

            const submitBtn = document.getElementById('wifi-save-btn');
            const submitBtnText = document.getElementById('wifi-save-btn-text');
            const originalText = submitBtnText?.innerHTML;

            if (submitBtn && submitBtnText) {
                submitBtn.disabled = true;
                submitBtnText.innerHTML = i18n.t('wifi-saving-mode');
            }

            this._showMessage('wifi-success', false);
            this._showMessage('wifi-error', false);

            const noticeTextEl = document.getElementById('wifi-mode-notice-text');
            this._setWifiModeNotice('');

            apiPut('/api/network/config', config)
                .then(res => {
                    if (res && res.success) {
                        this._showMessage('wifi-success', true);

                        const data = res.data || {};
                        let message = i18n.t('wifi-save-ok');
                        let noticeMessage = i18n.t('wifi-mode-notice-title');

                        if (data.restartRequired) {
                            message += '<br>' + this._renderWifiNote(i18n.t('wifi-restart-hint'));
                        }

                        const mode = data.mode;
                        const modeText = data.modeText || '';

                        if (mode === 0 || modeText === 'STA') {
                            if (data.mdnsDomain) {
                                const hint = i18n.t('wifi-mode-sta-hint').replace('{domain}', data.mdnsDomain);
                                message += '<br>' + this._renderWifiNote(hint);
                                noticeMessage += i18n.t('wifi-mode-notice-sta').replace('{domain}', data.mdnsDomain);
                            }
                        } else if (mode === 1 || modeText === 'AP') {
                            const hint = i18n.t('wifi-mode-ap-hint')
                                .replace('{ssid}', data.apSSID || 'fastbee-ap')
                                .replace('{ip}', data.apIP || '192.168.4.1');
                            message += '<br>' + this._renderWifiNote(hint);
                            noticeMessage += i18n.t('wifi-mode-notice-ap')
                                .replace('{ssid}', data.apSSID || 'fastbee-ap')
                                .replace('{ip}', data.apIP || '192.168.4.1');
                        }

                        message += '<br>' + this._renderWifiNote(i18n.t('wifi-reconnect-hint'));
                        this._setWifiModeNotice(noticeMessage);

                        Notification.show({
                            type: 'success',
                            title: i18n.t('wifi-mode-changed-title'),
                            message: message,
                            duration: 8000
                        });

                        // 15秒倒计时后恢复按钮
                        let countdown = 15;
                        const countdownInterval = setInterval(() => {
                            countdown--;
                            if (countdown <= 0) {
                                clearInterval(countdownInterval);
                                if (submitBtn && submitBtnText) {
                                    submitBtn.disabled = false;
                                    submitBtnText.textContent = originalText || i18n.t('wifi-save-ready');
                                }
                                if (noticeTextEl && noticeMessage) {
                                    noticeTextEl.textContent = noticeMessage;
                                }
                            } else {
                                if (submitBtnText) {
                                    submitBtnText.textContent = i18n.t('wifi-saving-mode') + ' (' + countdown + 's)';
                                }
                                if (noticeTextEl) {
                                    const countdownText = i18n.t('wifi-mode-notice-countdown').replace('{seconds}', countdown);
                                    noticeTextEl.textContent = noticeMessage + ' ' + countdownText;
                                }
                            }
                        }, 1000);

                    } else {
                        this._showMessage('wifi-error', true);
                        Notification.error(res?.error || i18n.t('net-save-fail'), i18n.t('net-settings-title'));
                        if (submitBtn && submitBtnText) {
                            submitBtn.disabled = false;
                            submitBtnText.innerHTML = originalText || i18n.t('wifi-save-ready');
                        }
                    }
                })
                .catch(err => {
                    console.error('Save network config failed:', err);
                    const isNetworkTransitionError = err && (
                        err.name === 'AbortError' ||
                        err.name === 'TypeError' ||
                        (err.message && (
                            err.message.includes('fetch') ||
                            err.message.includes('network') ||
                            err.message.includes('Failed to fetch')
                        ))
                    );

                    if (isNetworkTransitionError) {
                        this._showMessage('wifi-success', true);
                        Notification.show({
                            type: 'success',
                            title: i18n.t('wifi-mode-changed-title'),
                            message: i18n.t('wifi-save-ok') + '<br>' + this._renderWifiNote(i18n.t('wifi-restart-hint')),
                            duration: 8000
                        });
                    } else {
                        this._showMessage('wifi-error', true);
                        Notification.error(i18n.t('net-save-fail'), i18n.t('net-settings-title'));
                        if (submitBtn && submitBtnText) {
                            submitBtn.disabled = false;
                            submitBtnText.innerHTML = originalText || i18n.t('wifi-save-ready');
                        }
                    }
                });
        },

        /**
         * 保存热点配置
         */
        saveAPConfig() {
            const config = {
                apSSID: document.getElementById('ap-ssid')?.value || '',
                apPassword: document.getElementById('ap-password')?.value || '',
                apChannel: document.getElementById('ap-channel')?.value || '1',
                apHidden: document.getElementById('ap-hidden')?.value || '0',
                apMaxConnections: document.getElementById('ap-max-connections')?.value || '4'
            };

            const submitBtn = document.querySelector('#ap-form button[type="submit"]');
            const originalText = submitBtn?.innerHTML;
            if (submitBtn) {
                submitBtn.disabled = true;
                submitBtn.innerHTML = i18n.t('net-saving-html');
            }

            apiPut('/api/network/config', config)
                .then(res => {
                    if (res && res.success) {
                        this._showMessage('ap-success', true);
                        Notification.success(i18n.t('ap-save-ok'), i18n.t('net-settings-title'));
                    } else {
                        Notification.error(res?.error || i18n.t('net-save-fail'), i18n.t('net-settings-title'));
                    }
                })
                .catch(err => {
                    console.error('Save AP config failed:', err);
                    Notification.error(i18n.t('net-save-fail'), i18n.t('net-settings-title'));
                })
                .finally(() => {
                    if (submitBtn) {
                        submitBtn.disabled = false;
                        submitBtn.innerHTML = originalText;
                    }
                });
        },

        /**
         * 保存高级配置
         */
        saveAdvancedConfig() {
            const config = {
                ipConfigType: document.getElementById('wifi-dhcp')?.value || '0',
                staticIP: document.getElementById('static-ip')?.value || '',
                gateway: document.getElementById('gateway')?.value || '',
                subnet: document.getElementById('subnet')?.value || '',
                dns1: document.getElementById('dns1')?.value || '',
                dns2: document.getElementById('dns2')?.value || '',
                enableMDNS: document.getElementById('enable-mdns')?.value || '0',
                customDomain: document.getElementById('custom-domain')?.value || '',
                connectTimeout: document.getElementById('connect-timeout')?.value || '10000',
                reconnectInterval: document.getElementById('reconnect-interval')?.value || '5000',
                maxReconnectAttempts: document.getElementById('max-reconnect-attempts')?.value || '5',
                conflictDetection: document.getElementById('conflict-detection')?.value || '3'
            };

            const submitBtn = document.querySelector('#advanced-form button[type="submit"]');
            const originalText = submitBtn?.innerHTML;
            if (submitBtn) {
                submitBtn.disabled = true;
                submitBtn.innerHTML = i18n.t('net-saving-html');
            }

            apiPut('/api/network/config', config)
                .then(res => {
                    if (res && res.success) {
                        this._showMessage('advanced-success', true);
                        Notification.success(i18n.t('advanced-save-ok'), i18n.t('net-settings-title'));
                    } else {
                        Notification.error(res?.error || i18n.t('net-save-fail'), i18n.t('net-settings-title'));
                    }
                })
                .catch(err => {
                    console.error('Save advanced config failed:', err);
                    Notification.error(i18n.t('net-save-fail'), i18n.t('net-settings-title'));
                })
                .finally(() => {
                    if (submitBtn) {
                        submitBtn.disabled = false;
                        submitBtn.innerHTML = originalText;
                    }
                });
        },

        /**
         * 扫描 WiFi 网络
         */
        scanWifiNetworks() {
            const scanBtn = document.getElementById('wifi-scan-btn');
            const modal = document.getElementById('wifi-modal');
            const modalBody = document.getElementById('wifi-modal-body');

            if (!modal || !modalBody) return;

            this.showModal(modal);
            this._ensureWifiModalEvents(modal);
            modalBody.innerHTML = i18n.t('wifi-scanning-result');

            if (scanBtn) {
                scanBtn.disabled = true;
                scanBtn.innerHTML = i18n.t('wifi-scanning-html');
            }

            apiGet('/api/wifi/scan')
                .then(res => {
                    if (!res || !res.success) {
                        if (res && res.error === 'scan_busy') {
                            modalBody.innerHTML = i18n.t('wifi-scan-busy');
                        } else {
                            modalBody.innerHTML = this._renderWifiScanState(
                                i18n.t('wifi-scan-fail-msg'),
                                res?.error || 'Unknown error'
                            );
                        }
                        return;
                    }

                    const networks = res.data || [];

                    if (networks.length === 0) {
                        modalBody.innerHTML = i18n.t('wifi-no-network');
                        return;
                    }

                    networks.sort((a, b) => b.rssi - a.rssi);

                    let html = '<div class="wifi-grid">';
                    networks.forEach((net) => {
                        const signalClass = net.rssi > -50 ? 'strong' : net.rssi > -70 ? 'medium' : 'weak';
                        const encryptIcon = this._getWifiLockIcon(net.encryption > 0);
                        const securityType = net.encryption > 0 ? 'wpa' : 'none';

                        html += `
                            <div class="wifi-grid-item" data-ssid="${net.ssid}" data-encryption="${securityType}">
                                <div class="wifi-info">
                                    <div class="wifi-ssid">${net.ssid}</div>
                                    <div class="wifi-meta">
                                        ${encryptIcon} ${net.encryption > 0 ? i18n.t('wifi-encrypted') : i18n.t('wifi-open')}
                                    </div>
                                </div>
                                <div class="wifi-signal ${signalClass}">
                                    <i class="fas fa-signal"></i> ${net.rssi} dBm
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

                            const securitySelect = document.getElementById('wifi-security');
                            if (securitySelect) {
                                securitySelect.value = encryption === 'none' ? '0' : '1';
                            }

                            const passwordInput = document.getElementById('wifi-password');
                            if (passwordInput) passwordInput.value = '';

                            this.hideModal(modal);
                            Notification.success(`${i18n.t('wifi-selected-prefix')}${ssid}`, i18n.t('wifi-scan-title'));
                        });
                    });
                })
                .catch(err => {
                    console.error('WiFi scan failed:', err);
                    modalBody.innerHTML = this._renderWifiScanState(i18n.t('wifi-scan-fail-msg'));
                })
                .finally(() => {
                    if (scanBtn) {
                        scanBtn.disabled = false;
                        scanBtn.innerHTML = i18n.t('wifi-scan-btn-html');
                    }
                });
        },

    });

    // 自动绑定事件
    if (typeof AppState.setupNetworkEvents === 'function') {
        AppState.setupNetworkEvents();
    }
})();
