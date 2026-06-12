/**
 * protocol/mqtt-config.js — MQTT 主题配置管理 + 连接测试/状态/断开/NTP
 */
(function() {
    Object.assign(AppState, {

        // ============ MQTT 发布主题配置 ============

        _mqttTopicTypeOptions(selected) {
            const types = [
                { value: 0, label: '数据上报' },
                { value: 1, label: '数据下发' },
                { value: 2, label: '设备信息' },
                { value: 3, label: '实时监测' },
                { value: 4, label: '设备事件' },
                { value: 5, label: 'OTA升级' },
                { value: 6, label: 'OTA二进制' },
                { value: 7, label: 'NTP时间同步' }
            ];
            return types.map(t =>
                `<option value="${t.value}" ${Number(selected) === t.value ? 'selected' : ''}>${t.label}</option>`
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
                    '<input type="checkbox" class="' + toggle.inputClass + '"' + checked + '> ' + toggle.label +
                    '</label>';
            }).join('');

            return `
                <span class="${options.indexClass}">${index + 1}</span>
                <button type="button" class="mqtt-topic-delete">删除</button>
                <div class="config-form-grid">
                    <div class="fb-form-group">
                        <label>${options.topicLabel}</label>
                        <input type="text" class="${options.topicInputClass}" value="${topicData.topic || ''}" placeholder="/topic/path">
                    </div>
                    <div class="fb-form-group">
                        <label>${options.topicTypeLabel}</label>
                        <select class="${options.topicTypeInputClass}">
                            ${this._mqttTopicTypeOptions(topicType)}
                        </select>
                    </div>
                    <div class="fb-form-group">
                        <label>${options.qosLabel}</label>
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
                topicLabel: '发布主题',
                topicInputClass: 'mqtt-topic-input',
                topicTypeLabel: '主题类型',
                topicTypeInputClass: 'mqtt-topic-type-input',
                qosLabel: '发布QoS',
                qosInputClass: 'mqtt-qos-input',
                enabledInputClass: 'mqtt-enabled-input',
                autoPrefixInputClass: 'mqtt-autoprefix-input',
                retainInputClass: 'mqtt-retain-input',
                defaultTopicType: 0,
                toggles: [
                    { inputClass: 'mqtt-retain-input', checked: topicData.retain === true, label: '保留消息' },
                    { inputClass: 'mqtt-enabled-input', checked: topicData.enabled !== false, label: '启用' },
                    { inputClass: 'mqtt-autoprefix-input', checked: topicData.autoPrefix === true, label: '自动前缀' }
                ]
            };
        },

        _getMqttSubscribeTopicOptions(topicData) {
            return {
                itemClass: 'mqtt-topic-item mqtt-topic-item-sub',
                indexClass: 'mqtt-topic-index mqtt-topic-index-sub',
                topicLabel: '订阅主题',
                topicInputClass: 'mqtt-sub-topic-input',
                topicTypeLabel: '主题类型',
                topicTypeInputClass: 'mqtt-sub-topic-type-input',
                qosLabel: '订阅QoS',
                qosInputClass: 'mqtt-sub-qos-input',
                enabledInputClass: 'mqtt-sub-enabled-input',
                autoPrefixInputClass: 'mqtt-sub-autoprefix-input',
                defaultTopicType: 1,
                toggles: [
                    { inputClass: 'mqtt-sub-enabled-input', checked: topicData.enabled !== false, label: '启用' },
                    { inputClass: 'mqtt-sub-autoprefix-input', checked: topicData.autoPrefix === true, label: '自动前缀' }
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
                if (resultEl) { resultEl.textContent = '请先填写Broker地址'; resultEl.style.color = '#f56c6c'; }
                return;
            }
            this._clearMqttDeferredTestTimer();
            if (clientId) {
                if (authType === '0' && !clientId.startsWith('S&')) {
                    if (resultEl) { resultEl.textContent = "提示: FastBee平台简单认证模式下，客户端ID通常以'S&'开头（格式: S&设备编号&产品ID&用户ID）"; resultEl.style.color = '#e6a23c'; }
                }
                if (authType === '1' && !clientId.startsWith('E&')) {
                    if (resultEl) { resultEl.textContent = "提示: FastBee平台加密认证模式下，客户端ID通常以'E&'开头（格式: E&设备编号&产品ID&用户ID）"; resultEl.style.color = '#e6a23c'; }
                }
            }
            if (btn) { btn.disabled = true; btn.textContent = '测试中...'; btn.classList.remove('mqtt-test-success', 'mqtt-test-fail'); }
            if (resultEl) { resultEl.textContent = '测试中...'; resultEl.style.color = '#909399'; }
            apiMqttTest({ server, port, clientId, username, password, authCode, authType, mqttSecret })
                .then(res => {
                    if (res && res.success && res.data) {
                        if (res.data.deferred) {
                            if (resultEl) { resultEl.textContent = 'MQTT 正在异步连接中，请关注状态变化...'; resultEl.style.color = '#e6a23c'; }
                            if (btn) btn.classList.add('mqtt-test-success');
                            this._scheduleMqttDeferredStatusPoll(0, btn, resultEl);
                            return;
                        }
                        if (res.data.connected) {
                            if (res.data.realConnected) {
                                if (resultEl) { resultEl.textContent = '连接成功！'; resultEl.style.color = '#67c23a'; }
                                if (btn) btn.classList.add('mqtt-test-success');
                                this._updateMqttStatusPanel(true, server, port, clientId);
                                this._startMqttStatusPolling({ initialDelayMs: 2500 });
                            } else {
                                const realErr = res.data.realError;
                                const errMsg = realErr ? this._mqttErrorCodeToText(realErr) : '';
                                if (resultEl) { resultEl.textContent = '凭证验证通过，但实际连接失败，请先点击保存配置后再测试' + (errMsg ? ' (' + errMsg + ')' : ''); resultEl.style.color = '#e6a23c'; }
                                if (btn) btn.classList.add('mqtt-test-fail');
                            }
                        } else {
                            const errCode = res.data.error || 'Unknown';
                            let errMsg = this._mqttErrorCodeToText(errCode);
                            if (authType === '1' && String(errCode) === '4') {
                                const hint = 'AES认证失败，请检查用户名、密码、产品密钥(mqttSecret)和授权码(authCode)是否与平台一致';
                                errMsg = errMsg + ' - ' + hint;
                            }
                            if (resultEl) { resultEl.textContent = '连接失败: ' + errMsg; resultEl.style.color = '#f56c6c'; }
                            if (btn) btn.classList.add('mqtt-test-fail');
                        }
                    } else {
                        if (resultEl) { resultEl.textContent = '测试请求失败'; resultEl.style.color = '#f56c6c'; }
                        if (btn) btn.classList.add('mqtt-test-fail');
                    }
                })
                .catch(err => {
                    console.error('MQTT test failed:', err);
                    if (resultEl) {
                        const isTimeout = err && (err.name === 'AbortError' || (err.message && err.message.includes('abort')));
                        const isUnauthorized = err && err.status === 401;
                        if (isUnauthorized) { resultEl.textContent = '测试请求失败，请稍后重试'; }
                        else if (isTimeout) { resultEl.textContent = '测试超时，请检查Broker地址是否正确'; }
                        else {
                            const errData = err && err.data;
                            const errMsg = (errData && errData.error) ? errData.error : '测试请求失败';
                            resultEl.textContent = errMsg;
                        }
                        resultEl.style.color = '#f56c6c';
                    }
                    if (btn) btn.classList.add('mqtt-test-fail');
                })
                .finally(() => {
                    if (btn) { btn.disabled = false; btn.textContent = '测试连接'; }
                    setTimeout(() => this._loadMqttStatus(), 2500);
                    setTimeout(() => { if (btn) btn.classList.remove('mqtt-test-success', 'mqtt-test-fail'); }, 3000);
                });
        },

        disconnectMqtt() {
            const btn = document.querySelector('#mqtt-form .mqtt-disconnect-btn');
            if (btn) { btn.disabled = true; btn.textContent = '断开中...'; }
            this._clearMqttDeferredTestTimer();
            this._stopMqttStatusPolling();
            apiPostSilent('/api/mqtt/disconnect', {})
                .then(res => {
                    if (res && res.success) {
                        Notification.success('MQTT已断开连接', 'MQTT');
                        const badge = document.getElementById('mqtt-status-badge');
                        if (badge) { badge.className = 'mqtt-status-badge mqtt-status-offline'; badge.textContent = '未连接'; }
                        const resultEl = document.getElementById('mqtt-test-result');
                        if (resultEl) { resultEl.textContent = 'MQTT已断开连接'; resultEl.style.color = '#909399'; }
                    } else {
                        Notification.error('MQTT断开失败', 'MQTT');
                        this._startMqttStatusPolling();
                    }
                })
                .catch(err => {
                    console.error('MQTT disconnect failed:', err);
                    Notification.error('MQTT断开失败', 'MQTT');
                    this._startMqttStatusPolling();
                })
                .finally(() => {
                    if (btn) { btn.disabled = false; btn.textContent = '断开连接'; }
                    setTimeout(() => this._loadMqttStatus(), 500);
                });
        },

        mqttNtpSync() {
            const btn = document.querySelector('#mqtt-form .mqtt-ntp-sync-btn');
            const resultEl = document.getElementById('mqtt-test-result');
            if (btn) { btn.disabled = true; btn.textContent = '同步中...'; }
            if (resultEl) resultEl.textContent = '';
            apiPost('/api/mqtt/ntp-sync', {})
                .then(res => {
                    if (res && res.success) {
                        Notification.success('MQTT时间同步请求已发送', 'MQTT');
                        if (resultEl) { resultEl.style.color = '#67c23a'; resultEl.textContent = 'MQTT时间同步请求已发送'; }
                    } else {
                        const errMsg = (res && res.error) ? res.error : 'MQTT时间同步失败';
                        Notification.error(errMsg, 'MQTT');
                        if (resultEl) { resultEl.style.color = '#f56c6c'; resultEl.textContent = errMsg; }
                    }
                })
                .catch(err => {
                    console.error('MQTT NTP sync failed:', err);
                    Notification.error('MQTT时间同步失败', 'MQTT');
                    if (resultEl) { resultEl.style.color = '#f56c6c'; resultEl.textContent = 'MQTT时间同步失败'; }
                })
                .finally(() => {
                    if (btn) { btn.disabled = false; btn.textContent = 'MQTT时间同步'; }
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

        _clearMqttDeferredTestTimer() {
            if (this._mqttDeferredTestTimer) {
                clearTimeout(this._mqttDeferredTestTimer);
                this._mqttDeferredTestTimer = null;
            }
        },

        _getMqttDeferredPollIntervalMs() {
            if (typeof apiGetPressureAwareInterval === 'function') {
                return apiGetPressureAwareInterval('mqttDeferred', 2000);
            }
            return 2000;
        },

        _scheduleMqttDeferredStatusPoll(pollCount, btn, resultEl) {
            var self = this;
            this._clearMqttDeferredTestTimer();
            this._mqttDeferredTestTimer = setTimeout(function() {
                self._mqttDeferredTestTimer = null;
                var nextPollCount = pollCount + 1;
                self._loadMqttStatus();

                const badge = document.getElementById('mqtt-status-badge');
                if (badge && badge.classList.contains('mqtt-status-online')) {
                    if (resultEl) {
                        resultEl.textContent = '连接成功！';
                        resultEl.style.color = '#67c23a';
                    }
                    return;
                }

                if (nextPollCount >= 15) {
                    if (resultEl && !resultEl.textContent.includes('连接成功')) {
                        resultEl.textContent = '异步连接超时，请检查配置或查看日志';
                        resultEl.style.color = '#f56c6c';
                        if (btn) {
                            btn.classList.remove('mqtt-test-success');
                            btn.classList.add('mqtt-test-fail');
                        }
                    }
                    return;
                }

                self._scheduleMqttDeferredStatusPoll(nextPollCount, btn, resultEl);
            }, this._getMqttDeferredPollIntervalMs());
        },

        _startMqttStatusPolling(options) {
            options = options || {};
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
            // 延迟首次REST状态查询，给后端MQTT客户端初始化留出时间
            var delayMs = options.initialDelayMs || 0;
            if (delayMs > 0) {
                this._mqttStatusTimer = setTimeout(function() {
                    self._mqttStatusTimer = null;
                    self._loadMqttStatus();
                }, delayMs);
            } else {
                this._loadMqttStatus();
            }
        },

        _stopMqttStatusPolling() {
            this._clearMqttDeferredTestTimer();
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
                        if (badge) { badge.className = 'mqtt-status-badge mqtt-status-offline'; badge.textContent = '未授权'; }
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
                if (!d.initialized) { badge.className = 'mqtt-status-badge mqtt-status-offline'; badge.textContent = '未初始化'; }
                else if (d.connected) { badge.className = 'mqtt-status-badge mqtt-status-online'; badge.textContent = '已连接'; }
                else { badge.className = 'mqtt-status-badge mqtt-status-offline'; badge.textContent = '未连接'; }
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
                if (connected) { badge.className = 'mqtt-status-badge mqtt-status-online'; badge.textContent = '已连接'; }
                else { badge.className = 'mqtt-status-badge mqtt-status-offline'; badge.textContent = '未连接'; }
            }
            if (serverEl) serverEl.textContent = server ? (server + ':' + port) : '--';
            if (clientEl) clientEl.textContent = clientId || '--';
        }
    });
})();
