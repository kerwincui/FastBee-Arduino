/**
 * protocol/common.js — 共享状态、事件绑定、UI 渲染辅助函数
 * 注意：使用 Object.assign 而非 registerModule，避免子模块加载时过早触发回调
 *       最终 registerModule 调用由入口 protocol.js 在所有子模块加载完成后执行
 */
(function() {
    Object.assign(AppState, {

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

        // ============ 事件处理器 ============

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

        // ============ UI 渲染辅助 ============

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
            return '<button type="button" class="fb-btn fb-btn-sm protocol-action-btn protocol-action-btn--' + tone +
                '" ' + attrs + '>' + label + '</button>';
        },

        _renderProtocolActionCell(buttons) {
            return '<td class="protocol-action-cell">' + buttons.join('') + '</td>';
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

        _renderMqttQosOptions(selectedQos) {
            return [
                '<option value="0"' + (selectedQos === 0 ? ' selected' : '') + '>0</option>',
                '<option value="1"' + (selectedQos === 1 ? ' selected' : '') + '>1</option>',
                '<option value="2"' + (selectedQos === 2 ? ' selected' : '') + '>2</option>'
            ].join('');
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

        // ========== 设备行渲染 ==========

        _renderAllDeviceSensorRow(task, index, fcNames, typeLabels) {
            var label = task.name || task.label || ('Slave ' + (task.slaveAddress || 1));
            var info = (fcNames[task.functionCode] || 'FC03') + ' @' + (task.startAddress || 0) + ' x' + (task.quantity || 10);
            var mappingCount = (task.mappings && task.mappings.length) || 0;
            if (mappingCount > 0) info += ' [' + mappingCount + (i18n.t('modbus-dev-mappings-suffix') || '映射') + ']';
            var isTransparent = this._isTransparentMode();
            var actions = [
                this._renderProtocolActionButton(i18n.t('modbus-task-edit-btn') || '编辑', 'primary', 'edit-device', index, 'sensor')
            ];
            if (!isTransparent) {
                actions.push(this._renderProtocolActionButton(i18n.t('modbus-mapping-btn') || '映射', 'warning', 'open-mapping', index));
            }
            actions.push(this._renderProtocolActionButton(i18n.t('modbus-master-delete-task') || '删除', 'danger', 'delete-device', index, 'sensor'));
            return '<tr>' +
                '<td>' + escapeHtml(label) + '</td>' +
                '<td>' + this._renderProtocolBadge('sensor', typeLabels.sensor) + '</td>' +
                '<td>' + (task.slaveAddress || 1) + '</td>' +
                '<td><small>' + escapeHtml(info) + '</small></td>' +
                '<td>' + this._renderProtocolStatus(task.enabled) + '</td>' +
                this._renderProtocolActionCell(actions) +
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

        // ========== UI 辅助 ==========

        onHttpAuthTypeChange(type) {
            const userGroup = document.getElementById('http-auth-user')?.closest('.fb-form-group');
            const tokenGroup = document.getElementById('http-auth-token')?.closest('.fb-form-group');
            if (userGroup) AppState.toggleVisible(userGroup, type === 'basic');
            if (tokenGroup) AppState.toggleVisible(tokenGroup, type === 'basic' || type === 'bearer');
        },

        onTcpModeChange(mode) {
            const clientConfig = document.getElementById('tcp-client-config');
            const serverConfig = document.getElementById('tcp-server-config');
            if (clientConfig) AppState.toggleVisible(clientConfig, mode === 'client');
            if (serverConfig) AppState.toggleVisible(serverConfig, mode === 'server');
        },

        _getProtocolName(formId) {
            var map = {
                'modbus-rtu-form': 'Modbus RTU',
                'modbus-tcp-form': 'Modbus TCP',
                'mqtt-form': 'MQTT',
                'http-form': 'HTTP',
                'coap-form': 'CoAP',
                'tcp-form': 'TCP'
            };
            return map[formId] || 'Protocol';
        }
    });
})();
