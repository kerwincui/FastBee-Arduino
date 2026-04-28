/**
 * protocol/protocol-config.js — 协议配置加载/保存、Master 状态刷新
 */
(function() {
    Object.assign(AppState, {

        // ============ 协议配置 ============

        // Tab ID → 分片文件名和容器 ID 的映射
        _protocolFragmentMap: {
            'mqtt': { container: 'mqtt-fragment-container', fragment: 'protocol-mqtt' },
            'modbus-rtu': { container: 'modbus-rtu-fragment-container', fragment: 'protocol-modbus-rtu' },
            'http': { container: 'http-fragment-container', fragment: 'protocol-http' },
            'coap': { container: 'coap-fragment-container', fragment: 'protocol-coap' },
            'tcp': { container: 'tcp-fragment-container', fragment: 'protocol-tcp' }
        },

        loadProtocolConfig(tabId) {
            // 确保页面 DOM 加载后绑定事件
            if (!this._protocolEventsBound) {
                this.setupProtocolEvents();
            }
            if (tabId !== 'modbus-rtu') {
                this._stopMasterStatusRefresh();
                if (this._coilAutoRefreshTimer) {
                    clearInterval(this._coilAutoRefreshTimer);
                    this._coilAutoRefreshTimer = null;
                }
            }
            if (tabId !== 'mqtt' && typeof this._stopMqttStatusPolling === 'function') {
                this._stopMqttStatusPolling();
            }
            if (tabId === 'modbus-rtu') {
                this._updateDelayChannelSelect();
            }

            // 按需加载分片 HTML，加载完成后再填充表单
            var fragInfo = this._protocolFragmentMap[tabId];
            if (fragInfo) {
                var self = this;
                PageLoader.loadFragment(fragInfo.container, fragInfo.fragment, function() {
                    // 分片加载完成后重新尝试绑定事件（新 DOM 元素可能需要）
                    if (!self._protocolEventsBound) {
                        self.setupProtocolEvents();
                    }
                    self._loadProtocolConfigData(tabId);
                });
            } else {
                this._loadProtocolConfigData(tabId);
            }
        },

        _loadProtocolConfigData(tabId) {
            if (this._protocolConfig) {
                this._fillProtocolForm(tabId, this._protocolConfig);
                return;
            }
            apiGet('/api/protocol/config')
                .then(res => {
                    if (!res || !res.success) return;
                    this._protocolConfig = res.data || {};
                    this._fillProtocolForm(tabId, this._protocolConfig);
                })
                .catch(err => {
                    console.error('加载协议配置失败:', err);
                });
        },

        _fillProtocolForm(tabId, config) {
            if (tabId === 'modbus-rtu' && config.modbusRtu) {
                const rtu = config.modbusRtu;
                this._setCheckbox('modbus-rtu-enabled', rtu.enabled ?? false);
                this._loadUartPeripherals(rtu.peripheralId || '');
                this._setValue('rtu-de-pin', rtu.dePin ?? 14);
                this._setValue('rtu-transfer-type', rtu.transferType ?? 0);
                this.onModbusModeChange('master');
                if (rtu.master) {
                    this._masterTasks = rtu.master.tasks || [];
                    this._modbusDevices = rtu.master.devices || [];
                } else {
                    this._masterTasks = [];
                    this._modbusDevices = [];
                }
                this._modbusRtuLoaded = true;
                // 先加载设备列表（内部会调用 _renderAllDevices），再启动状态刷新
                this._loadModbusDevices();
                this._onTransferTypeChange();  // 根据传输类型联动 UI
                this.refreshMasterStatus();
                this._startMasterStatusRefresh();
                this._updateDelayChannelSelect();
            }
            if (tabId === 'modbus-tcp' && config.modbusTcp) {
                const tcp = config.modbusTcp;
                this._setCheckbox('modbus-tcp-enabled', tcp.enabled ?? false);
                this._setValue('tcp-ip', tcp.server || '192.168.1.100');
                this._setValue('tcp-mport', tcp.port || 502);
                this._setValue('tcp-slave-id', tcp.slaveId || 1);
                this._setValue('tcp-mtimeout', tcp.timeout || 5000);
            }
            if (tabId === 'mqtt' && config.mqtt) {
                const mqtt = config.mqtt;
                this._setCheckbox('mqtt-enabled', mqtt.enabled ?? true);
                this._setValue('mqtt-broker', mqtt.server || 'iot.fastbee.cn');
                this._setValue('mqtt-port', mqtt.port || 1883);
                this._setValue('mqtt-client-id', mqtt.clientId || '');
                this._setValue('mqtt-username', mqtt.username || '');
                this._setValue('mqtt-password', mqtt.password || '');
                this._setValue('mqtt-alive', mqtt.keepAlive || 60);
                this._setValue('mqtt-conn-timeout', mqtt.connectionTimeout ?? 30000);
                this._setCheckbox('mqtt-auto-reconnect', mqtt.autoReconnect ?? true);
                this._setValue('mqtt-will-topic', mqtt.willTopic || '');
                this._setValue('mqtt-will-payload', mqtt.willPayload || '');
                this._setValue('mqtt-will-qos', mqtt.willQos ?? 0);
                this._setCheckbox('mqtt-will-retain', mqtt.willRetain ?? false);
                this._setValue('mqtt-longitude', mqtt.longitude ?? 0);
                this._setValue('mqtt-latitude', mqtt.latitude ?? 0);
                this._setValue('mqtt-iccid', mqtt.iccid || '');
                this._setValue('mqtt-card-platform-id', mqtt.cardPlatformId ?? 0);
                this._setValue('mqtt-summary', mqtt.summary || '');
                this._setValue('mqtt-auth-type', mqtt.authType ?? 0);
                this._setValue('mqtt-secret', mqtt.mqttSecret || 'K451265A72244J79');
                this._setValue('mqtt-auth-code', mqtt.authCode || '');
                this._loadMqttPublishTopics(mqtt.publishTopics || []);
                this._loadMqttSubscribeTopics(mqtt.subscribeTopics || []);
            }
            if (tabId === 'http' && config.http) {
                const http = config.http;
                this._setCheckbox('http-enabled', http.enabled ?? false);
                this._setValue('http-url', http.url || 'https://api.example.com');
                this._setValue('http-port', http.port || 80);
                this._setValue('http-method', http.method || 'POST');
                this._setValue('http-timeout', http.timeout || 30);
                this._setValue('http-interval', http.interval || 60);
                this._setValue('http-retry', http.retry || 3);
                this._setValue('http-auth-type', http.authType || 'none');
                this._setValue('http-auth-user', http.authUser || '');
                this._setValue('http-auth-token', http.authToken || '');
                this._setValue('http-content-type', http.contentType || 'application/json');
                this.onHttpAuthTypeChange(http.authType || 'none');
            }
            if (tabId === 'coap' && config.coap) {
                const coap = config.coap;
                this._setCheckbox('coap-enabled', coap.enabled ?? false);
                this._setValue('coap-server', coap.server || 'coap://example.com');
                this._setValue('coap-port', coap.port || 5683);
                this._setValue('coap-method', coap.method || 'POST');
                this._setValue('coap-path', coap.path || 'sensors/temperature');
                this._setValue('coap-msg-type', coap.msgType || 'CON');
                this._setValue('coap-retransmit', coap.retransmit ?? 3);
                this._setValue('coap-timeout', coap.timeout ?? 5000);
            }
            if (tabId === 'tcp' && config.tcp) {
                const tcp = config.tcp;
                this._setCheckbox('tcp-enabled', tcp.enabled ?? false);
                const tcpMode = tcp.mode || 'client';
                this._setValue('tcp-mode', tcpMode);
                this.onTcpModeChange(tcpMode);
                this._setValue('tcp-server', tcp.server || '192.168.1.200');
                this._setValue('tcp-port', tcp.port || 5000);
                this._setValue('tcp-timeout', tcp.timeout || 5000);
                this._setValue('tcp-keepalive', tcp.keepAlive || 60);
                this._setValue('tcp-retry', tcp.maxRetry || 5);
                this._setValue('tcp-reconnect', tcp.reconnectInterval || 10);
                this._setValue('tcp-local-port', tcp.localPort ?? 8080);
                this._setValue('tcp-max-clients', tcp.maxClients ?? 5);
                this._setValue('tcp-heartbeat-msg', tcp.heartbeatMsg || '\\n');
                this._setValue('tcp-idle-timeout', tcp.idleTimeout ?? 120);
            }
        },

        saveProtocolConfig(formId) {
            const data = {};
            data.modbusRtu_enabled = document.getElementById('modbus-rtu-enabled')?.checked ? 'true' : 'false';
            data.modbusRtu_peripheralId = document.getElementById('rtu-peripheral-id')?.value || '';
            data.modbusRtu_mode = 'master';
            data.modbusRtu_dePin = document.getElementById('rtu-de-pin')?.value || '14';
            data.modbusRtu_transferType = document.getElementById('rtu-transfer-type')?.value || '0';
            if (data.modbusRtu_enabled === 'true' && !data.modbusRtu_peripheralId) {
                Notification.warning(i18n.t('rtu-no-uart-peripherals'));
                return;
            }
            // 仅在 modbus-rtu 标签页已加载时才发送 tasks/devices 数据，
            // 避免从其他标签页保存时发送空数据覆盖现有配置
            if (this._modbusRtuLoaded) {
                data.modbusRtu_master_tasks = JSON.stringify(this._masterTasks || []);
                data.modbusRtu_master_devices = JSON.stringify(this._modbusDevices || []);
            }
            data.modbusTcp_enabled = document.getElementById('modbus-tcp-enabled')?.checked ? 'true' : 'false';
            data.modbusTcp_server = document.getElementById('tcp-ip')?.value || '192.168.1.100';
            data.modbusTcp_port = document.getElementById('tcp-mport')?.value || '502';
            data.modbusTcp_slaveId = document.getElementById('tcp-slave-id')?.value || '1';
            data.modbusTcp_timeout = document.getElementById('tcp-mtimeout')?.value || '5000';
            data.mqtt_enabled = document.getElementById('mqtt-enabled')?.checked ? 'true' : 'false';
            data.mqtt_server = document.getElementById('mqtt-broker')?.value || 'iot.fastbee.cn';
            data.mqtt_port = document.getElementById('mqtt-port')?.value || '1883';
            data.mqtt_clientId = document.getElementById('mqtt-client-id')?.value || '';
            data.mqtt_username = document.getElementById('mqtt-username')?.value || '';
            data.mqtt_password = document.getElementById('mqtt-password')?.value || '';
            data.mqtt_keepAlive = document.getElementById('mqtt-alive')?.value || '60';
            data.mqtt_connectionTimeout = document.getElementById('mqtt-conn-timeout')?.value || '30000';
            data.mqtt_autoReconnect = document.getElementById('mqtt-auto-reconnect')?.checked ?? true;
            data.mqtt_authType = document.getElementById('mqtt-auth-type')?.value || '0';
            data.mqtt_mqttSecret = document.getElementById('mqtt-secret')?.value || '';
            data.mqtt_authCode = document.getElementById('mqtt-auth-code')?.value || '';
            data.mqtt_willTopic = document.getElementById('mqtt-will-topic')?.value || '';
            data.mqtt_willPayload = document.getElementById('mqtt-will-payload')?.value || '';
            data.mqtt_willQos = document.getElementById('mqtt-will-qos')?.value || '0';
            data.mqtt_willRetain = document.getElementById('mqtt-will-retain')?.checked ? 'true' : 'false';
            data.mqtt_longitude = document.getElementById('mqtt-longitude')?.value || '0';
            data.mqtt_latitude = document.getElementById('mqtt-latitude')?.value || '0';
            data.mqtt_iccid = document.getElementById('mqtt-iccid')?.value || '';
            data.mqtt_cardPlatformId = document.getElementById('mqtt-card-platform-id')?.value || '0';
            data.mqtt_summary = document.getElementById('mqtt-summary')?.value || '';
            data.mqtt_publishTopics = JSON.stringify(this._collectMqttPublishTopics());
            data.mqtt_subscribeTopics = JSON.stringify(this._collectMqttSubscribeTopics());
            data.http_enabled = document.getElementById('http-enabled')?.checked ? 'true' : 'false';
            data.http_url = document.getElementById('http-url')?.value || 'https://api.example.com';
            data.http_port = document.getElementById('http-port')?.value || '80';
            data.http_method = document.getElementById('http-method')?.value || 'POST';
            data.http_timeout = document.getElementById('http-timeout')?.value || '30';
            data.http_interval = document.getElementById('http-interval')?.value || '60';
            data.http_retry = document.getElementById('http-retry')?.value || '3';
            data.http_authType = document.getElementById('http-auth-type')?.value || 'none';
            data.http_authUser = document.getElementById('http-auth-user')?.value || '';
            data.http_authToken = document.getElementById('http-auth-token')?.value || '';
            data.http_contentType = document.getElementById('http-content-type')?.value || 'application/json';
            data.coap_enabled = document.getElementById('coap-enabled')?.checked ? 'true' : 'false';
            data.coap_server = document.getElementById('coap-server')?.value || 'coap://example.com';
            data.coap_port = document.getElementById('coap-port')?.value || '5683';
            data.coap_method = document.getElementById('coap-method')?.value || 'POST';
            data.coap_path = document.getElementById('coap-path')?.value || 'sensors/temperature';
            data.coap_msgType = document.getElementById('coap-msg-type')?.value || 'CON';
            data.coap_retransmit = document.getElementById('coap-retransmit')?.value || '3';
            data.coap_timeout = document.getElementById('coap-timeout')?.value || '5000';
            data.tcp_enabled = document.getElementById('tcp-enabled')?.checked ? 'true' : 'false';
            data.tcp_mode = document.getElementById('tcp-mode')?.value || 'client';
            data.tcp_server = document.getElementById('tcp-server')?.value || '192.168.1.200';
            data.tcp_port = document.getElementById('tcp-port')?.value || '5000';
            data.tcp_timeout = document.getElementById('tcp-timeout')?.value || '5000';
            data.tcp_keepAlive = document.getElementById('tcp-keepalive')?.value || '60';
            data.tcp_maxRetry = document.getElementById('tcp-retry')?.value || '5';
            data.tcp_reconnectInterval = document.getElementById('tcp-reconnect')?.value || '10';
            data.tcp_localPort = document.getElementById('tcp-local-port')?.value || '8080';
            data.tcp_maxClients = document.getElementById('tcp-max-clients')?.value || '5';
            data.tcp_heartbeatMsg = document.getElementById('tcp-heartbeat-msg')?.value || '\\n';
            data.tcp_idleTimeout = document.getElementById('tcp-idle-timeout')?.value || '120';
            const protocolName = this._getProtocolName(formId);
            apiPost('/api/protocol/config', data, 30000)
                .then(res => {
                    if (res && res.success) {
                        this._protocolConfig = null;
                        if (res.data && typeof res.data.mqttReconnected !== 'undefined') {
                            if (res.data.mqttReconnected && res.data.mqttDeferred) {
                                Notification.success(i18n.t('mqtt-reconnect-ok'), i18n.t('protocol-config-title'));
                            } else if (res.data.mqttReconnected) {
                                Notification.success(i18n.t('mqtt-reconnect-ok'), i18n.t('protocol-config-title'));
                            } else if (res.data.mqttDisconnected) {
                                Notification.success(i18n.t('mqtt-disconnect-ok'), i18n.t('protocol-config-title'));
                            } else if (data.mqtt_enabled === 'true') {
                                const errCode = res.data.mqttError || '';
                                const errMsg = errCode ? this._mqttErrorCodeToText(errCode) : '';
                                Notification.warning(
                                    i18n.t('mqtt-reconnect-fail') + (errMsg ? ' (' + errMsg + ')' : ''),
                                    i18n.t('protocol-config-title')
                                );
                            }
                        }
                        Notification.success(`${protocolName} ${i18n.t('protocol-save-ok-suffix')}`, i18n.t('protocol-config-title'));
                        // 如果后端自动生成了 clientId，更新输入框显示
                        if (res.data && res.data.mqttClientId) {
                            const clientIdInput = document.getElementById('mqtt-client-id');
                            if (clientIdInput) {
                                clientIdInput.value = res.data.mqttClientId;
                            }
                        }
                        const form = document.getElementById(formId);
                        const ok = form?.querySelector('.message-success');
                        if (ok) {
                            AppState.showElement(ok);
                            setTimeout(() => { AppState.hideElement(ok); }, 3000);
                        }
                        if (typeof window.apiInvalidateCache === 'function') {
                            window.apiInvalidateCache('/api/protocol/config');
                        }
                        if (this.currentPage === 'protocol') {
                            var mqttTab = document.getElementById('mqtt-tab');
                            if (mqttTab && mqttTab.classList.contains('active')) {
                                this._startMqttStatusPolling();
                            }
                        }
                    } else {
                        Notification.error(res?.message || i18n.t('protocol-save-fail'), i18n.t('protocol-title'));
                    }
                })
                .catch(err => {
                    console.error('saveProtocolConfig error:', err);
                    if (err && err.name === 'AbortError') {
                        Notification.error(i18n.t('protocol-save-timeout') || '保存超时，设备可能正忙，请稍后重试', i18n.t('protocol-title'));
                    } else if (err && err._pageAborted) {
                        // 页面切换导致取消，静默忽略
                    } else if (err instanceof TypeError && /fetch/i.test(err.message)) {
                        Notification.error('设备连接失败，请检查网络后重试', i18n.t('protocol-title'));
                    } else if (err && err.status === 504) {
                        Notification.error('设备响应超时(504)，请稍后重试', i18n.t('protocol-title'));
                    } else {
                        Notification.error(i18n.t('protocol-save-fail'), i18n.t('protocol-title'));
                    }
                });
        },

        // ============ Master 状态刷新 ============

        _getMasterRiskMeta(level) {
            switch ((level || 'low').toLowerCase()) {
                case 'high':
                    return { text: i18n.t('modbus-master-risk-high') || 'HIGH', className: 'modbus-risk-high' };
                case 'medium':
                    return { text: i18n.t('modbus-master-risk-medium') || 'MEDIUM', className: 'modbus-risk-medium' };
                default:
                    return { text: i18n.t('modbus-master-risk-low') || 'LOW', className: 'modbus-risk-low' };
            }
        },

        _getMasterTaskStatusMeta(status) {
            switch ((status || 'pending').toLowerCase()) {
                case 'ok':
                    return { text: i18n.t('modbus-master-task-ok') || 'OK', className: 'is-ok', cardClass: '' };
                case 'stale':
                    return { text: i18n.t('modbus-master-task-stale') || 'STALE', className: 'is-stale', cardClass: 'is-stale' };
                case 'error':
                    return { text: i18n.t('modbus-master-task-error') || 'ERROR', className: 'is-error', cardClass: 'is-error' };
                case 'disabled':
                    return { text: i18n.t('modbus-master-task-disabled') || 'OFF', className: 'is-pending', cardClass: 'is-pending' };
                default:
                    return { text: i18n.t('modbus-master-task-pending') || 'PENDING', className: 'is-pending', cardClass: 'is-pending' };
            }
        },

        _formatMasterAge(ageSec) {
            var age = Number(ageSec || 0);
            if (!age) return '--';
            if (age < 60) return age + 's';
            if (age < 3600) return Math.floor(age / 60) + 'm';
            return Math.floor(age / 3600) + 'h';
        },

        _formatMasterPercent(value) {
            var num = Number(value || 0);
            if (!isFinite(num)) num = 0;
            return (Math.round(num * 10) / 10) + '%';
        },

        _renderMasterHealth(health) {
            var warningsEl = document.getElementById('master-health-warnings');
            var riskBadge = document.getElementById('master-risk-badge');
            if (!warningsEl || !riskBadge) return;

            if (!health) {
                riskBadge.className = 'modbus-risk-badge modbus-risk-low';
                riskBadge.textContent = this._getMasterRiskMeta('low').text;
                this._setText('master-enabled-task-count', 0);
                this._setText('master-min-interval', '--');
                this._setText('master-last-poll-age', '--');
                this._setText('master-timeout-rate', '0%');
                warningsEl.classList.add('is-hidden');
                warningsEl.innerHTML = '';
                return;
            }

            var riskMeta = this._getMasterRiskMeta(health.riskLevel);
            riskBadge.className = 'modbus-risk-badge ' + riskMeta.className;
            riskBadge.textContent = riskMeta.text;

            this._setText('master-enabled-task-count', health.enabledTaskCount ?? 0);
            this._setText('master-min-interval', health.minPollInterval ? (health.minPollInterval + 's') : '--');
            this._setText('master-last-poll-age', this._formatMasterAge(health.lastPollAgeSec));
            this._setText('master-timeout-rate', this._formatMasterPercent(health.timeoutRate));

            var warnings = Array.isArray(health.warnings) ? health.warnings : [];
            warningsEl.innerHTML = warnings.map(function(msg) {
                return '<div class="modbus-health-warning"><i class="fas fa-exclamation-triangle"></i><span>' +
                    escapeHtml(msg || '') + '</span></div>';
            }).join('');

            warningsEl.classList.toggle('is-hidden', warnings.length === 0);
        },

        _buildMasterDataCard(name, value, task) {
            var statusMeta = this._getMasterTaskStatusMeta(task.status);
            var metaText = task.cacheAgeSec ? this._formatMasterAge(task.cacheAgeSec) :
                (task.pollInterval ? (task.pollInterval + 's') : '--');
            if (task.status === 'error' && task.lastError) {
                metaText = 'E' + task.lastError + ' / ' + metaText;
            }

            return '<div class="master-data-item ' + statusMeta.cardClass + '">' +
                '<span class="master-data-name">' + escapeHtml(name) + '</span>' +
                '<span class="master-data-val">' + escapeHtml(value) + '</span>' +
                '<span class="master-data-status ' + statusMeta.className + '">' + statusMeta.text + '</span>' +
                '<span class="master-data-meta">' + escapeHtml(metaText) + '</span>' +
                '</div>';
        },

        refreshMasterStatus() {
            apiGet('/api/modbus/status')
                .then(res => {
                    if (!res || !res.success || !res.data) {
                        this._renderMasterHealth(null);
                        this._renderMasterDataGrid([]);
                        return;
                    }
                    this._updateModbusStatusUI(res.data);
                })
                .catch(() => {
                    this._renderMasterHealth(null);
                    this._renderMasterDataGrid([]);
                });
        },

        _updateModbusStatusUI: function(data) {
            const d = data || {};
            this._setText('master-stat-total', d.totalPolls ?? 0);
            this._setText('master-stat-success', d.successPolls ?? 0);
            this._setText('master-stat-failed', d.failedPolls ?? 0);
            this._setText('master-stat-timeout', d.timeoutPolls ?? 0);
            this._renderMasterHealth(d.health || null);
            if (d.tasks) this._renderMasterDataGrid(d.tasks);
        },

        _startMasterStatusRefresh() {
            this._stopMasterStatusRefresh();

            // 使用全局 SSE 监听 modbus-status 事件
            AppState.connectSSE();
            var self = this;
            this._modbusStatusSSEHandler = function(e) {
                try {
                    var data = JSON.parse(e.data);
                    self._updateModbusStatusUI(data);
                } catch (err) {
                    console.warn('[SSE] Modbus 状态解析失败:', err);
                }
            };
            AppState.onSSEEvent('modbus-status', this._modbusStatusSSEHandler);

            // 初始加载一次
            this.refreshMasterStatus();
        },

        _stopMasterStatusRefresh() {
            if (this._masterStatusTimer) {
                clearInterval(this._masterStatusTimer);
                this._masterStatusTimer = null;
            }
            if (this._modbusStatusSSEHandler) {
                AppState.offSSEEvent('modbus-status', this._modbusStatusSSEHandler);
                this._modbusStatusSSEHandler = null;
            }
        },

        _renderMasterDataGrid(tasks) {
            var grid = document.getElementById('master-data-grid');
            if (!grid) return;
            var html = '';
            for (var i = 0; i < (tasks || []).length; i++) {
                var t = tasks[i];
                if (!t.enabled) continue;
                if (t.mappings && t.mappings.length > 0) {
                    // 按 regOffset 排序 mappings，确保显示顺序正确
                    var sortedMappings = t.mappings.slice().sort(function(a, b) {
                        return (a.regOffset || 0) - (b.regOffset || 0);
                    });
                    for (var j = 0; j < sortedMappings.length; j++) {
                        var m = sortedMappings[j];
                        var rawVal = null;
                        if (t.cachedData && t.cachedData.values && m.regOffset < t.cachedData.values.length) {
                            switch (m.dataType) {
                                case 0: rawVal = t.cachedData.values[m.regOffset]; break;
                                case 1: rawVal = t.cachedData.values[m.regOffset]; if (rawVal > 32767) rawVal -= 65536; break;
                                case 2: if (m.regOffset + 1 < t.cachedData.values.length) rawVal = (t.cachedData.values[m.regOffset] << 16) | t.cachedData.values[m.regOffset + 1]; break;
                                case 3: if (m.regOffset + 1 < t.cachedData.values.length) { rawVal = (t.cachedData.values[m.regOffset] << 16) | t.cachedData.values[m.regOffset + 1]; if (rawVal > 2147483647) rawVal -= 4294967296; } break;
                                default: rawVal = t.cachedData.values[m.regOffset]; break;
                            }
                        }
                        var displayVal = '--';
                        if (rawVal !== null) {
                            var scaled = rawVal * (m.scaleFactor || 1);
                            displayVal = scaled.toFixed(m.decimalPlaces || 0);
                        }
                        html += this._buildMasterDataCard(
                            m.sensorId || ('R' + (t.startAddress + m.regOffset)),
                            String(displayVal),
                            t
                        );
                    }
                } else if (t.cachedData && t.cachedData.values && t.cachedData.values.length) {
                    for (var k = 0; k < t.cachedData.values.length; k++) {
                        html += this._buildMasterDataCard(
                            'R' + (t.startAddress + k),
                            String(t.cachedData.values[k]),
                            t
                        );
                    }
                } else {
                    html += this._buildMasterDataCard(t.name || t.label || ('Task ' + (i + 1)), '--', t);
                }
            }
            grid.innerHTML = html;
            grid.classList.toggle('is-hidden', !html);
        },

    });
})();
