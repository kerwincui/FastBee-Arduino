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

        /**
         * 无 PSRAM 设备选择 mqtts 时显示内存不足提示
         * @param {boolean} tlsSupported 后端返回的 TLS 支持状态
         * @param {string} currentScheme 当前选中的协议
         */
        _updateMqttsMemoryHint: function(tlsSupported, currentScheme) {
            var schemeEl = document.getElementById('mqtt-scheme');
            if (!schemeEl) return;

            // 存储 tlsSupported 供后续事件使用
            this._mqttTlsSupported = tlsSupported;

            // 使用 HTML 中已有的 span#mqtts-memory-hint（位于底部按钮组右侧）
            var hintEl = document.getElementById('mqtts-memory-hint');
            if (hintEl) {
                if (tlsSupported === false && currentScheme === 'mqtts') {
                    hintEl.textContent = '\u26a0\ufe0f \u6b64\u8bbe\u5907\u5185\u5b58\u6709\u9650\uff0cmqtts(TLS)\u8fde\u63a5\u53ef\u80fd\u56e0\u5185\u5b58\u4e0d\u8db3\u800c\u5931\u8d25\u3002\u5982\u679c\u8fde\u63a5\u5931\u8d25\uff0c\u5efa\u8bae\u5207\u6362\u4e3a mqtt\u3002';
                    hintEl.style.display = '';
                } else {
                    hintEl.style.display = 'none';
                }
            }

            // 绑定 change 事件（仅首次）
            if (!this._mqttSchemeBound) {
                var self = this;
                schemeEl.addEventListener('change', function() {
                    self._updateMqttsMemoryHint(self._mqttTlsSupported, this.value);
                });
                this._mqttSchemeBound = true;
            }
        },

        _fillProtocolForm: function(tabId, config) {
            if (tabId !== 'mqtt') return;
            var mqtt = config && config.mqtt;
            if (!mqtt) return;
            this._setCheckbox('mqtt-enabled', mqtt.enabled ?? true);
            this._setValue('mqtt-scheme', mqtt.scheme || 'mqtt');
            // 无 PSRAM 设备选择 mqtts 时显示内存不足提示
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
            } else if (mqtt.enabled === false) {
                // MQTT 未启用时，直接显示"未连接"，不进行状态检测
                if (typeof this._stopMqttStatusPolling === 'function') {
                    this._stopMqttStatusPolling();
                }
                var badge = document.getElementById('mqtt-status-badge');
                if (badge) {
                    badge.className = 'mqtt-status-badge mqtt-status-offline';
                    badge.textContent = '未连接';
                }
            }
        },

        saveProtocolConfig: function(formId) {
            var self = this;
            // 快照当前表单值：_loadFullProtocolConfig 会从服务器重新填充表单，
            // 覆盖用户正在编辑的数据（尤其是主题列表），必须先保存再恢复
            var snapshot = {};
            var form = document.getElementById('mqtt-form');
            if (form) {
                var inputs = form.querySelectorAll('input, select, textarea');
                for (var i = 0; i < inputs.length; i++) {
                    var el = inputs[i];
                    var key = el.id || el.className;
                    if (!key) continue;
                    if (el.type === 'checkbox') {
                        snapshot[key] = { type: 'checkbox', checked: el.checked };
                    } else {
                        snapshot[key] = { type: 'value', value: el.value };
                    }
                }
                // 快照动态主题容器的完整 HTML
                var pubTopics = document.getElementById('mqtt-publish-topics');
                var subTopics = document.getElementById('mqtt-subscribe-topics');
                snapshot['__publishTopicsHTML'] = pubTopics ? pubTopics.innerHTML : '';
                snapshot['__subscribeTopicsHTML'] = subTopics ? subTopics.innerHTML : '';
            }
            return this._loadFullProtocolConfig('mqtt').then(function() {
                // 恢复用户编辑的表单值
                if (form) {
                    var inputs = form.querySelectorAll('input, select, textarea');
                    for (var i = 0; i < inputs.length; i++) {
                        var el = inputs[i];
                        var key = el.id || el.className;
                        if (!key || !snapshot[key]) continue;
                        if (el.type === 'checkbox') {
                            el.checked = snapshot[key].checked;
                        } else {
                            el.value = snapshot[key].value;
                        }
                    }
                    // 恢复动态主题容器
                    if (snapshot['__publishTopicsHTML'] !== undefined && pubTopics) {
                        pubTopics.innerHTML = snapshot['__publishTopicsHTML'];
                    }
                    if (snapshot['__subscribeTopicsHTML'] !== undefined && subTopics) {
                        subTopics.innerHTML = snapshot['__subscribeTopicsHTML'];
                    }
                }
                return self.saveProtocolConfig(formId);
            });
        }
    });
})();
