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

            // MQTT 已禁用时不启动轮询或自动重连，但仍查询一次状态。
            // HealthMonitor 可能把 enabled=false 作为内存保护动作持久化；
            // 完全跳过 API 会把“内存自动禁用”误显示成普通的“MQTT已禁用”，
            // 用户也就看不到切换 mqtt/释放外设后的恢复建议。
            var enabledCheckbox = document.getElementById('mqtt-enabled');
            if (enabledCheckbox && !enabledCheckbox.checked) {
                if (btn) { btn.disabled = false; btn.textContent = '刷新状态'; }
                if (resultEl) { resultEl.textContent = 'MQTT已禁用'; resultEl.style.color = '#909399'; }
                this._stopMqttStatusPolling();
                var badge = document.getElementById('mqtt-status-badge');
                if (badge) {
                    badge.className = 'mqtt-status-badge mqtt-status-offline';
                    badge.textContent = '未连接';
                }
                this._setMqttStatusReason('MQTT 已禁用');
                this._loadMqttStatus();
                setTimeout(() => {
                    if (!resultEl) return;
                    const currentBadge = document.getElementById('mqtt-status-badge');
                    const currentReason = document.getElementById('mqtt-status-reason');
                    if (currentBadge && currentBadge.textContent === '内存保护') {
                        resultEl.textContent = currentReason && currentReason.textContent
                            ? currentReason.textContent : '设备内存紧张，MQTT 已自动禁用';
                        resultEl.style.color = '#e6a23c';
                    }
                }, 800);
                setTimeout(() => { if (resultEl) resultEl.textContent = ''; }, 3000);
                return;
            }

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
                '-5': 'MQTTS 功能未启用（请到功能配置页启用）',
                '-11': 'MQTT_CLIENT_INIT_FAILED（可能是内存不足或配置无效）',
                '-12': 'MQTT 配置未加载或格式无效，请检查网络配置',
                '-13': '设备内存紧张，MQTT 已自动禁用，请恢复内存后重新启用',
                '-14': '设备内存紧张，MQTT 已暂时停止，内存恢复后将自动重连',
                '-4': '连接 MQTT Broker 超时，请检查地址和端口',
                '-3': 'MQTT 连接已断开，设备正在重连',
                '-2': '无法连接 MQTT Broker，请检查 DNS、网络和端口',
                '-1': 'MQTT 客户端当前未连接',
                '1': 'Broker 拒绝：协议版本不支持',
                '2': 'Broker 拒绝：客户端 ID 无效或重复',
                '3': 'Broker 当前不可用，请稍后重试',
                '4': 'Broker 拒绝：用户名或密码错误',
                '5': 'Broker 拒绝：客户端未授权'
            };
            return map[String(code)] || ('Error ' + code);
        },

        _setMqttStatusReason(reason) {
            const el = document.getElementById('mqtt-status-reason');
            if (!el) return;
            const text = reason === undefined || reason === null || String(reason).trim() === ''
                ? '--' : String(reason);
            el.textContent = text;
            el.title = text === '--' ? '' : text;
        },

        _setMqttFailureStatus(reason, badgeText) {
            const text = reason === undefined || reason === null || String(reason).trim() === ''
                ? 'MQTT 重连失败，请稍后重试' : String(reason);
            this._setMqttStatusReason(text);
            const badge = document.getElementById('mqtt-status-badge');
            if (badge) {
                badge.className = 'mqtt-status-badge mqtt-status-offline';
                badge.textContent = badgeText || '连接失败';
                badge.title = text;
            }
        },

        _getMqttStatusFailureReason(data) {
            const sources = [];
            const addSource = (value, depth) => {
                if (!value || typeof value !== 'object' || depth > 2 || sources.indexOf(value) !== -1) return;
                sources.push(value);
                // HTTP failures from request-governor are shaped as
                // {status, data: {message/error/...}}; API envelopes may nest
                // an error object one level further. Keep the traversal bounded.
                addSource(value.data, depth + 1);
                addSource(value.error, depth + 1);
            };
            addSource(data, 0);

            const findText = (fields) => {
                for (let i = 0; i < sources.length; i++) {
                    const source = sources[i];
                    for (let j = 0; j < fields.length; j++) {
                        const value = source[fields[j]];
                        if (typeof value === 'string' && value.trim() !== '') return value;
                    }
                }
                return '';
            };
            const httpStatus = data && Number(data.status);
            // Prefer structured backend diagnostics even when an Error wrapper
            // also carries a generic top-level message.
            const structured = findText(['statusReason', 'lastErrorMessage', 'errorMessage']);
            if (structured) return structured;

            // Fetch and AbortController errors otherwise leak browser-specific
            // English strings ("Failed to fetch", "signal is aborted...")
            // into the device UI.  Keep page-navigation aborts silent in the
            // caller, but make genuine timeout/network failures actionable.
            const errorName = data && String(data.name || '');
            const errorText = data && typeof data.message === 'string' ? data.message : '';
            if (errorName === 'AbortError' && data && data._requestTimeout) {
                return '状态接口请求超时，请检查设备响应或稍后重试';
            }
            if (errorName === 'TypeError' || /failed to fetch|networkerror|connection reset|网络错误|网络连接/i.test(errorText)) {
                return '无法连接设备，请检查网络连接';
            }

            // The firmware deliberately keeps the 401/503 response body small
            // (`error: "Unauthorized"`, or a short memory-guard message).  Do
            // not expose those implementation strings directly in the MQTT
            // panel; turn them into an actionable explanation instead.
            if (httpStatus === 401) return '登录已过期，请重新登录';
            if (httpStatus === 403) return '请求被拒绝，请检查当前用户权限';
            if (httpStatus === 503) {
                const httpText = findText(['message', 'error', 'reason']).toLowerCase();
                if (/low\s+memory|critical\s+memory|memory\s+pressure|insufficient\s+memory|内存紧张|内存压力|低内存|资源紧张/.test(httpText)) {
                    return '设备当前内存紧张，已暂时拒绝此操作，请稍后重试';
                }
            }
            const explicitError = findText(['error']);
            if (explicitError) return explicitError;
            const message = findText(['message', 'reason']);
            if (message) return message;
            for (let i = 0; i < sources.length; i++) {
                const source = sources[i];
                if (source.lastError !== undefined && source.lastError !== null && Number(source.lastError) !== 0) {
                    return this._mqttErrorCodeToText(source.lastError);
                }
            }
            if (httpStatus === 503) return '服务暂时不可用，请稍后重试';
            // Preserve an otherwise useful HTTP diagnostic when the server did
            // not provide a body, while leaving ordinary status payloads quiet.
            if (httpStatus >= 400) return 'HTTP ' + httpStatus;
            return '';
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
                        self._setMqttStatusReason(self._getMqttStatusFailureReason(res) || '状态接口返回失败');
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
                    // Navigation/unmount cancellation is expected and must not
                    // turn the MQTT badge into a false connection failure.
                    if (err && (
                        err._pageAborted ||
                        (typeof window.isPageAbortError === 'function' && window.isPageAbortError(err))
                    )) return;
                    // Preserve diagnostics from request-governor handled HTTP
                    // failures, but do not replace them with a generic message.
                    const diagnostic = self._getMqttStatusFailureReason(err);
                    if (diagnostic) self._setMqttStatusReason(diagnostic);
                    if (err && err._handled) {
                        // A handled 401/403 can still leave the old green
                        // badge visible when another page request triggered
                        // the global session handler.  Make the MQTT panel
                        // self-consistent immediately while the login overlay
                        // is being shown.
                        self._mqttApiFailCount++;
                        const httpStatus = Number(err.status || 0);
                        const badgeText = httpStatus === 401 ? '登录已过期'
                            : (httpStatus === 403 ? '访问受限' : '状态不可用');
                        self._setMqttFailureStatus(
                            diagnostic || 'MQTT 状态接口请求失败',
                            badgeText
                        );
                        return;
                    }
                    self._mqttApiFailCount++;
                    self._setMqttStatusReason(diagnostic || '状态接口不可用，请检查设备连接');
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
            const reasonEl = document.getElementById('mqtt-status-reason');
            if (reasonEl) {
                const reason = this._getMqttStatusFailureReason(d);
                reasonEl.textContent = reason || '--';
                reasonEl.title = reason || '';
            }
            if (badge) {
                // connecting: 后端确认客户端处于自动重连状态（非 stopped、非 connected、autoReconnect=true）
                // 注意：autoStartStarted 仅表示 restartMQTTDeferred() 已被调用，不等同于 connecting
                // 如果后端没有返回 connecting 字段（如 MQTT 对象不存在），则只根据后端实际状态判断
                const connecting = !!(d.connecting);
                const autoStarted = !!(d.autoStartStarted);  // 后端已发起自动启动
                const errorCode = d.lastErrorCode !== undefined && d.lastErrorCode !== null
                    ? d.lastErrorCode : d.lastError;
                const errorClass = String(d.lastErrorClass || '');
                const hasError = (errorCode !== undefined && errorCode !== null && Number(errorCode) !== 0)
                    || (errorClass && errorClass !== 'none' && errorClass !== 'config')
                    || d.tlsMemoryFailure === true;

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
                            apiPostSilent('/api/mqtt/reconnect', {}).then(function(reconnectRes) {
                                const reconnectData = reconnectRes && reconnectRes.data
                                    ? reconnectRes.data : {};
                                // The firmware reports some local guard failures
                                // as HTTP 200 with `reconnecting:false` so the
                                // caller can remain non-blocking.  Handle that
                                // envelope explicitly; otherwise the UI would
                                // keep polling and claim it is still connecting.
                                if (!reconnectRes || reconnectRes.success === false) {
                                    self._setMqttFailureStatus(
                                        self._getMqttStatusFailureReason(reconnectRes) || 'MQTT 重连请求失败，请稍后重试'
                                    );
                                    return;
                                }
                                if (reconnectData.mqttDisabled) {
                                    self._setMqttFailureStatus('MQTT 已禁用', '未连接');
                                    return;
                                }
                                if (reconnectData.autoReconnectDisabled) {
                                    self._setMqttFailureStatus('自动重连已关闭', '未连接');
                                    return;
                                }
                                if (reconnectData.rebooting) {
                                    self._setMqttStatusReason('设备将重启以重新连接 MQTTS');
                                    return;
                                }
                                if (reconnectData.reconnecting === false && reconnectData.connected !== true) {
                                    var immediateReason = self._getMqttStatusFailureReason(reconnectRes);
                                    if (/insufficient\s+memory|low\s+memory|内存紧张|内存压力|低内存/i.test(immediateReason)) {
                                        immediateReason = '设备当前内存紧张，MQTT 重连已跳过，请切换 mqtt 或稍后重试';
                                    }
                                    self._setMqttFailureStatus(immediateReason || 'MQTT 重连失败，请稍后重试');
                                    return;
                                }
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
                            }).catch(function(err) {
                                // Reconnect is intentionally silent at the
                                // notification layer, but its failure still
                                // belongs in the MQTT status panel.  Otherwise
                                // a 401/503/low-memory response is swallowed
                                // until (or unless) the next status poll.
                                if (err && (
                                    err._pageAborted ||
                                    (typeof window.isPageAbortError === 'function' && window.isPageAbortError(err))
                                )) return;
                                var diagnostic = self._getMqttStatusFailureReason(err);
                                self._setMqttFailureStatus(diagnostic || 'MQTT 重连请求失败，请稍后重试');
                            });
                        }
                    }
                }

                if (d.testConnected) {
                    // 测试连接成功（后端已自动恢复原配置）
                    badge.className = 'mqtt-status-badge mqtt-status-online';
                    badge.textContent = '测试成功';
                    this._mqttConnectingStartTime = 0;
                    this._mqttTestConnectedDetected = true;  // 通知 deferred poll 停止
                } else if (d.disabledByMemory === true) {
                    // HealthMonitor intentionally persists this state across
                    // reboot. Keep it distinct from a normal user-disabled
                    // MQTT setting and show the recovery reason prominently.
                    const memoryReason = this._getMqttStatusFailureReason(d) ||
                        '设备内存紧张，MQTT 已自动禁用，请恢复内存后重新启用';
                    badge.className = 'mqtt-status-badge mqtt-status-offline';
                    badge.textContent = '内存保护';
                    badge.title = memoryReason;
                    this._setMqttStatusReason(memoryReason);
                    this._mqttConnectingStartTime = 0;
                } else if (d.stoppedByMemory === true || Number(errorCode) === -14) {
                    // Severe pressure can stop MQTT temporarily before the
                    // persistent-disable threshold. Keep this distinct from
                    // a user-selected stop and explain that recovery is
                    // automatic once the heap is healthy.
                    const memoryReason = this._getMqttStatusFailureReason(d) ||
                        '设备内存紧张，MQTT 已暂时停止，内存恢复后将自动重连';
                    badge.className = 'mqtt-status-badge mqtt-status-offline';
                    badge.textContent = '内存保护';
                    badge.title = memoryReason;
                    this._setMqttStatusReason(memoryReason);
                    this._mqttConnectingStartTime = 0;
                } else if (!d.enabled) {
                    // MQTT 已禁用：优先显示“未连接”，不检查 connected 字段
                    // 即使后端客户端仍在运行（重启前窗口期），前端也必须显示禁用状态
                    badge.className = 'mqtt-status-badge mqtt-status-offline';
                    badge.textContent = '未连接';
                    this._mqttConnectingStartTime = 0;
                } else if (d.connected) {
                    badge.className = 'mqtt-status-badge mqtt-status-online';
                    badge.textContent = '已连接';
                    this._mqttConnectingStartTime = 0; // 重置超时计时
                } else if (d.networkConnected === false ||
                           (d.networkConnected === undefined && d.internetAvailable === false)) {
                    // 只有底层 STA/以太网确实断开时才显示“网络未连接”。
                    // internetAvailable 可能只是 DNS/外网探测滞后；若 STA
                    // 仍在线，MQTT 可能正在连接 Broker，不能误导用户。
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
                    if (Number(errorCode) === -5) {
                        // MQTTS 功能门控拒绝：明确告知原因与解决路径
                        badge.textContent = 'MQTTS 功能未启用';
                        badge.title = '请先到「功能配置」页启用「MQTTS 加密传输」（TLS 需约 42KB 内存）';
                    } else {
                        badge.textContent = '连接失败';
                        badge.title = this._getMqttStatusFailureReason(d) || '';
                    }
                    this._mqttConnectingStartTime = 0;
                } else if (hasError) {
                    // autoReconnect may be disabled, or the runtime may have
                    // stopped after a memory/TLS failure.  Keep the reason
                    // visible instead of reducing a known failure to "未连接".
                    badge.className = 'mqtt-status-badge mqtt-status-offline';
                    badge.textContent = Number(errorCode) === -5 ? 'MQTTS 功能未启用' : '连接失败';
                    badge.title = this._getMqttStatusFailureReason(d) || '';
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
            this._setMqttStatusReason(connected ? '' : '未连接');
            if (badge) {
                if (connected) { badge.className = 'mqtt-status-badge mqtt-status-online'; badge.textContent = '已连接'; }
                else { badge.className = 'mqtt-status-badge mqtt-status-offline'; badge.textContent = '未连接'; }
            }
            if (serverEl) serverEl.textContent = server ? (server + ':' + port) : '--';
            if (clientEl) clientEl.textContent = clientId || '--';
        }
    });
})();
