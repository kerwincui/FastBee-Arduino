/**
 * 协议配置模块
 * 包含 MQTT/Modbus RTU/TCP/HTTP/CoAP/TCP 协议配置
 * 以及 Modbus Master 设备管理、线圈/PWM/PID 控制
 */
(function() {
    AppState.registerModule('protocol', {

        // ============ 状态变量 ============
        _protocolConfig: null,
        _masterTasks: [],
        _modbusRtuLoaded: false,
        _coilStates: [],
        _coilAutoRefreshTimer: null,
        _coilAutoRefreshErrors: 0,
        _modbusDevices: [],
        _deviceCoilCache: {},
        _devicePwmCache: {},
        _pwmStates: [],
        _pidValues: {},
        _pidAutoRefreshTimer: null,
        _pidAutoRefreshErrors: 0,
        _devicePidCache: {},
        _activeDeviceIdx: -1,
        _editingDeviceIdx: -1,
        _editingTaskIdx: -1,
        _currentMappingTaskIdx: -1,
        _currentMappings: [],
        _masterStatusTimer: null,
        _uartPeripherals: [],
        _mqttStatusTimer: null,
        _protocolEventsBound: false,

        // ============ 事件绑定 ============
        setupProtocolEvents() {
            if (this._protocolEventsBound) return;

            var allDevicesBody = document.getElementById('all-devices-body');
            var modbusDevicesBody = document.getElementById('modbus-devices-body');

            // 如果关键表格元素尚未加载到 DOM，延迟绑定
            if (!allDevicesBody && !modbusDevicesBody) {
                return; // 不设置 _protocolEventsBound，下次调用时重试
            }

            var publishTopics = document.getElementById('mqtt-publish-topics');
            if (publishTopics) {
                publishTopics.addEventListener('click', (event) => this._handlePublishTopicClick(event));
            }
            var subscribeTopics = document.getElementById('mqtt-subscribe-topics');
            if (subscribeTopics) {
                subscribeTopics.addEventListener('click', (event) => this._handleSubscribeTopicClick(event));
            }
            var mappingTableBody = document.getElementById('mapping-table-body');
            if (mappingTableBody) {
                mappingTableBody.addEventListener('click', (event) => this._handleMappingTableClick(event));
            }
            if (allDevicesBody) {
                allDevicesBody.addEventListener('click', (event) => this._handleProtocolActionClick(event));
            }
            if (modbusDevicesBody) {
                modbusDevicesBody.addEventListener('click', (event) => this._handleProtocolActionClick(event));
            }
            var pwmGrid = document.getElementById('pwm-channel-grid');
            if (pwmGrid) {
                pwmGrid.addEventListener('input', (event) => this._handlePwmGridInput(event));
                pwmGrid.addEventListener('change', (event) => this._handlePwmGridChange(event));
            }
            var pidGrid = document.getElementById('pid-data-grid');
            if (pidGrid) {
                pidGrid.addEventListener('click', (event) => this._handlePidGridClick(event));
            }
            var coilGrid = document.getElementById('coil-status-grid');
            if (coilGrid) {
                coilGrid.addEventListener('click', (event) => this._handleCoilGridClick(event));
            }
            this._protocolEventsBound = true;
        },

        // ============ MQTT 发布主题配置管理 ============

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

        _renderProtocolEmptyRow(colspan, text) {
            return '<tr><td colspan="' + colspan + '" class="u-empty-cell">' + (text || '') + '</td></tr>';
        },

        _renderProtocolStatus(enabled) {
            const isEnabled = enabled !== false;
            return '<span class="' + (isEnabled ? 'protocol-status-on' : 'protocol-status-off') + '">' +
                (isEnabled ? 'ON' : 'OFF') + '</span>';
        },

        _getProtocolBadgeTone(type) {
            const tones = {
                sensor: 'protocol-badge--sensor',
                relay: 'protocol-badge--relay',
                pwm: 'protocol-badge--pwm',
                pid: 'protocol-badge--pid'
            };
            return tones[type] || 'protocol-badge--sensor';
        },

        _renderProtocolBadge(type, label) {
            return '<span class="protocol-badge ' + this._getProtocolBadgeTone(type) + '">' +
                (label || type || '-') + '</span>';
        },

        _renderProtocolActionButton(label, tone, action, index, source) {
            var attrs = 'data-protocol-action="' + action + '"';
            if (index !== undefined && index !== null) attrs += ' data-index="' + index + '"';
            if (source) attrs += ' data-source="' + source + '"';
            return '<button type="button" class="pure-button protocol-action-btn protocol-action-btn--' + tone +
                '" ' + attrs + '>' + label + '</button>';
        },

        _renderProtocolActionCell(buttons) {
            return '<td class="protocol-action-cell">' + buttons.join('') + '</td>';
        },

        _getProtocolControlDeviceInfo(device) {
            var deviceType = device.deviceType || 'relay';
            var info = (device.channelCount || 2) + 'ch';
            if (deviceType === 'relay') {
                info += ' ' + (device.controlProtocol === 1 ? 'Reg' : 'Coil') + '@' + (device.coilBase || 0);
            } else if (deviceType === 'pwm') {
                info += ' Reg@' + (device.pwmRegBase || 0) + ' ' + (device.pwmResolution || 8) + 'bit';
            } else if (deviceType === 'pid') {
                info += ' PV@' + ((device.pidAddrs && device.pidAddrs[0]) || 0);
            }
            if (device.sensorId) info += ' id:' + device.sensorId;
            return info;
        },

        _renderAllDeviceSensorRow(task, index, fcNames, typeLabels) {
            var label = task.name || task.label || ('Slave ' + (task.slaveAddress || 1));
            var info = (fcNames[task.functionCode] || 'FC03') + ' @' + (task.startAddress || 0) + ' x' + (task.quantity || 10);
            var mappingCount = (task.mappings && task.mappings.length) || 0;
            if (mappingCount > 0) info += ' [' + mappingCount + (i18n.t('modbus-dev-mappings-suffix') || '映射') + ']';
            return '<tr>' +
                '<td>' + escapeHtml(label) + '</td>' +
                '<td>' + this._renderProtocolBadge('sensor', typeLabels.sensor) + '</td>' +
                '<td>' + (task.slaveAddress || 1) + '</td>' +
                '<td><small>' + escapeHtml(info) + '</small></td>' +
                '<td>' + this._renderProtocolStatus(task.enabled) + '</td>' +
                this._renderProtocolActionCell([
                    this._renderProtocolActionButton(i18n.t('modbus-task-edit-btn') || '编辑', 'primary', 'edit-device', index, 'sensor'),
                    this._renderProtocolActionButton(i18n.t('modbus-mapping-btn') || '映射', 'warning', 'open-mapping', index),
                    this._renderProtocolActionButton(i18n.t('modbus-master-delete-task') || '删除', 'danger', 'delete-device', index, 'sensor')
                ]) +
                '</tr>';
        },

        _renderAllDeviceControlRow(device, index, typeLabels) {
            var deviceType = device.deviceType || 'relay';
            return '<tr>' +
                '<td>' + escapeHtml(device.name || '-') + '</td>' +
                '<td>' + this._renderProtocolBadge(deviceType, typeLabels[deviceType] || deviceType) + '</td>' +
                '<td>' + (device.slaveAddress || 1) + '</td>' +
                '<td><small>' + escapeHtml(this._getProtocolControlDeviceInfo(device)) + '</small></td>' +
                '<td>' + this._renderProtocolStatus(device.enabled) + '</td>' +
                this._renderProtocolActionCell([
                    this._renderProtocolActionButton(i18n.t('modbus-device-edit-btn') || '编辑', 'primary', 'edit-device', index, 'control'),
                    this._renderProtocolActionButton(i18n.t('modbus-device-select-btn') || '控制', 'warning', 'open-control-modal', index),
                    this._renderProtocolActionButton(i18n.t('modbus-master-delete-task') || '删除', 'danger', 'delete-device', index, 'control')
                ]) +
                '</tr>';
        },

        _renderModbusDeviceRow(device, index, typeLabels, protocolLabels) {
            var deviceType = device.deviceType || 'relay';
            var controlProtocol = device.controlProtocol || 0;
            return '<tr>' +
                '<td>' + escapeHtml(device.name || '-') + '</td>' +
                '<td>' + this._renderProtocolBadge(deviceType, typeLabels[deviceType] || deviceType) + '</td>' +
                '<td>' + (device.slaveAddress || 1) + '</td>' +
                '<td>' + (device.channelCount || 2) + '</td>' +
                '<td>' + (device.coilBase || 0) + '</td>' +
                '<td>' + (protocolLabels[controlProtocol] || 'Coil') + '</td>' +
                '<td>' + (device.ncMode ? 'ON' : '-') + '</td>' +
                this._renderProtocolActionCell([
                    this._renderProtocolActionButton(i18n.t('modbus-device-edit-btn') || '编辑', 'primary', 'open-edit-modal', index),
                    this._renderProtocolActionButton(i18n.t('modbus-device-select-btn') || '控制', 'warning', 'open-control-modal', index),
                    this._renderProtocolActionButton(i18n.t('modbus-master-delete-task') || '删除', 'danger', 'remove-device', index)
                ]) +
                '</tr>';
        },

        _getProtocolIndexedItem(ref, itemSelector) {
            if (!ref || typeof ref.closest !== 'function') return null;
            var item = ref.closest(itemSelector);
            if (!item) return null;
            var index = parseInt(item.dataset.index, 10);
            return {
                item: item,
                index: isNaN(index) ? -1 : index
            };
        },

        _handlePublishTopicClick(event) {
            var deleteBtn = event.target.closest('.mqtt-topic-delete');
            if (!deleteBtn) return;
            var info = this._getProtocolIndexedItem(deleteBtn, '.mqtt-topic-item');
            if (info && info.index >= 0) this.deleteMqttPublishTopic(info.index);
        },

        _handleSubscribeTopicClick(event) {
            var deleteBtn = event.target.closest('.mqtt-topic-delete');
            if (!deleteBtn) return;
            var info = this._getProtocolIndexedItem(deleteBtn, '.mqtt-topic-item');
            if (info && info.index >= 0) this.deleteMqttSubscribeTopic(info.index);
        },

        _handleMappingTableClick(event) {
            var button = event.target.closest('.protocol-mapping-remove');
            if (!button) return;
            var index = parseInt(button.getAttribute('data-index'), 10);
            if (!isNaN(index)) this.removeMapping(index);
        },

        _handleProtocolActionClick(event) {
            var button = event.target.closest('.protocol-action-btn[data-protocol-action]');
            if (!button) return;
            var index = parseInt(button.getAttribute('data-index'), 10);
            var action = button.getAttribute('data-protocol-action');
            var source = button.getAttribute('data-source') || '';
            if (isNaN(index) || !action) return;
            if (action === 'edit-device') this._editDevice(source, index);
            else if (action === 'open-mapping') this.openMappingModal(index);
            else if (action === 'delete-device') this._deleteDevice(source, index);
            else if (action === 'open-edit-modal') this._openEditModal(index);
            else if (action === 'open-control-modal') this._openCtrlModal(index);
            else if (action === 'remove-device') this._removeDevice(index);
        },

        _handlePwmGridInput(event) {
            var target = event.target;
            if (!target || !target.classList.contains('pwm-slider')) return;
            var ch = parseInt(target.getAttribute('data-ch'), 10);
            if (!isNaN(ch)) this._onPwmSliderInput(ch, target.value);
        },

        _handlePwmGridChange(event) {
            var target = event.target;
            if (!target) return;
            var ch = parseInt(target.getAttribute('data-ch'), 10);
            if (isNaN(ch)) return;
            if (target.classList.contains('pwm-slider')) this._onPwmSliderChange(ch, target.value);
            else if (target.classList.contains('pwm-num-input')) this._onPwmNumChange(ch, target.value);
        },

        _handlePidGridClick(event) {
            var button = event.target.closest('.pid-set-btn');
            if (!button) return;
            var paramName = button.getAttribute('data-param');
            var input = button.parentNode ? button.parentNode.querySelector('.pid-input') : null;
            if (paramName && input) this._onPidInputChange(paramName, input.value);
        },

        _handleCoilGridClick(event) {
            var card = event.target.closest('.coil-card');
            if (!card) return;
            var ch = parseInt(card.getAttribute('data-ch'), 10);
            if (!isNaN(ch)) this.toggleCoil(ch);
        },

        _renderMqttQosOptions(selectedQos) {
            return [
                '<option value="0"' + (selectedQos === 0 ? ' selected' : '') + '>0</option>',
                '<option value="1"' + (selectedQos === 1 ? ' selected' : '') + '>1</option>',
                '<option value="2"' + (selectedQos === 2 ? ' selected' : '') + '>2</option>'
            ].join('');
        },

        _buildMqttTopicItemHtml(index, topicData, options) {
            const topicType = topicData.topicType ?? options.defaultTopicType;
            const qos = topicData.qos ?? 0;
            const toggleHtml = options.toggles.map((toggle) => {
                const checked = toggle.checked ? ' checked' : '';
                return '<label class="pure-checkbox">' +
                    '<input type="checkbox" class="' + toggle.inputClass + '"' + checked + '> ' + i18n.t(toggle.labelKey) +
                    '</label>';
            }).join('');

            return `
                <span class="${options.indexClass}">${index + 1}</span>
                <button type="button" class="mqtt-topic-delete">${i18n.t('mqtt-delete-topic-btn')}</button>
                <div class="config-form-grid">
                    <div class="pure-control-group">
                        <label>${i18n.t(options.topicLabelKey)}</label>
                        <input type="text" class="pure-input-1 ${options.topicInputClass}" value="${topicData.topic || ''}" placeholder="/topic/path">
                    </div>
                    <div class="pure-control-group">
                        <label>${i18n.t(options.topicTypeLabelKey)}</label>
                        <select class="pure-input-1 ${options.topicTypeInputClass}">
                            ${this._mqttTopicTypeOptions(topicType)}
                        </select>
                    </div>
                    <div class="pure-control-group">
                        <label>${i18n.t(options.qosLabelKey)}</label>
                        <select class="pure-input-1 ${options.qosInputClass}">
                            ${this._renderMqttQosOptions(qos)}
                        </select>
                    </div>
                    <div class="pure-control-group mqtt-topic-toggle-row">
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

        // ============ 协议配置 ============

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
                } else {
                    this._masterTasks = [];
                }
                this._modbusRtuLoaded = true;
                // 先加载设备列表（内部会调用 _renderAllDevices），再启动状态刷新
                this._loadModbusDevices();
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
            data.modbusRtu_workMode = document.getElementById('rtu-work-mode')?.value || '1';
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

        // ============ Modbus Master 模式管理 ============

        onModbusModeChange(mode) {
            // 固定为主站模式
        },

        onWorkModeChange(mode) {
            var show = (mode === '1') ? '' : 'none';
            var sec = document.getElementById('master-config-section');
            if (sec) { if (show === 'none') AppState.hideElement(sec); else AppState.showElement(sec); }
            var st = document.getElementById('master-status-section');
            if (st) { if (show === 'none') AppState.hideElement(st); else AppState.showElement(st); }
        },

        _renderAllDevices() {
            var tbody = document.getElementById('all-devices-body');
            if (!tbody) return;
            var tasks = this._masterTasks || [];
            var devices = this._modbusDevices || [];
            if (tasks.length === 0 && devices.length === 0) {
                tbody.innerHTML = this._renderProtocolEmptyRow(6, i18n.t('modbus-no-devices') || '暂无子设备');
                return;
            }
            var fcNames = {1: 'FC01', 2: 'FC02', 3: 'FC03', 4: 'FC04'};
            var typeLabels = {
                sensor: i18n.t('modbus-type-sensor') || '采集',
                relay: i18n.t('modbus-type-relay') || '继电器',
                pwm: i18n.t('modbus-type-pwm') || 'PWM',
                pid: i18n.t('modbus-type-pid') || 'PID'
            };
            var rows = '';
            for (var i = 0; i < tasks.length; i++) {
                rows += this._renderAllDeviceSensorRow(tasks[i], i, fcNames, typeLabels);
            }
            for (var j = 0; j < devices.length; j++) {
                rows += this._renderAllDeviceControlRow(devices[j], j, typeLabels);
            }
            tbody.innerHTML = rows;
        },

        _editDevice(source, idx) {
            if (source === 'sensor') {
                this._openTaskEditModal(idx);
            } else {
                this._openEditModal(idx);
            }
        },

        _deleteDevice(source, idx) {
            if (source === 'sensor') {
                if (this._masterTasks) this._masterTasks.splice(idx, 1);
            } else {
                if (this._modbusDevices) this._modbusDevices.splice(idx, 1);
            }
            this._renderAllDevices();
        },

        _showAddDeviceMenu() {
            var menu = document.getElementById('add-device-menu');
            if (!menu) return;
            var isVisible = !menu.classList.contains('fb-hidden');
            if (isVisible) {
                menu.classList.add('fb-hidden');
            } else {
                menu.classList.remove('fb-hidden');
                var closeMenu = function(e) {
                    if (!menu.contains(e.target)) {
                        menu.classList.add('fb-hidden');
                        document.removeEventListener('click', closeMenu);
                    }
                };
                setTimeout(function() { document.addEventListener('click', closeMenu); }, 0);
            }
        },

        _addSensorDevice() {
            var menu = document.getElementById('add-device-menu');
            if (menu) menu.classList.add('fb-hidden');
            if (!this._masterTasks) this._masterTasks = [];
            if (this._masterTasks.length >= 8) {
                Notification.warning('Max 8 sensor devices', i18n.t('modbus-all-devices-title'));
                return;
            }
            this._openTaskEditModal(-1);
        },

        _addControlDevice() {
            var menu = document.getElementById('add-device-menu');
            if (menu) menu.classList.add('fb-hidden');
            if (!this._modbusDevices) this._modbusDevices = [];
            if (this._modbusDevices.length >= 8) {
                Notification.warning('Max 8 control devices', i18n.t('modbus-all-devices-title'));
                return;
            }
            this._openEditModal(-1);
        },

        // ============ 轮询任务编辑弹窗 ============

        _openTaskEditModal(idx) {
            var modal = document.getElementById('task-edit-modal');
            if (!modal) return;
            this._editingTaskIdx = idx;
            var task;
            if (idx >= 0 && this._masterTasks && this._masterTasks[idx]) {
                task = this._masterTasks[idx];
            } else {
                task = { slaveAddress: 1, functionCode: 3, startAddress: 0, quantity: 10, enabled: true, name: '', mappings: [] };
            }
            var f = function(id) { return document.getElementById(id); };
            f('task-edit-slave-addr').value = task.slaveAddress || 1;
            f('task-edit-fc').value = task.functionCode || 3;
            f('task-edit-start-addr').value = task.startAddress || 0;
            f('task-edit-quantity').value = task.quantity || 10;
            f('task-edit-name').value = task.name || task.label || '';
            f('task-edit-type').value = task.deviceType || 'holding';
            f('task-edit-enabled').checked = task.enabled !== false;
            var titleEl = modal.querySelector('.modal-header h3');
            if (titleEl) titleEl.textContent = idx < 0 ? i18n.t('modbus-task-add-title') : i18n.t('modbus-task-edit-title');
            AppState.showModal(modal);
        },
        
        _closeTaskEditModal() {
            var modal = document.getElementById('task-edit-modal');
            if (modal) AppState.hideModal(modal);
            this._editingTaskIdx = -1;
        },

        _onTaskTypeChange(val) {
            var fcMap = { holding: '3', input: '4', coil: '1', discrete: '2' };
            var fcEl = document.getElementById('task-edit-fc');
            if (fcEl && fcMap[val]) fcEl.value = fcMap[val];
        },

        _saveTaskEditModal() {
            var f = function(id) { return document.getElementById(id); };
            var task = {
                slaveAddress: parseInt(f('task-edit-slave-addr').value) || 1,
                functionCode: parseInt(f('task-edit-fc').value) || 3,
                startAddress: parseInt(f('task-edit-start-addr').value) || 0,
                quantity: parseInt(f('task-edit-quantity').value) || 10,
                name: f('task-edit-name').value || '',
                deviceType: f('task-edit-type').value || 'holding',
                enabled: f('task-edit-enabled').checked
            };
            if (!this._masterTasks) this._masterTasks = [];
            if (this._editingTaskIdx >= 0 && this._masterTasks[this._editingTaskIdx]) {
                var oldTask = this._masterTasks[this._editingTaskIdx];
                task.mappings = oldTask.mappings || [];
                task.pollInterval = oldTask.pollInterval;
                this._masterTasks[this._editingTaskIdx] = task;
            } else {
                task.mappings = [];
                this._masterTasks.push(task);
            }
            this._renderAllDevices();
            this._closeTaskEditModal();
        },

        addMasterPollTask() {
            if (!this._masterTasks) this._masterTasks = [];
            if (this._masterTasks.length >= 8) {
                Notification.warning('Max 8 tasks', i18n.t('modbus-master-title'));
                return;
            }
            this._openTaskEditModal(-1);
        },

        removeMasterPollTask(idx) {
            if (this._masterTasks) {
                this._masterTasks.splice(idx, 1);
                this._renderAllDevices();
            }
        },

        // ============ 寄存器映射管理 ============

        openMappingModal(taskIdx) {
            if (!this._masterTasks || !this._masterTasks[taskIdx]) return;
            this._currentMappingTaskIdx = taskIdx;
            this._currentMappings = JSON.parse(JSON.stringify(this._masterTasks[taskIdx].mappings || []));
            this._renderMappingTable();
            AppState.showModal('mapping-modal');
        },
        
        closeMappingModal() {
            AppState.hideModal('mapping-modal');
            this._currentMappingTaskIdx = -1;
            this._currentMappings = [];
        },

        saveMappingModal() {
            if (this._currentMappingTaskIdx < 0) return;
            this._collectMappingValues();
            this._masterTasks[this._currentMappingTaskIdx].mappings = this._currentMappings;
            this._renderAllDevices();
            this.closeMappingModal();
        },

        addMapping() {
            if (this._currentMappings.length >= 8) {
                Notification.warning(i18n.t('modbus-mapping-max'));
                return;
            }
            this._collectMappingValues();
            this._currentMappings.push({ regOffset: 0, dataType: 0, scaleFactor: 0.1, decimalPlaces: 1, sensorId: '' });
            this._renderMappingTable();
        },

        removeMapping(idx) {
            this._collectMappingValues();
            this._currentMappings.splice(idx, 1);
            this._renderMappingTable();
        },

        _collectMappingValues() {
            const tbody = document.getElementById('mapping-table-body');
            if (!tbody) return;
            const rows = tbody.querySelectorAll('tr');
            rows.forEach((row, idx) => {
                if (idx >= this._currentMappings.length) return;
                const inputs = row.querySelectorAll('input, select');
                if (inputs.length >= 5) {
                    this._currentMappings[idx].regOffset = parseInt(inputs[0].value) || 0;
                    this._currentMappings[idx].dataType = parseInt(inputs[1].value) || 0;
                    this._currentMappings[idx].scaleFactor = parseFloat(inputs[2].value) || 1.0;
                    this._currentMappings[idx].decimalPlaces = parseInt(inputs[3].value) || 1;
                    this._currentMappings[idx].sensorId = inputs[4].value || '';
                }
            });
        },

        _renderMappingTable() {
            const tbody = document.getElementById('mapping-table-body');
            if (!tbody) return;
            const dtOpts = [
                {v: 0, t: 'uint16'}, {v: 1, t: 'int16'},
                {v: 2, t: 'uint32'}, {v: 3, t: 'int32'}, {v: 4, t: 'float32'}
            ];
            if (this._currentMappings.length === 0) {
                tbody.innerHTML = this._renderProtocolEmptyRow(6, i18n.t('modbus-master-no-tasks'));
                return;
            }
            tbody.innerHTML = this._currentMappings.map((m, idx) => {
                const dtSelect = dtOpts.map(o =>
                    '<option value="' + o.v + '"' + (m.dataType === o.v ? ' selected' : '') + '>' + o.t + '</option>'
                ).join('');
                return '<tr>' +
                    '<td><input type="number" class="protocol-mapping-num-sm" value="' + (m.regOffset || 0) + '" min="0" max="124"></td>' +
                    '<td><select class="protocol-mapping-select">' + dtSelect + '</select></td>' +
                    '<td><input type="number" class="protocol-mapping-num-md" value="' + (m.scaleFactor ?? 0.1) + '" step="0.001"></td>' +
                    '<td><input type="number" class="protocol-mapping-num-sm" value="' + (m.decimalPlaces ?? 1) + '" min="0" max="6"></td>' +
                    '<td><input type="text" class="protocol-mapping-text" value="' + (m.sensorId || '') + '" maxlength="15" placeholder="temperature"></td>' +
                    '<td><button type="button" class="pure-button protocol-mapping-remove" data-index="' + idx + '">X</button></td>' +
                '</tr>';
            }).join('');
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
        }
    });

    // Part 2: Device management, coil/PWM/PID, UART, MQTT enhanced, export/import
    Object.assign(AppState, {

        /** 从后端协议配置加载设备列表（含 localStorage 迁移） */
        _loadModbusDevices() {
            var serverDevices = [];
            try {
                var rtu = this._protocolConfig && this._protocolConfig.modbusRtu;
                if (rtu && rtu.master && rtu.master.devices && rtu.master.devices.length > 0) {
                    // 使用 slice() 创建数组副本，确保是真正的数组
                    serverDevices = Array.isArray(rtu.master.devices) 
                        ? rtu.master.devices.slice() 
                        : Object.keys(rtu.master.devices).map(function(k) { return rtu.master.devices[k]; });
                }
            } catch(e) {}

            if (serverDevices.length === 0) {
                try {
                    var raw = localStorage.getItem('modbus_devices');
                    if (raw) {
                        var localDevices = JSON.parse(raw);
                        if (localDevices && localDevices.length > 0) {
                            serverDevices = localDevices.map(function(d) {
                                return {
                                    name: d.name || 'Device',
                                    sensorId: d.sensorId || '',
                                    deviceType: d.deviceType || d.type || 'relay',
                                    slaveAddress: d.slaveAddress || 1,
                                    channelCount: d.channelCount || 2,
                                    coilBase: d.coilBase || 0,
                                    ncMode: !!d.ncMode,
                                    controlProtocol: d.relayMode === 'register' ? 1 : 0,
                                    batchRegister: d.batchRegister || 0,
                                    pwmRegBase: d.pwmRegBase || 0,
                                    pwmResolution: d.pwmResolution || 8,
                                    pidAddrs: [
                                        d.pidPvAddr || 0, d.pidSvAddr || 1, d.pidOutAddr || 2,
                                        d.pidPAddr || 3, d.pidIAddr || 4, d.pidDAddr || 5
                                    ],
                                    pidDecimals: d.pidDecimals || 1,
                                    enabled: d.enabled !== false
                                };
                            });
                        }
                    }
                } catch(e) {}
            }

            this._modbusDevices = serverDevices;
            for (var i = 0; i < this._modbusDevices.length; i++) {
                if (!this._modbusDevices[i].deviceType) {
                    this._modbusDevices[i].deviceType = this._modbusDevices[i].type || 'relay';
                }
            }
            this._activeDeviceIdx = -1;
            this._renderAllDevices();
        },

        /** 刷新 Modbus 子设备列表（防抖 + 加载反馈） */
        _refreshModbusDeviceList() {
            var btn = document.getElementById('modbus-refresh-devices-btn');
            if (btn && btn.disabled) return;
            if (btn) {
                btn.disabled = true;
                btn.innerHTML = '<span class="fb-spin">&#x21bb;</span> 加载中...';
            }
            this._protocolConfig = null;
            if (typeof window.apiInvalidateCache === 'function') {
                window.apiInvalidateCache('/api/protocol/config');
            }
            var self = this;
            apiGet('/api/protocol/config')
                .then(function(res) {
                    if (res && res.success) {
                        self._protocolConfig = res.data || {};
                        self._fillProtocolForm('modbus-rtu', self._protocolConfig);
                        Notification.success('设备列表已刷新');
                    } else {
                        Notification.error('获取配置失败');
                    }
                })
                .catch(function(err) {
                    if (!(err && err._pageAborted)) {
                        Notification.error('刷新失败，请检查网络');
                    }
                })
                .finally(function() {
                    if (btn) {
                        btn.disabled = false;
                        btn.innerHTML = '&#x21bb; 刷新列表';
                    }
                });
        },

        _renderDeviceTable() {
            var tbody = document.getElementById('modbus-devices-body');
            if (!tbody) return;
            if (!this._modbusDevices || this._modbusDevices.length === 0) {
                tbody.innerHTML = this._renderProtocolEmptyRow(8, i18n.t('modbus-device-no-devices') || '暂无子设备');
                return;
            }
            var typeLabels = { relay: i18n.t('modbus-ctrl-type-relay') || '继电器', pwm: 'PWM', pid: 'PID' };
            var protLabels = ['Coil', 'Register'];
            var self = this;
            tbody.innerHTML = this._modbusDevices.map(function(dev, idx) {
                return self._renderModbusDeviceRow(dev, idx, typeLabels, protLabels);
            }).join('');
        },

        _updateDevice(idx, field, value) {
            if (this._modbusDevices && this._modbusDevices[idx]) {
                this._modbusDevices[idx][field] = value;
                if (field === 'deviceType') {
                    if (idx === this._activeDeviceIdx) {
                        this._activateDevice(idx);
                    } else {
                        this._renderAllDevices();
                    }
                }
            }
        },

        _updateActiveDeviceExt(field, value) {
            if (this._activeDeviceIdx < 0 || !this._modbusDevices) return;
            var dev = this._modbusDevices[this._activeDeviceIdx];
            if (!dev) return;
            if (field.indexOf('pidAddrs.') === 0) {
                var arrIdx = parseInt(field.split('.')[1]);
                if (!dev.pidAddrs) dev.pidAddrs = [0, 1, 2, 3, 4, 5];
                dev.pidAddrs[arrIdx] = value;
            } else {
                dev[field] = value;
            }
        },

        addModbusDevice() {
            if (!this._modbusDevices) this._modbusDevices = [];
            if (this._modbusDevices.length >= 8) {
                Notification.warning(i18n.t('modbus-device-max-reached') || '最多8个子设备');
                return;
            }
            this._openEditModal(-1);
        },

        _removeDevice(idx) {
            if (!this._modbusDevices) return;
            var msg = i18n.t('modbus-device-delete-confirm') || '确定要删除此设备？';
            if (!confirm(msg)) return;
            var devKey = 'dev_' + idx;
            delete this._deviceCoilCache[devKey];
            delete this._devicePwmCache[devKey];
            delete this._devicePidCache[devKey];
            this._modbusDevices.splice(idx, 1);
            if (this._activeDeviceIdx === idx) {
                this._activeDeviceIdx = -1;
            } else if (this._activeDeviceIdx > idx) {
                this._activeDeviceIdx--;
            }
            this._renderAllDevices();
        },

        // ========== 编辑弹窗 ==========

        _openEditModal(idx) {
            this._editingDeviceIdx = idx;
            var dev = (idx >= 0 && this._modbusDevices && this._modbusDevices[idx])
                ? this._modbusDevices[idx] : null;
            var modal = document.getElementById('modbus-device-edit-modal');
            if (!modal) return;
            document.getElementById('mdev-edit-name').value = dev ? (dev.name || '') : ((i18n.t('modbus-ctrl-device-default-name') || '设备') + ((this._modbusDevices ? this._modbusDevices.length : 0) + 1));
            document.getElementById('mdev-edit-sensorid').value = dev ? (dev.sensorId || '') : '';
            document.getElementById('mdev-edit-type').value = dev ? (dev.deviceType || 'relay') : 'relay';
            document.getElementById('mdev-edit-addr').value = dev ? (dev.slaveAddress || 1) : 1;
            document.getElementById('mdev-edit-ch').value = String(dev ? (dev.channelCount || 2) : 2);
            document.getElementById('mdev-edit-base').value = dev ? (dev.coilBase || 0) : 0;
            document.getElementById('mdev-edit-protocol').value = String(dev ? (dev.controlProtocol || 0) : 0);
            document.getElementById('mdev-edit-nc').value = dev ? (dev.ncMode ? 'true' : 'false') : 'true';
            document.getElementById('mdev-edit-enabled').checked = dev ? (dev.enabled !== false) : true;
            document.getElementById('mdev-edit-pwm-reg-base').value = dev ? (dev.pwmRegBase || 0) : 0;
            document.getElementById('mdev-edit-pwm-resolution').value = String(dev ? (dev.pwmResolution || 8) : 8);
            var pidA = dev ? (dev.pidAddrs || [0,1,2,3,4,5]) : [0,1,2,3,4,5];
            var pidFields = ['pv','sv','out','p','i','d'];
            for (var pi = 0; pi < pidFields.length; pi++) {
                var pe = document.getElementById('mdev-edit-pid-' + pidFields[pi]);
                if (pe) pe.value = pidA[pi] || pi;
            }
            document.getElementById('mdev-edit-pid-decimals').value = String(dev ? (dev.pidDecimals || 1) : 1);
            this._onEditTypeChange();
            var title = document.getElementById('modbus-edit-modal-title');
            if (title) title.textContent = (idx >= 0)
                ? (i18n.t('modbus-device-edit-title') || '编辑子设备')
                : (i18n.t('modbus-device-add') || '添加设备');
            AppState.showModal(modal);
        },
        
        _onEditTypeChange() {
            var type = document.getElementById('mdev-edit-type').value;
            var pwmSec = document.getElementById('mdev-edit-pwm-section');
            var pidSec = document.getElementById('mdev-edit-pid-section');
            // 先移除 fb-hidden（含 !important），再用 toggleVisible 统一控制显隐
            if (pwmSec) pwmSec.classList.remove('fb-hidden');
            if (pidSec) pidSec.classList.remove('fb-hidden');
            if (pwmSec) AppState.toggleVisible(pwmSec, type === 'pwm');
            if (pidSec) AppState.toggleVisible(pidSec, type === 'pid');
        },
        
        _closeEditModal() {
            var modal = document.getElementById('modbus-device-edit-modal');
            if (modal) AppState.hideModal(modal);
            this._editingDeviceIdx = -1;
        },

        _saveEditModal() {
            var idx = this._editingDeviceIdx;
            var isNew = (idx < 0);
            if (isNew) {
                // 确保 _modbusDevices 是真正的数组
                if (!this._modbusDevices || !Array.isArray(this._modbusDevices)) {
                    this._modbusDevices = [];
                }
                if (this._modbusDevices.length >= 8) {
                    Notification.warning(i18n.t('modbus-device-max-reached') || '最多8个子设备');
                    return;
                }
                this._modbusDevices.push({});
                idx = this._modbusDevices.length - 1;
            }
            var dev = this._modbusDevices[idx];
            // 防御性检查：确保 dev 对象存在
            if (!dev) {
                console.error('_saveEditModal: dev is undefined, idx=', idx, 'devices=', this._modbusDevices);
                return;
            }
            dev.name = document.getElementById('mdev-edit-name').value || 'Device';
            dev.sensorId = (document.getElementById('mdev-edit-sensorid').value || '').trim();
            dev.deviceType = document.getElementById('mdev-edit-type').value || 'relay';
            dev.slaveAddress = parseInt(document.getElementById('mdev-edit-addr').value) || 1;
            dev.channelCount = parseInt(document.getElementById('mdev-edit-ch').value) || 2;
            dev.coilBase = parseInt(document.getElementById('mdev-edit-base').value) || 0;
            dev.controlProtocol = parseInt(document.getElementById('mdev-edit-protocol').value) || 0;
            dev.ncMode = (document.getElementById('mdev-edit-nc').value === 'true');
            dev.enabled = document.getElementById('mdev-edit-enabled').checked;
            dev.pwmRegBase = parseInt(document.getElementById('mdev-edit-pwm-reg-base').value) || 0;
            dev.pwmResolution = parseInt(document.getElementById('mdev-edit-pwm-resolution').value) || 8;
            var pidFields = ['pv','sv','out','p','i','d'];
            dev.pidAddrs = [];
            for (var pi = 0; pi < pidFields.length; pi++) {
                dev.pidAddrs.push(parseInt(document.getElementById('mdev-edit-pid-' + pidFields[pi]).value) || pi);
            }
            dev.pidDecimals = parseInt(document.getElementById('mdev-edit-pid-decimals').value) || 1;
            this._renderAllDevices();
            this._closeEditModal();
            // 不再自动保存，用户需点击底部"保存配置"按钮手动保存
        },

        // ========== 控制弹窗 ==========

        _openCtrlModal(idx) {
            console.log('[protocol] _openCtrlModal called, idx:', idx);
            if (!this._modbusDevices || !this._modbusDevices[idx]) {
                console.warn('[protocol] Device not found at index', idx);
                return;
            }
            // 切换设备前，清理旧的自动刷新定时器，防止竞争 ESP32 连接
            if (this._coilAutoRefreshTimer) {
                clearInterval(this._coilAutoRefreshTimer);
                this._coilAutoRefreshTimer = null;
            }
            if (this._pidAutoRefreshTimer) {
                clearInterval(this._pidAutoRefreshTimer);
                this._pidAutoRefreshTimer = null;
            }
            var autoEl = document.getElementById('modbus-ctrl-auto-refresh');
            if (autoEl) autoEl.checked = false;
            var pidAutoEl = document.getElementById('modbus-ctrl-pid-auto-refresh');
            if (pidAutoEl) pidAutoEl.checked = false;
            this._coilAutoRefreshErrors = 0;
            this._pidAutoRefreshErrors = 0;
            if (this._activeDeviceIdx >= 0) {
                var cacheKey = 'dev_' + this._activeDeviceIdx;
                if (this._coilStates.length > 0) this._deviceCoilCache[cacheKey] = this._coilStates.slice();
                if (this._pwmStates.length > 0) this._devicePwmCache[cacheKey] = this._pwmStates.slice();
                if (this._pidValues && Object.keys(this._pidValues).length > 0) {
                    this._devicePidCache[cacheKey] = JSON.parse(JSON.stringify(this._pidValues));
                }
            }
            this._activeDeviceIdx = idx;
            localStorage.setItem('modbus_active_device', String(idx));
            var dev = this._modbusDevices[idx];
            var type = dev.deviceType || 'relay';
            var title = document.getElementById('modbus-ctrl-modal-title');
            if (title) title.textContent = (dev.name || 'Device') + ' - ' + (i18n.t('modbus-device-ctrl-title') || '设备控制');
            var relayConfig = document.getElementById('modbus-relay-config');
            var relayPanel = document.getElementById('modbus-relay-panel');
            var pwmPanel = document.getElementById('modbus-pwm-panel');
            var pidPanel = document.getElementById('modbus-pid-panel');
            // 先移除 fb-hidden（含 !important），再用 toggleVisible 统一控制显隐
            [relayConfig, relayPanel, pwmPanel, pidPanel].forEach(function(el) {
                if (el) el.classList.remove('fb-hidden');
            });
            if (relayConfig) AppState.toggleVisible(relayConfig, type === 'relay');
            if (relayPanel) AppState.toggleVisible(relayPanel, type === 'relay');
            if (pwmPanel) AppState.toggleVisible(pwmPanel, type === 'pwm');
            if (pidPanel) AppState.toggleVisible(pidPanel, type === 'pid');
            this._updateDelayChannelSelect();
            var newCacheKey = 'dev_' + idx;
            if (type === 'pwm') {
                if (this._devicePwmCache[newCacheKey]) {
                    this._pwmStates = this._devicePwmCache[newCacheKey].slice();
                    this._renderPwmGrid();
                } else {
                    this._pwmStates = [];
                    this._renderPwmGrid(true); // 显示加载中
                    this.refreshPwmStatus();
                }
            } else if (type === 'pid') {
                if (this._devicePidCache[newCacheKey]) {
                    this._pidValues = JSON.parse(JSON.stringify(this._devicePidCache[newCacheKey]));
                    this._renderPidGrid();
                } else {
                    this._pidValues = {};
                    this._renderPidGrid(true); // 显示加载中
                    this.refreshPidStatus();
                }
            } else {
                if (this._deviceCoilCache[newCacheKey]) {
                    this._coilStates = this._deviceCoilCache[newCacheKey].slice();
                    this._renderCoilGrid();
                } else {
                    this._coilStates = [];
                    this._renderCoilGrid();
                    this.refreshCoilStatus();
                }
            }
            var modal = document.getElementById('modbus-device-ctrl-modal');
            if (!modal) {
                console.error('[protocol] Modal element "modbus-device-ctrl-modal" not found in DOM');
                return;
            }
            console.log('[protocol] Showing control modal for device type:', type);
            AppState.showModal(modal);
        },
        
        _closeCtrlModal() {
            if (this._coilAutoRefreshTimer) {
                clearInterval(this._coilAutoRefreshTimer);
                this._coilAutoRefreshTimer = null;
            }
            var autoEl = document.getElementById('modbus-ctrl-auto-refresh');
            if (autoEl) autoEl.checked = false;
            var pidAutoEl = document.getElementById('modbus-ctrl-pid-auto-refresh');
            if (pidAutoEl) pidAutoEl.checked = false;
            var modal = document.getElementById('modbus-device-ctrl-modal');
            if (modal) AppState.hideModal(modal);
        },

        _activateDevice(idx) {
            this._openCtrlModal(idx);
        },

        onDeviceTypeChange() {
            if (this._activeDeviceIdx < 0 || !this._modbusDevices) return;
            var dev = this._modbusDevices[this._activeDeviceIdx];
            if (!dev) return;
            var type = dev.deviceType || 'relay';
            var types = ['relay', 'pwm', 'pid'];
            types.forEach(function(t) {
                var panel = document.getElementById('modbus-' + t + '-panel');
                var config = document.getElementById('modbus-' + t + '-config');
                // 先移除 fb-hidden（含 !important），再用 toggleVisible 统一控制显隐
                if (panel) panel.classList.remove('fb-hidden');
                if (config) config.classList.remove('fb-hidden');
                if (panel) AppState.toggleVisible(panel, t === type);
                if (config) AppState.toggleVisible(config, t === type);
            });
        },

        onRelayModeChange() {
            if (this._activeDeviceIdx < 0 || !this._modbusDevices) return;
            var dev = this._modbusDevices[this._activeDeviceIdx];
            if (!dev) return;
            var mode = (dev.controlProtocol === 1) ? 'register' : 'coil';
            var hintEl = document.querySelector('#modbus-relay-config .field-hint');
            if (hintEl) {
                hintEl.textContent = (mode === 'register')
                    ? (i18n.t('modbus-ctrl-reg-base-hint') || '寄存器起始地址，如M88继电器从0x0008开始')
                    : (i18n.t('modbus-ctrl-coil-base-hint') || '线圈起始地址，默认0');
            }
        },

        // ========== PWM 控制 ==========

        _getPwmParams() {
            var dev = (this._activeDeviceIdx >= 0 && this._modbusDevices)
                ? this._modbusDevices[this._activeDeviceIdx] : null;
            var res = dev ? (dev.pwmResolution || 8) : 8;
            return {
                slaveAddress: dev ? (dev.slaveAddress || 1) : 1,
                channelCount: dev ? (dev.channelCount || 4) : 4,
                regBase: dev ? (dev.pwmRegBase || 0) : 0,
                resolution: res,
                maxValue: (1 << res) - 1
            };
        },

        _renderPwmGrid(loading) {
            var grid = document.getElementById('pwm-channel-grid');
            if (!grid) return;
            var p = this._getPwmParams();
            var html = '';
            
            // 如果处于加载状态，显示加载提示
            if (loading) {
                html = '<div class="pwm-loading-placeholder" style="text-align:center;padding:30px;color:#999;">'
                    + '<div style="font-size:14px;margin-bottom:10px;">加载中...</div>'
                    + '<div style="font-size:12px;">正在获取 PWM 通道状态</div>'
                    + '</div>';
                grid.innerHTML = html;
                return;
            }
            
            for (var i = 0; i < p.channelCount; i++) {
                var val = i < this._pwmStates.length ? this._pwmStates[i] : 0;
                var pct = p.maxValue > 0 ? Math.round(val / p.maxValue * 100) : 0;
                html += '<div class="pwm-card">'
                    + '<div class="pwm-ch">CH' + i + '</div>'
                    + '<input type="range" class="pwm-slider" min="0" max="' + p.maxValue + '" value="' + val + '" '
                    + 'data-ch="' + i + '">'
                    + '<div class="pwm-value-row">'
                    + '<input type="number" class="pwm-num-input" min="0" max="' + p.maxValue + '" value="' + val + '" '
                    + 'data-ch="' + i + '">'
                    + '<span class="pwm-pct">' + pct + '%</span>'
                    + '</div></div>';
            }
            grid.innerHTML = html;
        },

        async refreshPwmStatus() {
            var p = this._getPwmParams();
            try {
                var res = await apiGetSilent('/api/modbus/register/read', {
                    slaveAddress: p.slaveAddress,
                    startAddress: p.regBase,
                    quantity: p.channelCount,
                    functionCode: 3
                });
                if (res && res.success && res.data && res.data.values) {
                    this._pwmStates = res.data.values;
                    var cacheKey = 'dev_' + this._activeDeviceIdx;
                    this._devicePwmCache[cacheKey] = this._pwmStates.slice();
                    this._renderPwmGrid();
                    this._appendDebugLog(res.debug, 'ReadRegs FC03');
                }
                } catch (e) {
                // 设备未连接或读取失败，显示默认界面（值为0）
                if (this._pwmStates.length === 0) {
                    var p2 = this._getPwmParams();
                    this._pwmStates = new Array(p2.channelCount).fill(0);
                }
                this._renderPwmGrid();
            }
        },

        async setPwmChannel(ch, value) {
            var p = this._getPwmParams();
            try {
                var res = await apiPost('/api/modbus/register/write', {
                    slaveAddress: p.slaveAddress,
                    registerAddress: p.regBase + ch,
                    value: value
                });
                if (res && res.success) {
                    if (ch < this._pwmStates.length) this._pwmStates[ch] = value;
                    this._appendDebugLog(res.debug, 'WriteReg CH' + ch + '=' + value);
                }
            } catch (e) {
                Notification.error(i18n.t('modbus-ctrl-fail'));
            }
        },

        async batchPwm(action) {
            var p = this._getPwmParams();
            var values = [];
            var fillVal = action === 'max' ? p.maxValue : 0;
            for (var i = 0; i < p.channelCount; i++) values.push(fillVal);
            try {
                var res = await apiPost('/api/modbus/register/batch-write', {
                    slaveAddress: p.slaveAddress,
                    startAddress: p.regBase,
                    values: JSON.stringify(values)
                });
                if (res && res.success) {
                    this._pwmStates = values;
                    this._renderPwmGrid();
                    Notification.success(i18n.t('modbus-ctrl-success'));
                }
            } catch (e) {
                Notification.error(i18n.t('modbus-ctrl-fail'));
            }
        },

        _onPwmSliderInput(ch, val) {
            var numInput = document.querySelector('.pwm-num-input[data-ch="' + ch + '"]');
            var card = numInput ? numInput.closest('.pwm-card') : null;
            var pctSpan = card ? card.querySelector('.pwm-pct') : null;
            var p = this._getPwmParams();
            if (numInput) numInput.value = val;
            if (pctSpan) pctSpan.textContent = (p.maxValue > 0 ? Math.round(val / p.maxValue * 100) : 0) + '%';
        },

        _onPwmSliderChange(ch, val) {
            this.setPwmChannel(ch, parseInt(val));
        },

        _onPwmNumChange(ch, val) {
            var p = this._getPwmParams();
            val = Math.max(0, Math.min(parseInt(val) || 0, p.maxValue));
            var slider = document.querySelector('.pwm-slider[data-ch="' + ch + '"]');
            if (slider) slider.value = val;
            var card = slider ? slider.closest('.pwm-card') : null;
            var pctSpan = card ? card.querySelector('.pwm-pct') : null;
            if (pctSpan) pctSpan.textContent = (p.maxValue > 0 ? Math.round(val / p.maxValue * 100) : 0) + '%';
            this.setPwmChannel(ch, val);
        },

        // ========== PID 控制器 ==========

        _getPidParams() {
            var dev = (this._activeDeviceIdx >= 0 && this._modbusDevices)
                ? this._modbusDevices[this._activeDeviceIdx] : null;
            var pidA = dev ? (dev.pidAddrs || [0,1,2,3,4,5]) : [0,1,2,3,4,5];
            var decimals = dev ? (dev.pidDecimals || 1) : 1;
            return {
                slaveAddress: dev ? (dev.slaveAddress || 1) : 1,
                pvAddr: pidA[0] || 0, svAddr: pidA[1] || 1, outAddr: pidA[2] || 2,
                pAddr: pidA[3] || 3, iAddr: pidA[4] || 4, dAddr: pidA[5] || 5,
                decimals: decimals,
                scaleFactor: Math.pow(10, decimals)
            };
        },

        _renderPidGrid(loading) {
            var container = document.getElementById('pid-data-grid');
            if (!container) return;
                    
            // 如果处于加载状态，显示加载提示
            if (loading) {
                var loadingHtml = '<div class="pid-loading-placeholder" style="text-align:center;padding:30px;color:#999;">'
                    + '<div style="font-size:14px;margin-bottom:10px;">加载中...</div>'
                    + '<div style="font-size:12px;">正在获取 PID 数据</div>'
                    + '</div>';
                container.innerHTML = loadingHtml;
                return;
            }
                    
            var p = this._getPidParams();
            var v = this._pidValues || {};
            var sf = p.scaleFactor;
            var fmtVal = function(raw, dec) {
                if (raw === undefined || raw === null) return '--';
                return (raw / sf).toFixed(dec);
            };
            var fmtPct = function(raw) {
                if (raw === undefined || raw === null) return '--';
                return (raw / sf).toFixed(p.decimals) + '%';
            };
            var cards = [
                { key: 'pv', label: i18n.t('modbus-ctrl-pid-pv-label') || '过程值 PV', value: fmtVal(v.pv, p.decimals), editable: false, big: true },
                { key: 'sv', label: i18n.t('modbus-ctrl-pid-sv-label') || '设定值 SV', value: fmtVal(v.sv, p.decimals), editable: true },
                { key: 'out', label: i18n.t('modbus-ctrl-pid-out-label') || '输出 %', value: fmtPct(v.out), editable: false },
                { key: 'p', label: i18n.t('modbus-ctrl-pid-p-label') || 'P 比例', value: fmtVal(v.p, p.decimals), editable: true },
                { key: 'i', label: i18n.t('modbus-ctrl-pid-i-label') || 'I 积分', value: fmtVal(v.i, p.decimals), editable: true },
                { key: 'd', label: i18n.t('modbus-ctrl-pid-d-label') || 'D 微分', value: fmtVal(v.d, p.decimals), editable: true }
            ];
            var html = '';
            for (var ci = 0; ci < cards.length; ci++) {
                var c = cards[ci];
                var cls = 'pid-card' + (c.big ? ' pid-pv-card' : '') + (c.editable ? ' pid-editable' : '');
                html += '<div class="' + cls + '">';
                html += '<div class="pid-card-label">' + c.label + '</div>';
                if (c.editable) {
                    var rawVal = (v[c.key] !== undefined && v[c.key] !== null) ? (v[c.key] / sf).toFixed(p.decimals) : '';
                    html += '<div class="pid-card-value">' + c.value + '</div>';
                    html += '<div class="pid-edit-row">';
                    html += '<input type="number" class="pid-input" step="' + (1 / sf) + '" value="' + rawVal + '" data-param="' + c.key + '">';
                    html += '<button type="button" class="btn btn-sm btn-enable pid-set-btn pid-set-btn-wide" data-param="' + c.key + '">'
                          + (i18n.t('modbus-ctrl-pid-set') || '设置') + '</button>';
                    html += '</div>';
                } else {
                    html += '<div class="pid-card-value">' + c.value + '</div>';
                }
                html += '</div>';
            }
            container.innerHTML = html;
        },

        refreshPidStatus() {
            var p = this._getPidParams();
            if (!p.slaveAddress) return;
            var addrs = [p.pvAddr, p.svAddr, p.outAddr, p.pAddr, p.iAddr, p.dAddr];
            var minAddr = Math.min.apply(null, addrs);
            var maxAddr = Math.max.apply(null, addrs);
            var quantity = maxAddr - minAddr + 1;
            if (quantity > 125) {
                Notification.warning('PID 寄存器地址跨度过大 (>' + 125 + ')');
                return;
            }
            var self = this;
            apiGetSilent('/api/modbus/register/read', {
                slaveAddress: p.slaveAddress, startAddress: minAddr, quantity: quantity, functionCode: 3
            }).then(function(res) {
                if (res && res.success && res.data && res.data.values) {
                    self._pidAutoRefreshErrors = 0;
                    var vals = res.data.values;
                    self._pidValues = {
                        pv: vals[p.pvAddr - minAddr], sv: vals[p.svAddr - minAddr],
                        out: vals[p.outAddr - minAddr], p: vals[p.pAddr - minAddr],
                        i: vals[p.iAddr - minAddr], d: vals[p.dAddr - minAddr]
                    };
                    var cacheKey = 'dev_' + self._activeDeviceIdx;
                    self._devicePidCache[cacheKey] = JSON.parse(JSON.stringify(self._pidValues));
                    self._renderPidGrid();
                } else {
                    self._pidAutoRefreshErrors++;
                    self._stopPidAutoRefreshOnErrors();
                }
                if (res && res.debug) {
                    self._appendDebugLog(res.debug.tx ? { tx: res.debug.tx, rx: res.debug.rx } : null, 'PID Read');
                }
            }).catch(function() {
                self._pidAutoRefreshErrors++;
                self._stopPidAutoRefreshOnErrors();
                // 设备未连接或读取失败，显示默认界面（值为0）
                if (!self._pidValues || Object.keys(self._pidValues).length === 0) {
                    self._pidValues = { pv: 0, sv: 0, out: 0, p: 0, i: 0, d: 0 };
                }
                self._renderPidGrid();
            });
        },

        setPidRegister(paramName, rawValue) {
            var p = this._getPidParams();
            var addrMap = { sv: p.svAddr, p: p.pAddr, i: p.iAddr, d: p.dAddr };
            var addr = addrMap[paramName];
            if (addr === undefined) return;
            var self = this;
            apiPost('/api/modbus/register/write', {
                slaveAddress: p.slaveAddress, registerAddress: addr, value: rawValue
            }).then(function(res) {
                if (res && res.success) {
                    self._pidValues[paramName] = rawValue;
                    var cacheKey = 'dev_' + self._activeDeviceIdx;
                    self._devicePidCache[cacheKey] = JSON.parse(JSON.stringify(self._pidValues));
                    self._renderPidGrid();
                }
                if (res && res.debug) {
                    self._appendDebugLog(res.debug.tx ? { tx: res.debug.tx, rx: res.debug.rx } : null, 'PID Write ' + paramName);
                }
            }).catch(function() {});
        },

        _onPidInputChange(paramName, displayValue) {
            var p = this._getPidParams();
            var val = parseFloat(displayValue);
            if (isNaN(val)) return;
            var rawValue = Math.round(val * p.scaleFactor);
            this.setPidRegister(paramName, rawValue);
        },

        _stopPidAutoRefresh() {
            if (this._pidAutoRefreshTimer) {
                clearInterval(this._pidAutoRefreshTimer);
                this._pidAutoRefreshTimer = null;
            }
        },

        togglePidAutoRefresh() {
            var checked = document.getElementById('modbus-ctrl-pid-auto-refresh')?.checked;
            if (checked) {
                this._pidAutoRefreshErrors = 0;
                this.refreshPidStatus();
                var self = this;
                this._pidAutoRefreshTimer = setInterval(function() { self.refreshPidStatus(); }, 8000);
            } else {
                this._stopPidAutoRefresh();
            }
        },

        // ========== 调试日志 ==========

        _appendDebugLog(debug, label) {
            var log = document.getElementById('modbus-debug-log');
            if (!log) return;
            if (!debug && !label) return;
            var time = new Date().toLocaleTimeString();
            var entry = document.createElement('div');
            entry.className = 'modbus-debug-entry';
            var html = '<div class="modbus-debug-time">[' + time + '] ' + (label || '') + '</div>';
            if (debug && debug.tx) html += '<div class="modbus-debug-tx">TX: ' + debug.tx + '</div>';
            if (debug && debug.rx) html += '<div class="modbus-debug-rx">RX: ' + debug.rx + '</div>';
            entry.innerHTML = html;
            log.appendChild(entry);
            log.scrollTop = log.scrollHeight;
            while (log.children.length > 100) log.removeChild(log.firstChild);
        },

        _appendDebugError(msg, debug) {
            var log = document.getElementById('modbus-debug-log');
            if (!log) return;
            var time = new Date().toLocaleTimeString();
            var entry = document.createElement('div');
            entry.className = 'modbus-debug-entry';
            var html = '<div class="modbus-debug-time">[' + time + ']</div>';
            html += '<div class="modbus-debug-err">' + msg + '</div>';
            if (debug && debug.tx) html += '<div class="modbus-debug-tx">TX: ' + debug.tx + '</div>';
            if (debug && debug.rx) html += '<div class="modbus-debug-rx">RX: ' + debug.rx + '</div>';
            entry.innerHTML = html;
            log.appendChild(entry);
            log.scrollTop = log.scrollHeight;
            while (log.children.length > 100) log.removeChild(log.firstChild);
        },

        clearModbusDebugLog() {
            var log = document.getElementById('modbus-debug-log');
            if (log) log.innerHTML = '';
        },

        // ========== 线圈控制 ==========

        _getCoilParams() {
            var dev = (this._activeDeviceIdx >= 0 && this._modbusDevices[this._activeDeviceIdx])
                ? this._modbusDevices[this._activeDeviceIdx] : {};
            return {
                slaveAddress: dev.slaveAddress || 1,
                channelCount: dev.channelCount || 8,
                coilBase: dev.coilBase || 0,
                ncMode: !!dev.ncMode,
                relayMode: (dev.controlProtocol === 1) ? 'register' : 'coil'
            };
        },

        async refreshCoilStatus() {
            const p = this._getCoilParams();
            try {
                const res = await apiGetSilent('/api/modbus/coil/status', {
                    slaveAddress: p.slaveAddress, channelCount: p.channelCount,
                    coilBase: p.coilBase, mode: p.relayMode
                });
                if (res && res.success && res.data && res.data.states) {
                    this._coilAutoRefreshErrors = 0;
                    this._coilStates = res.data.states;
                    var cacheKey = 'dev_' + this._activeDeviceIdx;
                    this._deviceCoilCache[cacheKey] = this._coilStates.slice();
                    this._renderCoilGrid();
                    this._appendDebugLog(res.debug, 'ReadCoils FC01');
                } else if (res && !res.success) {
                    this._coilAutoRefreshErrors++;
                    this._stopAutoRefreshOnErrors();
                    Notification.error(res.error || i18n.t('modbus-ctrl-fail'));
                    this._appendDebugError(res.error || 'ReadCoils failed', res.debug);
                } else {
                    if (this._coilStates.length === 0) {
                        for (var i = 0; i < p.channelCount; i++) this._coilStates.push(false);
                    }
                    this._renderCoilGrid();
                }
            } catch (e) {
                this._coilAutoRefreshErrors++;
                this._stopAutoRefreshOnErrors();
                if (this._coilStates.length === 0) {
                    for (var i = 0; i < p.channelCount; i++) this._coilStates.push(false);
                }
                this._renderCoilGrid();
            }
        },

        _renderCoilGrid() {
            const grid = document.getElementById('coil-status-grid');
            if (!grid) return;
            const p = this._getCoilParams();
            const onText = i18n.t('modbus-ctrl-status-on') || 'ON';
            const offText = i18n.t('modbus-ctrl-status-off') || 'OFF';
            let html = '';
            for (let i = 0; i < p.channelCount; i++) {
                const coilState = i < this._coilStates.length ? this._coilStates[i] : false;
                const isOn = p.ncMode ? !coilState : coilState;
                const cls = isOn ? 'coil-on' : 'coil-off';
                html += '<div class="coil-card ' + cls + '" data-ch="' + i + '">'
                      + '<div class="coil-ch">CH' + i + '</div>'
                      + '<div class="coil-st">' + (isOn ? onText : offText) + '</div>'
                      + '</div>';
            }
            grid.innerHTML = html;
        },

        async toggleCoil(ch) {
            const card = document.querySelector('.coil-card[data-ch="' + ch + '"]');
            if (card) card.classList.add('coil-loading');
            const p = this._getCoilParams();
            try {
                const res = await apiPost('/api/modbus/coil/control', {
                    slaveAddress: p.slaveAddress, channel: ch, coilBase: p.coilBase,
                    action: 'toggle', mode: p.relayMode
                });
                if (res && res.success && res.data) {
                    if (ch < this._coilStates.length) this._coilStates[ch] = res.data.state;
                    this._renderCoilGrid();
                    Notification.success(i18n.t('modbus-ctrl-success'));
                    this._appendDebugLog(res.debug, 'Toggle CH' + ch);
                } else {
                    Notification.error((res && res.error) || i18n.t('modbus-ctrl-fail'));
                    this._appendDebugError('Toggle CH' + ch + ' failed', res && res.debug);
                }
            } catch (e) {
                Notification.error(i18n.t('modbus-ctrl-fail'));
            }
            if (card) card.classList.remove('coil-loading');
        },

        async batchCoil(action) {
            const p = this._getCoilParams();
            let modbusAction = action;
            if (p.ncMode) {
                if (action === 'allOn') modbusAction = 'allOff';
                else if (action === 'allOff') modbusAction = 'allOn';
            }
            try {
                const res = await apiPost('/api/modbus/coil/batch', {
                    slaveAddress: p.slaveAddress, channelCount: p.channelCount,
                    coilBase: p.coilBase, action: modbusAction, mode: p.relayMode
                });
                if (res && res.success && res.data && res.data.states) {
                    this._coilStates = res.data.states;
                    this._renderCoilGrid();
                    Notification.success(i18n.t('modbus-ctrl-success'));
                    this._appendDebugLog(res.debug, 'Batch: ' + action);
                } else {
                    Notification.error((res && res.error) || i18n.t('modbus-ctrl-fail'));
                    this._appendDebugError('Batch ' + action + ' failed', res && res.debug);
                }
            } catch (e) {
                Notification.error(i18n.t('modbus-ctrl-fail'));
            }
        },

        async startCoilDelay() {
            const p = this._getCoilParams();
            const ch = parseInt(document.getElementById('modbus-ctrl-delay-ch')?.value || '0');
            const units = parseInt(document.getElementById('modbus-ctrl-delay-units')?.value || '50');
            if (units < 1 || units > 255) {
                Notification.warning(i18n.t('modbus-ctrl-fail') + ': 1-255 (x100ms)');
                return;
            }
            try {
                const params = {
                    slaveAddress: p.slaveAddress, channel: ch, delayBase: 0x0200,
                    delayUnits: units, ncMode: p.ncMode, coilBase: p.coilBase
                };
                const res = await apiPost('/api/modbus/coil/delay', params);
                if (res && res.success) {
                    Notification.success(i18n.t('modbus-ctrl-delay-ok'));
                    this._appendDebugLog(res.debug, 'Delay CH' + ch + ' ' + units + 'x100ms' + (p.ncMode ? ' (NC)' : ''));
                    const refreshDelay = p.ncMode ? (units * 100 + 500) : 500;
                    setTimeout(() => this.refreshCoilStatus(), refreshDelay);
                } else {
                    Notification.error((res && res.error) || i18n.t('modbus-ctrl-fail'));
                    this._appendDebugError('Delay CH' + ch + ' failed', res && res.debug);
                }
            } catch (e) {
                Notification.error(i18n.t('modbus-ctrl-fail'));
            }
        },

        onCoilChannelCountChange() {
            this._updateDelayChannelSelect();
            this._renderCoilGrid();
        },

        _updateDelayChannelSelect() {
            const sel = document.getElementById('modbus-ctrl-delay-ch');
            if (!sel) return;
            var dev = (this._activeDeviceIdx >= 0 && this._modbusDevices)
                ? this._modbusDevices[this._activeDeviceIdx] : null;
            const count = dev ? (dev.channelCount || 8) : 8;
            let html = '';
            for (let i = 0; i < count; i++) {
                html += '<option value="' + i + '">CH' + i + '</option>';
            }
            sel.innerHTML = html;
        },

        toggleCoilAutoRefresh() {
            const checked = document.getElementById('modbus-ctrl-auto-refresh')?.checked;
            if (checked) {
                this._coilAutoRefreshErrors = 0;
                this.refreshCoilStatus();
                this._coilAutoRefreshTimer = setInterval(() => this.refreshCoilStatus(), 8000);
            } else {
                if (this._coilAutoRefreshTimer) {
                    clearInterval(this._coilAutoRefreshTimer);
                    this._coilAutoRefreshTimer = null;
                }
            }
        },

        // 连续失败 3 次后自动停止 Coil 自动刷新，避免 503 连锁
        _stopAutoRefreshOnErrors() {
            if (this._coilAutoRefreshErrors >= 3 && this._coilAutoRefreshTimer) {
                clearInterval(this._coilAutoRefreshTimer);
                this._coilAutoRefreshTimer = null;
                var el = document.getElementById('modbus-ctrl-auto-refresh');
                if (el) el.checked = false;
                console.warn('[protocol] Coil auto-refresh stopped after', this._coilAutoRefreshErrors, 'consecutive errors');
            }
        },

        // 连续失败 3 次后自动停止 PID 自动刷新
        _stopPidAutoRefreshOnErrors() {
            if (this._pidAutoRefreshErrors >= 3 && this._pidAutoRefreshTimer) {
                clearInterval(this._pidAutoRefreshTimer);
                this._pidAutoRefreshTimer = null;
                var el = document.getElementById('modbus-ctrl-pid-auto-refresh');
                if (el) el.checked = false;
                console.warn('[protocol] PID auto-refresh stopped after', this._pidAutoRefreshErrors, 'consecutive errors');
            }
        },

        // ========== 设备地址/波特率 ==========

        async readDeviceAddress() {
            const slaveAddr = parseInt(document.getElementById('modbus-debug-slave-addr')?.value || '0');
            const addrReg = parseInt(document.getElementById('modbus-ctrl-addr-reg')?.value || '0');
            try {
                const res = await apiPost('/api/modbus/device/address', { slaveAddress: slaveAddr, addressRegister: addrReg });
                const display = document.getElementById('modbus-ctrl-current-addr');
                if (res && res.success && res.data) {
                    if (display) display.textContent = 'Current: ' + res.data.currentAddress;
                    Notification.success(i18n.t('modbus-ctrl-success'));
                    this._appendDebugLog(res.debug, 'ReadAddr reg=' + addrReg);
                } else {
                    if (display) display.textContent = '';
                    Notification.error((res && res.error) || i18n.t('modbus-ctrl-fail'));
                    this._appendDebugError('ReadAddr failed', res && res.debug);
                }
            } catch (e) { Notification.error(i18n.t('modbus-ctrl-fail')); }
        },

        async setDeviceAddress() {
            const slaveAddr = parseInt(document.getElementById('modbus-debug-slave-addr')?.value || '0');
            const addrReg = parseInt(document.getElementById('modbus-ctrl-addr-reg')?.value || '0');
            const newAddr = parseInt(document.getElementById('modbus-ctrl-new-addr')?.value || '0');
            if (newAddr < 1 || newAddr > 255) { Notification.warning('Invalid address (1-255)'); return; }
            try {
                const res = await apiPost('/api/modbus/device/address', { slaveAddress: slaveAddr, addressRegister: addrReg, newAddress: newAddr });
                if (res && res.success) {
                    Notification.success(i18n.t('modbus-ctrl-success'));
                    this._appendDebugLog(res.debug, 'SetAddr -> ' + newAddr);
                } else {
                    Notification.error((res && res.error) || i18n.t('modbus-ctrl-fail'));
                    this._appendDebugError('SetAddr failed', res && res.debug);
                }
            } catch (e) { Notification.error(i18n.t('modbus-ctrl-fail')); }
        },

        async setDeviceBaudrate() {
            const slaveAddr = parseInt(document.getElementById('modbus-debug-slave-addr')?.value || '0');
            const baud = parseInt(document.getElementById('modbus-ctrl-baudrate')?.value || '9600');
            try {
                const res = await apiPost('/api/modbus/device/baudrate', { slaveAddress: slaveAddr, baudRate: baud });
                if (res && res.success) {
                    Notification.success(i18n.t('modbus-ctrl-success'));
                    this._appendDebugLog(res.debug, 'SetBaud -> ' + baud);
                } else {
                    Notification.error((res && res.error) || i18n.t('modbus-ctrl-fail'));
                    this._appendDebugError('SetBaud failed', res && res.debug);
                }
            } catch (e) { Notification.error(i18n.t('modbus-ctrl-fail')); }
        },

        async readDiscreteInputs() {
            const slaveAddr = parseInt(document.getElementById('modbus-debug-slave-addr')?.value || '0');
            const inputCount = parseInt(document.getElementById('modbus-ctrl-input-count')?.value || '4');
            const inputBase = parseInt(document.getElementById('modbus-ctrl-input-base')?.value || '0');
            try {
                const res = await apiGet('/api/modbus/device/inputs', { slaveAddress: slaveAddr, inputCount: inputCount, inputBase: inputBase });
                const display = document.getElementById('modbus-ctrl-inputs-display');
                if (res && res.success && res.data && res.data.states) {
                    const states = res.data.states;
                    let html = '<div class="protocol-chip-list">';
                    for (let i = 0; i < states.length; i++) {
                        html += '<span class="protocol-chip' + (states[i] ? ' is-on' : '') + '">'
                              + 'IN' + i + ':' + (states[i] ? 'ON' : 'OFF') + '</span>';
                    }
                    html += '</div>';
                    if (display) display.innerHTML = html;
                    Notification.success(i18n.t('modbus-ctrl-success'));
                    this._appendDebugLog(res.debug, 'ReadInputs FC02');
                } else {
                    if (display) display.innerHTML = '';
                    Notification.error((res && res.error) || i18n.t('modbus-ctrl-fail'));
                    this._appendDebugError('ReadInputs failed', res && res.debug);
                }
            } catch (e) { Notification.error(i18n.t('modbus-ctrl-fail')); }
        },

        // ========== UART 外设 ==========

        async _loadUartPeripherals(selectedId) {
            const select = document.getElementById('rtu-peripheral-id');
            if (!select) return;
            try {
                const res = await apiGet('/api/peripherals?pageSize=50');
                if (!res || !res.success) return;
                this._uartPeripherals = (res.data || []).filter(p => p.type === 1 && p.enabled);
                select.innerHTML = '<option value="" disabled>' + i18n.t('rtu-peripheral-placeholder') + '</option>';
                if (this._uartPeripherals.length === 0) {
                    select.innerHTML += '<option value="" disabled>' + i18n.t('rtu-no-uart-peripherals') + '</option>';
                    return;
                }
                this._uartPeripherals.forEach(p => {
                    const pinsText = p.pins && p.pins.length >= 2 ? ' (RX:' + p.pins[0] + ', TX:' + p.pins[1] + ')' : '';
                    const opt = document.createElement('option');
                    opt.value = p.id;
                    opt.textContent = p.name + pinsText;
                    select.appendChild(opt);
                });
                if (selectedId) {
                    select.value = selectedId;
                    this.onRtuPeripheralChange(selectedId);
                }
            } catch (e) { console.error('Failed to load UART peripherals:', e); }
        },

        onRtuPeripheralChange(peripheralId) {
            const infoDiv = document.getElementById('rtu-peripheral-info');
            if (!infoDiv || !peripheralId) { if (infoDiv) AppState.hideElement(infoDiv); return; }
            const periph = (this._uartPeripherals || []).find(p => p.id === peripheralId);
            if (!periph) { AppState.hideElement(infoDiv); return; }
            apiGet('/api/peripherals/?id=' + peripheralId).then(res => {
                if (!res || !res.success) return;
                const data = res.data;
                const baudRate = data.params?.baudRate || i18n.t('unknown') || '未知';
                const pins = data.pins || [];
                infoDiv.textContent = 'RX: GPIO' + pins[0] + ', TX: GPIO' + pins[1] + ', ' + i18n.t('uart-baudrate-label') + ': ' + baudRate;
                AppState.showElement(infoDiv);
            }).catch(() => {
                const pins = periph.pins || [];
                infoDiv.textContent = 'RX: GPIO' + pins[0] + ', TX: GPIO' + pins[1];
                AppState.showElement(infoDiv);
            });
        }
    });

    // Part 3: MQTT enhanced, UI helpers, export/import
    Object.assign(AppState, {

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

            // 使用全局 SSE 监听 mqtt-status 事件，替代 5s 轮询
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

            // 初始加载一次
            this._loadMqttStatus();
        },

        _stopMqttStatusPolling() {
            // 清理旧的定时器（向后兼容）
            if (this._mqttStatusTimer) {
                clearInterval(this._mqttStatusTimer);
                this._mqttStatusTimer = null;
            }
            // 清理 SSE 监听
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
                    // 其他错误（如网络超时）保持"检测中..."状态，不更新UI
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
        },

        // ========== UI 辅助 ==========

        onHttpAuthTypeChange(type) {
            const userGroup = document.getElementById('http-auth-user')?.closest('.pure-control-group');
            const tokenGroup = document.getElementById('http-auth-token')?.closest('.pure-control-group');
            if (userGroup) AppState.toggleVisible(userGroup, type === 'basic');
            if (tokenGroup) AppState.toggleVisible(tokenGroup, type === 'basic' || type === 'bearer');
        },
        
        onTcpModeChange(mode) {
            const clientConfig = document.getElementById('tcp-client-config');
            const serverConfig = document.getElementById('tcp-server-config');
            if (clientConfig) AppState.toggleVisible(clientConfig, mode === 'client');
            if (serverConfig) AppState.toggleVisible(serverConfig, mode === 'server');
        },

        // ========== 导入/导出 ==========

        exportProtocolConfig() {
            apiGet('/api/protocol/config')
                .then(res => {
                    if (!res || !res.success) { Notification.error(i18n.t('protocol-export-fail'), i18n.t('protocol-config-title')); return; }
                    const jsonStr = JSON.stringify(res.data || {}, null, 2);
                    const blob = new Blob([jsonStr], { type: 'application/json' });
                    const url = URL.createObjectURL(blob);
                    const a = document.createElement('a');
                    a.href = url; a.download = 'protocol-config.json';
                    document.body.appendChild(a); a.click(); document.body.removeChild(a);
                    URL.revokeObjectURL(url);
                    Notification.success(i18n.t('protocol-export-ok'), i18n.t('protocol-config-title'));
                })
                .catch(err => {
                    console.error('Export protocol config failed:', err);
                    Notification.error(i18n.t('protocol-export-fail'), i18n.t('protocol-config-title'));
                });
        },

        importProtocolConfig() {
            const input = document.createElement('input');
            input.type = 'file'; input.accept = '.json';
            input.onchange = (e) => {
                const file = e.target.files[0];
                if (!file) return;
                const reader = new FileReader();
                reader.onload = (evt) => {
                    try {
                        const config = JSON.parse(evt.target.result);
                        if (!confirm(i18n.t('protocol-import-confirm'))) return;
                        const data = {};
                        const flatten = (obj, prefix) => {
                            for (const key in obj) {
                                const val = obj[key];
                                const flatKey = prefix ? prefix + '_' + key : key;
                                if (val !== null && typeof val === 'object' && !Array.isArray(val)) {
                                    flatten(val, flatKey);
                                } else {
                                    data[flatKey] = typeof val === 'object' ? JSON.stringify(val) : String(val);
                                }
                            }
                        };
                        flatten(config, '');
                        apiPost('/api/protocol/config', data)
                            .then(res => {
                                if (res && res.success) {
                                    this._protocolConfig = null;
                                    Notification.success(i18n.t('protocol-import-ok'), i18n.t('protocol-config-title'));
                                    const activeTab = document.querySelector('#protocol-page .config-tab.active');
                                    if (activeTab) { const tabId = activeTab.getAttribute('data-tab'); this.loadProtocolConfig(tabId); }
                                } else {
                                    Notification.error(i18n.t('protocol-import-fail'), i18n.t('protocol-config-title'));
                                }
                            })
                            .catch(() => { Notification.error(i18n.t('protocol-import-fail'), i18n.t('protocol-config-title')); });
                    } catch (parseErr) {
                        Notification.error(i18n.t('protocol-import-invalid'), i18n.t('protocol-config-title'));
                    }
                };
                reader.readAsText(file);
            };
            input.click();
        }
    });

    // 自动绑定事件
    if (typeof AppState.setupProtocolEvents === 'function') {
        AppState.setupProtocolEvents();
    }
})();
