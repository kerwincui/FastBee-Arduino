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
        _peCurrentPage: 1,
        _pePageSize: 10,
        _peTotalRules: 0,
        _peEventsBound: false,
        _peModbusHealth: null,
        _peModbusHealthFetchedAt: 0,
        _peModbusHealthPromise: null,
        _periphExecRunPromptState: null,

        // ============ 事件绑定 ============
        setupPeriphExecEvents() {
            if (this._peEventsBound) return;
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
            var ruleTableBody = document.getElementById('periph-exec-table-body');
            if (ruleTableBody) {
                ruleTableBody.addEventListener('click', (event) => {
                    var button = event.target.closest('[data-pe-action]');
                    if (!button) return;
                    var action = button.getAttribute('data-pe-action');
                    var ruleId = button.getAttribute('data-id');
                    if (!action || !ruleId) return;
                    if (action === 'run') this.runPeriphExecOnce(ruleId, button.getAttribute('data-has-set-mode') === 'true', button.getAttribute('data-name') || '');
                    else if (action === 'edit') this.editPeriphExecRule(ruleId);
                    else if (action === 'toggle') this.togglePeriphExecRule(ruleId, button.getAttribute('data-next-enabled') === 'true');
                    else if (action === 'delete') this.deletePeriphExecRule(ruleId);
                });
            }
            // 刷新按钮
            var peRefreshBtn = document.getElementById('periph-exec-refresh-btn');
            if (peRefreshBtn) peRefreshBtn.addEventListener('click', () => this._refreshPeriphExecList());
            this._peEventsBound = true;
        },

        // ============ 模态框 ============

        openPeriphExecModal(editId) {
            const modal = document.getElementById('periph-exec-modal');
            if (!modal) return;
            const titleEl = document.getElementById('periph-exec-modal-title');
            var safeId = (editId && editId !== 'null' && editId !== 'undefined') ? editId : '';
            document.getElementById('periph-exec-original-id').value = safeId;
            this.clearInlineError('periph-exec-error');
            if (safeId) {
                if (titleEl) titleEl.textContent = i18n.t('periph-exec-edit-modal-title');
            } else {
                if (titleEl) titleEl.textContent = i18n.t('periph-exec-add-modal-title');
                document.getElementById('periph-exec-form').reset();
            }
            const triggersContainer = document.getElementById('periph-exec-triggers');
            const actionsContainer = document.getElementById('periph-exec-actions');
            if (triggersContainer) triggersContainer.innerHTML = '';
            if (actionsContainer) actionsContainer.innerHTML = '';
            this._clearPeriphExecRiskNotice();
            if (safeId) { this.showModal(modal); return; }
            this._pePeripherals = [];
            this._peDataSources = [];
            apiGet('/api/peripherals?pageSize=50').then(res => {
                if (res && res.success && res.data) this._pePeripherals = res.data.filter(p => p.enabled);
                return apiGet('/api/protocol/config');
            }).then(protoRes => {
                if (protoRes && protoRes.success && protoRes.data) {
                    const protoData = protoRes.data;
                    this._peDataSources = this._extractDataSources(protoData);
                    if (protoData.modbusRtu && protoData.modbusRtu.master) {
                        this._masterTasks = protoData.modbusRtu.master.tasks || [];
                        this._modbusDevices = protoData.modbusRtu.master.devices || [];
                    }
                }
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
            this._clearPeriphExecRiskNotice();
        },

        // ============ 通用辅助 ============

        _hiddenClass(visible) {
            return visible ? '' : ' is-hidden';
        },

        _setSectionVisible(ref, visible, displayValue) {
            return visible ? this.showElement(ref, displayValue) : this.hideElement(ref);
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
                this._setPeriphExecRunValuePromptError(i18n.t('periph-exec-set-value-required') || '请输入要设置的值');
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
                    i18n.t('periph-exec-set-value-prompt') || '请输入要设置的值 (如: PWM占空比、PID参数等):',
                    opts.defaultValue != null ? String(opts.defaultValue) : ''
                );
                if (fallbackValue === null) return Promise.resolve(null);
                var normalizedValue = String(fallbackValue || '').trim();
                if (!normalizedValue.length) {
                    Notification.warning(i18n.t('periph-exec-set-value-required') || '请输入要设置的值', i18n.t('periph-exec-title'));
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

            if (titleEl) titleEl.textContent = opts.title || i18n.t('periph-exec-set-value-title') || '输入执行值';
            if (labelEl) labelEl.textContent = opts.label || i18n.t('periph-exec-set-value-label') || '执行值';

            var helpText = opts.helpText || i18n.t('periph-exec-set-value-help') || '请输入本次执行要设置的值，例如 PWM 占空比、PID 参数或通道值。';
            if (opts.ruleName) helpText = opts.ruleName + ' - ' + helpText;
            if (helpEl) helpEl.textContent = helpText;

            if (cancelBtn) cancelBtn.textContent = i18n.t('cancel') || '取消';
            if (confirmBtn) confirmBtn.textContent = opts.confirmText || i18n.t('periph-exec-run-once') || '执行一次';
            if (input) {
                input.value = opts.defaultValue != null ? String(opts.defaultValue) : '';
                input.placeholder = opts.placeholder || i18n.t('periph-exec-set-value-placeholder') || '';
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
            selectEl.innerHTML = '<option value="">' + i18n.t('periph-exec-select-periph') + '</option>';
            (this._pePeripherals || []).forEach(p => {
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
                existingActions.push({
                    actionType: parseInt(item.querySelector('.pe-action-type')?.value || '0'),
                    targetPeriphId: item.querySelector('.pe-target-periph')?.value || '',
                    actionValue: item.querySelector('.pe-action-value')?.value || '',
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
            var label = enabled ? i18n.t('periph-exec-status-on') : i18n.t('periph-exec-status-off');
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
            return '<button class="fb-btn fb-btn-sm ' + (extraClass || '') + '" ' + attrs + '>' + escapeHtml(label) + '</button>';
        },

        _renderPeriphExecRuleActions(ruleId, enabled, hasSetMode, ruleName) {
            var buttons = [];
            var runAttrs = 'data-name="' + escapeHtml(ruleName || '') + '"';
            if (hasSetMode) runAttrs += ' data-has-set-mode="true"';
            buttons.push(this._renderPeriphExecActionButton('run', ruleId, i18n.t('periph-exec-run-once'), 'fb-btn-outline-primary', runAttrs));
            buttons.push(this._renderPeriphExecActionButton('edit', ruleId, i18n.t('peripheral-edit'), 'fb-btn-primary'));
            buttons.push(this._renderPeriphExecActionButton(
                'toggle',
                ruleId,
                enabled ? i18n.t('peripheral-disable') : i18n.t('peripheral-enable'),
                enabled ? 'fb-btn-warning' : 'fb-btn-success',
                'data-next-enabled="' + (enabled ? 'false' : 'true') + '"'
            ));
            buttons.push(this._renderPeriphExecActionButton('delete', ruleId, i18n.t('peripheral-delete'), 'fb-btn-danger'));
            return buttons.join(' ');
        },

        _renderPeriphExecRuleRow(rule, triggerLabels, actionLabels) {
            var displayName = escapeHtml(rule.name || rule.id || '');
            var triggerText = this._getPeriphExecTriggerSummary(rule, triggerLabels);
            var actionText = this._getPeriphExecActionSummary(rule, actionLabels);
            var periphName = escapeHtml(rule.targetPeriphName || '-');
            var statsText = escapeHtml(i18n.t('periph-exec-stats-count') + ': ' + (rule.triggerCount || 0));
            var ruleId = String(rule.id || '');
            var html = '<tr>';
            html += '<td>' + displayName + '</td>';
            html += '<td>' + this._renderPeriphExecStatusBadge(rule.enabled) + '</td>';
            html += '<td class="u-fs-12">' + escapeHtml(triggerText) + '</td>';
            html += '<td>' + periphName + '</td>';
            html += '<td class="u-fs-12">' + escapeHtml(actionText) + '</td>';
            html += '<td class="u-fs-12">' + statsText + '</td>';
            html += '<td class="u-cell-nowrap">' + this._renderPeriphExecRuleActions(ruleId, !!rule.enabled, !!rule.hasSetMode, rule.name || '') + '</td>';
            html += '</tr>';
            return html;
        }
    });

    // ============ CRUD operations and page loading ============
    Object.assign(AppState, {

        editPeriphExecRule(id) {
            this.openPeriphExecModal(id);
            Promise.all([apiGet('/api/periph-exec?id=' + id), apiGet('/api/peripherals?pageSize=50'), apiGet('/api/protocol/config')])
                .then(([execRes, periphRes, protoRes]) => {
                    if (periphRes && periphRes.success && periphRes.data) {
                        this._pePeripherals = periphRes.data.filter(p => p.enabled);
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
                });
        },

        deletePeriphExecRule(id) {
            if (!confirm(i18n.t('periph-exec-confirm-delete'))) return;
            apiDelete('/api/periph-exec/', { id: id })
                .then(res => {
                    if (res && res.success) {
                        Notification.success(i18n.t('periph-exec-delete-ok'), i18n.t('periph-exec-title'));
                        if (this.currentPage === 'periph-exec') this.loadPeriphExecPage();
                    } else {
                        Notification.error(res?.error || i18n.t('periph-exec-delete-fail'), i18n.t('periph-exec-title'));
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
                        Notification.error(i18n.t('device-offline-error'), i18n.t('periph-exec-title'));
                    } else {
                        Notification.error(i18n.t('periph-exec-delete-fail'), i18n.t('periph-exec-title'));
                    }
                });
        },

        togglePeriphExecRule(id, enable) {
            const url = enable ? '/api/periph-exec/enable' : '/api/periph-exec/disable';
            apiPost(url, { id: id })
                .then(res => {
                    if (res && res.success) {
                        if (this.currentPage === 'periph-exec') this.loadPeriphExecPage();
                    } else {
                        Notification.error(res?.error || i18n.t('periph-exec-toggle-fail'), i18n.t('periph-exec-title'));
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
                        Notification.error(i18n.t('device-offline-error'), i18n.t('periph-exec-title'));
                    } else {
                        Notification.error(i18n.t('periph-exec-toggle-fail'), i18n.t('periph-exec-title'));
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
                            Notification.success(i18n.t('periph-exec-run-submitted'), i18n.t('periph-exec-title'));
                            if (self.currentPage === 'periph-exec') self.loadPeriphExecPage();
                        } else {
                            Notification.error(res?.error || i18n.t('periph-exec-run-fail'), i18n.t('periph-exec-title'));
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
                            Notification.error(i18n.t('device-offline-error'), i18n.t('periph-exec-title'));
                        } else {
                            Notification.error(i18n.t('periph-exec-run-fail'), i18n.t('periph-exec-title'));
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
            const filterSel = document.getElementById('periph-exec-filter-periph');
            const filterPeriphName = filterSel ? filterSel.value : '';

            this._populatePeriphExecFilter();
            this.renderEmptyTableRow(tbody, 7, i18n.t('periph-exec-loading'));

            const apiUrl = '/api/periph-exec?page=' + this._peCurrentPage + '&pageSize=' + this._pePageSize;
            apiGet(apiUrl)
                .then(execRes => {
                    this._peTotalRules = execRes && execRes.total ? execRes.total : 0;
                    const currentPage = execRes && execRes.page ? execRes.page : 1;
                    const currentPageSize = execRes && execRes.pageSize ? execRes.pageSize : 10;
                    
                    if (!execRes || !execRes.success || !execRes.data || execRes.data.length === 0) {
                        this.renderEmptyTableRow(tbody, 7, i18n.t('periph-exec-no-data'));
                        this._renderPeriphExecPagination(this._peTotalRules, currentPage, currentPageSize);
                        return;
                    }
                    let rules = execRes.data;
                    if (filterPeriphName) {
                        rules = rules.filter(r => r.targetPeriphName === filterPeriphName);
                    }
                    rules.sort((a, b) => {
                        if (a.enabled !== b.enabled) return a.enabled ? -1 : 1;
                        return (a.name || '').localeCompare(b.name || '', 'zh');
                    });
                    if (rules.length === 0) {
                        this.renderEmptyTableRow(tbody, 7, i18n.t('periph-exec-no-data'));
                        this._renderPeriphExecPagination(this._peTotalRules, currentPage, currentPageSize);
                        return;
                    }
                    const triggerLabels = {
                        0: i18n.t('periph-exec-trigger-platform'),
                        1: i18n.t('periph-exec-trigger-timer'),
                        4: i18n.t('periph-exec-trigger-event'),
                        5: i18n.t('periph-exec-trigger-poll'),
                        6: i18n.t('periph-exec-trigger-periph-exec')
                    };
                    const actionLabels = {
                        0: i18n.t('periph-exec-action-high'), 1: i18n.t('periph-exec-action-low'),
                        2: i18n.t('periph-exec-action-blink'), 3: i18n.t('periph-exec-action-breathe'),
                        4: i18n.t('periph-exec-action-pwm'), 5: i18n.t('periph-exec-action-dac'),
                        6: i18n.t('periph-exec-action-restart'), 7: i18n.t('periph-exec-action-factory'),
                        8: i18n.t('periph-exec-action-ntp'), 9: i18n.t('periph-exec-action-ota'),
                        10: i18n.t('periph-exec-action-ap'), 11: i18n.t('periph-exec-action-ble'),
                        12: i18n.t('periph-exec-action-call-periph'),
                        15: i18n.t('periph-exec-action-script')
                    };
                    let html = '';
                    rules.forEach(r => {
                        html += this._renderPeriphExecRuleRow(r, triggerLabels, actionLabels);
                    });
                    tbody.innerHTML = html;
                    this._renderPeriphExecPagination(this._peTotalRules, currentPage, currentPageSize);
                })
                .catch(() => {
                    this.renderEmptyTableRow(tbody, 7, i18n.t('periph-exec-no-data'));
                });
        },

        _renderPeriphExecPagination(total, page, pageSize) {
            this.renderPagination('periph-exec-pagination', {
                total,
                page,
                pageSize,
                summaryText: i18n.t('periph-exec-total') + ': ' + total,
                onPageChange: (nextPage) => {
                    this._peCurrentPage = nextPage;
                    this.loadPeriphExecPage();
                }
            });
        },

        _populatePeriphExecFilter() {
            const sel = document.getElementById('periph-exec-filter-periph');
            if (!sel || sel._populated) return;
            apiGet('/api/peripherals?pageSize=50').then(res => {
                if (!res || !res.success || !res.data) return;
                const currentVal = sel.value;
                let opts = '<option value="">' + i18n.t('periph-exec-filter-all') + '</option>';
                res.data.forEach(p => {
                    opts += '<option value="' + p.name + '">' + p.name + '</option>';
                });
                sel.innerHTML = opts;
                if (currentVal) sel.value = currentVal;
                sel._populated = true;
            });
        }
    });

    // ============ 加载子模块，完成后绑定事件 ============
    var _peSubModules = ['/js/modules/periph-exec-modbus.js', '/js/modules/periph-exec-form.js'];
    var _peSubIdx = 0;
    function _loadNextPeSub() {
        if (_peSubIdx >= _peSubModules.length) {
            // 所有子模块加载完成，绑定事件
            if (typeof AppState.setupPeriphExecEvents === 'function') {
                AppState.setupPeriphExecEvents();
            }
            return;
        }
        var s = document.createElement('script');
        s.src = _peSubModules[_peSubIdx++];
        s.onload = _loadNextPeSub;
        s.onerror = function() {
            console.warn('[periph-exec] Failed to load sub-module: ' + s.src);
            _loadNextPeSub(); // 继续加载下一个，不阻塞
        };
        document.head.appendChild(s);
    }
    _loadNextPeSub();
})();
