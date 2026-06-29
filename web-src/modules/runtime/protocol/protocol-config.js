/**
 * protocol/protocol-config.js — 协议配置加载/保存、Master 状态刷新
 */
(function() {
    Object.assign(AppState, {

        // ============ 协议配置 ============

        // Tab ID → 分片文件名和容器 ID 的映射
        _protocolFragmentMap: {
            'mqtt': { container: 'mqtt-fragment-container', fragment: 'protocol-mqtt' },
            'modbus-rtu': { container: 'modbus-rtu-fragment-container', fragment: 'protocol-modbus-rtu' }
        },

        loadProtocolConfig(tabId, options) {
            var self = this;
            if (tabId === 'modbus-rtu' &&
                typeof ModuleLoader !== 'undefined' &&
                ModuleLoader &&
                typeof ModuleLoader.isLoaded === 'function' &&
                !ModuleLoader.isLoaded('protocol-modbus-rtu')) {
                if (typeof this._setProtocolFragmentLoading === 'function') {
                    this._setProtocolFragmentLoading(tabId);
                }
                return new Promise(function(resolve, reject) {
                    var timer = setTimeout(function() {
                        if (typeof self._setProtocolFragmentError === 'function') {
                            self._setProtocolFragmentError(tabId);
                        }
                        reject(new Error('Modbus RTU module load timeout'));
                    }, 30000);
                    ModuleLoader.loadModule('protocol-modbus-rtu', function() {
                        clearTimeout(timer);
                        Promise.resolve(self.loadProtocolConfig(tabId, options)).then(resolve, reject);
                    });
                });
            }

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
            if (tabId === 'modbus-rtu' && typeof this._updateDelayChannelSelect === 'function') {
                this._updateDelayChannelSelect();
            }

            // 按需加载分片 HTML，加载完成后再填充表单
            var fragInfo = this._protocolFragmentMap[tabId];
            if (fragInfo) {
                if (typeof this._setProtocolFragmentLoading === 'function') {
                    this._setProtocolFragmentLoading(tabId);
                }
                return PageLoader.loadFragment(fragInfo.container, fragInfo.fragment).then(function() {
                    // 分片加载完成后重新尝试绑定事件（新 DOM 元素可能需要）
                    self.setupProtocolEvents();
                    self.applyDeveloperModeState();
                    return self._loadProtocolConfigData(tabId, options);
                });
            }
            return this._loadProtocolConfigData(tabId, options);
        },

        _hasProtocolConfigSection(tabId, config) {
            if (!config) return false;
            if (tabId === 'mqtt') return !!config.mqtt;
            if (tabId === 'modbus-rtu') return !!config.modbusRtu;
            return true;
        },

        _loadProtocolConfigData(tabId, options) {
            if (options && options.noCache === true) {
                this._protocolConfig = null;
            }
            if (this._hasProtocolConfigSection(tabId, this._protocolConfig)) {
                this._fillProtocolForm(tabId, this._protocolConfig, options);
                return Promise.resolve(this._protocolConfig);
            }
            var getter = (options && options.noCache === true && typeof apiGetFresh === 'function') ? apiGetFresh : apiGet;
            var params = undefined;
            if (tabId === 'mqtt') {
                params = { compact: 1, section: 'mqtt' };
            } else if (tabId === 'modbus-rtu') {
                params = { compact: 1, section: 'runtime' };
            }
            return getter('/api/protocol/config', params)
                .then(res => {
                    if (!res || !res.success) return null;
                    this._protocolConfig = res.data || {};
                    this._fillProtocolForm(tabId, this._protocolConfig, options);
                    return this._protocolConfig;
                })
                .catch(err => {
                    console.error('加载协议配置失败:', err);
                    throw err;
                });
        },

        _fillProtocolForm(tabId, config, options) {
            if (tabId === 'modbus-rtu' && config.modbusRtu) {
                const rtu = config.modbusRtu;
                this._setCheckbox('modbus-rtu-enabled', rtu.enabled ?? true);
                this._loadUartPeripherals(rtu.peripheralId || '', options);
                this._setValue('rtu-de-pin', rtu.dePin ?? 14);
                this._setValue('rtu-transfer-type', rtu.transferType ?? 0);
                // Master 高级参数
                if (rtu.master) {
                    this._setValue('rtu-response-timeout', rtu.master.responseTimeout ?? 1000);
                    this._setValue('rtu-max-retries', rtu.master.maxRetries ?? 2);
                    this._setValue('rtu-inter-poll-delay', rtu.master.interPollDelay ?? 100);
                }
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
                this.refreshMasterStatus({ noCache: options && options.noCache === true });
                this._startMasterStatusRefresh();
                if (typeof this._updateDelayChannelSelect === 'function') {
                    this._updateDelayChannelSelect();
                }
                this.applyDeveloperModeState();
            }
            if (tabId === 'mqtt' && config.mqtt) {
                const mqtt = config.mqtt;
                this._setCheckbox('mqtt-enabled', mqtt.enabled ?? true);
                this._setValue('mqtt-scheme', mqtt.scheme || 'mqtt');
                // 无 PSRAM 设备选择 mqtts 时在状态信息后追加内存提示
                var schemeEl = document.getElementById('mqtt-scheme');
                if (schemeEl) {
                    this._updateMqttsMemoryHint(mqtt.tlsSupported, schemeEl.value);
                }
                this._setValue('mqtt-broker', mqtt.server ?? '');
                this._setValue('mqtt-port', mqtt.port || 1883);
                this._setValue('mqtt-client-id', mqtt.clientId ?? '');
                this._setValue('mqtt-username', mqtt.username || '');
                this._setValue('mqtt-password', mqtt.password || '');
                this._setValue('mqtt-alive', mqtt.keepAlive || 60);
                this._setCheckbox('mqtt-auto-reconnect', mqtt.autoReconnect ?? true);
                this._setValue('mqtt-will-topic', mqtt.willTopic || '');
                this._setValue('mqtt-will-payload', mqtt.willPayload || '');
                this._setValue('mqtt-will-qos', mqtt.willQos ?? 0);
                this._setCheckbox('mqtt-will-retain', mqtt.willRetain ?? false);
                this._setValue('mqtt-longitude', mqtt.longitude ?? 0);
                this._setValue('mqtt-latitude', mqtt.latitude ?? 0);
                this._setValue('mqtt-iccid', mqtt.iccid || '');
                this._setValue('mqtt-card-platform-id', mqtt.cardPlatformId ?? 0);
                this._setValue('mqtt-summary', mqtt.summary ?? '{"name":"fastbee","chip":"ESP32"}');
                this._setValue('mqtt-auth-type', mqtt.authType ?? 0);
                this._setValue('mqtt-secret', mqtt.mqttSecret ?? '');
                this._setValue('mqtt-auth-code', mqtt.authCode || '');
                this._loadMqttPublishTopics(mqtt.publishTopics || []);
                this._loadMqttSubscribeTopics(mqtt.subscribeTopics || []);
                // 重置自动重连标志，确保切换页面后首次状态检测能正确触发
                this._mqttAutoReconnectTriggered = false;
                // MQTT 启用时自动开始状态轮询，确保顶部状态指示器反映实际连接状态
                if (mqtt.enabled) {
                    this._startMqttStatusPolling({ initialDelayMs: 1500 });
                } else {
                    // MQTT 未启用时，直接显示"未连接"，不进行状态检测
                    this._stopMqttStatusPolling();
                    var badge = document.getElementById('mqtt-status-badge');
                    if (badge) {
                        badge.className = 'mqtt-status-badge mqtt-status-offline';
                        badge.textContent = '未连接';
                    }
                }
            }
        },

        /**
         * 存储 tlsSupported 和当前 scheme，供状态 badge 追加内存提示
         */
        _updateMqttsMemoryHint(tlsSupported, currentScheme) {
            this._mqttTlsSupported = tlsSupported;
            this._mqttCurrentScheme = currentScheme;

            // 绑定 scheme change 事件（仅首次）
            const schemeEl = document.getElementById('mqtt-scheme');
            if (schemeEl && !this._mqttSchemeBound) {
                schemeEl.addEventListener('change', () => {
                    this._mqttCurrentScheme = schemeEl.value;
                });
                this._mqttSchemeBound = true;
            }
        },

        _getProtocolSaveEndpoint(formId) {
            if (formId === 'mqtt-form') return '/api/protocol/mqtt/config';
            if (formId === 'modbus-rtu-form') return '/api/protocol/modbus-rtu/config';
            return '/api/protocol/config';
        },

        saveProtocolConfig(formId) {
            const data = {};
            const isModbusForm = formId === 'modbus-rtu-form' || (!formId && document.getElementById('modbus-rtu-enabled'));
            const isMqttForm = formId === 'mqtt-form' || (!formId && document.getElementById('mqtt-enabled'));
            if (isModbusForm && !this.guardDeveloperModeAction()) return;
            if (isModbusForm) {
                data.modbusRtu_enabled = document.getElementById('modbus-rtu-enabled')?.checked ? 'true' : 'false';
                data.modbusRtu_peripheralId = document.getElementById('rtu-peripheral-id')?.value || '';
                data.modbusRtu_mode = 'master';
                data.modbusRtu_dePin = document.getElementById('rtu-de-pin')?.value || '14';
                data.modbusRtu_transferType = document.getElementById('rtu-transfer-type')?.value || '0';
                // Master 高级参数
                data.modbusRtu_responseTimeout = document.getElementById('rtu-response-timeout')?.value || '1000';
                data.modbusRtu_maxRetries = document.getElementById('rtu-max-retries')?.value || '2';
                data.modbusRtu_interPollDelay = document.getElementById('rtu-inter-poll-delay')?.value || '100';
                if (data.modbusRtu_enabled === 'true' && !data.modbusRtu_peripheralId) {
                    Notification.warning('未找到已启用的UART外设，请先在外设管理中配置');
                    return;
                }
                data.modbusRtu_master_tasks = JSON.stringify(this._masterTasks || []);
                data.modbusRtu_master_devices = JSON.stringify(this._modbusDevices || []);
            }
            if (isMqttForm) {
                data.mqtt_enabled = document.getElementById('mqtt-enabled')?.checked ? 'true' : 'false';
                data.mqtt_scheme = document.getElementById('mqtt-scheme')?.value || 'mqtt';
                data.mqtt_server = document.getElementById('mqtt-broker')?.value ?? '';
                data.mqtt_port = document.getElementById('mqtt-port')?.value || '1883';
                data.mqtt_clientId = document.getElementById('mqtt-client-id')?.value || '';
                data.mqtt_username = document.getElementById('mqtt-username')?.value || '';
                data.mqtt_password = document.getElementById('mqtt-password')?.value || '';
                data.mqtt_keepAlive = document.getElementById('mqtt-alive')?.value || '60';
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
            }
            const protocolName = this._getProtocolName(formId);
            return apiPost(this._getProtocolSaveEndpoint(formId), data, 30000)
                .then(res => {
                    if (res && res.success) {
                        this._protocolConfig = null;
                        if (res.data && typeof res.data.mqttReconnected !== 'undefined') {
                            if (res.data.mqttReconnected && res.data.mqttDeferred) {
                                Notification.success('MQTT已重新连接', '通信协议');
                            } else if (res.data.mqttReconnected) {
                                Notification.success('MQTT已重新连接', '通信协议');
                            } else if (res.data.mqttDisconnected) {
                                Notification.success('MQTT已断开连接', '通信协议');
                            } else if (data.mqtt_enabled === 'true') {
                                const errCode = res.data.mqttError || '';
                                const errMsg = errCode ? this._mqttErrorCodeToText(errCode) : '';
                                Notification.warning(
                                    'MQTT重连失败' + (errMsg ? ' (' + errMsg + ')' : ''),
                                    '通信协议'
                                );
                            }
                        }
                        Notification.success(`${protocolName} 配置保存成功！`, '通信协议');
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
                                // 仅当 MQTT 已启用时才启动状态轮询
                                // 禁用时启动轮询会导致后端返回 connected=true（重启前窗口期）
                                var mqttEnabledCb = document.getElementById('mqtt-enabled');
                                if (mqttEnabledCb && mqttEnabledCb.checked) {
                                    this._startMqttStatusPolling({ initialDelayMs: 2000 });
                                } else {
                                    this._stopMqttStatusPolling();
                                    var badge = document.getElementById('mqtt-status-badge');
                                    if (badge) {
                                        badge.className = 'mqtt-status-badge mqtt-status-offline';
                                        badge.textContent = '未连接';
                                    }
                                }
                            }
                        }
                    } else {
                        Notification.error(res?.message || '保存失败', '通信协议配置');
                    }
                })
                .catch(err => {
                    console.error('saveProtocolConfig error:', err);
                    if (typeof window.apiNotifyError === 'function') {
                        window.apiNotifyError(err, '保存失败', '通信协议配置');
                        return;
                    }
                    if (err && err.name === 'AbortError') {
                        Notification.error('保存超时，设备可能正忙，请稍后重试', '通信协议配置');
                    } else if (err && err._pageAborted) {
                        // 页面切换导致取消，静默忽略
                    } else if (err instanceof TypeError && /fetch/i.test(err.message)) {
                        Notification.error('设备连接失败，请检查网络后重试', '通信协议配置');
                    } else if (err && err.status === 504) {
                        Notification.error('设备响应超时(504)，请稍后重试', '通信协议配置');
                    } else {
                        Notification.error('保存失败', '通信协议配置');
                    }
                });
        },

        // ============ Master 状态刷新 ============

        _getMasterRiskMeta(level) {
            switch ((level || 'low').toLowerCase()) {
                case 'high':
                    return { text: '高', className: 'modbus-risk-high' };
                case 'medium':
                    return { text: '中', className: 'modbus-risk-medium' };
                default:
                    return { text: '低', className: 'modbus-risk-low' };
            }
        },

        _getMasterTaskStatusMeta(status) {
            switch ((status || 'pending').toLowerCase()) {
                case 'ok':
                    return { text: 'OK', className: 'is-ok', cardClass: '' };
                case 'stale':
                    return { text: '过时', className: 'is-stale', cardClass: 'is-stale' };
                case 'error':
                    return { text: '错误', className: 'is-error', cardClass: 'is-error' };
                case 'disabled':
                    return { text: '禁用', className: 'is-pending', cardClass: 'is-pending' };
                default:
                    return { text: '等待中', className: 'is-pending', cardClass: 'is-pending' };
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
                return '<div class="modbus-health-warning"><span>' +
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

        refreshMasterStatusFresh() {
            return this.refreshMasterStatus({ noCache: true });
        },

        refreshMasterStatus(options) {
            var getter = (options && options.noCache === true && typeof apiGetFresh === 'function') ? apiGetFresh : apiGet;
            return getter('/api/modbus/status?full=1')
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
