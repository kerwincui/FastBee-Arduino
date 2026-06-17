/**
 * 网络配置模块
 * 包含网络配置加载/保存、WiFi扫描、AP配置、高级配置
 */
(function() {
    AppState.registerModule('network', {

        // ============ 事件绑定 ============
        setupNetworkEvents() {
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

            // 以太网配置表单提交
            const ethernetForm = document.getElementById('ethernet-form');
            if (ethernetForm) {
                ethernetForm.addEventListener('submit', (e) => {
                    e.preventDefault();
                    this.saveEthernetConfig();
                });
            }

            // 4G 配置表单提交
            const cellularForm = document.getElementById('cellular-form');
            if (cellularForm) {
                cellularForm.addEventListener('submit', (e) => {
                    e.preventDefault();
                    this.saveCellularConfig();
                });
            }

            // LoRa 配置表单提交
            const loraForm = document.getElementById('lora-form');
            if (loraForm) {
                loraForm.addEventListener('submit', (e) => {
                    e.preventDefault();
                    this.saveLoRaConfig();
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

        /**
         * 联网方式切换处理
         */
        _onNetworkTypeChange(value) {
            const panels = ['wifi-panel', 'ethernet-panel', 'cellular-panel', 'lora-panel'];
            panels.forEach(id => {
                const el = document.getElementById(id);
                if (el) this.hideElement(el);
            });

            const panelMap = { '0': 'wifi-panel', '1': 'ethernet-panel', '2': 'cellular-panel', '3': 'lora-panel' };
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

            // 动态调整高级配置内容显示
            // IP配置部分：仅WiFi和以太网需要DHCP/静态IP配置
            const ipConfigSection = document.querySelector('#advance .fb-section-title');
            if (ipConfigSection) {
                const ipContainer = ipConfigSection.parentElement;
                const ipGrid = ipConfigSection.nextElementSibling;
                const showIpConfig = (value === '0' || value === '1');
                if (ipConfigSection) ipConfigSection.style.display = showIpConfig ? '' : 'none';
                if (ipGrid && ipGrid.classList.contains('config-form-grid')) {
                    ipGrid.style.display = showIpConfig ? '' : 'none';
                }
            }

            // 连接设置部分：仅WiFi模式需要超时/重连配置
            const connSettings = document.querySelectorAll('#advance .fb-section-title-mt');
            if (connSettings.length >= 2) {
                const connTitle = connSettings[1];
                const connGrid = connTitle.nextElementSibling;
                const showConnConfig = (value === '0');
                if (connTitle) connTitle.style.display = showConnConfig ? '' : 'none';
                if (connGrid && connGrid.classList.contains('config-form-grid')) {
                    connGrid.style.display = showConnConfig ? '' : 'none';
                }
            }

            const saveBtn = document.getElementById('network-type-save-btn');
            if (saveBtn) saveBtn.disabled = false;
            setTimeout(() => this.loadNetworkStatus(), 300);
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
                '<div class="wifi-scan-state-text">' + message + '</div>';
            if (detail) {
                html += '<div class="wifi-scan-state-detail">' + detail + '</div>';
            }
            html += '</div>';
            return html;
        },

        _getWifiLockIcon(isEncrypted) {
            return isEncrypted
                ? '<span class="wifi-lock-icon-secure">🔒</span>'
                : '<span class="wifi-lock-icon-open">🔓</span>';
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
                        Notification.error('加载网络配置失败', '网络设置');
                        return;
                    }

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
                    this._setValue('wifi-security', sta.security || 'wpa');

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
                    this._setValue('enable-mdns', network.enableMDNS ? '1' : '0');
                    this._setValue('custom-domain', network.customDomain || '');

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

                    // ========== LoRa 配置 ==========
                    const lora = data.lora || {};
                    this._setValue('lora-tx', lora.txPin !== undefined ? lora.txPin.toString() : '39');
                    this._setValue('lora-rx', lora.rxPin !== undefined ? lora.rxPin.toString() : '40');
                    this._setValue('lora-m1', lora.m1Pin !== undefined ? lora.m1Pin.toString() : '41');
                    this._setValue('lora-baud', lora.baudRate !== undefined ? lora.baudRate.toString() : '9600');
                })
                .catch(err => {
                    console.error('Load network config failed:', err);
                    Notification.error('加载网络配置失败', '网络设置');
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
        saveActiveNetworkConfig() {
            const networkType = document.getElementById('network-type')?.value || '0';
            switch (networkType) {
                case '1':
                    return this.saveEthernetConfig();
                case '2':
                    return this.saveCellularConfig();
                case '3':
                    return this.saveLoRaConfig();
                default:
                    return this.saveNetworkConfig();
            }
        },

        _refreshNetworkStatusSoon(delayMs) {
            setTimeout(() => this.loadNetworkStatus(), delayMs || 800);
        },

        saveNetworkConfig() {
            const config = {
                networkType: document.getElementById('network-type')?.value || '0',
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
                submitBtnText.innerHTML = '网络模式切换中...';
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
                        let message = '网络设置保存成功！';
                        let noticeMessage = '网络模式切换提醒：';

                        if (data.restartRequired) {
                            message += '<br>' + this._renderWifiNote('网络配置变更需要重启网络服务才能生效，请等待约10秒...');
                        }

                        const mode = data.mode;
                        const modeText = data.modeText || '';

                        if (mode === 0 || modeText === 'STA') {
                            if (data.mdnsDomain) {
                                const hint = '可通过域名 http://' + data.mdnsDomain + ' 访问Web服务';
                                message += '<br>' + this._renderWifiNote(hint);
                                noticeMessage += '切换到STA模式后，可通过 http://' + data.mdnsDomain + ' 访问';
                            }
                        } else if (mode === 1 || modeText === 'AP') {
                            const hint = '请先连接热点 [' + (data.apSSID || 'fastbee-ap') + ']，然后访问 http://' + (data.apIP || '192.168.4.1');
                            message += '<br>' + this._renderWifiNote(hint);
                            noticeMessage += '切换到AP模式后，请连接热点 [' + (data.apSSID || 'fastbee-ap') + '] 访问 http://' + (data.apIP || '192.168.4.1');
                        }

                        message += '<br>' + this._renderWifiNote('如无法访问，请刷新页面或重新连接网络。');
                        this._setWifiModeNotice(noticeMessage);

                        Notification.show({
                            type: 'success',
                            title: '网络配置已保存',
                            message: message,
                            html: true,
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
                                    submitBtnText.textContent = originalText || '保存配置';
                                }
                                if (noticeTextEl && noticeMessage) {
                                    noticeTextEl.textContent = noticeMessage;
                                }
                            } else {
                                if (submitBtnText) {
                                    submitBtnText.textContent = '网络模式切换中... (' + countdown + 's)';
                                }
                                if (noticeTextEl) {
                                    const countdownText = '按钮将在 ' + countdown + ' 秒后恢复可用。';
                                    noticeTextEl.textContent = noticeMessage + ' ' + countdownText;
                                }
                            }
                        }, 1000);

                    } else {
                        this._showMessage('wifi-error', true);
                        Notification.error(res?.error || '保存失败', '网络设置');
                        if (submitBtn && submitBtnText) {
                            submitBtn.disabled = false;
                            submitBtnText.innerHTML = originalText || '保存配置';
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
                            title: '网络配置已保存',
                            message: '网络设置保存成功！<br>' + this._renderWifiNote('网络配置变更需要重启网络服务才能生效，请等待约10秒...'),
                            html: true,
                            duration: 8000
                        });
                    } else {
                        this._showMessage('wifi-error', true);
                        Notification.error('保存失败', '网络设置');
                        if (submitBtn && submitBtnText) {
                            submitBtn.disabled = false;
                            submitBtnText.innerHTML = originalText || '保存配置';
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
                apIP: document.getElementById('ap-ip')?.value || '192.168.4.1',
                apChannel: document.getElementById('ap-channel')?.value || '1',
                apHidden: document.getElementById('ap-hidden')?.value || '0',
                apMaxConnections: document.getElementById('ap-max-connections')?.value || '4'
            };

            const submitBtn = document.querySelector('#ap-form button[type="submit"]');
            const originalText = submitBtn?.innerHTML;
            if (submitBtn) {
                submitBtn.disabled = true;
                submitBtn.innerHTML = '… 保存中...';
            }

            apiPut('/api/network/config', config)
                .then(res => {
                    if (res && res.success) {
                        this._showMessage('ap-success', true);
                        Notification.success('热点配置保存成功！', '网络设置');
                    } else {
                        Notification.error(res?.error || '保存失败', '网络设置');
                    }
                })
                .catch(err => {
                    console.error('Save AP config failed:', err);
                    Notification.error('保存失败', '网络设置');
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
                submitBtn.innerHTML = '… 保存中...';
            }

            apiPut('/api/network/config', config)
                .then(res => {
                    if (res && res.success) {
                        this._showMessage('advanced-success', true);
                        Notification.success('高级配置保存成功！', '网络设置');
                    } else {
                        Notification.error(res?.error || '保存失败', '网络设置');
                    }
                })
                .catch(err => {
                    console.error('Save advanced config failed:', err);
                    Notification.error('保存失败', '网络设置');
                })
                .finally(() => {
                    if (submitBtn) {
                        submitBtn.disabled = false;
                        submitBtn.innerHTML = originalText;
                    }
                });
        },

        /**
         * 保存以太网配置
         */
        saveEthernetConfig() {
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
            apiPut('/api/network/config', config)
                .then(res => {
                    if (res && res.success) {
                        Notification.success('网络设置保存成功', '网络设置');
                        this._refreshNetworkStatusSoon(1200);
                    } else {
                        Notification.error(res?.error || '保存失败', '网络设置');
                    }
                })
                .catch(() => Notification.error('保存失败', '网络设置'));
        },

        /**
         * 保存4G配置
         */
        saveCellularConfig() {
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
            apiPut('/api/network/config', config)
                .then(res => {
                    if (res && res.success) {
                        Notification.success('网络设置保存成功', '网络设置');
                        this._refreshNetworkStatusSoon(1200);
                    } else {
                        Notification.error(res?.error || '保存失败', '网络设置');
                    }
                })
                .catch(() => Notification.error('保存失败', '网络设置'));
        },

        /**
         * 保存LoRa配置
         */
        saveLoRaConfig() {
            const config = {
                networkType: '3',
                lora: {
                    txPin: parseInt(document.getElementById('lora-tx')?.value || '39'),
                    rxPin: parseInt(document.getElementById('lora-rx')?.value || '40'),
                    m1Pin: parseInt(document.getElementById('lora-m1')?.value || '41'),
                    baudRate: parseInt(document.getElementById('lora-baud')?.value || '9600')
                }
            };
            apiPut('/api/network/config', config)
                .then(res => {
                    if (res && res.success) {
                        Notification.success('网络设置保存成功', '网络设置');
                        this._refreshNetworkStatusSoon(1200);
                    } else {
                        Notification.error(res?.error || '保存失败', '网络设置');
                    }
                })
                .catch(() => Notification.error('保存失败', '网络设置'));
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

                    const networks = res.data || [];

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
                        const isOpen = net.encryption === 'open';
                        const encryptIcon = this._getWifiLockIcon(!isOpen);
                        const encLabel = encryptLabels[net.encryption] || net.encryption;

                        html += `
                            <div class="wifi-grid-item" data-ssid="${net.ssid}" data-encryption="${net.encryption}">
                                <div class="wifi-info">
                                    <div class="wifi-ssid">${net.ssid}</div>
                                    <div class="wifi-meta">
                                        ${encryptIcon} ${encLabel}
                                    </div>
                                </div>
                                <div class="wifi-signal ${signalClass}">
                                    ${net.rssi} dBm
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
            getter('/api/network/status')
                .then(res => {
                    if (!res || !res.success || !res.data) {
                        // 响应无效，重试一次
                        if (retryCount < 2) {
                            setTimeout(() => this.loadNetworkStatus(retryCount + 1), 2000);
                        } else {
                            this._loadNetworkStatusFallback();
                        }
                        return;
                    }
                    this._updateNetworkStatus(res.data);
                })
                .catch(err => {
                    console.error('Failed to load network status:', err);
                    // DNS解析失败（如4G模式关闭WiFi后mDNS不可用）无需重试
                    if (this._isDnsError(err)) {
                        console.warn('[Network] DNS resolution failed, device may be unreachable via current hostname');
                        return;
                    }
                    // 其他网络错误时重试（可能是内存压力导致503）
                    if (retryCount < 2) {
                        setTimeout(() => this.loadNetworkStatus(retryCount + 1), 3000);
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
            // TypeError: Failed to fetch 且无 status 字段，通常是DNS/网络层错误
            if (err instanceof TypeError && !err.status) return true;
            // AbortError 也可能是DNS超时
            if (err.name === 'AbortError') return true;
            return false;
        },

        _loadNetworkStatusFallback() {
            const getter = (typeof apiGetSilentFresh === 'function')
                ? apiGetSilentFresh
                : (typeof apiGetFresh === 'function' ? apiGetFresh : apiGet);
            getter('/api/system/status')
                .then(res => {
                    if (!res || !res.success || !res.data) return;
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
            ['wifi-status-panel', 'ethernet-status-panel', 'cellular-status-panel', 'lora-status-panel'].forEach(id => {
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
                case '3': // LoRa
                    this._updateLoRaStatus(data);
                    document.getElementById('lora-status-panel')?.classList.remove('fb-hidden');
                    break;
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

            document.getElementById('wifi-mode-display').textContent = data.mode || '--';
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
         * 更新LoRa状态显示
         */
        _updateLoRaStatus(data) {
            const statusBadge = document.getElementById('lora-status-badge');
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

            document.getElementById('lora-mode-display').textContent = data.loraMode || '透传模式';
            document.getElementById('lora-address').textContent = data.loraAddress || '--';
            document.getElementById('lora-frequency').textContent = data.loraFrequency || '470MHz';
            document.getElementById('lora-airrate').textContent = data.loraAirRate || '2.4kbps';
            document.getElementById('lora-channel').textContent = data.loraChannel || '--';
        },

        /**
         * 初始化网络状态刷新按钮
         */
        setupStatusRefresh() {
            const refreshBtn = document.getElementById('refresh-status-btn');
            if (refreshBtn) {
                refreshBtn.addEventListener('click', () => {
                    refreshBtn.disabled = true;
                    this.loadNetworkStatus();
                    setTimeout(() => {
                        refreshBtn.disabled = false;
                    }, 1500);
                });
            }

            // 首次加载网络配置时同时加载状态
            const originalLoadConfig = this.loadNetworkConfig.bind(this);
            this.loadNetworkConfig = () => {
                originalLoadConfig();
                setTimeout(() => this.loadNetworkStatus(), 500);
            };
        },

    });

    // 自动绑定事件
    if (typeof AppState.setupNetworkEvents === 'function') {
        AppState.setupNetworkEvents();
    }

    // 初始化状态刷新功能
    if (typeof AppState.setupStatusRefresh === 'function') {
        AppState.setupStatusRefresh();
    }

    // 根据固件能力隐藏不支持的联网方式
    (function filterNetworkTypesByCapabilities() {
        var getter = (typeof apiGetSilentFresh === 'function') ? apiGetSilentFresh
            : (typeof apiGetFresh === 'function' ? apiGetFresh : (typeof apiGet === 'function' ? apiGet : null));
        if (!getter) return;
        getter('/api/system/capabilities').then(function(res) {
            if (!res || !res.success || !res.data) return;
            var caps = res.data;
            var select = document.getElementById('network-type');
            if (!select) return;
            // value="1" = 以太网 W5500, value="2" = 4G EC801E, value="3" = LoRa 透传
            if (!caps.ethernet) {
                var opt1 = select.querySelector('option[value="1"]');
                if (opt1) opt1.remove();
            }
            if (!caps.cellular) {
                var opt2 = select.querySelector('option[value="2"]');
                if (opt2) opt2.remove();
            }
            if (!caps.lora) {
                var opt3 = select.querySelector('option[value="3"]');
                if (opt3) opt3.remove();
            }
        }).catch(function() { /* 静默失败，保留所有选项 */ });
    })();
})();
