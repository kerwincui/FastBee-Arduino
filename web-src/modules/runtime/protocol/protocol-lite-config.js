/**
 * Lightweight protocol loader for the default MQTT tab.
 * Heavy Modbus/save helpers are loaded only when the user needs them.
 */
(function() {
    Object.assign(AppState, {
        _protocolFragmentMap: {
            'mqtt': { container: 'mqtt-fragment-container', fragment: 'protocol-mqtt' },
            'modbus-rtu': { container: 'modbus-rtu-fragment-container', fragment: 'protocol-modbus-rtu' }
        },

        _loadFullProtocolConfig: function(tabId, options) {
            var self = this;
            var liteLoader = this.loadProtocolConfig;
            var sequence = tabId === 'modbus-rtu' ? 'protocol-modbus-rtu' : 'protocol-full-config';
            if (typeof this._setProtocolFragmentLoading === 'function') {
                this._setProtocolFragmentLoading(tabId);
            }
            if (typeof ModuleLoader !== 'undefined' &&
                ModuleLoader &&
                typeof ModuleLoader.loadModule === 'function' &&
                (!ModuleLoader.isLoaded || !ModuleLoader.isLoaded(sequence))) {
                return new Promise(function(resolve, reject) {
                    var timer = setTimeout(function() {
                        if (typeof self._setProtocolFragmentError === 'function') {
                            self._setProtocolFragmentError(tabId);
                        }
                        reject(new Error('Protocol module load timeout'));
                    }, 30000);
                    ModuleLoader.loadModule(sequence, function() {
                        clearTimeout(timer);
                        if (self.loadProtocolConfig === liteLoader) {
                            if (typeof self._setProtocolFragmentError === 'function') {
                                self._setProtocolFragmentError(tabId);
                            }
                            reject(new Error('Protocol module did not register'));
                            return;
                        }
                        Promise.resolve(self.loadProtocolConfig(tabId, options)).then(resolve, reject);
                    });
                });
            }
            if (this.loadProtocolConfig !== liteLoader) {
                return this.loadProtocolConfig(tabId, options);
            }
            return Promise.reject(new Error('Protocol module loader unavailable'));
        },

        loadProtocolConfig: function(tabId, options) {
            if (tabId !== 'mqtt') {
                return this._loadFullProtocolConfig(tabId, options);
            }

            var self = this;
            if (!this._protocolEventsBound) {
                this.setupProtocolEvents();
            }
            if (typeof this._stopMasterStatusRefresh === 'function') {
                this._stopMasterStatusRefresh();
            }
            if (this._coilAutoRefreshTimer) {
                clearInterval(this._coilAutoRefreshTimer);
                this._coilAutoRefreshTimer = null;
            }

            var fragInfo = this._protocolFragmentMap[tabId];
            if (fragInfo) {
                if (typeof this._setProtocolFragmentLoading === 'function') {
                    this._setProtocolFragmentLoading(tabId);
                }
                return PageLoader.loadFragment(fragInfo.container, fragInfo.fragment).then(function() {
                    self.setupProtocolEvents();
                    return self._loadProtocolConfigData(tabId, options);
                });
            }
            return this._loadProtocolConfigData(tabId, options);
        },

        _loadProtocolConfigData: function(tabId, options) {
            if (tabId !== 'mqtt') {
                return this._loadFullProtocolConfig(tabId, options);
            }
            if (options && options.noCache === true) {
                this._protocolLiteConfig = null;
            }
            if (this._protocolLiteConfig) {
                this._fillProtocolForm(tabId, this._protocolLiteConfig, options);
                return Promise.resolve(this._protocolLiteConfig);
            }
            var getter = (options && options.noCache === true && typeof apiGetFresh === 'function') ? apiGetFresh : apiGet;
            return getter('/api/protocol/config', { compact: 1, section: 'mqtt' })
                .then(res => {
                    if (!res || !res.success) return null;
                    this._protocolLiteConfig = res.data || {};
                    this._fillProtocolForm(tabId, this._protocolLiteConfig, options);
                    return this._protocolLiteConfig;
                })
                .catch(err => {
                    console.error('load MQTT protocol config failed:', err);
                    throw err;
                });
        },

        _fillProtocolForm: function(tabId, config) {
            if (tabId !== 'mqtt') return;
            var mqtt = config && config.mqtt;
            if (!mqtt) return;
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
            // 重置自动重连标志，确保切换页面后首次状态检测能正确触发
            this._mqttAutoReconnectTriggered = false;
            // MQTT 启用时做有限次状态轮询（不用SSE持久连接，避免耗尽ESP32 socket池）
            // 使用递归setTimeout而非定时器，轮询10次后自动停止
            // 覆盖窗口：3s起始 + 9*5s = 48s，充分覆盖MQTT 30-40s连接时间
            if (mqtt.enabled !== false && typeof this._loadMqttStatus === 'function') {
                var self = this;
                self._mqttLitePollCount = 0;
                self._mqttConnectingStartTime = 0; // 重置超时计时，防止旧值干扰
                var mqttMaxPolls = 10;
                function mqttPollOnce() {
                    // 检查是否已被取消（页面离开时 _stopMqttStatusPolling 会清除标志）
                    if (self._mqttLitePollStopped) return;
                    // 若前次轮询已检测到连接成功，立即停止后续轮询
                    var badge = document.getElementById('mqtt-status-badge');
                    if (badge && badge.classList.contains('mqtt-status-online')) {
                        return;
                    }
                    self._loadMqttStatus();
                    self._mqttLitePollCount++;
                    if (self._mqttLitePollCount < mqttMaxPolls && !self._mqttLitePollStopped) {
                        self._mqttLitePollTimer = setTimeout(mqttPollOnce, 5000);
                    } else if (self._mqttLitePollCount >= mqttMaxPolls) {
                        // 所有轮询耗尽，等待最后一次API响应后做最终判断
                        self._mqttLitePollTimer = setTimeout(function() {
                            if (self._mqttLitePollStopped) return;
                            var finalBadge = document.getElementById('mqtt-status-badge');
                            if (finalBadge && !finalBadge.classList.contains('mqtt-status-online')) {
                                if (finalBadge.classList.contains('mqtt-status-connecting')) {
                                    finalBadge.className = 'mqtt-status-badge mqtt-status-offline';
                                    finalBadge.textContent = '连接超时';
                                }
                            }
                        }, 3000);
                    }
                }
                self._mqttLitePollStopped = false;
                self._mqttLitePollTimer = setTimeout(mqttPollOnce, 3000);
            }
        },

        saveProtocolConfig: function(formId) {
            var self = this;
            return this._loadFullProtocolConfig('mqtt').then(function() {
                return self.saveProtocolConfig(formId);
            });
        }
    });
})();
