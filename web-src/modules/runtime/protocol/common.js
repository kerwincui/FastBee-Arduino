/**
 * protocol/common.js — 共享状态、事件绑定、UI 渲染辅助函数
 * 注意：使用 Object.assign 而非 registerModule，避免子模块加载时过早触发回调
 *       最终 registerModule 调用由入口 protocol.js 在所有子模块加载完成后执行
 */
(function() {
    // This lazy module must also work when protocol is the first page opened.
    const t = typeof window.t === 'function' ? window.t : (key, fallback) => fallback;
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
        _modbusCtrlOffline: false,
        _protocolEventsBound: false,
        _modbusControlModuleLoading: false,
        _modbusControlPendingIndex: -1,
        _modbusControlPendingButton: null,
        _modbusControlLoadTimer: null,

        // ============ 事件绑定 ============
        setupProtocolEvents() {
            var boundAny = false;
            var bindOnce = function(el, eventName, flagName, handler) {
                if (!el || el[flagName]) return false;
                el.addEventListener(eventName, handler);
                el[flagName] = true;
                return true;
            };
            var allDevicesBody = document.getElementById('all-devices-body');
            var modbusDevicesBody = document.getElementById('modbus-devices-body');

            // 如果关键表格元素尚未加载到 DOM，延迟绑定
            if (false && !allDevicesBody && !modbusDevicesBody) {
                return; // 不设置 _protocolEventsBound，下次调用时重试
            }

            var publishTopics = document.getElementById('mqtt-publish-topics');
            boundAny = bindOnce(publishTopics, 'click', '__fbProtocolPublishTopicClick', (event) => this._handlePublishTopicClick(event)) || boundAny;
            var subscribeTopics = document.getElementById('mqtt-subscribe-topics');
            boundAny = bindOnce(subscribeTopics, 'click', '__fbProtocolSubscribeTopicClick', (event) => this._handleSubscribeTopicClick(event)) || boundAny;
            var mappingTableBody = document.getElementById('mapping-table-body');
            boundAny = bindOnce(mappingTableBody, 'click', '__fbProtocolMappingClick', (event) => this._handleMappingTableClick(event)) || boundAny;
            boundAny = bindOnce(allDevicesBody, 'click', '__fbProtocolActionClick', (event) => this._handleProtocolActionClick(event)) || boundAny;
            boundAny = bindOnce(modbusDevicesBody, 'click', '__fbProtocolActionClick', (event) => this._handleProtocolActionClick(event)) || boundAny;
            var pwmGrid = document.getElementById('pwm-channel-grid');
            boundAny = bindOnce(pwmGrid, 'input', '__fbProtocolPwmInput', (event) => this._handlePwmGridInput(event)) || boundAny;
            boundAny = bindOnce(pwmGrid, 'change', '__fbProtocolPwmChange', (event) => this._handlePwmGridChange(event)) || boundAny;
            var pidGrid = document.getElementById('pid-data-grid');
            boundAny = bindOnce(pidGrid, 'click', '__fbProtocolPidClick', (event) => this._handlePidGridClick(event)) || boundAny;
            var coilGrid = document.getElementById('coil-status-grid');
            boundAny = bindOnce(coilGrid, 'click', '__fbProtocolCoilClick', (event) => this._handleCoilGridClick(event)) || boundAny;
            // MQTT 协议切换：scheme 下拉框变更时自动更新端口默认值
            var mqttScheme = document.getElementById('mqtt-scheme');
            boundAny = bindOnce(mqttScheme, 'change', '__fbMqttSchemeChange', (event) => {
                var portEl = document.getElementById('mqtt-port');
                if (!portEl) return;
                var newScheme = event.target.value;
                // 仅在端口为默认值时自动切换，避免覆盖用户自定义端口
                var currentPort = parseInt(portEl.value, 10);
                if (newScheme === 'mqtts' && (currentPort === 1883 || !portEl.value)) {
                    portEl.value = 8883;
                } else if (newScheme === 'mqtt' && (currentPort === 8883 || !portEl.value)) {
                    portEl.value = 1883;
                }
                // 切换到 mqtts 时提醒内存占用与功能开关状态：
                // TLS 握手需 ~42KB DRAM，功能配置页未启用时设备会拒绝连接
                if (newScheme === 'mqtts' && typeof Notification !== 'undefined' && typeof apiGet === 'function') {
                    apiGet('/api/features/status').then(function(res) {
                        var feats = res && res.data && res.data.features;
                        var f = Array.isArray(feats) ? feats.find(function(x) { return x.key === 'mqtts'; }) : null;
                        if (!f) return;
                        if (!f.compiled) {
                            Notification.warning('MQTTS 未编入当前固件，无法使用', 'MQTTS');
                        } else if (!f.active) {
                            Notification.warning('TLS 需约 42KB 内存：请先到功能配置页启用「MQTTS 加密传输」，否则设备将拒绝连接', 'MQTTS');
                        } else {
                            Notification.info('MQTTS 已启用：TLS 握手约占用 42KB 内存', 'MQTTS');
                        }
                    }).catch(function() { /* 状态查询失败不阻断配置流程 */ });
                }
            }) || boundAny;
            if (boundAny) this._protocolEventsBound = true;
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
            if (action !== 'open-control-modal' && !this.guardDeveloperModeAction()) return;
            if (action === 'edit-device') this._editDevice(source, index);
            else if (action === 'open-mapping') this.openMappingModal(index);
            else if (action === 'delete-device') this._deleteDevice(source, index);
            else if (action === 'open-edit-modal') this._openEditModal(index);
            else if (action === 'open-control-modal') this._openModbusControlModal(index, button);
            else if (action === 'remove-device') this._removeDevice(index);
        },

        _setProtocolActionLoading(button, loading) {
            if (!button) return;
            if (loading) {
                if (!button.dataset.fbOriginalText) button.dataset.fbOriginalText = button.textContent || '';
                button.disabled = true;
                button.textContent = '加载中...';
                return;
            }
            if (button.dataset.fbOriginalText) {
                button.textContent = button.dataset.fbOriginalText;
                delete button.dataset.fbOriginalText;
            }
            button.disabled = false;
        },

        _openModbusControlModal(index, button) {
            if (typeof this._openCtrlModal === 'function') {
                this._openCtrlModal(index);
                return;
            }
            if (typeof ModuleLoader === 'undefined' || typeof ModuleLoader.loadModule !== 'function') {
                Notification.error('加载失败，请刷新重试');
                return;
            }

            if (this._modbusControlPendingButton && this._modbusControlPendingButton !== button) {
                this._setProtocolActionLoading(this._modbusControlPendingButton, false);
            }
            this._modbusControlPendingIndex = index;
            this._modbusControlPendingButton = button || this._modbusControlPendingButton;
            this._setProtocolActionLoading(this._modbusControlPendingButton, true);

            if (this._modbusControlModuleLoading) return;

            var self = this;
            this._modbusControlModuleLoading = true;
            if (this._modbusControlLoadTimer) clearTimeout(this._modbusControlLoadTimer);
            this._modbusControlLoadTimer = setTimeout(function() {
                if (!self._modbusControlModuleLoading) return;
                self._modbusControlModuleLoading = false;
                self._setProtocolActionLoading(self._modbusControlPendingButton, false);
                self._modbusControlPendingButton = null;
                self._modbusControlPendingIndex = -1;
                Notification.error(t('common-load-fail-retry', '加载失败，请刷新重试'));
            }, 20000);

            ModuleLoader.loadModule('protocol-modbus-control', function() {
                var targetIndex = self._modbusControlPendingIndex;
                var targetButton = self._modbusControlPendingButton;
                self._modbusControlModuleLoading = false;
                if (self._modbusControlLoadTimer) {
                    clearTimeout(self._modbusControlLoadTimer);
                    self._modbusControlLoadTimer = null;
                }
                self._modbusControlPendingIndex = -1;
                self._modbusControlPendingButton = null;
                self._setProtocolActionLoading(targetButton, false);

                if (typeof self.setupProtocolEvents === 'function') self.setupProtocolEvents();
                if (typeof self._openCtrlModal === 'function' && targetIndex >= 0) {
                    self._openCtrlModal(targetIndex);
                } else {
                    Notification.error(t('common-load-fail-retry', '加载失败，请刷新重试'));
                }
            });
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
            if (typeof this._isCtrlModalOffline === 'function' && this._isCtrlModalOffline()) return;
            var card = event.target.closest('.coil-card');
            if (!card) return;
            var ch = parseInt(card.getAttribute('data-ch'), 10);
            if (!isNaN(ch)) this.toggleCoil(ch);
        },

        // ============ UI 渲染辅助 ============

        _setProtocolFragmentLoading(tabId) {
            var fragInfo = this._protocolFragmentMap && this._protocolFragmentMap[tabId];
            if (!fragInfo) return;
            var container = document.getElementById(fragInfo.container);
            if (!container || container.querySelector('form')) return;
            var title = '加载中...';
            container.innerHTML = '<div class="fb-loading-placeholder protocol-loading-placeholder">' +
                '<div class="fb-loading-placeholder-title">' + title + '</div>' +
                '</div>';
        },

        _setProtocolFragmentError(tabId, message) {
            var fragInfo = this._protocolFragmentMap && this._protocolFragmentMap[tabId];
            if (!fragInfo) return;
            var container = document.getElementById(fragInfo.container);
            if (!container || container.querySelector('form')) return;
            container.innerHTML = '<div class="message message-error">' +
                escapeHtml(message || '加载失败，请刷新重试') +
                '</div>';
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

        // ========== UI 辅助 ==========

        _getProtocolName(formId) {
            var map = {
                'modbus-rtu-form': 'Modbus RTU',
                'mqtt-form': 'MQTT'
            };
            return map[formId] || 'Protocol';
        }
    });
})();
