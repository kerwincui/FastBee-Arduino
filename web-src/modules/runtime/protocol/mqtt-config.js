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

        // ========== MQTT 刷新状态 ==========

        refreshMqttStatus() {
            const btn = document.querySelector('#mqtt-form [data-action="refreshMqttStatus"]');
            const resultEl = document.getElementById('mqtt-test-result');
            if (btn) { btn.disabled = true; btn.textContent = '刷新中...'; }
            if (resultEl) { resultEl.textContent = '正在刷新状态...'; resultEl.style.color = '#909399'; }
            this._stopMqttStatusPolling();
            this._loadMqttStatus();
            // 启动轮询，确保状态及时更新
            this._startMqttStatusPolling({ initialDelayMs: 1500 });
            setTimeout(() => {
                if (btn) { btn.disabled = false; btn.textContent = '刷新状态'; }
                if (resultEl) {
                    const badge = document.getElementById('mqtt-status-badge');
                    if (badge) {
                        resultEl.textContent = '状态已刷新: ' + badge.textContent;
                        resultEl.style.color = badge.classList.contains('mqtt-status-online') ? '#67c23a' : '#909399';
                    } else {
                        resultEl.textContent = '状态已刷新';
                        resultEl.style.color = '#909399';
                    }
                }
            }, 800);
            // 3秒后清除提示文字
            setTimeout(() => { if (resultEl) resultEl.textContent = ''; }, 4000);
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

                // 测试连接成功：检查 badge 或存储的标志
                const badge = document.getElementById('mqtt-status-badge');
                if (badge && badge.classList.contains('mqtt-status-online')) {
                    if (resultEl) {
                        resultEl.textContent = '连接成功！';
                        resultEl.style.color = '#67c23a';
                    }
                    return;
                }
                // 检查 _updateMqttStatusUI 是否检测到 testConnected
                if (self._mqttTestConnectedDetected) {
                    self._mqttTestConnectedDetected = false;
                    if (resultEl) {
                        resultEl.textContent = '测试连接成功！';
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
            var self = this;

            // 纯 REST 轮询模式：不再创建 SSE 持久连接
            // SSE 会永久占用 1 个 TCP 槽位，在 ESP32 classic（MAX_CONNECTIONS=6）上
            // 导致其他页面请求无法建立连接。MQTT 状态变化不频繁，5s 轮询即可。
            this._ensureBadgeNotStuck();
            var delayMs = options.initialDelayMs || 1500;
            this._mqttStatusTimer = setTimeout(function() {
                self._mqttStatusTimer = null;
                self._loadMqttStatus();
                // 定期轮询备份，确保状态更新
                self._mqttPollInterval = setInterval(function() {
                    self._loadMqttStatus();
                }, 5000);
            }, delayMs);
        },

        _stopMqttStatusPolling() {
            // 清理 lite 路径的递归轮询定时器
            this._mqttLitePollStopped = true;
            if (this._mqttLitePollTimer) {
                clearTimeout(this._mqttLitePollTimer);
                this._mqttLitePollTimer = null;
            }
            this._clearMqttDeferredTestTimer();
            if (this._mqttStatusTimer) {
                clearTimeout(this._mqttStatusTimer);
                this._mqttStatusTimer = null;
            }
            if (this._mqttPollInterval) {
                clearInterval(this._mqttPollInterval);
                this._mqttPollInterval = null;
            }
            if (this._mqttRetryTimer) {
                clearTimeout(this._mqttRetryTimer);
                this._mqttRetryTimer = null;
            }
            // 清理自动重连轮询定时器
            if (this._mqttAutoReconnPollTimer) {
                clearTimeout(this._mqttAutoReconnPollTimer);
                this._mqttAutoReconnPollTimer = null;
            }
        },

        _loadMqttStatus() {
            this._mqttApiFailCount = this._mqttApiFailCount || 0;
            var self = this;
            apiGetSilent('/api/mqtt/status')
                .then(function(res) {
                    if (!res || !res.success) {
                        self._mqttApiFailCount++;
                        self._handleMqttApiFail();
                        // 快速重试：首次失败后3秒再试一次（避免与其他请求叠加）
                        if (self._mqttApiFailCount === 1) {
                            self._mqttRetryTimer = setTimeout(function() {
                                self._mqttRetryTimer = null;
                                self._loadMqttStatus();
                            }, 3000);
                        }
                        return;
                    }
                    self._mqttApiFailCount = 0;
                    self._updateMqttStatusUI(res.data || {});
                })
                .catch(function(err) {
                    self._mqttApiFailCount++;
                    self._handleMqttApiFail();
                    // 快速重试：首次失败后3秒再试一次
                    if (self._mqttApiFailCount === 1) {
                        self._mqttRetryTimer = setTimeout(function() {
                            self._mqttRetryTimer = null;
                            self._loadMqttStatus();
                        }, 3000);
                    }
                });
        },

        /**
         * 处理 MQTT 状态 API 调用失败
         * 连续失败超过阈值时更新 badge 为“连接超时”
         */
        _handleMqttApiFail() {
            const badge = document.getElementById('mqtt-status-badge');
            if (!badge) return;
            // 初始“检测中...”状态立即转为“连接中”
            if (badge.classList.contains('mqtt-status-detecting')) {
                badge.className = 'mqtt-status-badge mqtt-status-connecting';
                badge.textContent = '连接中';
                return;
            }
            // 连续4次失败后，将"连接中"更新为"连接超时"
            // 阈值从2提升到4，避免设备启动初期API短暂不可用时过早显示超时
            if (this._mqttApiFailCount >= 4 && badge.classList.contains('mqtt-status-connecting')) {
                badge.className = 'mqtt-status-badge mqtt-status-offline';
                badge.textContent = '连接超时';
            }
        },

        /**
         * 确保 badge 不停留在初始的“检测中...”状态
         */
        _ensureBadgeNotStuck() {
            const badge = document.getElementById('mqtt-status-badge');
            if (!badge) return;
            if (badge.classList.contains('mqtt-status-detecting')) {
                badge.className = 'mqtt-status-badge mqtt-status-connecting';
                badge.textContent = '连接中';
            }
        },

        _updateMqttStatusUI: function(data) {
            const d = data || {};
            const badge = document.getElementById('mqtt-status-badge');
            const serverEl = document.getElementById('mqtt-status-server');
            const clientEl = document.getElementById('mqtt-status-clientid');
            const reconnEl = document.getElementById('mqtt-status-reconnects');
            if (badge) {
                // connecting: 后端确认客户端处于自动重连状态（非 stopped、非 connected、autoReconnect=true）
                // 注意：autoStartStarted 仅表示 restartMQTTDeferred() 已被调用，不等同于 connecting
                // 如果后端没有返回 connecting 字段（如 MQTT 对象不存在），则只根据后端实际状态判断
                const connecting = !!(d.connecting);
                const autoStarted = !!(d.autoStartStarted);  // 后端已发起自动启动
                const hasError = d.lastError && d.lastError !== 0;

                // 自动重连：首次检测到 MQTT 已启用且 autoReconnect 为 true 但未连接时，触发非阻塞 deferred 重连
                // ⚠️ 绝对不能加 !connecting 条件！autoStartStarted/connecting=true 仅表示
                // 后端做了 deferred restart（初始化 MQTTClient + begin()），实际连接由
                // loop 异步完成且不可靠。必须调用 /api/mqtt/reconnect 让后端重置并重新启动
                // ⚠️ 必须检查 d.autoReconnect！用户取消勾选自动重连后不应触发重连
                if (!this._mqttAutoReconnectTriggered && d.enabled !== false && d.autoReconnect !== false) {
                    this._mqttAutoReconnectTriggered = true;
                    if (!d.connected) {
                        var self = this;
                        if (typeof apiPostSilent === 'function') {
                            apiPostSilent('/api/mqtt/reconnect', {}).then(function() {
                                // deferred 重连已启动，用跟踪的定时器轮询状态
                                var pollCount = 0;
                                var maxPolls = 6; // 最多 6 次，覆盖 30s
                                function pollStatus() {
                                    // 检查是否被 _stopMqttStatusPolling 取消
                                    if (self._mqttLitePollStopped) return;
                                    pollCount++;
                                    self._loadMqttStatus();
                                    if (pollCount < maxPolls && !self._mqttLitePollStopped) {
                                        self._mqttAutoReconnPollTimer = setTimeout(function() {
                                            self._mqttAutoReconnPollTimer = null;
                                            var badge = document.getElementById('mqtt-status-badge');
                                            if (badge && badge.classList.contains('mqtt-status-online')) return;
                                            pollStatus();
                                        }, 5000);
                                    }
                                }
                                self._mqttAutoReconnPollTimer = setTimeout(pollStatus, 3000);
                            }).catch(function(){});
                        }
                    }
                }

                if (d.testConnected) {
                    // 测试连接成功（后端已自动恢复原配置）
                    badge.className = 'mqtt-status-badge mqtt-status-online';
                    badge.textContent = '测试成功';
                    this._mqttConnectingStartTime = 0;
                    this._mqttTestConnectedDetected = true;  // 通知 deferred poll 停止
                } else if (d.connected) {
                    badge.className = 'mqtt-status-badge mqtt-status-online';
                    badge.textContent = '已连接';
                    this._mqttConnectingStartTime = 0; // 重置超时计时
                } else if (d.internetAvailable === false && d.enabled !== false) {
                    // 网络本身不可用时（如 4G 失败回退 AP、以太网断线），
                    // 显示“网络未连接”而非泛泛的“未连接”，帮助用户定位根因
                    badge.className = 'mqtt-status-badge mqtt-status-offline';
                    badge.textContent = '网络未连接';
                    this._mqttConnectingStartTime = 0;
                } else if (connecting && !hasError) {
                    // 跟踪“连接中”状态持续时间
                    if (!this._mqttConnectingStartTime) {
                        this._mqttConnectingStartTime = Date.now();
                    }
                    var connectingDuration = Date.now() - this._mqttConnectingStartTime;
                    if (connectingDuration > 60000 && (d.reconnectCount === 0 || d.reconnectCount === undefined)) {
                        badge.className = 'mqtt-status-badge mqtt-status-offline';
                        badge.textContent = '连接超时';
                    } else {
                        badge.className = 'mqtt-status-badge mqtt-status-connecting';
                        badge.textContent = '连接中';
                    }
                } else if (connecting && hasError) {
                    badge.className = 'mqtt-status-badge mqtt-status-offline';
                    badge.textContent = '连接失败';
                    this._mqttConnectingStartTime = 0;
                } else if (!d.enabled) {
                    badge.className = 'mqtt-status-badge mqtt-status-offline';
                    badge.textContent = '未连接';
                    this._mqttConnectingStartTime = 0;
                } else if (!d.initialized) {
                    badge.className = 'mqtt-status-badge mqtt-status-connecting';
                    badge.textContent = '初始化中';
                } else {
                    badge.className = 'mqtt-status-badge mqtt-status-offline';
                    badge.textContent = '未连接';
                    this._mqttConnectingStartTime = 0;
                }

                // 无 PSRAM 设备使用 mqtts 时，在状态文本后追加内存提示
                if (this._mqttTlsSupported === false &&
                    (this._mqttCurrentScheme === 'mqtts' || d.scheme === 'mqtts')) {
                    badge.textContent += '\uff08\u5185\u5b58\u6709\u9650\uff0c\u5efa\u8bae\u5207\u6362\u4e3amqtt\uff09';
                }
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
