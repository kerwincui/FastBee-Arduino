/**
 * 外设执行管理模块 - 核心入口
 * 页面初始化、CRUD API 调用、列表渲染、分页、事件绑定
 * 子模块: periph-exec-modbus.js (Modbus专用), periph-exec-form.js (表单)
 */
(function() {
    AppState.registerModule('periph-exec', {

        // ============ 状态变量 ============
        _pePeripherals: [],
        _peDataSources: [],
        _peSensorSources: [],
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
        _periphExecRunPromptState: null,

        _isPeriphExecEditorReady() {
            return typeof this._createPeriphExecTriggerElement === 'function' &&
                typeof this._createPeriphExecActionElement === 'function' &&
                typeof this.savePeriphExecRule === 'function';
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

        openPeriphExecModal(editId) {
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
            const modal = document.getElementById('periph-exec-modal');
            if (!modal) return;
            const titleEl = document.getElementById('periph-exec-modal-title');
            var safeId = (editId && editId !== 'null' && editId !== 'undefined') ? editId : '';
            document.getElementById('periph-exec-original-id').value = safeId;
            this.clearInlineError('periph-exec-error');
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
            if (safeId) { this.showModal(modal); return; }
            this._pePeripherals = [];
            this._peDataSources = [];
            this._peSensorSources = [];
            this._peExecRules = [];
            // 优化：并行请求三个 API，减少模态框打开等待时间
            Promise.all([
                apiGet('/api/peripherals', { pageSize: 100, compact: 1, enabledOnly: 1 }),
                apiGet('/api/protocol/config', { compact: 1, section: 'periph-exec' }),
                apiGet('/api/periph-exec', { pageSize: 100 })
            ]).then(([periphRes, protoRes, rulesRes]) => {
                if (periphRes && periphRes.success && periphRes.data) this._pePeripherals = periphRes.data;
                if (protoRes && protoRes.success && protoRes.data) {
                    const protoData = protoRes.data;
                    this._peDataSources = this._extractDataSources(protoData);
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
            }).catch(err => {
                console.error('Failed to load periph exec data:', err);
                this._createPeriphExecTriggerElement({}, 0);
                this._createPeriphExecActionElement({}, 0);
                this._refreshPeriphExecRiskNotice({ allowFetch: true });
            });
            this.showModal(modal);
        },

        openPeriphExecModalStandalone() { this.openPeriphExecModal(); },

        closePeriphExecModal() {
            this.hideModal('periph-exec-modal');
            this._pePeripherals = [];
            this._peDataSources = [];
            this._peSensorSources = [];
            if (typeof this._clearPeriphExecRiskNotice === 'function') {
                this._clearPeriphExecRiskNotice();
            }
        },

        // ============ 通用辅助 ============

        _hiddenClass(visible) {
            return visible ? '' : ' is-hidden';
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
                    btn.title = '执行规则已超出推荐上限(' + profile.max + ')，但仍可新增';
                } else {
                    btn.removeAttribute('title');
                }
            }
        },

        _formatPeriphExecCapacitySummary(total) {
            var summary = '共 ' + total + ' 项';
            var profile = this._peProfile;
            if (profile && profile.max !== undefined) {
                summary += ' (' + (profile.used || 0) + '/' + profile.max + ')';
                if ((profile.used || 0) > profile.max) {
                    summary += ' ⚠️ 已超出推荐上限';
                }
            }
            return summary;
        },

        // ============ 执行值输入弹窗 ============

        _ensurePeriphExecRunValueModal() {
            var modal = document.getElementById('periph-exec-run-value-modal');
            if (modal) return modal;
            if (!document.body) return null;

            modal = document.createElement('div');
            modal.id = 'periph-exec-run-value-modal';
            modal.className = 'modal is-hidden';
            modal.innerHTML = '' +
                '<div class="modal-content u-modal-content-700">' +
                    '<div class="modal-header">' +
                        '<h2 class="modal-title" id="periph-exec-run-value-modal-title"></h2>' +
                        '<button type="button" id="close-periph-exec-run-value-modal" class="modal-close-btn">&times;</button>' +
                    '</div>' +
                    '<div class="modal-body">' +
                        '<div class="fb-form">' +
                            '<div class="fb-form-group">' +
                                '<label for="periph-exec-run-value-input" id="periph-exec-run-value-label"></label>' +
                                '<input type="text" id="periph-exec-run-value-input" class="" maxlength="128" autocomplete="off" spellcheck="false">' +
                                '<small id="periph-exec-run-value-help" class="pe-help-text"></small>' +
                            '</div>' +
                            '<div id="periph-exec-run-value-error" class="message message-error is-hidden"></div>' +
                        '</div>' +
                    '</div>' +
                    '<div class="modal-footer">' +
                        '<button class="fb-btn" id="cancel-periph-exec-run-value-btn" type="button"></button>' +
                        '<button class="fb-btn fb-btn-primary" id="confirm-periph-exec-run-value-btn" type="button"></button>' +
                    '</div>' +
                '</div>';
            document.body.appendChild(modal);

            var self = this;
            var closePrompt = function() {
                self._resolvePeriphExecRunValuePrompt(null);
            };

            modal.addEventListener('click', function(event) {
                if (event.target === modal) closePrompt();
            });

            var closeBtn = document.getElementById('close-periph-exec-run-value-modal');
            if (closeBtn) {
                closeBtn.addEventListener('click', function(event) {
                    event.preventDefault();
                    closePrompt();
                });
            }

            var cancelBtn = document.getElementById('cancel-periph-exec-run-value-btn');
            if (cancelBtn) {
                cancelBtn.addEventListener('click', function() {
                    closePrompt();
                });
            }

            var confirmBtn = document.getElementById('confirm-periph-exec-run-value-btn');
            if (confirmBtn) {
                confirmBtn.addEventListener('click', function() {
                    self._submitPeriphExecRunValuePrompt();
                });
            }

            var input = document.getElementById('periph-exec-run-value-input');
            if (input) {
                input.addEventListener('keydown', function(event) {
                    if (event.key === 'Enter') {
                        event.preventDefault();
                        self._submitPeriphExecRunValuePrompt();
                    }
                });
            }

            return modal;
        },

        _setPeriphExecRunValuePromptError(message) {
            var errorEl = document.getElementById('periph-exec-run-value-error');
            if (!errorEl) return;
            errorEl.textContent = message || '';
            if (message) this.showElement(errorEl, 'block');
            else this.hideElement(errorEl);
        },

        _resolvePeriphExecRunValuePrompt(value) {
            var pending = this._periphExecRunPromptState;
            this._periphExecRunPromptState = null;
            this._setPeriphExecRunValuePromptError('');

            var input = document.getElementById('periph-exec-run-value-input');
            if (input) input.value = '';

            this.hideModal('periph-exec-run-value-modal');

            if (pending && typeof pending.resolve === 'function') {
                pending.resolve(value);
            }
        },

        _submitPeriphExecRunValuePrompt() {
            var input = document.getElementById('periph-exec-run-value-input');
            if (!input) {
                this._resolvePeriphExecRunValuePrompt(null);
                return;
            }

            var value = String(input.value || '').trim();
            if (!value.length) {
                this._setPeriphExecRunValuePromptError('请输入要设置的值');
                input.focus();
                return;
            }

            this._resolvePeriphExecRunValuePrompt(value);
        },

        promptPeriphExecRunValue(options) {
            var opts = options || {};
            var modal = this._ensurePeriphExecRunValueModal();
            if (!modal) {
                var fallbackValue = prompt(
                    '请输入要设置的值 (如: PWM占空比、PID参数等):',
                    opts.defaultValue != null ? String(opts.defaultValue) : ''
                );
                if (fallbackValue === null) return Promise.resolve(null);
                var normalizedValue = String(fallbackValue || '').trim();
                if (!normalizedValue.length) {
                    Notification.warning('请输入要设置的值', '外设执行');
                    return Promise.resolve(null);
                }
                return Promise.resolve(normalizedValue);
            }

            var titleEl = document.getElementById('periph-exec-run-value-modal-title');
            var labelEl = document.getElementById('periph-exec-run-value-label');
            var helpEl = document.getElementById('periph-exec-run-value-help');
            var input = document.getElementById('periph-exec-run-value-input');
            var cancelBtn = document.getElementById('cancel-periph-exec-run-value-btn');
            var confirmBtn = document.getElementById('confirm-periph-exec-run-value-btn');

            if (titleEl) titleEl.textContent = opts.title || '输入执行值';
            if (labelEl) labelEl.textContent = opts.label || '执行值';

            var helpText = opts.helpText || '请输入本次执行要设置的值，例如 PWM 占空比、PID 参数或通道值。';
            if (opts.ruleName) helpText = opts.ruleName + ' - ' + helpText;
            if (helpEl) helpEl.textContent = helpText;

            if (cancelBtn) cancelBtn.textContent = '取消';
            if (confirmBtn) confirmBtn.textContent = opts.confirmText || '执行一次';
            if (input) {
                input.value = opts.defaultValue != null ? String(opts.defaultValue) : '';
                input.placeholder = opts.placeholder || '';
            }

            this._setPeriphExecRunValuePromptError('');

            if (this._periphExecRunPromptState && typeof this._periphExecRunPromptState.resolve === 'function') {
                this._periphExecRunPromptState.resolve(null);
            }

            return new Promise((resolve) => {
                this._periphExecRunPromptState = { resolve: resolve };
                this.showModal(modal);
                setTimeout(function() {
                    if (input) {
                        input.focus();
                        input.select();
                    }
                }, 0);
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
            if (target.classList.contains('pe-target-periph')) this._onTargetPeriphChange(target, index);
            else if (target.classList.contains('pe-action-type')) this.onPeriphExecActionTypeChangeInBlock(target.value, index);
            else if (target.classList.contains('pe-use-received-value')) this.onPeriphExecUseRecvChangeInBlock(target, index);
            else if (target.classList.contains('pe-sensor-category')) this.onSensorCategoryChange(target, index);
            else if (target.classList.contains('pe-modbus-device-select')) this._onPeriphDeviceSelect(target);
            else if (target.classList.contains('pe-modbus-channel-select')) this._onPeriphChannelSelect(target);
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

        _refreshActionPeriphSelects() {
            var pollOnly = this._isPollTriggerActive();
            var container = document.getElementById('periph-exec-actions');
            if (!container) return;
            container.querySelectorAll('.pe-target-periph').forEach(sel => {
                this._populatePeriphSelect(sel, sel.value, pollOnly);
            });
        },

        _rebuildActionBlocksForTriggerChange() {
            var container = document.getElementById('periph-exec-actions');
            if (!container) return;
            // 收集当前动作数据
            var existingActions = [];
            container.querySelectorAll('.periph-exec-config-item').forEach(item => {
                var actionType = parseInt(item.querySelector('.pe-action-type')?.value || '0');
                var actionValue = '';
                if (actionType === 15) actionValue = item.querySelector('.pe-action-value-script')?.value || '';
                else if (actionType === 27) actionValue = item.querySelector('.pe-action-value-oled')?.value || '';
                else actionValue = item.querySelector('.pe-action-value')?.value || '';
                existingActions.push({
                    actionType: actionType,
                    targetPeriphId: item.querySelector('.pe-target-periph')?.value || '',
                    actionValue: actionValue,
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
            html += '<td>' + this._renderPeriphExecStatusBadge(rule.enabled) + '</td>';
            html += '<td class="u-fs-12">' + flowHtml + '</td>';
            html += '<td class="u-fs-12 u-text-center">' + escapeHtml(statsText) + '</td>';
            html += '<td class="u-cell-nowrap">' + this._renderPeriphExecRuleActions(ruleId, !!rule.enabled, !!rule.hasSetMode, rule.name || '') + '</td>';
            html += '</tr>';
            return html;
        }
    });

    // ============ CRUD operations and page loading ============
    Object.assign(AppState, {

        editPeriphExecRule(id) {
            if (!this.guardDeveloperModeAction()) return;
            if (!this._isPeriphExecEditorReady()) {
                var self = this;
                return this._ensurePeriphExecEditor().then(function() {
                    return self.editPeriphExecRule(id);
                });
            }
            this.openPeriphExecModal(id);
            // 串行请求：ESP32 heap 紧张时并发 4 个请求会 OOM，必须逐个等完
            const _editResults = {};
            const loadRule = typeof apiGetFresh === 'function' ? apiGetFresh : apiGet;
            loadRule('/api/periph-exec', { id: id })
                .then(execRes => { _editResults.execRes = execRes; return apiGet('/api/peripherals', { pageSize: 100, compact: 1, enabledOnly: 1 }); })
                .then(periphRes => { _editResults.periphRes = periphRes; return apiGet('/api/protocol/config', { compact: 1, section: 'periph-exec' }); })
                .then(protoRes => { _editResults.protoRes = protoRes; return apiGet('/api/periph-exec', { pageSize: 100 }); })
                .then(rulesRes => {
                    _editResults.rulesRes = rulesRes;
                    const { execRes, periphRes, protoRes } = _editResults;
                    if (periphRes && periphRes.success && periphRes.data) {
                        this._pePeripherals = periphRes.data;
                    }
                    this._peDataSources = [];
                    if (protoRes && protoRes.success && protoRes.data) {
                        const protoData = protoRes.data;
                        this._peDataSources = this._extractDataSources(protoData);
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

                    if (this._hasSetModeTrigger()) {
                        this._syncSetModeToActions(true);
                    }
                    this._refreshPeriphExecRiskNotice({ allowFetch: true });
                })
                .catch(err => {
                    console.error('Failed to load periph exec edit data:', err);
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
            var self = this;
            var doRun = function(value) {
                var payload = { id: id };
                if (value !== undefined && value !== '') payload.value = value;
                apiPost('/api/periph-exec/run', payload)
                    .then(res => {
                        if (res && res.success) {
                            Notification.success('执行已提交', '外设执行');
                            if (self.currentPage === 'periph-exec') self.loadPeriphExecPage();
                        } else {
                            Notification.error(res?.error || '规则执行失败', '外设执行');
                        }
                    })
                    .catch(err => {
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
                            Notification.error('设备离线或不可达，请检查网络连接', '外设执行');
                        } else {
                            Notification.error('规则执行失败', '外设执行');
                        }
                    });
            };
            if (hasSetMode) {
                this.promptPeriphExecRunValue({ ruleName: ruleName || '' }).then(inputValue => {
                    if (inputValue === null) return;
                    doRun(inputValue);
                });
            } else {
                doRun('');
            }
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
                if (btn) { btn.disabled = false; btn.innerHTML = '&#x21bb; 刷新'; }
            }, 2000);
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

            this.renderEmptyTableRow(tbody, 7, '加载中...');

            const self = this;
            const apiUrl = '/api/periph-exec?page=' + this._peCurrentPage + '&pageSize=' + this._pePageSize;

            // 串行化：先填充 filter（仅首次调用会发请求），再发主请求，
            // 避免并发两个请求在 ESP32 低 heap 状态下触发 OOM TCP RST。
            Promise.resolve()
                .then(function () { return self._populatePeriphExecFilter(); })
                .catch(function () { /* filter 失败静默，不阻塞主流程 */ })
                .then(function () { return apiGet(apiUrl); })
                .then(execRes => {
                    this._peTotalRules = execRes && execRes.total ? execRes.total : 0;
                    this._setPeriphExecCapacity(execRes && execRes.profile ? execRes.profile : null);
                    if (execRes && Array.isArray(execRes.sensorSources)) {
                        this._peSensorSources = execRes.sensorSources;
                    }
                    const currentPage = execRes && execRes.page ? execRes.page : 1;
                    const currentPageSize = execRes && execRes.pageSize ? execRes.pageSize : 10;
                    
                    if (!execRes || !execRes.success || !execRes.data || execRes.data.length === 0) {
                        this.renderEmptyTableRow(tbody, 5, '暂无外设执行');
                        this._renderPeriphExecPagination(this._peTotalRules, currentPage, currentPageSize);
                        return;
                    }
                    let rules = execRes.data;
                    if (filterVal) {
                        if (filterVal.startsWith('trigger:')) {
                            const triggerType = parseInt(filterVal.split(':')[1], 10);
                            rules = rules.filter(r => r.triggerSummary === triggerType);
                        } else {
                            rules = rules.filter(r => r.targetPeriphName === filterVal);
                        }
                    }
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
                        4: '设置PWM', 5: '设置DAC',
                        6: '系统重启', 7: '恢复出厂',
                        8: 'NTP同步', 9: 'OTA升级',
                        10: '调用外设',
                        13: '高电平反转', 14: '低电平反转',
                        15: '命令脚本',
                        16: 'Modbus线圈写入', 17: 'Modbus寄存器写入',
                        18: 'Modbus轮询采集',
                        19: '传感器数据读取',
                        20: '预留动作',
                        21: '触发设备事件',
                        22: '启用执行规则', 23: '禁用执行规则',
                        24: '显示数字',
                        25: '显示文本',
                        26: '数码管清屏',
                        27: 'OLED自定义显示'
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
                    this.renderEmptyTableRow(tbody, 5, '暂无外设执行');
                });
        },

        _renderPeriphExecPagination(total, page, pageSize) {
            this.renderPagination('periph-exec-pagination', {
                total,
                page,
                pageSize,
                maxVisiblePages: 3,
                summaryText: this._formatPeriphExecCapacitySummary(total),
                onPageChange: (nextPage) => {
                    this._peCurrentPage = nextPage;
                    this.loadPeriphExecPage();
                }
            });
        },

        _refreshPeriphExecResults() {
            // 已删除“最近执行结果”功能
        },

        _formatPeriphExecResultStatus(statusName) {
            var name = String(statusName || '').toLowerCase();
            if (name === 'completed') return { text: '成功', className: 'badge-success' };
            if (name === 'failed') return { text: '失败', className: 'badge-danger' };
            if (name === 'running') return { text: '运行中', className: 'badge-warning' };
            if (name === 'pending') return { text: '等待中', className: 'badge-info' };
            return { text: '未知', className: 'badge-info' };
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
            return apiGet('/api/peripherals', { pageSize: 100, compact: 1, enabledOnly: 1 }).then(res => {
                if (!res || !res.success || !res.data || res.data.length === 0) {
                    sel._populated = true;
                    return;
                }
                const currentVal = sel.value;
                // 在静态触发类型选项之后追加外设名称分组
                const separator = '<option disabled>─── 按外设筛选 ───</option>';
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
            });
        }
    });

    // ============ 合并模式：子模块代码已被构建脚本 prepend 到本文件之前 ============
    // 不再运行时动态加载，直接绑定事件（3 个 HTTP 请求 → 1 个）
    if (typeof AppState.setupPeriphExecEvents === 'function') {
        AppState.setupPeriphExecEvents();
    }
})();
