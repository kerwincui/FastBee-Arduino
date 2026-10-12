/**
 * 外设执行管理模块 - 核心入口
 * 页面初始化、CRUD API 调用、列表渲染、分页、事件绑定
 * 子模块: periph-exec-modbus.js、periph-exec-form.js，以及按需加载的传感器/灯效扩展
 */
(function() {
    Object.assign(AppState, window.PeriphExecRunValuePrompt);
    AppState.registerModule('periph-exec', {

        // ============ 状态变量 ============
        _pePeripherals: [],
        _peDataSources: [],
        _peSensorSources: [],
        _peThermostats: null,
        _peSdRecorder: undefined,
        _peRemoteChannels: [],
        _peWeatherSources: [],
        _peRemoteResourcesAvailable: false,
        _peCurrentPage: 1,
        _pePageSize: 10,
        _peTotalRules: 0,
        _peProfile: null,
        _peEventsBound: false,
        _peModbusHealth: null,
        _peModbusHealthFetchedAt: 0,
        _peModbusHealthPromise: null,
        _peEventCategories: null,
        _peEventCategoriesFetchedAt: 0,
        _peEventCategoriesPromise: null,
        _peStaticEvents: null,
        _peStaticEventsFetchedAt: 0,
        _peStaticEventsPromise: null,
        _peDynamicEvents: null,
        _peDynamicEventsFetchedAt: 0,
        _peDynamicEventsPromise: null,
        _periphExecRunPromptState: AppState._periphExecRunPromptState || null,
        _peEditLoadSeq: 0,

        _isPeriphExecEditorReady() {
            return typeof this._createPeriphExecTriggerElement === 'function' &&
                typeof this._createPeriphExecActionElement === 'function' &&
                typeof this.savePeriphExecRule === 'function' &&
                typeof this._uartSendHelp === 'function';
        },

        _ensurePeriphExecEditor() {
            var self = this;
            if (this._isPeriphExecEditorReady()) {
                return Promise.resolve();
            }
            if (typeof ModuleLoader === 'undefined' ||
                !ModuleLoader ||
                typeof ModuleLoader.loadModule !== 'function') {
                return Promise.reject(new Error('Periph exec editor loader unavailable'));
            }
            return new Promise(function(resolve, reject) {
                var timer = setTimeout(function() {
                    reject(new Error('Periph exec editor load timeout'));
                }, 15000);
                ModuleLoader.loadModule('periph-exec-editor', function() {
                    clearTimeout(timer);
                    if (self._isPeriphExecEditorReady()) {
                        resolve();
                    } else {
                        reject(new Error('Periph exec editor did not register'));
                    }
                });
            });
        },

        _buildPeriphExecInlineActionPanel(actionType, actionValue) {
            if (actionType === 15) {
                return '<textarea class="pe-action-value-script pe-script-textarea" rows="7" maxlength="1024" placeholder="每行一条命令">' + escapeHtml(actionValue || '') + '</textarea>' +
                    '<small class="pe-help-text">可用: GPIO pin HIGH/LOW, DELAY ms, PWM pin duty, DAC pin val, LOG msg, PERIPH id 动作</small>';
            }
            if (actionType === 21) {
                return '<select class="pe-trigger-event-select"><option value="">-- 选择事件 --</option>' +
                    '<optgroup label="WiFi"><option value="wifi_connected">WiFi连接成功</option><option value="wifi_disconnected">WiFi断开连接</option><option value="wifi_conn_failed">WiFi连接失败</option></optgroup>' +
                    '<optgroup label="MQTT"><option value="mqtt_connected">MQTT连接成功</option><option value="mqtt_disconnected">MQTT断开连接</option><option value="mqtt_conn_failed">MQTT连接失败</option><option value="mqtt_enabled">MQTT协议启用</option></optgroup>' +
                    '<optgroup label="系统"><option value="system_boot">系统启动</option><option value="system_ready">系统就绪</option><option value="system_error">系统错误</option><option value="factory_reset">恢复出厂设置</option><option value="ntp_synced">NTP同步完成</option><option value="ota_start">OTA升级开始</option><option value="ota_success">OTA升级成功</option><option value="ota_failed">OTA升级失败</option><option value="break_down">设备故障</option><option value="restart">设备重启</option><option value="device_alarm">设备告警</option><option value="low_power">低电量预警</option></optgroup>' +
                    '<optgroup label="按键"><option value="button_click">按键单击</option><option value="button_double_click">按键双击</option><option value="button_long_press_2s">按键长按2秒</option><option value="button_long_press_5s">按键长按5秒</option><option value="button_long_press_10s">按键长按10秒</option><option value="button_press">按键按下</option><option value="button_release">按键释放</option></optgroup>' +
                    '<optgroup label="数据"><option value="data_receive">数据接收</option><option value="data_report">数据上报</option></optgroup>' +
                    '<optgroup label="外设执行"><option value="periph_exec_completed">外设执行完成</option></optgroup>' +
                    '<optgroup label="RFID/红外/RF"><option value="rfid_card_detected">RFID卡片检测</option><option value="rfid_card_removed">RFID卡片移开</option><option value="ir_code_received">红外编码接收</option><option value="rf_code_received">RF编码接收</option></optgroup></select>' +
                    '<small class="pe-trigger-event-help">选择要模拟触发的设备事件</small>';
            }
            return '';
        },


        _snapshotPeriphExecLazyPanel(host) {
            if (!host) return [];
            return Array.from(host.querySelectorAll('input, select, textarea')).map(function(control) {
                return {
                    value: control.value,
                    checked: control.type === 'checkbox' ? !!control.checked : null
                };
            });
        },

        _restorePeriphExecLazyPanel(host, snapshot) {
            if (!host || !Array.isArray(snapshot)) return;
            var controls = host.querySelectorAll('input, select, textarea');
            snapshot.forEach(function(state, index) {
                var control = controls[index];
                if (!control) return;
                if (state.checked !== null && control.type === 'checkbox') control.checked = state.checked;
                if (state.value !== undefined) control.value = state.value;
            });
        },

        _getPeriphExecLazyActionModule(actionType, targetId) {
            if (actionType === 19 || actionType === 39 || actionType === 40) return 'periph-exec-sensor';
            if (actionType === 27 || actionType === 11 ||
                (actionType === 10 && this._isNeoPixelTarget(targetId))) {
                return 'periph-exec-effects';
            }
            return '';
        },

        _loadPeriphExecLazyActionModule(moduleName, block, actionType, data) {
            var host = block.querySelector('.pe-action-lazy-host');
            var self = this;
            var token = (block._peLazyLoadToken || 0) + 1;
            block._peLazyLoadToken = token;
            host.innerHTML = '<small class="pe-help-text">正在加载配置...</small>';
            if (typeof ModuleLoader === 'undefined' || !ModuleLoader ||
                typeof ModuleLoader.loadModule !== 'function') {
                host.innerHTML = '<small class="pe-help-text">配置模块加载失败，请刷新页面重试</small>';
                block.dataset.peLazyModuleError = moduleName;
                return;
            }
            ModuleLoader.loadModule(moduleName, function() {
                if (!block.isConnected || block._peLazyLoadToken !== token) return;
                delete block.dataset.peLazyModuleError;
                self._mountPeriphExecLazyActionPanel(block, actionType, data, true);
            });
        },

        _isNeoPixelTarget(periphId) {
            if (!periphId) return false;
            return (this._pePeripherals || []).some(function(periph) {
                return periph.id === periphId && periph.type === 45;
            });
        },

        _isSetPwmTarget(periphId) {
            if (!periphId) return false;
            return (this._pePeripherals || []).some(function(periph) {
                return periph.id === periphId &&
                    (periph.type === 17 || (periph.type === 12 && periph.passive === true));
            });
        },

        _isRfModuleTarget(periphId) {
            if (!periphId) return false;
            return (this._pePeripherals || []).some(function(periph) {
                return periph.id === periphId && periph.type === 48;
            });
        },

        // ============ 事件绑定 ============
        setupPeriphExecEvents() {
            if (this._peEventsBound) return;
            var ruleTableBody = document.getElementById('periph-exec-table-body');
            if (ruleTableBody) {
                ruleTableBody.addEventListener('click', (event) => {
                    var button = event.target.closest('[data-pe-action]');
                    if (!button) return;
                    var action = button.getAttribute('data-pe-action');
                    var ruleId = button.getAttribute('data-id');
                    if (!action || !ruleId) return;
                    if (action !== 'run' && !this.guardDeveloperModeAction()) return;
                    if (action === 'run') this.runPeriphExecOnce(ruleId, button.getAttribute('data-has-set-mode') === 'true', button.getAttribute('data-name') || '');
                    else if (action === 'edit') this.editPeriphExecRule(ruleId);
                    else if (action === 'toggle') this.togglePeriphExecRule(ruleId, button.getAttribute('data-next-enabled') === 'true');
                    else if (action === 'delete') this.deletePeriphExecRule(ruleId);
                });
            }
            // 刷新按钮
            var peRefreshBtn = document.getElementById('periph-exec-refresh-btn');
            if (peRefreshBtn) peRefreshBtn.addEventListener('click', () => this._refreshPeriphExecList());
            var peFilter = document.getElementById('periph-exec-filter-periph');
            if (peFilter) {
                var loadFilterOptions = () => this._populatePeriphExecFilter();
                peFilter.addEventListener('focus', loadFilterOptions);
                peFilter.addEventListener('pointerdown', loadFilterOptions);
            }
            // 注册模态窗事件绑定器（模态窗 DOM 加载后由 _loadModals 触发）
            if (typeof this._registerModalBinder === 'function') {
                this._registerModalBinder('periph-exec', () => this._bindPeriphExecModalEvents());
            }
            this._peEventsBound = true;
        },

        /**
         * 绑定模态窗内的事件（模态窗 DOM 已就绪后调用）
         * 由 _loadModals → _rebindAllModalEvents 触发
         */
        _bindPeriphExecModalEvents() {
            if (this._peModalBound) return;
            var closePeriphExecModal = document.getElementById('close-periph-exec-modal');
            if (closePeriphExecModal) closePeriphExecModal.addEventListener('click', () => this.closePeriphExecModal());
            var cancelPeriphExecBtn = document.getElementById('cancel-periph-exec-btn');
            if (cancelPeriphExecBtn) cancelPeriphExecBtn.addEventListener('click', () => this.closePeriphExecModal());
            var savePeriphExecBtn = document.getElementById('save-periph-exec-btn');
            if (savePeriphExecBtn) savePeriphExecBtn.addEventListener('click', () => this.savePeriphExecRule());
            var historySettingsLink = document.getElementById('periph-exec-history-settings-link');
            if (historySettingsLink) historySettingsLink.addEventListener('click', () => {
                window.open(window.location.pathname + '?page=screen-panel&tab=history', '_blank', 'noopener');

            });
            var triggerContainer = document.getElementById('periph-exec-triggers');
            if (triggerContainer) {
                triggerContainer.addEventListener('click', (event) => this._handlePeriphExecTriggerClick(event));
                triggerContainer.addEventListener('change', (event) => this._handlePeriphExecTriggerChange(event));
                triggerContainer.addEventListener('input', (event) => this._handlePeriphExecTriggerInput(event));
            }
            var actionContainer = document.getElementById('periph-exec-actions');
            if (actionContainer) {
                actionContainer.addEventListener('click', (event) => this._handlePeriphExecActionClick(event));
                actionContainer.addEventListener('change', (event) => this._handlePeriphExecActionChange(event));
            }
            this._peModalBound = true;
        },

        // ============ 模态框 ============

        async openPeriphExecModal(editId) {
            if (!this.guardDeveloperModeAction()) return;
            if (!editId && this._isPeriphExecCapacityFull()) {
                Notification.warning('当前资源档位执行规则数量已达上限', '外设执行');
                return;
            }
            if (!this._isPeriphExecEditorReady()) {
                var self = this;
                return this._ensurePeriphExecEditor()
                    .then(function() { return self.openPeriphExecModal(editId); })
                    .catch(function(err) {
                        console.error('Failed to load periph exec editor:', err);
                        if (typeof Notification !== 'undefined') {
                            Notification.error('加载执行规则编辑器失败');
                        }
                    });
            }
            const modal = await this._ensureModalForAction('periph-exec-modal');
            if (!modal) return;
            this._peEditorDataReady = false;
            const saveButton = document.getElementById('save-periph-exec-btn');
            if (saveButton) saveButton.disabled = true;
            const titleEl = document.getElementById('periph-exec-modal-title');
            var safeId = (editId && editId !== 'null' && editId !== 'undefined') ? editId : '';
            document.getElementById('periph-exec-original-id').value = safeId;
            this.clearInlineError('periph-exec-error');
            this._pePreflightReview = null;
            document.getElementById('periph-exec-preflight')?.remove();
            if (safeId) {
                if (titleEl) titleEl.textContent = '编辑外设执行';
            } else {
                if (titleEl) titleEl.textContent = '新增外设执行';
                document.getElementById('periph-exec-form').reset();
            }
            const triggersContainer = document.getElementById('periph-exec-triggers');
            const actionsContainer = document.getElementById('periph-exec-actions');
            if (triggersContainer) triggersContainer.innerHTML = '';
            if (actionsContainer) actionsContainer.innerHTML = '';
            if (typeof this._updatePeriphExecAddButtons === 'function') this._updatePeriphExecAddButtons();
            this._clearPeriphExecRiskNotice();
            if (typeof this._resetPeriphExecTemplateWizard === 'function') this._resetPeriphExecTemplateWizard(!safeId);
            if (safeId) { this.showModal(modal); return; }
            this._pePeripherals = [];
            this._peDataSources = [];
            this._peSensorSources = [];
            this._peExecRules = [];
            this._peRemoteChannels = [];
            this._peWeatherSources = [];
            this._peRemoteResourcesAvailable = false;
            const loadSeq = ++this._peEditLoadSeq;
            const pageGeneration = this._getPeriphExecPageGeneration();
            const ensureCreateActive = () => {
                if (!this._isPeriphExecEditLoadActive(loadSeq, pageGeneration)) {
                    const err = new Error('Request cancelled by page navigation');
                    err._pageAborted = true;
                    throw err;
                }
            };
            // Keep the create editor to one in-flight request. Three parallel
            // responses can exhaust the classic ESP32 TCP/heap reserve.
            const createResults = {};
            Promise.resolve()
                .then(() => {
                    ensureCreateActive();
                    return apiGet('/api/peripherals', { pageSize: 100, compact: 1 });
                })
                .then((periphRes) => {
                    ensureCreateActive();
                    this._requirePeriphExecData(periphRes);
                    createResults.periphRes = periphRes;
                    return apiGet('/api/peripherals/types');
                })
                .then((typeRes) => {
                    ensureCreateActive();
                    this._requirePeriphExecData(typeRes);
                    this._storePeriphExecTypeCatalog(typeRes);
                    return apiGet('/api/protocol/config', { compact: 1, section: 'periph-exec' });
                })
                .then((protoRes) => {
                    ensureCreateActive();
                    this._requirePeriphExecData(protoRes);
                    createResults.protoRes = protoRes;
                    return apiGet('/api/periph-exec', { pageSize: 100 });
                })
                .then((rulesRes) => {
                    ensureCreateActive();
                    this._requirePeriphExecData(rulesRes);
                    createResults.rulesRes = rulesRes;
                    return this._loadPeriphExecRemoteChoices(ensureCreateActive);
                })
                .then(() => {
                    ensureCreateActive();
                    const periphRes = createResults.periphRes;
                    const protoRes = createResults.protoRes;
                    const rulesRes = createResults.rulesRes;
                    if (periphRes && periphRes.success && periphRes.data) this._pePeripherals = periphRes.data;
                    if (protoRes && protoRes.success && protoRes.data) {
                        const protoData = protoRes.data;
                        this._peDataSources = this._extractDataSources(protoData);
                        this._masterTasks = [];
                        this._modbusDevices = [];
                        this._peTemplateModbusConfig = protoData.degraded ? null : (protoData.modbusRtu || null);
                        if (protoData.modbusRtu && protoData.modbusRtu.master) {
                            this._masterTasks = protoData.modbusRtu.master.tasks || [];
                            this._modbusDevices = protoData.modbusRtu.master.devices || [];
                        }
                    }
                    if (rulesRes && rulesRes.success && Array.isArray(rulesRes.data)) {
                        this._peExecRules = rulesRes.data;
                    }
                    this._peSensorSources = (rulesRes && Array.isArray(rulesRes.sensorSources)) ? rulesRes.sensorSources : [];
                    this._createPeriphExecTriggerElement({}, 0);
                    this._createPeriphExecActionElement({}, 0);
                    this._refreshPeriphExecRiskNotice({ allowFetch: true });
                    this._peEditorDataReady = true;
                    if (typeof this._refreshPeriphExecTemplateChoices === 'function') this._refreshPeriphExecTemplateChoices(true);
                    if (saveButton) saveButton.disabled = !!this._peSavePending;
                }).catch(err => {
                    if (!this._isPeriphExecEditLoadActive(loadSeq, pageGeneration) || (err && err._pageAborted)) return;
                    this.showInlineError('periph-exec-error', '规则数据加载失败：' + (err.message || '请关闭后重试'));
                });
            this.showModal(modal);
        },

        openPeriphExecModalStandalone() { this.openPeriphExecModal(); },

        closePeriphExecModal() {
            this._peEditorDataReady = false;
            // Invalidate any in-flight edit chain before removing its DOM.
            this._peEditLoadSeq++;
            this.hideModal('periph-exec-modal');
            this._pePeripherals = [];
            this._peDataSources = [];
            this._peSensorSources = [];
            this._peRemoteChannels = [];
            this._peWeatherSources = [];
            this._peRemoteResourcesAvailable = false;
            if (typeof this._clearPeriphExecRiskNotice === 'function') {
                this._clearPeriphExecRiskNotice();
            }
        },

        // ============ 通用辅助 ============

        _requirePeriphExecData(res) {
            if (!res || res.success !== true || res.data == null) {
                throw new Error(res && (res.error || res.message) || '配置数据加载失败，请关闭后重试');
            }
            return res;
        },

        _hiddenClass(visible) {
            return visible ? '' : ' is-hidden';
        },

        _storePeriphExecTypeCatalog(response) {
            var groups = response?.success && response.data ? response.data : {};
            this._peTypeCatalog = Array.isArray(groups) ? groups : Object.keys(groups).reduce(function(list, key) {
                return Array.isArray(groups[key]) ? list.concat(groups[key]) : list;
            }, []);
        },

        _loadPeriphExecRemoteChoices(ensureActive) {
            var self = this;
            this._peRemoteChannels = [];
            this._peWeatherSources = [];
            this._peRemoteResourcesAvailable = false;
            return apiGet('/api/screen-panel/weather').then(function(response) {
                if (ensureActive) ensureActive();
                var data = response && response.success && response.data ? response.data : {};
                self._peWeatherSources = data.enabled && data.id ? [{ id: data.id, label: '心知天气', enabled: true }] : [];
                self._peRemoteResourcesAvailable = true;
            }).catch(function(error) {
                if (error && error._pageAborted) throw error;
                console.warn('Remote resource choices unavailable:', error && error.message);
            });
        },

        _setSectionVisible(ref, visible, displayValue) {
            return visible ? this.showElement(ref, displayValue) : this.hideElement(ref);
        },

        _invalidatePeriphExecRuntimeCaches() {
            if (typeof window.apiInvalidateCache === 'function') {
                window.apiInvalidateCache('/api/periph-exec');
            }
            if (typeof this._clearPeriphExecEventCatalogCache === 'function') {
                this._clearPeriphExecEventCatalogCache();
            }
        },

        _isPeriphExecCapacityFull() {
            return !!(this._peProfile && Number(this._peProfile.remaining) <= 0);
        },

        _setPeriphExecCapacity(profile) {
            this._peProfile = profile || null;
            var btn = document.getElementById('periph-exec-page-add-btn');
            if (!btn) return;
            // 执行规则是软限制，不锁定按钮，仅在超限时显示警告提示
            var overLimit = profile && (profile.used || 0) > profile.max;
            btn.removeAttribute('data-resource-locked');
            if (this.isDeveloperModeEnabled()) {
                btn.disabled = false;
                if (overLimit) {
                    btn.title = '执行规则已超出推荐上限' + '(' + profile.max + ')' + '，但仍可新增';
                } else {
                    btn.removeAttribute('title');
                }
            }
        },

        _formatPeriphExecCapacitySummary(total) {
            var summary = '共' + ' ' + total + ' ' + '项';
            var profile = this._peProfile;
            if (profile && profile.max !== undefined) {
                summary += ' · 建议容量 ' + (profile.used || 0) + ' / ' + profile.max;
                if ((profile.used || 0) > profile.max) {
                    summary += ' · 已超出推荐上限';
                }
            }
            if (Array.isArray(this._peThermostats)) {
                const uncertain = this._peThermostats.filter(item => item.retiring || item.padConfirmed !== true ||
                    !Number.isFinite(item.checkAgeMs) || item.checkAgeMs > 2000);
                if (uncertain.length) summary += ' · 温控输出待确认：' + uncertain.map(item => item.targetId || item.ruleId).join('、');
            }
            return summary;
        },

        promptPeriphExecRunValue(options) {
            return this._promptPeriphExecRunValue(options, {
                fallbackMessage: '请输入要设置的值 (如：PWM 占空比、PID 参数等):',
                warningTitle: '外设执行',
                placeholder: ''
            });
        },

        // ============ Block index helpers ============

        _getPeriphExecBlockIndex(ref) {
            if (!ref || typeof ref.closest !== 'function') return -1;
            var block = ref.closest('.periph-exec-config-item');
            if (!block) return -1;
            var index = parseInt(block.dataset.index, 10);
            return isNaN(index) ? -1 : index;
        },

        _getPeriphExecBlock(containerId, index) {
            var container = document.getElementById(containerId);
            if (!container) return null;
            var items = container.querySelectorAll('.periph-exec-config-item');
            return items[index] || null;
        },

        // ============ 事件委托 ============

        _handlePeriphExecTriggerClick(event) {
            var deleteBtn = event.target.closest('.mqtt-topic-delete');
            if (!deleteBtn) return;
            var index = this._getPeriphExecBlockIndex(deleteBtn);
            if (index >= 0) this.deletePeriphExecTrigger(index);
        },

        _handlePeriphExecTriggerChange(event) {
            var target = event.target;
            if (!target) return;
            var index = this._getPeriphExecBlockIndex(target);
            if (index < 0) return;
            if (target.classList.contains('pe-trigger-type')) this.onPeriphExecTriggerTypeChangeInBlock(target.value, index);
            else if (target.classList.contains('pe-operator')) this.onPeriphExecOperatorChangeInBlock(target.value, index);
            else if (target.classList.contains('pe-timer-mode')) this.onPeriphExecTimerModeChangeInBlock(target.value, index);
            else if (target.classList.contains('pe-event-category')) this.onPeriphExecEventCategoryChangeInBlock(target.value, index);
            else if (target.classList.contains('pe-event')) this.onPeriphExecEventChangeInBlock(target.value, index);
            this._refreshPeriphExecRiskNotice({ allowFetch: true });
        },

        _handlePeriphExecTriggerInput(event) {
            var target = event.target;
            if (!target) return;
            if (target.classList.contains('pe-poll-interval') ||
                target.classList.contains('pe-poll-timeout') ||
                target.classList.contains('pe-poll-retries') ||
                target.classList.contains('pe-poll-inter-delay')) {
                this._refreshPeriphExecRiskNotice({ allowFetch: false });
            }
        },

        _handlePeriphExecActionClick(event) {
            var deleteBtn = event.target.closest('.mqtt-topic-delete');
            if (!deleteBtn) return;
            var index = this._getPeriphExecBlockIndex(deleteBtn);
            if (index >= 0) this.deletePeriphExecAction(index);
        },

        _handlePeriphExecActionChange(event) {
            var target = event.target;
            if (!target) return;
            var index = this._getPeriphExecBlockIndex(target);
            if (index < 0) return;
            if (target.classList.contains('pe-target-periph')) {
                this._onTargetPeriphChange(target, index);
                if (typeof this._syncRemoteActionValueState === 'function') {
                    this._syncRemoteActionValueState(this._getPeriphExecBlock('periph-exec-actions', index));
                }
                if (typeof this._syncConfiguredSensorAction === 'function') {
                    var block = this._getPeriphExecBlock('periph-exec-actions', index);
                    var channel = block?.querySelector('.pe-sensor-channel-field');
                    if (channel) channel.innerHTML = '';
                    this._syncConfiguredSensorAction(block);
                }
                this._syncPca9685ActionFields?.(this._getPeriphExecBlock('periph-exec-actions', index));
            }
            else if (target.classList.contains('pe-action-type')) this.onPeriphExecActionTypeChangeInBlock(target.value, index);
            else if (target.classList.contains('pe-use-received-value')) this.onPeriphExecUseRecvChangeInBlock(target, index);
            else if (target.classList.contains('pe-sensor-category')) this.onSensorCategoryChange(target, index);
            else if (target.classList.contains('pe-sensor-datafield')) this._syncDhtSensorField(this._getPeriphExecBlock('periph-exec-actions', index));
            else if (target.classList.contains('pe-sensor-channel-field')) this._syncConfiguredSensorAction(this._getPeriphExecBlock('periph-exec-actions', index));
            else if (target.classList.contains('pe-modbus-device-select')) this._onPeriphDeviceSelect(target);
            else if (target.classList.contains('pe-modbus-channel-select')) this._onPeriphChannelSelect(target);
            else if (target.classList.contains('pe-neopixel-preset')) this._onNeoPixelPresetChange(target, index);
            this._refreshPeriphExecRiskNotice({ allowFetch: true });
        },

        // ============ Trigger periph select / poll helpers ============

        _populateTriggerPeriphSelect(selectEl, selectedValue) {
            if (!selectEl) return;
            selectEl.innerHTML = '<option value="">-- 选择外设 --</option>';
            (this._pePeripherals || []).forEach(p => {
                var opt = document.createElement('option');
                opt.value = p.id; opt.textContent = p.name + ' (' + p.id + ')';
                selectEl.appendChild(opt);
            });
            if (selectedValue) selectEl.value = selectedValue;
        },

        // 填充按键类型的触发外设下拉：仅保留数字输入（上拉=13 / 下拉=14）
        _populateButtonPeriphSelect(selectEl, selectedValue) {
            if (!selectEl) return;
            selectEl.innerHTML = '<option value="">-- 任意按键外设 --</option>';
            (this._pePeripherals || []).filter(p => p.type === 13 || p.type === 14).forEach(p => {
                var opt = document.createElement('option');
                opt.value = p.id; opt.textContent = p.name + ' (' + p.id + ')';
                selectEl.appendChild(opt);
            });
            if (selectedValue) selectEl.value = selectedValue;
        },

        _isPollTriggerActive() {
            var container = document.getElementById('periph-exec-triggers');
            if (!container) return false;
            var items = container.querySelectorAll('.periph-exec-config-item');
            for (var i = 0; i < items.length; i++) {
                var sel = items[i].querySelector('.pe-trigger-type');
                if (sel && sel.value === '5') return true;
            }
            return false;
        },


        _rebuildActionBlocksForTriggerChange() {
            var container = document.getElementById('periph-exec-actions');
            if (!container) return;
            // 收集当前动作数据（保留所有动作类型的特有配置）
            var existingActions = [];
            container.querySelectorAll('.periph-exec-config-item').forEach((item, index) => {
                var actionType = parseInt(item.querySelector('.pe-action-type')?.value || '0');
                var actionValue = '';
                var useReceivedValue = item.querySelector('.pe-use-received-value')?.checked || false;
                if (actionType === 15) {
                    actionValue = item.querySelector('.pe-action-value-script')?.value || '';
                } else if (actionType === 18) {
                    // Preserve poll index zero and device-control parameters via
                    // the same serializer used by save; never invent defaults.
                    const current = this._collectPeriphExecActions()[index];
                    actionValue = current?.actionType === 18 ? current.actionValue : '';
                } else if (actionType === 27) {
                    actionValue = item.querySelector('.pe-action-value-oled')?.value || '';
                } else if (actionType === 19) {
                    var sensorAction = {};
                    if (typeof this._collectPeriphExecSensorAction === 'function') {
                        this._collectPeriphExecSensorAction(item, sensorAction);
                        actionValue = sensorAction.actionValue;
                    }
                } else if (actionType === 21) {
                    // 触发设备事件：从事件下拉框取值
                    actionValue = item.querySelector('.pe-trigger-event-select')?.value || '';
                } else if (actionType === 33) {
                    var remoteTarget = item.querySelector('.pe-target-periph')?.value || '';
                    var remoteChannel = (this._peRemoteChannels || []).find(function(channel) {
                        return String(channel.id) === String(remoteTarget);
                    });
                    actionValue = useReceivedValue ? '' :
                        (remoteChannel && remoteChannel.type === 'number'
                            ? (item.querySelector('.pe-action-value')?.value || '')
                            : (item.querySelector('.pe-remote-action-value')?.value || ''));
                } else if (actionType === 35) {
                    actionValue = item.querySelector('.pe-rtc-command')?.value || '';
                    useReceivedValue = false;
                } else if (actionType === 36 || actionType === 38) {
                    actionValue = '';
                    useReceivedValue = false;
                } else if (actionType === 37) {
                    actionValue = item.querySelector('.pe-rtc-set-source')?.value || '';
                    useReceivedValue = false;
                } else if (actionType === 39) {
                    actionValue = item.querySelector('.pe-thermostat-sourceRef') && typeof this._collectThermostatConfig === 'function'
                        ? JSON.stringify(this._collectThermostatConfig(item)) : item._peThermostatData?.actionValue || '';
                    useReceivedValue = false;
                } else if (actionType === 40) {
                    actionValue = item.querySelector('.pe-sd-record-sourceRef') && typeof this._collectSdRecorderConfig === 'function'
                        ? JSON.stringify(this._collectSdRecorderConfig(item)) : item._peSdRecorderData?.actionValue || '';
                    useReceivedValue = false;
                } else {
                    actionValue = item.querySelector('.pe-action-value')?.value || '';
                }
                existingActions.push({
                    actionType: actionType,
                    targetPeriphId: item.querySelector('.pe-target-periph')?.value || '',
                    actionValue: actionValue,
                    useReceivedValue: useReceivedValue,
                    execMode: parseInt(item.querySelector('.pe-exec-mode')?.value || '0'),
                    syncDelayMs: parseInt(item.querySelector('.pe-sync-delay')?.value || '0')
                });
            });
            if (existingActions.length === 0) existingActions.push({});
            // 清空并重建
            container.innerHTML = '';
            for (var i = 0; i < existingActions.length; i++) {
                this._createPeriphExecActionElement(existingActions[i], i);
            }
        },

        // ============ 列表渲染辅助 ============

        _renderPeriphExecStatusBadge(enabled) {
            var badgeClass = enabled ? 'badge badge-success' : 'badge badge-info';
            var label = enabled ? '已启用' : '已禁用';
            return '<span class="' + badgeClass + '">' + escapeHtml(label) + '</span>';
        },

        _getPeriphExecTriggerSummary(rule, triggerLabels) {
            var triggerText = triggerLabels[rule.triggerSummary] || '?';
            if (rule.triggerCount > 1) triggerText += ' (+' + (rule.triggerCount - 1) + ')';
            return triggerText;
        },

        _getPeriphExecActionSummary(rule, actionLabels) {
            var actionText = actionLabels[rule.actionSummary] || '?';
            if (rule.targetPeriphName) actionText += ' \u2192 ' + rule.targetPeriphName;
            if (rule.actionCount > 1) actionText += ' (+' + (rule.actionCount - 1) + ')';
            return actionText;
        },

        _renderPeriphExecActionButton(action, ruleId, label, extraClass, extraAttrs) {
            var attrs = 'data-pe-action="' + action + '" data-id="' + escapeHtml(ruleId) + '"';
            if (extraAttrs) attrs += ' ' + extraAttrs;
            var locked = action !== 'run' && !this.isDeveloperModeEnabled();
            if (locked) attrs += ' disabled title="' + escapeHtml(this.getDeveloperModeDisabledText()) + '"';
            return '<button class="fb-btn fb-btn-sm fb-btn-compact ' + (extraClass || '') + (locked ? ' dev-mode-locked' : '') + '" ' + attrs + '>' + escapeHtml(label) + '</button>';
        },

        _renderPeriphExecRuleActions(ruleId, enabled, hasSetMode, ruleName) {
            var buttons = [];
            var runAttrs = 'data-name="' + escapeHtml(ruleName || '') + '"';
            if (hasSetMode) runAttrs += ' data-has-set-mode="true"';
            buttons.push(this._renderPeriphExecActionButton('run', ruleId, '执行一次', 'fb-btn-outline-primary', runAttrs));
            buttons.push(this._renderPeriphExecActionButton('edit', ruleId, '编辑', 'fb-btn-primary'));
            buttons.push(this._renderPeriphExecActionButton(
                'toggle',
                ruleId,
                enabled ? '禁用' : '启用',
                enabled ? 'fb-btn-warning' : 'fb-btn-success',
                'data-next-enabled="' + (enabled ? 'false' : 'true') + '"'
            ));
            buttons.push(this._renderPeriphExecActionButton('delete', ruleId, '删除', 'fb-btn-danger'));
            return '<div class="u-table-action-row">' + buttons.join('') + '</div>';
        },

        _renderPeriphExecRuleRow(rule, triggerLabels, actionLabels) {
            var displayName = escapeHtml(rule.name || rule.id || '');
            var ruleId = String(rule.id || '');
            // 流程摘要: 优先用后端 flowSummary, 回退到拼接旧字段
            var flowText = rule.flowSummary || '';
            if (!flowText) {
                flowText = this._getPeriphExecTriggerSummary(rule, triggerLabels)
                    + ' → ' + this._getPeriphExecActionSummary(rule, actionLabels);
            }
            var flowHtml = '<span class="pe-flow-summary">' + escapeHtml(flowText).replace(/ → /g, ' <span class="pe-flow-arrow">→</span> ') + '</span>';
            var statsText = String(rule.triggerCount || 0);
            var html = '<tr>';
            html += '<td>' + displayName + '</td>';
            html += '<td>' + this._renderPeriphExecStatusBadge(rule.enabled) + this._renderThermostatRuntime(rule) + this._renderSdRecorderRuntime(rule) + '</td>';
            html += '<td class="u-fs-12">' + flowHtml + '</td>';
            html += '<td class="u-fs-12 u-text-center">' + escapeHtml(statsText) + '</td>';
            html += '<td class="u-cell-nowrap">' + this._renderPeriphExecRuleActions(ruleId, !!rule.enabled, !!rule.hasSetMode, rule.name || '') + '</td>';
            html += '</tr>';
            return html;
        },

        _renderThermostatRuntime(rule) {
            const thermostat = (rule.actions || []).some(action => Number(action.actionType) === 39);
            const state = Array.isArray(this._peThermostats) ? this._peThermostats.find(item => item.ruleId === rule.id) : null;
            if (!thermostat && !state) return '';
            if (!Array.isArray(this._peThermostats)) return '<p class="u-help">温控诊断不可用；输出状态未知</p>';
            if (!state) return '<p class="u-help">无活动温控会话；负载状态待实测</p>';
            const labels = {WAITING_FRESH_SAMPLE: '等待新鲜温度', OFF_CONFIRMED: '关闭', ON_CONFIRMED: '开启',
                MINIMUM_OFF_TIME: '等待最小关闭时间', MINIMUM_ON_TIME: '保持最小开启时间',
                SOURCE_INVALID_SAFE_OFF: '温度无效，已请求关闭', FAULT_LATCHED_REENABLE_REQUIRED: '故障锁定，检查后停用再启用',
                SOURCE_CHANGED_REENABLE_REQUIRED: '来源变化，检查后停用再启用', OUTPUT_UNCONFIRMED: '曾出现输出未确认，需人工检查',
                MEMORY_SUSPENDED_SAFE_OFF: '内存保护，已请求关闭', RETIRING_OUTPUT_UNCONFIRMED: '正在停用，关闭尚未确认'};
            const fresh = Number.isFinite(state.checkAgeMs) && state.checkAgeMs <= 2000;
            const pad = state.padConfirmed === true && fresh ? (state.outputOn ? '引脚LOW' : '引脚HIGH') : '引脚状态未确认';
            return '<p class="u-help">' + escapeHtml(labels[state.phase] || state.phase || '状态未知') + ' · ' + pad +
                '（响应时快照，非负载反馈）</p>';
        },

        _renderSdRecorderRuntime(rule) {
            if (!(rule.actions || []).some(action => Number(action.actionType) === 40)) return '';
            const state = this._peSdRecorder;
            if (state === undefined) return '<p class="u-help">SD 记录诊断不可用</p>';
            if (!state || state.ruleId !== rule.id) return '<p class="u-help">无活动 SD 记录会话</p>';
            const count = value => Number.isInteger(value) && value >= 0 ? String(value) : '未知';
            const fresh = Number.isFinite(state.checkAgeMs) && state.checkAgeMs >= 0 && state.checkAgeMs <= 2000;
            const phase = state.phase === 'WAITING_FRESH_SAMPLE' ? '等待新鲜样本' : state.phase === 'RECORDING' ? '记录中' : state.phase || '状态未知';
            return '<p class="u-help">' + escapeHtml(phase) + (fresh ? '' : ' · 状态已失鲜或年龄未知') +
                ' · 排队 ' + count(state.pending) + '/8 · 接受 ' + count(state.accepted) + ' · 已写 ' + count(state.written) +
                ' · 丢弃 ' + count(state.dropped) + (state.lastError ? ' · ' + escapeHtml(String(state.lastError)) : '') +
                '（响应时快照；写入/flush 非断电持久性证明）</p>';
        }
    });

    // ============ CRUD operations and page loading ============
    Object.assign(AppState, {

        _getPeriphExecPageGeneration() {
            if (typeof window === 'undefined' || typeof window.apiGetGovernorState !== 'function') return null;
            try {
                var state = window.apiGetGovernorState();
                return state && state.pageGeneration !== undefined ? state.pageGeneration : null;
            } catch (_) {
                return null;
            }
        },

        _isPeriphExecEditLoadActive(seq, pageGeneration) {
            if (this._peEditLoadSeq !== seq) return false;
            if (this.currentPage && this.currentPage !== 'periph-exec') return false;
            if (pageGeneration === null || typeof window === 'undefined' || typeof window.apiGetGovernorState !== 'function') return true;
            var currentGeneration = this._getPeriphExecPageGeneration();
            return currentGeneration === null || Number(currentGeneration) === Number(pageGeneration);
        },

        _isPeriphExecEditRetryable(err) {
            if (!err || err._pageAborted) return false;
            var status = Number(err.status);
            if (isFinite(status) && status > 0) {
                return [408, 425, 429, 500, 502, 503, 504].indexOf(status) >= 0;
            }
            // AbortController is also used by the browser when a page is
            // navigated/unmounted.  Those native AbortErrors are terminal for
            // this editor chain; retry only the timeout error explicitly
            // annotated by request-governor, then handle network TypeErrors.
            if (err._requestTimeout) return true;
            if (err.name === 'AbortError') return false;
            if (err instanceof TypeError) return true;
            return !!(err.message && /fetch|network|timeout|reset|busy|unavailable/i.test(err.message));
        },

        editPeriphExecRule(id) {
            if (!this.guardDeveloperModeAction()) return;
            var loadSeq = ++this._peEditLoadSeq;
            var pageGeneration = this._getPeriphExecPageGeneration();
            if (!this._isPeriphExecEditorReady()) {
                var self = this;
                return this._ensurePeriphExecEditor().then(function() {
                    if (!self._isPeriphExecEditLoadActive(loadSeq, pageGeneration)) return;
                    return self.editPeriphExecRule(id);
                });
            }
            return this.openPeriphExecModal(id).then(() => {
                if (!this._isPeriphExecEditLoadActive(loadSeq, pageGeneration)) return;
                return this._loadPeriphExecEditData(id, 1, loadSeq, pageGeneration);
            });
        },

        _loadPeriphExecEditData(id, retriesLeft, loadSeq, pageGeneration) {
            var self = this;
            loadSeq = loadSeq === undefined ? this._peEditLoadSeq : loadSeq;
            pageGeneration = pageGeneration === undefined ? this._getPeriphExecPageGeneration() : pageGeneration;
            var isActive = function() {
                return self._isPeriphExecEditLoadActive(loadSeq, pageGeneration);
            };
            var pageAbortError = function() {
                var err = new Error('Request cancelled by page navigation');
                err._pageAborted = true;
                return err;
            };
            var ensureActive = function() {
                if (!isActive()) throw pageAbortError();
            };
            // 串行请求：ESP32 heap 紧张时并发 4 个请求会 OOM，必须逐个等完
            const _editResults = {};
            const loadRule = typeof apiGetFresh === 'function' ? apiGetFresh : apiGet;
            return Promise.resolve()
                .then(function() { ensureActive(); return loadRule('/api/periph-exec', { id: id }); })
                .then(execRes => { ensureActive(); this._requirePeriphExecData(execRes); _editResults.execRes = execRes; return apiGet('/api/peripherals', { pageSize: 100, compact: 1 }); })
                .then(periphRes => { ensureActive(); this._requirePeriphExecData(periphRes); _editResults.periphRes = periphRes; return apiGet('/api/peripherals/types'); })
                .then(typeRes => { ensureActive(); this._requirePeriphExecData(typeRes); this._storePeriphExecTypeCatalog(typeRes); return apiGet('/api/protocol/config', { compact: 1, section: 'periph-exec' }); })
                .then(protoRes => { ensureActive(); this._requirePeriphExecData(protoRes); _editResults.protoRes = protoRes; return apiGet('/api/periph-exec', { pageSize: 100 }); })
                .then(rulesRes => {
                    ensureActive();
                    this._requirePeriphExecData(rulesRes);
                    _editResults.rulesRes = rulesRes;
                    return this._loadPeriphExecRemoteChoices(ensureActive);
                })
                .then(() => {
                    ensureActive();
                    const { execRes, periphRes, protoRes } = _editResults;
                    const rulesRes = _editResults.rulesRes;
                    if (periphRes && periphRes.success && periphRes.data) {
                        this._pePeripherals = periphRes.data;
                    }
                    this._peDataSources = [];
                    if (protoRes && protoRes.success && protoRes.data) {
                        const protoData = protoRes.data;
                        this._peDataSources = this._extractDataSources(protoData);
                        this._masterTasks = [];
                        this._modbusDevices = [];
                        this._peTemplateModbusConfig = protoData.degraded ? null : (protoData.modbusRtu || null);
                        if (protoData.modbusRtu && protoData.modbusRtu.master) {
                            this._masterTasks = protoData.modbusRtu.master.tasks || [];
                            this._modbusDevices = protoData.modbusRtu.master.devices || [];
                        }
                    }
                    if (rulesRes && rulesRes.success && Array.isArray(rulesRes.data)) {
                        this._peExecRules = rulesRes.data;
                    } else {
                        this._peExecRules = [];
                    }
                    this._peSensorSources = (rulesRes && Array.isArray(rulesRes.sensorSources)) ? rulesRes.sensorSources : [];
                    if (!execRes || !execRes.success || !execRes.data) return;
                    const rule = execRes.data;
                    if (!rule) return;

                    document.getElementById('periph-exec-name').value = rule.name || '';
                    document.getElementById('periph-exec-enabled').checked = !!rule.enabled;
                    document.getElementById('periph-exec-report').checked = rule.reportAfterExec !== false;
                    var panelControl = document.getElementById('periph-exec-panel-control');
                    var recordHistory = document.getElementById('periph-exec-record-history');
                    if (panelControl) panelControl.checked = rule.allowPanelControl === true;
                    if (recordHistory) recordHistory.checked = rule.recordHistory === true;

                    let triggers = rule.triggers || [];
                    if (triggers.length === 0) {
                        triggers = [{
                            triggerType: String(rule.triggerType ?? 0),
                            triggerPeriphId: rule.triggerPeriphId || rule.sourcePeriphId || '',
                            operatorType: String(rule.operatorType ?? 0),
                            compareValue: rule.compareValue || '',
                            timerMode: String(rule.timerMode ?? 0),
                            intervalSec: rule.intervalSec || 60,
                            timePoint: rule.timePoint || '08:00',
                            eventId: rule.eventId || ''
                        }];
                    }

                    let actions = rule.actions || [];
                    if (actions.length === 0) {
                        actions = [{
                            targetPeriphId: rule.targetPeriphId || '',
                            actionType: String(rule.actionType ?? 0),
                            actionValue: rule.actionValue || ''
                        }];
                    }

                    const triggersContainer = document.getElementById('periph-exec-triggers');
                    if (triggersContainer) triggersContainer.innerHTML = '';
                    triggers.forEach((t, i) => this._createPeriphExecTriggerElement(t, i));

                    const actionsContainer = document.getElementById('periph-exec-actions');
                    if (actionsContainer) actionsContainer.innerHTML = '';
                    actions.forEach((a, i) => this._createPeriphExecActionElement(a, i));
                    if (typeof this._updatePeriphExecPanelOptions === 'function') {
                        this._updatePeriphExecPanelOptions();
                    }

                    if (this._hasSetModeTrigger()) {
                        this._syncSetModeToActions(true);
                    }
                    this._refreshPeriphExecRiskNotice({ allowFetch: true });
                    this._peEditorDataReady = true;
                    const saveButton = document.getElementById('save-periph-exec-btn');
                    if (saveButton) saveButton.disabled = !!this._peSavePending;
                })
                .catch(err => {
                    // 页面切换/关闭编辑器后，旧请求及其重试都应静默结束。
                    if (!isActive() || (err && err._pageAborted)) return;

                    // 认证和参数错误不会因重试恢复；Governor 已负责显示相应提示。
                    var status = Number(err && err.status);
                    var nonRetryableHttp = [400, 401, 403, 404, 409, 422].indexOf(status) >= 0;
                    if (retriesLeft > 0 && !nonRetryableHttp && self._isPeriphExecEditRetryable(err)) {
                        // 设备瞬时拥塞（TIME_WAIT 占满 PCB / 看门狗回收）会导致单个请求链被 abort，
                        // 延迟 1.5s 自动重试一次，避免用户手动刷新
                        console.warn('Periph exec edit data load failed, retrying:', err && err.message);
                        return new Promise(resolve => setTimeout(resolve, 1500))
                            .then(() => {
                                if (!isActive()) return;
                                return self._loadPeriphExecEditData(id, retriesLeft - 1, loadSeq, pageGeneration);
                            });
                    }
                    this.showInlineError('periph-exec-error', '规则数据加载失败，请关闭后重试');
                    // HTTP errors are already handled by request-governor; avoid
                    // duplicating console/notification noise for 401 and peers.
                    if (!(err && err._handled)) {
                        console.error('Failed to load periph exec edit data:', err);
                        if (typeof window.apiNotifyError === 'function') {
                            window.apiNotifyError(err, '规则数据加载失败，请稍后重试', '外设执行');
                        }
                    }
                });
        },

        deletePeriphExecRule(id) {
            if (!this.guardDeveloperModeAction()) return;
            if (!confirm('确定要删除此规则吗？')) return;
            apiDelete('/api/periph-exec/', { id: id })
                .then(res => {
                    if (res && res.success) {
                        this._invalidatePeriphExecRuntimeCaches();
                        Notification.success('规则已删除', '外设执行');
                        if (this.currentPage === 'periph-exec') this.loadPeriphExecPage();
                    } else {
                        Notification.error(res?.error || '删除失败', '外设执行');
                    }
                })
                .catch(err => {
                    console.error('Delete periph exec rule failed:', err);
                    const isNetworkError = err && (
                        err.name === 'TypeError' ||
                        (err.message && (
                            err.message.includes('Failed to fetch') ||
                            err.message.includes('fetch') ||
                            err.message.includes('network')
                        ))
                    );
                    if (isNetworkError) {
                        Notification.error('设备离线或不可达，请检查网络连接', '外设执行');
                    } else {
                        Notification.error('删除失败', '外设执行');
                    }
                });
        },

        togglePeriphExecRule(id, enable) {
            if (!this.guardDeveloperModeAction()) return;
            const url = enable ? '/api/periph-exec/enable' : '/api/periph-exec/disable';
            apiPost(url, { id: id })
                .then(res => {
                    if (res && res.success) {
                        this._invalidatePeriphExecRuntimeCaches();
                        if (this.currentPage === 'periph-exec') this.loadPeriphExecPage();
                    } else {
                        Notification.error(res?.error || '状态切换失败', '外设执行');
                    }
                })
                .catch(err => {
                    console.error('Toggle periph exec rule failed:', err);
                    const isNetworkError = err && (
                        err.name === 'TypeError' ||
                        (err.message && (
                            err.message.includes('Failed to fetch') ||
                            err.message.includes('fetch') ||
                            err.message.includes('network')
                        ))
                    );
                    if (isNetworkError) {
                        Notification.error('设备离线或不可达，请检查网络连接', '外设执行');
                    } else {
                        Notification.error('状态切换失败', '外设执行');
                    }
                });
        },

        runPeriphExecOnce(id, hasSetMode, ruleName) {
            if (!this._peManualRuns) this._peManualRuns = new Map();
            if (this._peManualRuns.has(id)) return this._peManualRuns.get(id);
            var self = this;
            var generation = this._getPeriphExecPageGeneration();
            var active = () => this.currentPage === 'periph-exec' &&
                (generation === null || generation === this._getPeriphExecPageGeneration());
            var doRun = function(value) {
                if (!active()) return;
                var payload = { id: id };
                if (value !== undefined && value !== '') payload.value = value;
                return apiPost('/api/periph-exec/run', payload)
                    .then(res => {
                        if (res && res.success) {
                            if (self.currentPage === 'periph-exec') self.loadPeriphExecPage();
                            return self._observeManualOutput(res, active);
                        } else {
                            if (active()) Notification.error(self._manualOutputMessage(res), '外设执行');
                        }
                    })
                    .catch(err => {
                        if (!active()) return;
                        console.error('Run periph exec rule failed:', err);
                        var isNetworkError = err && (
                            err.name === 'TypeError' ||
                            (err.message && (
                                err.message.includes('Failed to fetch') ||
                                err.message.includes('fetch') ||
                                err.message.includes('network')
                            ))
                        );
                        if (isNetworkError) {
                            Notification.error('执行结果未知，请检查设备和执行记录；不会自动重试输出', '外设执行');
                        } else {
                            Notification.error(self._manualOutputMessage(err?.data || err), '外设执行');
                        }
                    });
            };
            var pending = Promise.resolve().then(() => {
                if (!hasSetMode) return doRun('');
                return this.promptPeriphExecRunValue({ ruleName: ruleName || '' }).then(inputValue => {
                    if (inputValue === null) return;
                    return doRun(inputValue);
                });
            }).finally(() => {
                if (this._peManualRuns.get(id) === pending) this._peManualRuns.delete(id);
            });
            this._peManualRuns.set(id, pending);
            return pending;
        },

        _refreshPeriphExecList() {
            var btn = document.getElementById('periph-exec-refresh-btn');
            if (btn && btn.disabled) return;
            if (btn) { btn.disabled = true; btn.textContent = '加载中...'; }
            if (typeof window.apiInvalidateCache === 'function') {
                window.apiInvalidateCache('/api/periph-exec');
            }
            this.loadPeriphExecPage();
            setTimeout(function() {
                if (btn) { btn.disabled = false; btn.innerHTML = '&#x21bb; ' + '刷新'; }
            }, 2000);
        },

        _renderUartTextStatus(status, loading) {
            const el = document.getElementById('periph-exec-uart-text-status');
            if (!el) return;
            const fields = ['dispatchAttempts','oversizedFrames','invalidByteFrames','readFailures',
                'sourceChanges','staleFrames','serializationFailures','hardwareFifoFaults','hardwareBufferFaults',
                'hardwareFrameFaults','hardwareParityFaults','hardwareBreakFaults'];
            if (!status || status.available !== true || status.scope !== 'boot' || status.unit !== 'events' ||
                status.peerVerified !== false || status.maxFrameBytes !== 160 || status.idleBoundaryMs !== 500 ||
                status.hardwareErrorScope !== 'first_observed_generic_stream_fault' ||
                status.hardwareErrorRecovery !== 'explicit_restart' || status.hardwareErrorDeliveryConfirmed !== false ||
                typeof status.countsAreLowerBounds !== 'boolean' || fields.some(key =>
                    !Number.isSafeInteger(status[key]) || status[key] < 0 || status[key] > 4294967295) ||
                (fields.some(key => status[key] === 4294967295) && !status.countsAreLowerBounds)) {
                el.textContent = loading ? 'UART文本接收诊断加载中…' : 'UART文本接收诊断未确认，不能判断没有丢帧';
                return;
            }
            el.textContent = 'UART文本接收（本次启动累计' + (status.countsAreLowerBounds ? '，计数为下限' : '') +
                '）：提交规则匹配尝试 ' + status.dispatchAttempts + '；超长帧 ' + status.oversizedFrames +
                '，无效字节帧 ' + status.invalidByteFrames + '，读取失败 ' + status.readFailures +
                '，来源变化观察 ' + status.sourceChanges + '，发布时来源未确认 ' + status.staleFrames +
                '，序列化失败 ' + status.serializationFailures +
                '；硬件首故障 FIFO ' + status.hardwareFifoFaults + '，缓冲满 ' + status.hardwareBufferFaults +
                '，帧错误 ' + status.hardwareFrameFaults + '，奇偶错误 ' + status.hardwareParityFaults + '，BREAK ' + status.hardwareBreakFaults +
                '。硬件故障后接收锁存，核对线路后显式停用再启用以恢复；不会重放旧数据。SDK错误通知可能延迟或缺失。' +
                '160字节/换行或空读500ms边界；软件项仅统计规则文本接收，硬件项覆盖通用UART首故障，各项不可相加为丢帧总数，零计数不证明通信正常。提交不代表规则已执行或对端验证。';
        },

        _renderPendingReportStatus(status, loading) {
            const el = document.getElementById('periph-exec-report-status');
            if (!el) return;
            const fields = ['depth','capacity','maxPayloadBytes','retryIntervalMs','admitted','evicted',
                'rejectedInvalid','rejectedMemory','rejectedFull','rejectedBusy','contention','retryAccepted','retryRejected',
                'oldestAttempts','retryDelayMs','rejectedUnknownInstance','discardedInstanceChanged'];
            const retryState = status && (!status.depth ? 'empty' : status.inFlight ? 'in_flight' : status.retryDelayMs ? 'cooldown' : 'ready');
            if (!status || status.available !== true || status.deliveryVerified !== false ||
                status.bindingScope !== 'mqtt_instance' ||
                typeof status.inFlight !== 'boolean' || fields.some(key => !Number.isSafeInteger(status[key]) || status[key] < 0) ||
                status.depth > status.capacity || (status.inFlight && status.depth === 0) ||
                status.retryState !== retryState || !['none','accepted','rejected','instance_changed'].includes(status.lastRetryResult) ||
                status.retryDelayMs > 5000 || status.oldestAttempts > 4294967295 ||
                ((!status.depth || status.inFlight) && status.retryDelayMs !== 0) ||
                (!status.depth && status.oldestAttempts !== 0) ||
                (status.depth > 0 && (!Number.isSafeInteger(status.oldestAgeMs) || status.oldestAgeMs < 0 || status.oldestAgeMs > 4294967295)) ||
                status.capacity !== 5 || status.maxPayloadBytes !== 512 || status.retryIntervalMs !== 5000) {
                el.textContent = loading ? '离线上报状态加载中…' : '离线上报状态未确认（旧固件、忙碌或请求失败），不能判断缓存为空';
                return;
            }
            el.textContent = '离线上报缓存 ' + status.depth + '/' + status.capacity +
                (status.depth ? '，最旧缓存约 ' + Math.floor(status.oldestAgeMs / 1000) + ' 秒（设备计时）' : '') +
                (status.depth ? '，队首已尝试 ' + status.oldestAttempts + ' 次' : '') +
                (status.inFlight ? '，正在尝试入队' : status.retryDelayMs ?
                    '，冷却至少 ' + Math.ceil(status.retryDelayMs / 1000) + ' 秒后可重试' :
                    status.depth ? '，待连接和调度就绪后重试' : '，无待重试报告') +
                '；最近重试：' + ({none:'尚未完成',accepted:'本地接受',rejected:'拒绝，已保留',instance_changed:'实例已更换，报告未发送并丢弃'}[status.lastRetryResult]) +
                '；启动以来：重试本地接受 ' + status.retryAccepted + '，拒绝并保留 ' + status.retryRejected +
                '，满额淘汰旧报告 ' + status.evicted + '；拒绝缓存：格式/长度 ' + status.rejectedInvalid +
                '，内存 ' + status.rejectedMemory + '，在途满额 ' + status.rejectedFull + '，锁忙 ' + status.rejectedBusy +
                '，归属未确认 ' + status.rejectedUnknownInstance + '；实例更换丢弃 ' + status.discardedInstanceChanged +
                '；入队/重试锁竞争 ' + status.contention +
                '。只重试到原MQTT实例；同配置重建也不跨实例重放。仅内存缓存，重启丢失；本地接受不代表平台收到。';
        },

        loadPeriphExecPage() {
            const tbody = document.getElementById('periph-exec-table-body');
            if (!tbody) return;
            this.applyDeveloperModeState();
            // Show/hide developer mode hint banner
            var devHint = document.getElementById('periph-exec-dev-mode-hint');
            if (devHint) devHint.style.display = this.isDeveloperModeEnabled() ? 'none' : 'block';
            const filterSel = document.getElementById('periph-exec-filter-periph');
            const filterVal = filterSel ? filterSel.value : '';
            if (this._peListFilter !== filterVal) {
                this._peCurrentPage = 1;
                this._peListFilter = filterVal;
            }
            const requestSequence = this._peListSequence = (this._peListSequence || 0) + 1;
            this._renderPendingReportStatus(null, true);
            this._renderUartTextStatus(null, true);
            this._peThermostats = null;
            this._peSdRecorder = undefined;

            this.renderEmptyTableRow(tbody, 7, '加载中...');

            const apiUrl = '/api/periph-exec?page=' + this._peCurrentPage + '&pageSize=' + this._pePageSize + (filterVal ? '&filter=' + encodeURIComponent(filterVal) : '');

            // The dynamic filter options are secondary UI. Fetch the rules as
            // the only first-screen API request; filter options load when the
            // user focuses the selector, after the page is already usable.
            return apiGet(apiUrl)
                .then(execRes => {
                    if (requestSequence !== this._peListSequence) return;
                    this._renderPendingReportStatus(execRes && execRes.success ? execRes.pendingReports : null);
                    this._renderUartTextStatus(execRes && execRes.success === true ? execRes.uartText : null);
                    this._peThermostats = execRes && execRes.success === true && Array.isArray(execRes.thermostats) ? execRes.thermostats : null;
                    this._peSdRecorder = execRes?.success === true && Object.prototype.hasOwnProperty.call(execRes, 'sdRecorder') &&
                        (execRes.sdRecorder === null || (typeof execRes.sdRecorder === 'object' && !Array.isArray(execRes.sdRecorder))) ? execRes.sdRecorder : undefined;
                    // /api/periph-exec exposes the runtime degradation snapshot
                    // in its chunked header. Keep the page-level warning and
                    // request governor in sync with the device guard state.
                    if (typeof apiReportRuntimePressure === 'function') {
                        apiReportRuntimePressure(execRes, 'periph-exec');
                    }
                    this._peTotalRules = execRes && execRes.total ? execRes.total : 0;
                    this._setPeriphExecCapacity(execRes && execRes.profile ? execRes.profile : null);
                    if (execRes && Array.isArray(execRes.sensorSources)) {
                        this._peSensorSources = execRes.sensorSources;
                    }
                    const currentPage = execRes && execRes.page ? execRes.page : 1;
                    const currentPageSize = execRes && execRes.pageSize ? execRes.pageSize : 10;
                    const lastPage = Math.max(1, Math.ceil(this._peTotalRules / currentPageSize));
                    if (execRes && execRes.success) {
                        this._peCurrentPage = Math.min(currentPage, lastPage);
                        if (currentPage > lastPage) return this.loadPeriphExecPage();
                    }
                    
                    if (!execRes || !execRes.success || !execRes.data || execRes.data.length === 0) {
                        this.renderEmptyTableRow(tbody, 5, '暂无外设执行');
                        this._renderPeriphExecPagination(this._peTotalRules, currentPage, currentPageSize);
                        return;
                    }
                    let rules = execRes.data;
                    rules.sort((a, b) => {
                        if (a.enabled !== b.enabled) return a.enabled ? -1 : 1;
                        return (a.name || '').localeCompare(b.name || '', 'zh');
                    });
                    if (rules.length === 0) {
                        this.renderEmptyTableRow(tbody, 5, '暂无外设执行');
                        this._renderPeriphExecPagination(this._peTotalRules, currentPage, currentPageSize);
                        return;
                    }
                    const triggerLabels = {
                        0: '平台触发 (MQTT)',
                        1: '定时触发',
                        4: '事件触发',
                        5: '轮询触发 (本地数据)',
                        6: '规则链触发'
                    };
                    const actionLabels = {
                        0: '设置高电平', 1: '设置低电平',
                        2: '闪烁', 3: '呼吸灯',
                        4: '设置 PWM', 5: '设置 DAC',
                        6: '系统重启', 7: '恢复出厂',
                        8: 'NTP 同步', 9: 'OTA 升级',
                        10: '发送指令',
                        11: '灯效控制',
                        12: '电机控制',
                        13: '高电平反转', 14: '低电平反转',
                        15: '命令脚本',
                        16: 'Modbus 线圈写入', 17: 'Modbus 寄存器写入',
                        18: 'Modbus 轮询采集',
                        19: '传感器数据读取',
                        20: '预留动作',
                        21: '触发设备事件',
                        22: '启用执行规则', 23: '禁用执行规则',
                        24: '显示数字',
                        25: '显示文本',
                        26: '数码管清屏',
                        27: 'OLED 自定义显示',
                        28: '射频发送',
                        29: '串口发送',
                        30: '电平翻转',
                        31: '启用功能',
                        32: '禁用功能', 39: '温控继电器', 40: 'SD 持续数值记录'
                    };
                    let html = '';
                    rules.forEach(r => {
                        html += this._renderPeriphExecRuleRow(r, triggerLabels, actionLabels);
                    });
                    tbody.innerHTML = html;
                    this._renderPeriphExecPagination(this._peTotalRules, currentPage, currentPageSize);
                    this.applyDeveloperModeState(tbody);
                })
                .catch(() => {
                    if (requestSequence !== this._peListSequence) return;
                    this._renderPendingReportStatus(null);
                    this._renderUartTextStatus(null);
                    this.renderEmptyTableRow(tbody, 5, '暂无外设执行');
                    this._renderPeriphExecPagination(0, 1, 10);
                });
        },

        _renderPeriphExecPagination(total, page, pageSize) {
            const summaryText = this._formatPeriphExecCapacitySummary(total);
            const summary = document.getElementById('periph-exec-list-summary');
            if (summary) {
                summary.textContent = summaryText;
                summary.classList.toggle('is-warning', !!(this._peProfile && (this._peProfile.used || 0) > this._peProfile.max));
            }

            const pagination = document.getElementById('periph-exec-pagination');
            const hasMultiplePages = Math.ceil(Math.max(0, Number(total) || 0) / Math.max(1, Number(pageSize) || 10)) > 1;
            if (pagination) {
                pagination.hidden = !hasMultiplePages;
                if (!hasMultiplePages) pagination.innerHTML = '';
            }
            if (!hasMultiplePages) return;

            this.renderPagination('periph-exec-pagination', {
                total,
                page,
                pageSize,
                maxVisiblePages: 3,
                onPageChange: (nextPage) => {
                    this._peCurrentPage = nextPage;
                    this.loadPeriphExecPage();
                }
            });
        },

        _refreshPeriphExecResults() {
            // 已删除“最近执行结果”功能
        },


        _formatPeriphExecUptimeMs(value) {
            var ms = Number(value || 0);
            if (!ms) return '--';
            if (typeof formatUptime === 'function') return formatUptime(Math.floor(ms / 1000));
            return Math.floor(ms / 1000) + 's';
        },

        _renderPeriphExecResults(results) {
            // 已删除“最近执行结果”功能
        },

        loadPeriphExecResults(options) {
            // 已删除“最近执行结果”功能
            return Promise.resolve();
        },

        _populatePeriphExecFilter() {
            const sel = document.getElementById('periph-exec-filter-periph');
            if (!sel || sel._populated) return Promise.resolve();
            if (sel._populatePromise) return sel._populatePromise;
            sel._populatePromise = apiGet('/api/peripherals', { pageSize: 100, compact: 1, enabledOnly: 1 }).then(res => {
                if (!res || !res.success || !res.data || res.data.length === 0) {
                    sel._populated = true;
                    return;
                }
                const currentVal = sel.value;
                // 在静态触发类型选项之后追加外设名称分组
                const separator = '<option disabled>' + '─── 按外设筛选 ───' + '</option>';
                let periphOpts = '';
                res.data.forEach(p => {
                    periphOpts += '<option value="' + p.name + '">' + p.name + '</option>';
                });
                sel.insertAdjacentHTML('beforeend', separator + periphOpts);
                if (currentVal) sel.value = currentVal;
                sel._populated = true;
            }).catch(() => {
                // 静默处理：ESP32 OOM 时 RST/超时会抛出 rejection，不能变成 unhandled；
                // 保持 _populated=false 以便下一次重试
            }).finally(() => {
                sel._populatePromise = null;
            });
            return sel._populatePromise;
        }
    });

    // Invalidate editor chains even when a legacy caller changes pages without
    // going through the request governor's generation counter.
    if (typeof AppState.onPageHide === 'function') {
        AppState.onPageHide('periph-exec', function() {
            this._peListSequence = (this._peListSequence || 0) + 1;
        });
        AppState.onPageHide('periph-exec-editor', function() {
            this._peEditLoadSeq++;
        });
    }

    // ============ 合并模式：子模块代码已被构建脚本 prepend 到本文件之前 ============
    // 不再运行时动态加载，直接绑定事件（3 个 HTTP 请求 → 1 个）
    if (typeof AppState.setupPeriphExecEvents === 'function') {
        AppState.setupPeriphExecEvents();
    }
})();
