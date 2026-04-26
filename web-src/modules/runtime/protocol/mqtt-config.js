/**
 * protocol/mqtt-config.js — MQTT 主题配置管理 + 连接测试/状态/断开/NTP
 */
(function() {
    Object.assign(AppState, {

        // ============ MQTT 发布主题配置 ============

        _mqttTopicTypeOptions(selected) {
            const types = [
                { value: 0, key: 'mqtt-topic-type-data-report' },
                { value: 1, key: 'mqtt-topic-type-data-command' },
                { value: 2, key: 'mqtt-topic-type-device-info' },
                { value: 3, key: 'mqtt-topic-type-realtime-mon' },
                { value: 4, key: 'mqtt-topic-type-device-event' },
                { value: 5, key: 'mqtt-topic-type-ota-upgrade' },
                { value: 6, key: 'mqtt-topic-type-ota-binary' },
                { value: 7, key: 'mqtt-topic-type-ntp-sync' }
            ];
            return types.map(t =>
                `<option value="${t.value}" ${Number(selected) === t.value ? 'selected' : ''}>${i18n.t(t.key)}</option>`
            ).join('');
        },

        _loadMqttPublishTopics(topics) {
            const container = document.getElementById('mqtt-publish-topics');
            if (!container) return;
            container.innerHTML = '';
            if (!topics || topics.length === 0) {
                topics = [{ topic: '', qos: 0, retain: false, enabled: true, autoPrefix: false, topicType: 0 }];
            }
            topics.forEach((topic, index) => {
                this._createMqttPublishTopicElement(topic, index);
            });
        },

        _buildMqttTopicItemHtml(index, topicData, options) {
            const topicType = topicData.topicType ?? options.defaultTopicType;
            const qos = topicData.qos ?? 0;
            const toggleHtml = options.toggles.map((toggle) => {
                const checked = toggle.checked ? ' checked' : '';
                return '<label class="fb-checkbox">' +
                    '<input type="checkbox" class="' + toggle.inputClass + '"' + checked + '> ' + i18n.t(toggle.labelKey) +
                    '</label>';
            }).join('');

            return `
                <span class="${options.indexClass}">${index + 1}</span>
                <button type="button" class="mqtt-topic-delete">${i18n.t('mqtt-delete-topic-btn')}</button>
                <div class="config-form-grid">
                    <div class="fb-form-group">
                        <label>${i18n.t(options.topicLabelKey)}</label>
                        <input type="text" class="${options.topicInputClass}" value="${topicData.topic || ''}" placeholder="/topic/path">
                    </div>
                    <div class="fb-form-group">
                        <label>${i18n.t(options.topicTypeLabelKey)}</label>
                        <select class="${options.topicTypeInputClass}">
                            ${this._mqttTopicTypeOptions(topicType)}
                        </select>
                    </div>
                    <div class="fb-form-group">
                        <label>${i18n.t(options.qosLabelKey)}</label>
                        <select class="${options.qosInputClass}">
                            ${this._renderMqttQosOptions(qos)}
                        </select>
                    </div>
                    <div class="fb-form-group mqtt-topic-toggle-row">
                        ${toggleHtml}
                    </div>
                </div>
            `;
        },

        _createMqttTopicElement(containerId, topicData, index, options) {
            const container = document.getElementById(containerId);
            if (!container) return;
            const div = document.createElement('div');
            div.className = options.itemClass;
            div.dataset.index = index;
            div.innerHTML = this._buildMqttTopicItemHtml(index, topicData, options);
            container.appendChild(div);
        },

        _reindexMqttTopicItems(container) {
            const remainingItems = container.querySelectorAll('.mqtt-topic-item');
            remainingItems.forEach((item, idx) => {
                item.dataset.index = idx;
                const indexSpan = item.querySelector('.mqtt-topic-index');
                if (indexSpan) indexSpan.textContent = idx + 1;
            });
            return remainingItems.length;
        },

        _collectMqttTopics(containerId, options) {
            const container = document.getElementById(containerId);
            if (!container) return [];
            const topics = [];
            const items = container.querySelectorAll('.mqtt-topic-item');
            items.forEach((item) => {
                const topicInput = item.querySelector('.' + options.topicInputClass);
                const qosInput = item.querySelector('.' + options.qosInputClass);
                const enabledInput = item.querySelector('.' + options.enabledInputClass);
                const autoPrefixInput = item.querySelector('.' + options.autoPrefixInputClass);
                const topicTypeInput = item.querySelector('.' + options.topicTypeInputClass);
                if (!topicInput) return;
                const topic = {
                    topic: topicInput.value || '',
                    qos: parseInt(qosInput?.value || '0'),
                    enabled: enabledInput?.checked !== false,
                    autoPrefix: autoPrefixInput?.checked || false,
                    topicType: parseInt(topicTypeInput?.value || String(options.defaultTopicType))
                };
                if (options.retainInputClass) {
                    topic.retain = item.querySelector('.' + options.retainInputClass)?.checked || false;
                }
                topics.push(topic);
            });
            return topics;
        },

        _getMqttPublishTopicOptions(topicData) {
            return {
                itemClass: 'mqtt-topic-item',
                indexClass: 'mqtt-topic-index',
                topicLabelKey: 'mqtt-publish-label',
                topicInputClass: 'mqtt-topic-input',
                topicTypeLabelKey: 'mqtt-topic-type-label',
                topicTypeInputClass: 'mqtt-topic-type-input',
                qosLabelKey: 'mqtt-publish-qos-label',
                qosInputClass: 'mqtt-qos-input',
                enabledInputClass: 'mqtt-enabled-input',
                autoPrefixInputClass: 'mqtt-autoprefix-input',
                retainInputClass: 'mqtt-retain-input',
                defaultTopicType: 0,
                toggles: [
                    { inputClass: 'mqtt-retain-input', checked: topicData.retain === true, labelKey: 'mqtt-publish-retain-label' },
                    { inputClass: 'mqtt-enabled-input', checked: topicData.enabled !== false, labelKey: 'mqtt-topic-enabled-label' },
                    { inputClass: 'mqtt-autoprefix-input', checked: topicData.autoPrefix === true, labelKey: 'mqtt-auto-prefix-label' }
                ]
            };
        },

        _getMqttSubscribeTopicOptions(topicData) {
            return {
                itemClass: 'mqtt-topic-item mqtt-topic-item-sub',
                indexClass: 'mqtt-topic-index mqtt-topic-index-sub',
                topicLabelKey: 'mqtt-subscribe-topic-label',
                topicInputClass: 'mqtt-sub-topic-input',
                topicTypeLabelKey: 'mqtt-subscribe-topictype-label',
                topicTypeInputClass: 'mqtt-sub-topic-type-input',
                qosLabelKey: 'mqtt-subscribe-qos-label',
                qosInputClass: 'mqtt-sub-qos-input',
                enabledInputClass: 'mqtt-sub-enabled-input',
                autoPrefixInputClass: 'mqtt-sub-autoprefix-input',
                defaultTopicType: 1,
                toggles: [
                    { inputClass: 'mqtt-sub-enabled-input', checked: topicData.enabled !== false, labelKey: 'mqtt-topic-enabled-label' },
                    { inputClass: 'mqtt-sub-autoprefix-input', checked: topicData.autoPrefix === true, labelKey: 'mqtt-auto-prefix-label' }
                ]
            };
        },

        _createMqttPublishTopicElement(topicData, index) {
            this._createMqttTopicElement('mqtt-publish-topics', topicData, index, this._getMqttPublishTopicOptions(topicData));
        },

        addMqttPublishTopic() {
            const container = document.getElementById('mqtt-publish-topics');
            if (!container) return;
            const index = container.children.length;
            this._createMqttPublishTopicElement({ topic: '', qos: 0, retain: false, enabled: true, autoPrefix: false, topicType: 0 }, index);
        },

        deleteMqttPublishTopic(index) {
            const container = document.getElementById('mqtt-publish-topics');
            if (!container) return;
            const items = container.querySelectorAll('.mqtt-topic-item');
            if (items[index]) { items[index].remove(); }
            if (this._reindexMqttTopicItems(container) === 0) {
                this._createMqttPublishTopicElement({ topic: '', qos: 0, retain: false, enabled: true, autoPrefix: false, topicType: 0 }, 0);
            }
        },

        _collectMqttPublishTopics() {
            return this._collectMqttTopics('mqtt-publish-topics', this._getMqttPublishTopicOptions({}));
        },

        // ========== MQTT 订阅主题配置管理 ==========

        _loadMqttSubscribeTopics(topics) {
            const container = document.getElementById('mqtt-subscribe-topics');
            if (!container) return;
            container.innerHTML = '';
            if (!topics || topics.length === 0) {
                topics = [{ topic: '', qos: 0, enabled: true, autoPrefix: false, topicType: 1 }];
            }
            topics.forEach((topic, index) => {
                this._createMqttSubscribeTopicElement(topic, index);
            });
        },

        _createMqttSubscribeTopicElement(topicData, index) {
            this._createMqttTopicElement('mqtt-subscribe-topics', topicData, index, this._getMqttSubscribeTopicOptions(topicData));
        },

        addMqttSubscribeTopic() {
            const container = document.getElementById('mqtt-subscribe-topics');
            if (!container) return;
            const index = container.children.length;
            this._createMqttSubscribeTopicElement({ topic: '', qos: 0, enabled: true, autoPrefix: false, topicType: 1 }, index);
        },

        deleteMqttSubscribeTopic(index) {
            const container = document.getElementById('mqtt-subscribe-topics');
            if (!container) return;
            const items = container.querySelectorAll('.mqtt-topic-item');
            if (items[index]) { items[index].remove(); }
            if (this._reindexMqttTopicItems(container) === 0) {
                this._createMqttSubscribeTopicElement({ topic: '', qos: 0, enabled: true, autoPrefix: false, topicType: 1 }, 0);
            }
        },

        _collectMqttSubscribeTopics() {
            return this._collectMqttTopics('mqtt-subscribe-topics', this._getMqttSubscribeTopicOptions({}));
        },

        // ========== MQTT 连接测试 ==========

        testMqttConnection() {
            const resultEl = document.getElementById('mqtt-test-result');
            const btn = document.querySelector('#mqtt-form .mqtt-test-btn');
            const server = document.getElementById('mqtt-broker')?.value || '';
            const port = document.getElementById('mqtt-port')?.value || '1883';
            const clientId = document.getElementById('mqtt-client-id')?.value || '';
            const username = document.getElementById('mqtt-username')?.value || '';
            const password = document.getElementById('mqtt-password')?.value || '';
            const authCode = document.getElementById('mqtt-auth-code')?.value || '';
            const mqttSecret = document.getElementById('mqtt-secret')?.value || '';
            const authType = document.getElementById('mqtt-auth-type')?.value || '0';
            if (!server) {
                if (resultEl) { resultEl.textContent = i18n.t('mqtt-test-no-server'); resultEl.style.color = '#f56c6c'; }
                return;
            }
            if (clientId) {
                if (authType === '0' && !clientId.startsWith('S&')) {
                    if (resultEl) { resultEl.textContent = i18n.t('mqtt-test-clientid-simple-prefix'); resultEl.style.color = '#e6a23c'; }
                }
                if (authType === '1' && !clientId.startsWith('E&')) {
                    if (resultEl) { resultEl.textContent = i18n.t('mqtt-test-clientid-encrypted-prefix'); resultEl.style.color = '#e6a23c'; }
                }
            }
            if (btn) { btn.disabled = true; btn.textContent = i18n.t('mqtt-test-testing'); btn.classList.remove('mqtt-test-success', 'mqtt-test-fail'); }
            if (resultEl) { resultEl.textContent = i18n.t('mqtt-test-testing'); resultEl.style.color = '#909399'; }
            apiMqttTest({ server, port, clientId, username, password, authCode, authType, mqttSecret })
                .then(res => {
                    if (res && res.success && res.data) {
                        if (res.data.deferred) {
                            if (resultEl) { resultEl.textContent = i18n.t('mqtt-test-deferred') || 'MQTT connecting asynchronously, check status...'; resultEl.style.color = '#e6a23c'; }
                            if (btn) btn.classList.add('mqtt-test-success');
                            let pollCount = 0;
                            const pollInterval = setInterval(() => {
                                pollCount++;
                                this._loadMqttStatus();
                                const badge = document.getElementById('mqtt-status-badge');
                                if (badge && badge.classList.contains('mqtt-status-online')) {
                                    clearInterval(pollInterval);
                                    if (resultEl) { resultEl.textContent = i18n.t('mqtt-test-success'); resultEl.style.color = '#67c23a'; }
                                }
                                if (pollCount >= 15) {
                                    clearInterval(pollInterval);
                                    if (resultEl && !resultEl.textContent.includes(i18n.t('mqtt-test-success'))) {
                                        resultEl.textContent = i18n.t('mqtt-test-deferred-timeout') || 'Connection timeout, check logs';
                                        resultEl.style.color = '#f56c6c';
                                        if (btn) { btn.classList.remove('mqtt-test-success'); btn.classList.add('mqtt-test-fail'); }
                                    }
                                }
                            }, 2000);
                            return;
                        }
                        if (res.data.connected) {
                            if (res.data.realConnected) {
                                if (resultEl) { resultEl.textContent = i18n.t('mqtt-test-success'); resultEl.style.color = '#67c23a'; }
                                if (btn) btn.classList.add('mqtt-test-success');
                            } else {
                                const realErr = res.data.realError;
                                const errMsg = realErr ? this._mqttErrorCodeToText(realErr) : '';
                                if (resultEl) { resultEl.textContent = i18n.t('mqtt-test-ok-real-fail') + (errMsg ? ' (' + errMsg + ')' : ''); resultEl.style.color = '#e6a23c'; }
                                if (btn) btn.classList.add('mqtt-test-fail');
                            }
                        } else {
                            const errCode = res.data.error || 'Unknown';
                            let errMsg = this._mqttErrorCodeToText(errCode);
                            if (authType === '1' && String(errCode) === '4') {
                                const hint = i18n.t('mqtt-test-aes-bad-credentials-hint') || 'AES认证失败，请检查用户名、密码、产品密钥(mqttSecret)和授权码(authCode)是否与平台一致';
                                errMsg = errMsg + ' - ' + hint;
                            }
                            if (resultEl) { resultEl.textContent = i18n.t('mqtt-test-fail-prefix') + errMsg; resultEl.style.color = '#f56c6c'; }
                            if (btn) btn.classList.add('mqtt-test-fail');
                        }
                    } else {
                        if (resultEl) { resultEl.textContent = i18n.t('mqtt-test-error'); resultEl.style.color = '#f56c6c'; }
                        if (btn) btn.classList.add('mqtt-test-fail');
                    }
                })
                .catch(err => {
                    console.error('MQTT test failed:', err);
                    if (resultEl) {
                        const isTimeout = err && (err.name === 'AbortError' || (err.message && err.message.includes('abort')));
                        const isUnauthorized = err && err.status === 401;
                        if (isUnauthorized) { resultEl.textContent = i18n.t('mqtt-test-error') || '测试请求失败，请稍后重试'; }
                        else if (isTimeout) { resultEl.textContent = i18n.t('mqtt-test-timeout') || '测试超时，请检查Broker地址是否正确'; }
                        else {
                            const errData = err && err.data;
                            const errMsg = (errData && errData.error) ? errData.error : i18n.t('mqtt-test-error');
                            resultEl.textContent = errMsg;
                        }
                        resultEl.style.color = '#f56c6c';
                    }
                    if (btn) btn.classList.add('mqtt-test-fail');
                })
                .finally(() => {
                    if (btn) { btn.disabled = false; btn.textContent = i18n.t('mqtt-test-btn-text'); }
                    setTimeout(() => this._loadMqttStatus(), 1000);
                    setTimeout(() => { if (btn) btn.classList.remove('mqtt-test-success', 'mqtt-test-fail'); }, 3000);
                });
        },

        disconnectMqtt() {
            const btn = document.querySelector('#mqtt-form .mqtt-disconnect-btn');
            if (btn) { btn.disabled = true; btn.textContent = i18n.t('mqtt-disconnecting'); }
            this._stopMqttStatusPolling();
            apiPostSilent('/api/mqtt/disconnect', {})
                .then(res => {
                    if (res && res.success) {
                        Notification.success(i18n.t('mqtt-disconnect-ok'), 'MQTT');
                        const badge = document.getElementById('mqtt-status-badge');
                        if (badge) { badge.className = 'mqtt-status-badge mqtt-status-offline'; badge.textContent = i18n.t('mqtt-status-disconnected'); }
                        const resultEl = document.getElementById('mqtt-test-result');
                        if (resultEl) { resultEl.textContent = i18n.t('mqtt-disconnect-ok'); resultEl.style.color = '#909399'; }
                    } else {
                        Notification.error(i18n.t('mqtt-disconnect-fail'), 'MQTT');
                        this._startMqttStatusPolling();
                    }
                })
                .catch(err => {
                    console.error('MQTT disconnect failed:', err);
                    Notification.error(i18n.t('mqtt-disconnect-fail'), 'MQTT');
                    this._startMqttStatusPolling();
                })
                .finally(() => {
                    if (btn) { btn.disabled = false; btn.textContent = i18n.t('mqtt-disconnect-btn'); }
                    setTimeout(() => this._loadMqttStatus(), 500);
                });
        },

        mqttNtpSync() {
            const btn = document.querySelector('#mqtt-form .mqtt-ntp-sync-btn');
            const resultEl = document.getElementById('mqtt-test-result');
            if (btn) { btn.disabled = true; btn.textContent = i18n.t('mqtt-ntp-syncing'); }
            if (resultEl) resultEl.textContent = '';
            apiPost('/api/mqtt/ntp-sync', {})
                .then(res => {
                    if (res && res.success) {
                        Notification.success(i18n.t('mqtt-ntp-sync-ok'), 'MQTT');
                        if (resultEl) { resultEl.style.color = '#67c23a'; resultEl.textContent = i18n.t('mqtt-ntp-sync-ok'); }
                    } else {
                        const errMsg = (res && res.error) ? res.error : i18n.t('mqtt-ntp-sync-fail');
                        Notification.error(errMsg, 'MQTT');
                        if (resultEl) { resultEl.style.color = '#f56c6c'; resultEl.textContent = errMsg; }
                    }
                })
                .catch(err => {
                    console.error('MQTT NTP sync failed:', err);
                    Notification.error(i18n.t('mqtt-ntp-sync-fail'), 'MQTT');
                    if (resultEl) { resultEl.style.color = '#f56c6c'; resultEl.textContent = i18n.t('mqtt-ntp-sync-fail'); }
                })
                .finally(() => {
                    if (btn) { btn.disabled = false; btn.textContent = i18n.t('mqtt-ntp-sync-btn'); }
                    setTimeout(() => { if (resultEl) resultEl.textContent = ''; }, 3000);
                });
        },

        _mqttErrorCodeToText(code) {
            const map = {
                '-4': 'MQTT_CONNECTION_TIMEOUT', '-3': 'MQTT_CONNECTION_LOST',
                '-2': 'MQTT_CONNECT_FAILED', '-1': 'MQTT_DISCONNECTED',
                '1': 'MQTT_BAD_PROTOCOL', '2': 'MQTT_BAD_CLIENT_ID',
                '3': 'MQTT_UNAVAILABLE', '4': 'MQTT_BAD_CREDENTIALS', '5': 'MQTT_UNAUTHORIZED'
            };
            return map[String(code)] || ('Error ' + code);
        },

        _startMqttStatusPolling() {
            this._stopMqttStatusPolling();
            AppState.connectSSE();
            var self = this;
            this._mqttSSEHandler = function(e) {
                try {
                    var data = JSON.parse(e.data);
                    self._updateMqttStatusUI(data);
                } catch (err) {
                    console.warn('[SSE] MQTT 状态解析失败:', err);
                }
            };
            AppState.onSSEEvent('mqtt-status', this._mqttSSEHandler);
            this._loadMqttStatus();
        },

        _stopMqttStatusPolling() {
            if (this._mqttStatusTimer) {
                clearInterval(this._mqttStatusTimer);
                this._mqttStatusTimer = null;
            }
            if (this._mqttSSEHandler) {
                AppState.offSSEEvent('mqtt-status', this._mqttSSEHandler);
                this._mqttSSEHandler = null;
                AppState.disconnectSSE();
            }
        },

        _loadMqttStatus() {
            apiGetSilent('/api/mqtt/status')
                .then(res => {
                    if (!res || !res.success) return;
                    this._updateMqttStatusUI(res.data || {});
                })
                .catch(err => {
                    if (err && err.status === 401) {
                        this._stopMqttStatusPolling();
                        const badge = document.getElementById('mqtt-status-badge');
                        if (badge) { badge.className = 'mqtt-status-badge mqtt-status-offline'; badge.textContent = i18n.t('mqtt-status-auth-fail') || 'Unauthorized'; }
                    }
                });
        },

        _updateMqttStatusUI: function(data) {
            const d = data || {};
            const badge = document.getElementById('mqtt-status-badge');
            const serverEl = document.getElementById('mqtt-status-server');
            const clientEl = document.getElementById('mqtt-status-clientid');
            const reconnEl = document.getElementById('mqtt-status-reconnects');
            if (badge) {
                if (!d.initialized) { badge.className = 'mqtt-status-badge mqtt-status-offline'; badge.textContent = i18n.t('mqtt-status-uninit'); }
                else if (d.connected) { badge.className = 'mqtt-status-badge mqtt-status-online'; badge.textContent = i18n.t('mqtt-status-connected'); }
                else { badge.className = 'mqtt-status-badge mqtt-status-offline'; badge.textContent = i18n.t('mqtt-status-disconnected'); }
            }
            if (serverEl) serverEl.textContent = d.server ? (d.server + ':' + d.port) : '--';
            if (clientEl) clientEl.textContent = d.clientId || '--';
            if (reconnEl) reconnEl.textContent = d.reconnectCount ?? 0;
        },

        _updateMqttStatusPanel(connected, server, port, clientId) {
            const badge = document.getElementById('mqtt-status-badge');
            const serverEl = document.getElementById('mqtt-status-server');
            const clientEl = document.getElementById('mqtt-status-clientid');
            if (badge) {
                if (connected) { badge.className = 'mqtt-status-badge mqtt-status-online'; badge.textContent = i18n.t('mqtt-status-connected'); }
                else { badge.className = 'mqtt-status-badge mqtt-status-offline'; badge.textContent = i18n.t('mqtt-status-disconnected'); }
            }
            if (serverEl) serverEl.textContent = server ? (server + ':' + port) : '--';
            if (clientEl) clientEl.textContent = clientId || '--';
        }
    });
})();
