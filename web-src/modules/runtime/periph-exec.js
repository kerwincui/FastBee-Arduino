/**
 * 外设执行管理模块
 * 包含外设执行规则的创建、编辑、删除、启用/禁用
 * 触发器/动作的动态配置块管理
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

        _hiddenClass(visible) {
            return visible ? '' : ' is-hidden';
        },

        _setSectionVisible(ref, visible, displayValue) {
            return visible ? this.showElement(ref, displayValue) : this.hideElement(ref);
        },

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
                        '<div class="pure-form pure-form-stacked">' +
                            '<div class="pure-control-group">' +
                                '<label for="periph-exec-run-value-input" id="periph-exec-run-value-label"></label>' +
                                '<input type="text" id="periph-exec-run-value-input" class="pure-input-1" maxlength="128" autocomplete="off" spellcheck="false">' +
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

        // ============ 辅助方法 ============

        _clearPeriphExecRiskNotice() {
            var noteEl = document.getElementById('periph-exec-risk-note');
            if (!noteEl) return;
            noteEl.classList.add('is-hidden');
            noteEl.innerHTML = '';
        },

        _getPeriphExecCachedHealth() {
            if (this._peModbusHealth) return this._peModbusHealth;
            return this._modbusStatus && this._modbusStatus.health ? this._modbusStatus.health : null;
        },

        _ensurePeriphExecModbusHealth(forceRefresh) {
            var cached = this._getPeriphExecCachedHealth();
            var now = Date.now();
            if (!forceRefresh && cached && this._peModbusHealthFetchedAt > 0 && (now - this._peModbusHealthFetchedAt) < 15000) {
                return Promise.resolve(cached);
            }
            if (!forceRefresh && this._peModbusHealthPromise) {
                return this._peModbusHealthPromise;
            }
            this._peModbusHealthPromise = apiGetSilent('/api/modbus/status')
                .then((res) => {
                    var health = res && res.success && res.data ? (res.data.health || null) : null;
                    this._peModbusHealth = health;
                    this._peModbusHealthFetchedAt = health ? Date.now() : 0;
                    return health;
                })
                .catch(() => {
                    this._peModbusHealth = null;
                    this._peModbusHealthFetchedAt = 0;
                    return null;
                })
                .finally(() => {
                    this._peModbusHealthPromise = null;
                    this._refreshPeriphExecRiskNotice({ allowFetch: false });
                });
            return this._peModbusHealthPromise;
        },

        _getPeriphExecRiskMeta(level) {
            switch (String(level || 'medium').toLowerCase()) {
                case 'high':
                    return { text: i18n.t('modbus-master-risk-high'), className: 'modbus-risk-high' };
                case 'low':
                    return { text: i18n.t('modbus-master-risk-low'), className: 'modbus-risk-low' };
                default:
                    return { text: i18n.t('modbus-master-risk-medium'), className: 'modbus-risk-medium' };
            }
        },

        _formatPeriphExecAge(ageSec) {
            var age = Number(ageSec || 0);
            if (!age) return '--';
            if (age < 60) return age + 's';
            if (age < 3600) return Math.floor(age / 60) + 'm';
            return Math.floor(age / 3600) + 'h';
        },

        _formatPeriphExecPercent(value) {
            var num = Number(value || 0);
            if (!isFinite(num)) num = 0;
            return (Math.round(num * 10) / 10) + '%';
        },

        _buildPeriphExecFormWarnings(triggers) {
            var warnings = [];
            (triggers || []).filter(function(trigger) {
                return trigger && trigger.triggerType === 5;
            }).forEach(function(trigger) {
                var intervalSec = Number(trigger.intervalSec || 0);
                var timeoutMs = Number(trigger.pollResponseTimeout || 0);
                var retries = Number(trigger.pollMaxRetries || 0);
                var interDelayMs = Number(trigger.pollInterPollDelay || 0);
                if (intervalSec > 0 && intervalSec < 5) {
                    warnings.push(i18n.t('periph-exec-poll-interval-label') + ' < 5s');
                }
                if (timeoutMs > 3000) {
                    warnings.push(i18n.t('periph-exec-poll-timeout-label') + ' > 3000ms');
                }
                if (retries > 2) {
                    warnings.push(i18n.t('periph-exec-poll-retries-label') + ' > 2');
                }
                if (interDelayMs > 0 && interDelayMs < 100) {
                    warnings.push(i18n.t('periph-exec-poll-delay-label') + ' < 100ms');
                }
            });
            return warnings.filter(function(msg, idx, list) {
                return msg && list.indexOf(msg) === idx;
            });
        },

        _resolvePeriphExecRiskLevel(formWarningCount, health) {
            var current = health && health.riskLevel ? String(health.riskLevel).toLowerCase() : 'low';
            if (formWarningCount >= 2) return 'high';
            if (formWarningCount >= 1 && current === 'low') return 'medium';
            if (current === 'low') return 'medium';
            return current;
        },

        _refreshPeriphExecRiskNotice(options) {
            var noteEl = document.getElementById('periph-exec-risk-note');
            if (!noteEl) return;

            var triggers = this._collectPeriphExecTriggers();
            var actions = this._collectPeriphExecActions();
            var hasPollTrigger = triggers.some(function(trigger) {
                return trigger && trigger.triggerType === 5;
            });
            var hasModbusPollAction = actions.some(function(action) {
                return action && action.actionType === 18;
            });

            if (!(hasPollTrigger && hasModbusPollAction)) {
                noteEl.classList.add('is-hidden');
                noteEl.innerHTML = '';
                return;
            }

            var health = this._getPeriphExecCachedHealth();
            if (!health && (!options || options.allowFetch !== false)) {
                this._ensurePeriphExecModbusHealth(false);
            }

            var formWarnings = this._buildPeriphExecFormWarnings(triggers);
            var runtimeWarnings = health && Array.isArray(health.warnings) ? health.warnings.slice(0, 2) : [];
            var warnings = formWarnings.concat(runtimeWarnings).filter(function(msg, idx, list) {
                return msg && list.indexOf(msg) === idx;
            }).slice(0, 4);
            var riskMeta = this._getPeriphExecRiskMeta(this._resolvePeriphExecRiskLevel(formWarnings.length, health));

            var html = '<div class="u-header-between-wrap u-mb-12">' +
                '<h4 class="u-note-title u-note-title-warning u-mb-0"><i class="fas fa-exclamation-triangle"></i> ' +
                escapeHtml(i18n.t('periph-exec-risk-title')) + '</h4>' +
                '<span class="modbus-risk-badge ' + riskMeta.className + '">' + escapeHtml(riskMeta.text) + '</span>' +
                '</div>' +
                '<p class="u-text-secondary u-mb-0">' + escapeHtml(i18n.t('periph-exec-risk-body')) + '</p>';

            if (health) {
                html += '<div class="modbus-health-metrics u-mb-12">' +
                    '<span class="modbus-health-metric"><span class="u-text-muted u-fs-12">' + escapeHtml(i18n.t('modbus-master-enabled-tasks')) + '</span><strong>' + escapeHtml(String(health.enabledTaskCount ?? 0)) + '</strong></span>' +
                    '<span class="modbus-health-metric"><span class="u-text-muted u-fs-12">' + escapeHtml(i18n.t('modbus-master-min-interval')) + '</span><strong>' + escapeHtml(health.minPollInterval ? (health.minPollInterval + 's') : '--') + '</strong></span>' +
                    '<span class="modbus-health-metric"><span class="u-text-muted u-fs-12">' + escapeHtml(i18n.t('modbus-master-last-poll-age')) + '</span><strong>' + escapeHtml(this._formatPeriphExecAge(health.lastPollAgeSec)) + '</strong></span>' +
                    '<span class="modbus-health-metric"><span class="u-text-muted u-fs-12">' + escapeHtml(i18n.t('modbus-master-timeout-rate')) + '</span><strong>' + escapeHtml(this._formatPeriphExecPercent(health.timeoutRate)) + '</strong></span>' +
                    '</div>';
            }

            if (warnings.length > 0) {
                html += '<div class="modbus-health-warnings">' + warnings.map(function(msg) {
                    return '<div class="modbus-health-warning"><i class="fas fa-exclamation-triangle"></i><span>' +
                        escapeHtml(msg || '') + '</span></div>';
                }).join('') + '</div>';
            }

            html += '<p class="u-note-tip">' + escapeHtml(i18n.t('periph-exec-risk-guard')) + '</p>';
            noteEl.innerHTML = html;
            noteEl.classList.remove('is-hidden');
        },

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

        _populatePeriphSelect(selectEl, selectedValue, pollOnly) {
            if (!selectEl) return;
            var html = '<option value="">' + i18n.t('periph-exec-select-periph') + '</option>';
            if (pollOnly) {
                // 轮询触发模式: 仅显示 Modbus 采集类子设备
                var tasks = this._masterTasks || [];
                var fcNames = {1: 'FC01', 2: 'FC02', 3: 'FC03', 4: 'FC04'};
                var hasTask = false;
                for (var ti = 0; ti < tasks.length; ti++) { if (tasks[ti].enabled !== false) { hasTask = true; break; } }
                if (hasTask) {
                    html += '<optgroup label="' + escapeHtml(i18n.t('modbus-type-sensor') || '采集设备') + '">';
                    for (var i = 0; i < tasks.length; i++) {
                        var t = tasks[i];
                        if (t.enabled === false) continue;
                        var label = t.name || t.label || ('Slave ' + (t.slaveAddress || 1));
                        var desc = (fcNames[t.functionCode] || 'FC03') + ' @' + (t.startAddress || 0) + ' x' + (t.quantity || 10);
                        var val = 'modbus-task:' + i;
                        html += '<option value="' + escapeHtml(val) + '">' + escapeHtml(label + ' (' + desc + ')') + '</option>';
                    }
                    html += '</optgroup>';
                }
            } else {
                var gpioPeriphs = [];
                (this._pePeripherals || []).forEach(p => {
                    if (p.type !== 51) gpioPeriphs.push(p);
                });
                var modbusDevices = this._modbusDevices || [];
                var typeLabels = {relay: i18n.t('modbus-type-relay') || '继电器', pwm: i18n.t('modbus-type-pwm') || 'PWM', pid: i18n.t('modbus-type-pid') || 'PID', motor: i18n.t('modbus-type-motor') || '步进电机'};
                if (gpioPeriphs.length > 0) {
                    html += '<optgroup label="' + escapeHtml(i18n.t('periph-exec-periph-group') || '硬件外设') + '">';
                    gpioPeriphs.forEach(p => {
                        html += '<option value="' + escapeHtml(p.id) + '">' + escapeHtml(p.name + ' (' + p.id + ')') + '</option>';
                    });
                    html += '</optgroup>';
                }
                var hasModbus = false;
                for (var mi = 0; mi < modbusDevices.length; mi++) {
                    if (modbusDevices[mi].enabled !== false) { hasModbus = true; break; }
                }
                if (hasModbus) {
                    html += '<optgroup label="' + escapeHtml(i18n.t('periph-exec-modbus-group') || 'Modbus 子设备') + '">';
                    for (var j = 0; j < modbusDevices.length; j++) {
                        var d = modbusDevices[j];
                        if (d.enabled === false) continue;
                        var devId = 'modbus:' + j;
                        var dt = typeLabels[d.deviceType] || d.deviceType || '';
                        var label = (d.name || ('Device ' + (j + 1))) + ' (' + dt + ', Addr ' + (d.slaveAddress || 1) + ')';
                        html += '<option value="' + escapeHtml(devId) + '">' + escapeHtml(label) + '</option>';
                    }
                    html += '</optgroup>';
                }
            }
            selectEl.innerHTML = html;
            if (selectedValue) selectEl.value = selectedValue;
        },

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

        _extractDataSources(protocolConfig) {
            const sources = [];
            if (!protocolConfig) return sources;
            const rtu = protocolConfig.modbusRtu;
            if (rtu && rtu.enabled && rtu.master && rtu.master.tasks) {
                rtu.master.tasks.forEach(task => {
                    if (!task.enabled || !task.mappings) return;
                    task.mappings.forEach(m => {
                        if (m.sensorId) sources.push({ id: m.sensorId, label: (task.name || task.label || 'Modbus') + '/' + m.sensorId });
                    });
                });
            }
            const tcp = protocolConfig.modbusTcp;
            if (tcp && tcp.enabled && tcp.master && tcp.master.tasks) {
                tcp.master.tasks.forEach(task => {
                    if (!task.enabled || !task.mappings) return;
                    task.mappings.forEach(m => {
                        if (m.sensorId) sources.push({ id: m.sensorId, label: 'TCP/' + m.sensorId });
                    });
                });
            }
            return sources;
        },

        _populateSensorPeriphSelect(blockEl, category, selectedValue) {
            var sel = blockEl.querySelector('.pe-target-periph');
            if (!sel) return;
            var periphs = this._pePeripherals || [];
            var analogTypes = [15, 26]; var digitalTypes = [11, 13, 14]; var pulseTypes = [46];
            var allowedTypes;
            if (category === 'digital') allowedTypes = digitalTypes;
            else if (category === 'pulse') allowedTypes = pulseTypes;
            else allowedTypes = analogTypes;
            var prev = selectedValue || sel.value;
            sel.innerHTML = '<option value="">--</option>';
            periphs.filter(function(p) { return allowedTypes.indexOf(p.type) >= 0; }).forEach(function(p) {
                var opt = document.createElement('option');
                opt.value = p.id;
                var pinInfo = (p.pins && p.pins.length > 0) ? (' (Pin ' + p.pins[0] + ')') : '';
                opt.textContent = p.name + pinInfo;
                sel.appendChild(opt);
            });
            if (prev) sel.value = prev;
        },

        onSensorCategoryChange(selectEl, index) {
            var block = this._getPeriphExecBlock('periph-exec-actions', index);
            if (!block) return;
            this._populateSensorPeriphSelect(block, selectEl.value);
        },

        // 轮询触发专用: 仅显示采集传感器任务(无控制设备)
        _populatePollTasksOnly(container, actionValue) {
            if (!container) return;
            var tasks = this._masterTasks || [];
            if (tasks.length === 0) {
                container.innerHTML = '<span class="pe-empty-inline">' + (i18n.t('modbus-no-tasks') || '暂无采集任务') + '</span>';
                return;
            }
            // 解析已选任务索引
            var selTaskIdx = -1;
            if (actionValue) {
                if (actionValue.charAt(0) === '{') {
                    try { var p = JSON.parse(actionValue); if (p.poll && p.poll.length > 0) selTaskIdx = p.poll[0]; } catch(e) {}
                } else {
                    var parts = actionValue.split(',');
                    if (parts.length > 0 && parts[0].trim()) selTaskIdx = parseInt(parts[0].trim());
                }
            }
            var fcNames = {1: 'FC01', 2: 'FC02', 3: 'FC03', 4: 'FC04'};
            var html = '<div class="pe-modbus-select-grid">';
            html += '<div class="pure-control-group pe-field-stack-compact">';
            html += '<label class="pe-field-label-compact">' + (i18n.t('periph-exec-poll-select-task') || '选择采集任务') + '</label>';
            html += '<select class="pure-input-1 pe-poll-task-select u-fs-13">';
            html += '<option value="">--</option>';
            for (var i = 0; i < tasks.length; i++) {
                var t = tasks[i];
                if (t.enabled === false) continue;
                var label = t.name || t.label || ('Slave ' + (t.slaveAddress || 1));
                var desc = (fcNames[t.functionCode] || 'FC03') + ' @' + (t.startAddress || 0) + ' x' + (t.quantity || 10);
                var val = i;
                html += '<option value="' + val + '"' + (val === selTaskIdx ? ' selected' : '') + '>' + escapeHtml(label + ' (' + desc + ')') + '</option>';
            }
            html += '</select></div></div>';
            container.innerHTML = html;
        },

        _populateModbusDevicePanel(container, actionValue) {
            if (!container) return;
            var tasks = this._masterTasks || [];
            if (tasks.length === 0) {
                container.innerHTML = '<span class="pe-empty-inline">' + (i18n.t('modbus-no-devices') || '暂无子设备') + '</span>';
                return;
            }
            var selTaskIdx = '';
            if (actionValue) {
                if (actionValue.charAt(0) === '{') {
                    try {
                        var parsed = JSON.parse(actionValue);
                        if (parsed.poll && parsed.poll.length > 0) selTaskIdx = parsed.poll[0];
                    } catch(e) {}
                } else {
                    var parts = actionValue.split(',');
                    if (parts.length > 0 && parts[0].trim()) selTaskIdx = parseInt(parts[0].trim());
                }
            }
            var fcNames = {1: 'FC01', 2: 'FC02', 3: 'FC03', 4: 'FC04'};
            var html = '<div class="pe-modbus-select-grid">';
            html += '<div class="pure-control-group pe-field-stack-compact">';
            html += '<label class="pe-field-label-compact">' + (i18n.t('periph-exec-poll-select-task') || '选择采集任务') + '</label>';
            html += '<select class="pure-input-1 pe-modbus-device-select u-fs-13">';
            html += '<option value="">--</option>';
            for (var i = 0; i < tasks.length; i++) {
                var t = tasks[i]; if (t.enabled === false) continue;
                var label = t.name || t.label || ('Slave ' + (t.slaveAddress || 1));
                var desc = (fcNames[t.functionCode] || 'FC03') + ' @' + (t.startAddress || 0) + ' x' + (t.quantity || 10);
                var val = 'sensor-' + i;
                html += '<option value="' + val + '"' + (val === ('sensor-' + selTaskIdx) ? ' selected' : '') + '>' + escapeHtml(label + ' (' + desc + ')') + '</option>';
            }
            html += '</select></div></div>';
            container.innerHTML = html;
        },

        _onPeriphDeviceSelect(selectEl) {
            // _populateModbusDevicePanel 已精简为仅采集任务选择器，无需动态切换通道/动作面板
        },

        _onPeriphChannelSelect(selectEl) { },

        _onTargetPeriphChange(selectEl, index) {
            var block = this._getPeriphExecBlock('periph-exec-actions', index);
            if (!block) return;
            var periphId = selectEl.value;
            var isModbus = periphId && periphId.indexOf('modbus:') === 0;
            var isModbusTask = periphId && periphId.indexOf('modbus-task:') === 0;
            var actionTypeGroup = block.querySelector('.pe-action-type-group');
            var actionValueGroup = block.querySelector('.pe-action-value-group');
            var recvGroup = block.querySelector('.pe-use-received-value-group');
            var scriptGroup = block.querySelector('.pe-script-group');
            var pollTasksGroup = block.querySelector('.pe-poll-tasks-group');
            var sensorGroup = block.querySelector('.pe-sensor-group');
            var ctrlPanel = block.querySelector('.pe-modbus-ctrl-panel');
            if (isModbusTask) {
                // 采集类子设备：隐藏所有动作配置字段（仅轮询采集，无需控制）
                this._setSectionVisible(actionTypeGroup, false);
                this._setSectionVisible(actionValueGroup, false);
                this._setSectionVisible(recvGroup, false);
                this._setSectionVisible(scriptGroup, false);
                this._setSectionVisible(pollTasksGroup, false);
                this._setSectionVisible(sensorGroup, false);
                this._setSectionVisible(ctrlPanel, false);
            } else if (isModbus && !isModbusTask) {
                this._setSectionVisible(actionTypeGroup, false);
                this._setSectionVisible(actionValueGroup, false);
                this._setSectionVisible(recvGroup, false);
                this._setSectionVisible(scriptGroup, false);
                this._setSectionVisible(pollTasksGroup, false);
                this._setSectionVisible(sensorGroup, false);
                this._showModbusCtrlPanel(ctrlPanel, periphId, '');
            } else {
                this._setSectionVisible(ctrlPanel, false);
                this._setSectionVisible(actionTypeGroup, true);
                var actionType = block.querySelector('.pe-action-type');
                if (actionType) this.onPeriphExecActionTypeChangeInBlock(actionType.value, index);
            }
        },

        _getModbusDeviceByPeriphId(periphId) {
            if (!periphId || periphId.indexOf('modbus:') !== 0) return null;
            var idx = parseInt(periphId.substring(7));
            var devices = this._modbusDevices || [];
            return (idx >= 0 && idx < devices.length) ? devices[idx] : null;
        },

        _showModbusCtrlPanel(container, periphId, actionValue) {
            if (!container) return;
            var dev = this._getModbusDeviceByPeriphId(periphId);
            if (!dev) {
                container.innerHTML = '<span class="pe-empty-inline">' + (i18n.t('modbus-device-not-found') || '未找到对应子设备') + '</span>';
                this._setSectionVisible(container, true);
                return;
            }
            var selChannel = ''; var selAction = ''; var selValue = ''; var selParam = '';
            if (actionValue) {
                try {
                    var parsed = JSON.parse(actionValue);
                    if (parsed.ctrl && parsed.ctrl.length > 0) {
                        var c = parsed.ctrl[0];
                        selChannel = String(c.c || 0); selAction = c.a || ''; selValue = String(c.v || 0); selParam = c.p || '';
                    }
                } catch(e) {}
            }
            var html = '<div class="pe-modbus-select-grid">';
            var dt = dev.deviceType || 'relay';
            if (dt !== 'motor') {
                html += '<div class="pure-control-group pe-field-stack-compact">';
                html += '<label class="pe-field-label-compact">' + (i18n.t('periph-exec-select-channel') || '选择通道') + '</label>';
                html += '<select class="pure-input-1 pe-modbus-channel-select u-fs-13">';
                for (var ch = 0; ch < (dev.channelCount || 2); ch++) {
                    html += '<option value="' + ch + '"' + (String(ch) === selChannel ? ' selected' : '') + '>CH' + ch + '</option>';
                }
                html += '</select></div>';
            }
            if (dt === 'relay') {
                html += '<div class="pure-control-group pe-field-stack-compact">';
                html += '<label class="pe-field-label-compact">' + (i18n.t('periph-exec-select-action') || '子设备动作') + '</label>';
                html += '<select class="pure-input-1 pe-modbus-action-select u-fs-13">' +
                    '<option value="on"' + (selAction === 'off' ? '' : ' selected') + '>' + (i18n.t('periph-exec-ctrl-on') || '打开') + '</option>' +
                    '<option value="off"' + (selAction === 'off' ? ' selected' : '') + '>' + (i18n.t('periph-exec-ctrl-off') || '关闭') + '</option></select>';
                html += '</div>';
            } else if (dt === 'pwm') {
                var maxPwm = dev.pwmResolution === 10 ? 1023 : (dev.pwmResolution === 16 ? 65535 : 255);
                html += '<div class="pure-control-group pe-field-stack-compact">';
                html += '<label class="pe-field-label-compact">' + (i18n.t('periph-exec-ctrl-duty') || '占空比') + '</label>';
                html += '<input type="number" class="pure-input-1 pe-modbus-action-value u-fs-13" min="0" max="' + maxPwm + '" value="' + (selValue || 0) + '">';
                html += '</div>';
            } else if (dt === 'pid') {
                html += '<div class="pe-pid-fields">';
                html += '<div class="pure-control-group pe-field-stack-compact">';
                html += '<label class="pe-field-label-compact">' + (i18n.t('periph-exec-ctrl-pid-param') || '参数') + '</label>';
                html += '<select class="pure-input-1 pe-modbus-action-param u-fs-13">' +
                    '<option value="P"' + (selParam === 'I' || selParam === 'D' ? '' : ' selected') + '>P</option>' +
                    '<option value="I"' + (selParam === 'I' ? ' selected' : '') + '>I</option>' +
                    '<option value="D"' + (selParam === 'D' ? ' selected' : '') + '>D</option></select>';
                html += '</div>';
                html += '<div class="pure-control-group pe-field-stack-compact">';
                html += '<label class="pe-field-label-compact">' + (i18n.t('periph-exec-ctrl-value') || '值') + '</label>';
                html += '<input type="number" class="pure-input-1 pe-modbus-action-value u-fs-13" value="' + (selValue || 0) + '">';
                html += '</div>';
                html += '</div>';
            } else if (dt === 'motor') {
                html += '<div class="pure-control-group pe-field-stack-compact">';
                html += '<label class="pe-field-label-compact">' + (i18n.t('periph-exec-select-action') || '子设备动作') + '</label>';
                html += '<select class="pure-input-1 pe-modbus-action-select u-fs-13">' +
                    '<option value="forward"' + (selAction === 'forward' ? ' selected' : '') + '>' + (i18n.t('periph-exec-ctrl-forward') || '正转') + '</option>' +
                    '<option value="reverse"' + (selAction === 'reverse' ? ' selected' : '') + '>' + (i18n.t('periph-exec-ctrl-reverse') || '反转') + '</option>' +
                    '<option value="stop"' + (selAction === 'stop' ? ' selected' : '') + '>' + (i18n.t('periph-exec-ctrl-stop') || '停止') + '</option></select>';
                html += '</div>';
            }
            html += '</div>';
            container.innerHTML = html;
            this._setSectionVisible(container, true);
        },

        _showModbusTriggerCtrlPanel(container, deviceIndex, actionValue) {
            if (!container) return;
            var devices = this._modbusDevices || [];
            var dev = devices[deviceIndex];
            if (!dev) {
                container.innerHTML = '<span class="pe-empty-inline">' + (i18n.t('modbus-device-not-found') || '未找到对应子设备') + '</span>';
                this._setSectionVisible(container, true);
                return;
            }
            var selChannel = ''; var selAction = ''; var selValue = ''; var selParam = '';
            if (actionValue) {
                try {
                    var parsed = JSON.parse(actionValue);
                    if (parsed.ctrl && parsed.ctrl.length > 0) {
                        var c = parsed.ctrl[0];
                        selChannel = String(c.c || 0); selAction = c.a || ''; selValue = String(c.v || 0); selParam = c.p || '';
                    }
                } catch(e) {}
            }
            var html = '<div class="pe-modbus-select-grid">';
            var dt = dev.deviceType || 'relay';
            if (dt !== 'motor') {
                html += '<div class="pure-control-group pe-field-stack-compact">';
                html += '<label class="pe-field-label-compact">' + (i18n.t('periph-exec-select-channel') || '选择通道') + '</label>';
                html += '<select class="pure-input-1 pe-modbus-channel-select u-fs-13">';
                for (var ch = 0; ch < (dev.channelCount || 2); ch++) {
                    html += '<option value="' + ch + '"' + (String(ch) === selChannel ? ' selected' : '') + '>CH' + ch + '</option>';
                }
                html += '</select></div>';
            }
            if (dt === 'relay') {
                html += '<div class="pure-control-group pe-field-stack-compact">';
                html += '<label class="pe-field-label-compact">' + (i18n.t('periph-exec-select-action') || '子设备动作') + '</label>';
                html += '<select class="pure-input-1 pe-modbus-action-select u-fs-13">' +
                    '<option value="on"' + (selAction === 'off' ? '' : ' selected') + '>' + (i18n.t('periph-exec-ctrl-on') || '打开') + '</option>' +
                    '<option value="off"' + (selAction === 'off' ? ' selected' : '') + '>' + (i18n.t('periph-exec-ctrl-off') || '关闭') + '</option></select>';
                html += '</div>';
            } else if (dt === 'motor') {
                html += '<div class="pure-control-group pe-field-stack-compact">';
                html += '<label class="pe-field-label-compact">' + (i18n.t('periph-exec-select-action') || '子设备动作') + '</label>';
                html += '<select class="pure-input-1 pe-modbus-action-select u-fs-13">' +
                    '<option value="forward"' + (selAction === 'forward' ? ' selected' : '') + '>' + (i18n.t('periph-exec-ctrl-forward') || '正转') + '</option>' +
                    '<option value="reverse"' + (selAction === 'reverse' ? ' selected' : '') + '>' + (i18n.t('periph-exec-ctrl-reverse') || '反转') + '</option>' +
                    '<option value="stop"' + (selAction === 'stop' ? ' selected' : '') + '>' + (i18n.t('periph-exec-ctrl-stop') || '停止') + '</option></select>';
                html += '</div>';
            } else if (dt === 'pwm') {
                var maxPwm = dev.pwmResolution === 10 ? 1023 : (dev.pwmResolution === 16 ? 65535 : 255);
                html += '<div class="pure-control-group pe-field-stack-compact">';
                html += '<label class="pe-field-label-compact">' + (i18n.t('periph-exec-ctrl-duty') || '占空比') + '</label>';
                html += '<input type="number" class="pure-input-1 pe-modbus-action-value u-fs-13" min="0" max="' + maxPwm + '" value="' + (selValue || 0) + '">';
                html += '</div>';
            } else if (dt === 'pid') {
                html += '<div class="pe-pid-fields">';
                html += '<div class="pure-control-group pe-field-stack-compact">';
                html += '<label class="pe-field-label-compact">' + (i18n.t('periph-exec-ctrl-pid-param') || '参数') + '</label>';
                html += '<select class="pure-input-1 pe-modbus-action-param u-fs-13">' +
                    '<option value="P"' + (selParam === 'I' || selParam === 'D' ? '' : ' selected') + '>P</option>' +
                    '<option value="I"' + (selParam === 'I' ? ' selected' : '') + '>I</option>' +
                    '<option value="D"' + (selParam === 'D' ? ' selected' : '') + '>D</option></select>';
                html += '</div>';
                html += '<div class="pure-control-group pe-field-stack-compact">';
                html += '<label class="pe-field-label-compact">' + (i18n.t('periph-exec-ctrl-value') || '值') + '</label>';
                html += '<input type="number" class="pure-input-1 pe-modbus-action-value u-fs-13" value="' + (selValue || 0) + '">';
                html += '</div>';
                html += '</div>';
            }
            html += '</div>';
            container.innerHTML = html;
            this._setSectionVisible(container, true);
        },

        _onCtrlDeviceToggle(checkbox) {
            var panel = checkbox.closest('.pe-ctrl-device-item').querySelector('.pe-ctrl-options');
            this._setSectionVisible(panel, checkbox.checked);
        },

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
            return '<button class="btn btn-sm ' + (extraClass || '') + '" ' + attrs + '>' + escapeHtml(label) + '</button>';
        },

        _renderPeriphExecRuleActions(ruleId, enabled, hasSetMode, ruleName) {
            var buttons = [];
            var runAttrs = 'data-name="' + escapeHtml(ruleName || '') + '"';
            if (hasSetMode) runAttrs += ' data-has-set-mode="true"';
            buttons.push(this._renderPeriphExecActionButton('run', ruleId, i18n.t('periph-exec-run-once'), 'btn-run', runAttrs));
            buttons.push(this._renderPeriphExecActionButton('edit', ruleId, i18n.t('peripheral-edit'), 'btn-edit'));
            buttons.push(this._renderPeriphExecActionButton(
                'toggle',
                ruleId,
                enabled ? i18n.t('peripheral-disable') : i18n.t('peripheral-enable'),
                enabled ? 'btn-disable' : 'btn-enable',
                'data-next-enabled="' + (enabled ? 'false' : 'true') + '"'
            ));
            buttons.push(this._renderPeriphExecActionButton('delete', ruleId, i18n.t('peripheral-delete'), 'btn-delete'));
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
        },

        // ============ Part 2: Trigger/action element creation ============

        _createPeriphExecTriggerElement(data, index) {
            const container = document.getElementById('periph-exec-triggers');
            if (!container) return;
            const div = document.createElement('div');
            div.className = 'periph-exec-config-item';
            div.dataset.index = index;
            const triggerType = String(data.triggerType ?? 0);
            const showPlatform = triggerType === '0';
            const showTimer = triggerType === '1';
            const showEvent = triggerType === '4';
            const isDataSourceEvent = showEvent && data.eventId && String(data.eventId).indexOf('ds:') === 0;
            const isModbusCtrlEvent = showEvent && data.eventId && String(data.eventId).indexOf('mc:') === 0;
            const showPoll = triggerType === '5';
            const timerMode = String(data.timerMode ?? 0);
            const showInterval = timerMode === '0';
            const showDaily = timerMode === '1';
            const opVal = String(data.operatorType ?? 0);
            const mappedOp = (opVal === '0' || opVal === '1') ? opVal : '0';
            const showCompareValue = showPlatform && mappedOp !== '1';
            div.innerHTML = '<span class="mqtt-topic-index">' + (index + 1) + '</span>' +
                '<button type="button" class="mqtt-topic-delete">' + i18n.t('peripheral-delete') + '</button>' +
                '<div class="config-form-grid">' +
                    '<div class="pure-control-group"><label>' + i18n.t('periph-exec-trigger-type-label') + '</label>' +
                    '<select class="pure-input-1 pe-trigger-type">' +
                        '<option value="0"' + (triggerType === '0' ? ' selected' : '') + '>' + i18n.t('periph-exec-trigger-platform') + '</option>' +
                        '<option value="4"' + (triggerType === '4' ? ' selected' : '') + '>' + i18n.t('periph-exec-trigger-event') + '</option>' +
                        '<option value="1"' + (triggerType === '1' ? ' selected' : '') + '>' + i18n.t('periph-exec-trigger-timer') + '</option>' +
                        '<option value="5"' + (triggerType === '5' ? ' selected' : '') + '>' + i18n.t('periph-exec-trigger-poll') + '</option>' +
                    '</select></div>' +
                    '<div class="pure-control-group pe-trigger-periph-group' + this._hiddenClass(showPlatform) + '">' +
                    '<label>' + i18n.t('periph-exec-trigger-periph-label') + '</label>' +
                    '<select class="pure-input-1 pe-trigger-periph"><option value="">' + i18n.t('periph-exec-select-periph') + '</option></select></div>' +
                '</div>' +
                '<div class="pe-platform-condition' + this._hiddenClass(showPlatform) + '">' +
                    '<div class="config-form-grid">' +
                    '<div class="pure-control-group"><label>' + i18n.t('periph-exec-operator-label') + '</label>' +
                    '<select class="pure-input-1 pe-operator">' +
                        '<option value="0"' + (mappedOp === '0' ? ' selected' : '') + '>' + i18n.t('periph-exec-handle-match') + '</option>' +
                        '<option value="1"' + (mappedOp === '1' ? ' selected' : '') + '>' + i18n.t('periph-exec-handle-set') + '</option>' +
                    '</select></div>' +
                    '<div class="pure-control-group pe-compare-value-group' + this._hiddenClass(showCompareValue) + '">' +
                    '<label>' + i18n.t('periph-exec-compare-label') + '</label>' +
                    '<input type="text" class="pure-input-1 pe-compare-value" value="' + escapeHtml(data.compareValue) + '" placeholder="' + escapeHtml(i18n.t('periph-exec-compare-hint')) + '"></div>' +
                    '</div></div>' +
                '<div class="pe-poll-params' + this._hiddenClass(showPoll) + '">' +
                    '<div class="config-form-grid">' +
                    '<div class="pure-control-group"><label>' + i18n.t('periph-exec-poll-interval-label') + '</label><input type="number" class="pure-input-1 pe-poll-interval" value="' + (data.intervalSec || 60) + '" min="5" max="86400"></div>' +
                    '<div class="pure-control-group"><label>' + i18n.t('periph-exec-poll-timeout-label') + '</label><input type="number" class="pure-input-1 pe-poll-timeout" value="' + (data.pollResponseTimeout || 1000) + '" min="100" max="5000"></div>' +
                    '<div class="pure-control-group"><label>' + i18n.t('periph-exec-poll-retries-label') + '</label><input type="number" class="pure-input-1 pe-poll-retries" value="' + (data.pollMaxRetries ?? 2) + '" min="0" max="3"></div>' +
                    '<div class="pure-control-group"><label>' + i18n.t('periph-exec-poll-delay-label') + '</label><input type="number" class="pure-input-1 pe-poll-inter-delay" value="' + (data.pollInterPollDelay || 100) + '" min="20" max="1000"></div>' +
                    '</div></div>' +
                '<div class="pe-timer-config' + this._hiddenClass(showTimer) + '">' +
                    '<div class="config-form-grid">' +
                    '<div class="pure-control-group"><label>' + i18n.t('periph-exec-timer-mode-label') + '</label>' +
                    '<select class="pure-input-1 pe-timer-mode">' +
                        '<option value="0"' + (timerMode === '0' ? ' selected' : '') + '>' + i18n.t('periph-exec-timer-interval') + '</option>' +
                        '<option value="1"' + (timerMode === '1' ? ' selected' : '') + '>' + i18n.t('periph-exec-timer-daily') + '</option>' +
                    '</select></div>' +
                    '<div class="pure-control-group pe-interval-fields' + this._hiddenClass(showInterval) + '"><label>' + i18n.t('periph-exec-interval-label') + '</label><input type="number" class="pure-input-1 pe-interval" value="' + (data.intervalSec || 60) + '" min="1" max="86400"></div>' +
                    '<div class="pure-control-group pe-daily-fields' + this._hiddenClass(showDaily) + '"><label>' + i18n.t('periph-exec-timepoint-label') + '</label><input type="time" class="pure-input-1 pe-timepoint" value="' + (data.timePoint || '08:00') + '"></div>' +
                    '</div></div>' +
                '<div class="pe-event-group' + this._hiddenClass(showEvent) + '">' +
                    '<div class="config-form-grid">' +
                    '<div class="pure-control-group"><label>' + i18n.t('periph-exec-event-category-label') + '</label>' +
                    '<select class="pure-input-1 pe-event-category">' +
                    '<option value="">' + i18n.t('periph-exec-select-category') + '</option></select></div>' +
                    '<div class="pure-control-group"><label>' + i18n.t('periph-exec-event-label') + '</label>' +
                    '<select class="pure-input-1 pe-event"><option value="">' + i18n.t('periph-exec-select-event') + '</option></select></div>' +
                    '<div class="pure-control-group pe-event-condition-group' + this._hiddenClass(isDataSourceEvent) + '"><label>' + (i18n.t('periph-exec-event-operator-label') || '比较操作符') + '</label>' +
                    '<select class="pure-input-1 pe-event-operator">' +
                    '<option value="0"' + (opVal === '0' ? ' selected' : '') + '>' + i18n.t('periph-exec-op-eq') + '</option>' +
                    '<option value="1"' + (opVal === '1' ? ' selected' : '') + '>' + i18n.t('periph-exec-op-neq') + '</option>' +
                    '<option value="2"' + (opVal === '2' ? ' selected' : '') + '>' + i18n.t('periph-exec-op-gt') + '</option>' +
                    '<option value="3"' + (opVal === '3' ? ' selected' : '') + '>' + i18n.t('periph-exec-op-lt') + '</option>' +
                    '<option value="4"' + (opVal === '4' ? ' selected' : '') + '>' + i18n.t('periph-exec-op-gte') + '</option>' +
                    '<option value="5"' + (opVal === '5' ? ' selected' : '') + '>' + i18n.t('periph-exec-op-lte') + '</option>' +
                    '<option value="6"' + (opVal === '6' ? ' selected' : '') + '>' + i18n.t('periph-exec-op-between') + '</option>' +
                    '<option value="7"' + (opVal === '7' ? ' selected' : '') + '>' + i18n.t('periph-exec-op-not-between') + '</option>' +
                    '<option value="8"' + (opVal === '8' ? ' selected' : '') + '>' + i18n.t('periph-exec-op-contain') + '</option>' +
                    '<option value="9"' + (opVal === '9' ? ' selected' : '') + '>' + i18n.t('periph-exec-op-not-contain') + '</option>' +
                    '</select></div>' +
                    '<div class="pure-control-group pe-event-compare-group' + this._hiddenClass(isDataSourceEvent) + '"><label>' + (i18n.t('periph-exec-event-compare-label') || '比较值') + '</label>' +
                    '<input type="text" class="pure-input-1 pe-event-compare" value="' + escapeHtml(data.compareValue || '') + '" placeholder="' + escapeHtml(i18n.t('periph-exec-event-compare-hint') || '如: 25.5') + '"></div>' +
                    '</div>' +
                    '<div class="pe-event-modbus-ctrl-panel' + this._hiddenClass(isModbusCtrlEvent) + '"></div>' +
                    '</div>';
            container.appendChild(div);
            this._populateTriggerPeriphSelect(div.querySelector('.pe-trigger-periph'), data.triggerPeriphId || data.sourcePeriphId || '');
            if (showEvent) this._populateEventCategoriesInBlock(div, data.eventId, data.compareValue || '');
        },

        _createPeriphExecActionElement(data, index) {
            const container = document.getElementById('periph-exec-actions');
            if (!container) return;
            const div = document.createElement('div');
            div.className = 'periph-exec-config-item';
            div.dataset.index = index;
            const isPollMode = this._isPollTriggerActive();
            const actionType = String(isPollMode ? 18 : (data.actionType ?? 0));
            const actionTypeInt = parseInt(actionType);
            const isModbusPoll = actionTypeInt === 18;
            const isSensorRead = actionTypeInt === 19;
            const isModbusTarget = data.targetPeriphId && data.targetPeriphId.indexOf('modbus:') === 0;
            const showPeriphGroup = isPollMode || (isModbusTarget || !((actionTypeInt >= 6 && actionTypeInt <= 11) || actionTypeInt === 15 || isModbusPoll));
            const needsValue = !isPollMode && !isModbusTarget && (actionTypeInt >= 2 && actionTypeInt <= 5);
            const showRecv = !isPollMode && !isModbusTarget && this._hasSetModeTrigger() && needsValue;
            const isScript = !isPollMode && actionTypeInt === 15;
            const showActionType = !isPollMode && !isModbusTarget;
            const showExecRow = true;
            const execMode = parseInt(data.execMode ?? 0);
            const sel = (v) => actionType === String(v) ? 'selected' : '';
            var sensorCfg = {sensorCategory:'analog',periphId:'',scaleFactor:1,offset:0,decimalPlaces:2,sensorLabel:'',unit:''};
            if (isSensorRead && data.actionValue) { try { Object.assign(sensorCfg, JSON.parse(data.actionValue)); } catch(e) {} }
            const selCat = (v) => sensorCfg.sensorCategory === v ? 'selected' : '';
            div.innerHTML = '<span class="mqtt-topic-index">' + (index + 1) + '</span>' +
                '<button type="button" class="mqtt-topic-delete">' + i18n.t('peripheral-delete') + '</button>' +
                '<div class="pe-action-grid">' +
                    '<div class="pe-exec-row pe-span-all' + this._hiddenClass(showExecRow) + '">' +
                    '<div class="pure-control-group pe-exec-mode-group">' +
                    '<label>' + i18n.t('periph-exec-exec-mode-label') + '</label>' +
                    '<select class="pure-input-1 pe-exec-mode">' +
                    '<option value="0"' + (execMode === 0 ? ' selected' : '') + '>' + i18n.t('periph-exec-exec-mode-async') + '</option>' +
                    '<option value="1"' + (execMode === 1 ? ' selected' : '') + '>' + i18n.t('periph-exec-exec-mode-sync') + '</option>' +
                    '</select></div>' +
                    '<div class="pure-control-group pe-sync-delay-group">' +
                    '<label>' + i18n.t('periph-exec-sync-delay-label') + '</label>' +
                    '<input type="number" class="pure-input-1 pe-sync-delay" min="0" max="10000" step="100" value="' + (data.syncDelayMs || 0) + '" placeholder="0"></div></div>' +
                    '<div class="pure-control-group pe-target-group' + this._hiddenClass(showPeriphGroup) + '">' +
                    '<label>' + i18n.t('periph-exec-target-periph-label') + '</label>' +
                    '<select class="pure-input-1 pe-target-periph"><option value="">' + i18n.t('periph-exec-select-periph') + '</option></select></div>' +
                    '<div class="pure-control-group pe-action-type-group' + this._hiddenClass(showActionType) + '"><label>' + i18n.t('periph-exec-action-type-label') + '</label>' +
                    '<select class="pure-input-1 pe-action-type">' +
                        '<optgroup label="' + i18n.t('periph-exec-action-cat-gpio') + '">' +
                        '<option value="0" ' + sel(0) + '>' + i18n.t('periph-exec-action-high') + '</option>' +
                        '<option value="1" ' + sel(1) + '>' + i18n.t('periph-exec-action-low') + '</option>' +
                        '<option value="2" ' + sel(2) + '>' + i18n.t('periph-exec-action-blink') + '</option>' +
                        '<option value="3" ' + sel(3) + '>' + i18n.t('periph-exec-action-breathe') + '</option></optgroup>' +
                        '<optgroup label="' + i18n.t('periph-exec-action-cat-analog') + '">' +
                        '<option value="4" ' + sel(4) + '>' + i18n.t('periph-exec-action-pwm') + '</option>' +
                        '<option value="5" ' + sel(5) + '>' + i18n.t('periph-exec-action-dac') + '</option></optgroup>' +
                        '<optgroup label="' + i18n.t('periph-exec-action-cat-system') + '">' +
                        '<option value="6" ' + sel(6) + '>' + i18n.t('periph-exec-action-restart') + '</option>' +
                        '<option value="7" ' + sel(7) + '>' + i18n.t('periph-exec-action-factory') + '</option>' +
                        '<option value="8" ' + sel(8) + '>' + i18n.t('periph-exec-action-ntp') + '</option>' +
                        '<option value="9" ' + sel(9) + '>' + i18n.t('periph-exec-action-ota') + '</option>' +
                        '<option value="10" ' + sel(10) + '>' + i18n.t('periph-exec-action-ap') + '</option>' +
                        '<option value="11" ' + sel(11) + '>' + i18n.t('periph-exec-action-ble') + '</option></optgroup>' +
                        '<optgroup label="' + i18n.t('periph-exec-action-cat-advanced') + '">' +
                        '<option value="15" ' + sel(15) + '>' + i18n.t('periph-exec-action-script') + '</option>' +
                        '<option value="19" ' + sel(19) + '>' + i18n.t('periph-exec-action-sensor-read') + '</option></optgroup>' +
                    '</select></div>' +
                    '<div class="pure-control-group pe-action-value-group' + this._hiddenClass(needsValue) + '">' +
                    '<label>' + i18n.t('periph-exec-action-value-label') + '</label>' +
                    '<input type="text" class="pure-input-1 pe-action-value" value="' + (isScript ? '' : escapeHtml(data.actionValue)) + '" placeholder="' + escapeHtml(i18n.t('periph-exec-action-value-hint')) + '"' + (showRecv && data.useReceivedValue !== false ? ' readonly' : '') + '>' +
                    '<small class="pe-help-text">' + i18n.t('periph-exec-action-value-help') + '</small></div>' +
                    '<div class="pure-control-group pe-use-received-value-group' + this._hiddenClass(showRecv) + '">' +
                    '<label>' + (i18n.t('periph-exec-use-received-label') || '使用接收值') + '</label>' +
                    '<label class="pe-checkbox-label pe-checkbox-input-align"><input type="checkbox" class="pe-use-received-value"' + (showRecv && data.useReceivedValue !== false ? ' checked' : '') + '>' +
                    i18n.t('periph-exec-use-received-value-help') + '</label></div>' +
                    '<div class="pure-control-group pe-script-group pe-span-all' + this._hiddenClass(isScript) + '">' +
                    '<label>' + i18n.t('periph-exec-script-label') + '</label>' +
                    '<textarea class="pure-input-1 pe-script pe-script-textarea" rows="6" maxlength="1024" placeholder="' + escapeHtml(i18n.t('periph-exec-script-placeholder')) + '">' + (isScript ? escapeHtml(data.actionValue) : '') + '</textarea>' +
                    '<small class="pe-help-text">' + i18n.t('periph-exec-script-help') + '</small></div>' +
                    '<div class="pure-control-group pe-poll-tasks-group pe-span-all' + this._hiddenClass(!isPollMode && isModbusPoll && !isModbusTarget) + '">' +
                    '<label>' + i18n.t('periph-exec-poll-tasks-label') + '</label>' +
                    '<div class="pe-poll-tasks-list"></div>' +
                    '<small class="pe-help-text">' + i18n.t('periph-exec-poll-tasks-help') + '</small></div>' +
                    '<div class="pure-control-group pe-sensor-group pe-span-all' + this._hiddenClass(isSensorRead) + '">' +
                    '<div class="pe-sensor-config-grid">' +
                    '<div class="pure-control-group"><label>' + i18n.t('periph-exec-sensor-category') + '</label>' +
                    '<select class="pure-input-1 pe-sensor-category">' +
                    '<option value="analog" ' + selCat('analog') + '>' + i18n.t('periph-exec-sensor-cat-analog') + '</option>' +
                    '<option value="digital" ' + selCat('digital') + '>' + i18n.t('periph-exec-sensor-cat-digital') + '</option>' +
                    '<option value="pulse" ' + selCat('pulse') + '>' + i18n.t('periph-exec-sensor-cat-pulse') + '</option></select></div>' +
                    '<div class="pure-control-group"><label>' + i18n.t('periph-exec-sensor-scale') + '</label><input type="number" class="pure-input-1 pe-sensor-scale" step="any" value="' + sensorCfg.scaleFactor + '"></div>' +
                    '<div class="pure-control-group"><label>' + i18n.t('periph-exec-sensor-offset') + '</label><input type="number" class="pure-input-1 pe-sensor-offset" step="any" value="' + sensorCfg.offset + '"></div>' +
                    '<div class="pure-control-group"><label>' + i18n.t('periph-exec-sensor-decimals') + '</label><input type="number" class="pure-input-1 pe-sensor-decimals" min="0" max="6" value="' + sensorCfg.decimalPlaces + '"></div>' +
                    '<div class="pure-control-group"><label>' + i18n.t('periph-exec-sensor-label') + '</label><input type="text" class="pure-input-1 pe-sensor-label" value="' + escapeHtml(sensorCfg.sensorLabel) + '" placeholder="' + i18n.t('periph-exec-sensor-label') + '"></div>' +
                    '<div class="pure-control-group"><label>' + i18n.t('periph-exec-sensor-unit') + '</label><input type="text" class="pure-input-1 pe-sensor-unit" value="' + escapeHtml(sensorCfg.unit) + '" maxlength="8" placeholder="°C, %, V..."></div>' +
                    '</div></div>' +
                    '<div class="pure-control-group pe-modbus-ctrl-panel pe-span-all' + this._hiddenClass(isModbusTarget) + '"></div>' +
                    '</div>';
            container.appendChild(div);
            if (!isPollMode && isSensorRead) {
                this._populateSensorPeriphSelect(div, sensorCfg.sensorCategory || 'analog', sensorCfg.periphId || data.targetPeriphId || '');
            } else {
                this._populatePeriphSelect(div.querySelector('.pe-target-periph'), data.targetPeriphId || '', isPollMode);
            }
            if (!isPollMode && isModbusPoll && !isModbusTarget) this._populateModbusDevicePanel(div.querySelector('.pe-poll-tasks-list'), data.actionValue || '');
            if (isModbusTarget) this._showModbusCtrlPanel(div.querySelector('.pe-modbus-ctrl-panel'), data.targetPeriphId, data.actionValue || '');
        },

        addPeriphExecTrigger() {
            const container = document.getElementById('periph-exec-triggers');
            if (!container) return;
            this._createPeriphExecTriggerElement({}, container.children.length);
            this._refreshPeriphExecRiskNotice({ allowFetch: true });
        },

        deletePeriphExecTrigger(index) {
            const container = document.getElementById('periph-exec-triggers');
            if (!container) return;
            const items = container.querySelectorAll('.periph-exec-config-item');
            if (items.length <= 1) return;
            if (items[index]) items[index].remove();
            this._reindexPeriphExecBlocks(container);
            this._refreshPeriphExecRiskNotice({ allowFetch: false });
        },

        addPeriphExecAction() {
            const container = document.getElementById('periph-exec-actions');
            if (!container) return;
            this._createPeriphExecActionElement({}, container.children.length);
            this._refreshPeriphExecRiskNotice({ allowFetch: true });
        },

        deletePeriphExecAction(index) {
            const container = document.getElementById('periph-exec-actions');
            if (!container) return;
            const items = container.querySelectorAll('.periph-exec-config-item');
            if (items.length <= 1) return;
            if (items[index]) items[index].remove();
            this._reindexPeriphExecBlocks(container);
            this._refreshPeriphExecRiskNotice({ allowFetch: false });
        },

        _reindexPeriphExecBlocks(container) {
            const items = container.querySelectorAll('.periph-exec-config-item');
            items.forEach((item, idx) => {
                item.dataset.index = idx;
                const indexSpan = item.querySelector('.mqtt-topic-index');
                if (indexSpan) indexSpan.textContent = idx + 1;
            });
        }
    });

    // Part 3: Event handlers, data collection, CRUD, page loading
    Object.assign(AppState, {

        onPeriphExecTriggerTypeChangeInBlock(val, index) {
            const block = this._getPeriphExecBlock('periph-exec-triggers', index);
            if (!block) return;
            const triggerPeriphGroup = block.querySelector('.pe-trigger-periph-group');
            const platformCondition = block.querySelector('.pe-platform-condition');
            const pollParams = block.querySelector('.pe-poll-params');
            const timerConfig = block.querySelector('.pe-timer-config');
            const eventGroup = block.querySelector('.pe-event-group');
            this._setSectionVisible(triggerPeriphGroup, val === '0');
            this._setSectionVisible(platformCondition, val === '0');
            this._setSectionVisible(pollParams, val === '5');
            this._setSectionVisible(timerConfig, val === '1');
            if (eventGroup) {
                this._setSectionVisible(eventGroup, val === '4');
                if (val === '4') {
                    this._populateEventCategoriesInBlock(block);
                }
            }
            if (val !== '0') {
                this._checkAndSyncSetMode();
            }
            // 触发类型变更时重建执行动作区域(轮询触发模式UI完全不同)
            this._rebuildActionBlocksForTriggerChange();
        },

        onPeriphExecOperatorChangeInBlock(val, index) {
            const block = this._getPeriphExecBlock('periph-exec-triggers', index);
            if (!block) return;
            const compareGroup = block.querySelector('.pe-compare-value-group');
            this._setSectionVisible(compareGroup, val !== '1');
            this._checkAndSyncSetMode();
        },

        onPeriphExecUseRecvChangeInBlock(checkbox, index) {
            const block = checkbox.closest('.periph-exec-config-item');
            if (!block) return;
            const valueInput = block.querySelector('.pe-action-value');
            if (valueInput) {
                if (checkbox.checked) {
                    valueInput.setAttribute('readonly', '');
                } else {
                    valueInput.removeAttribute('readonly');
                }
            }
        },

        _checkAndSyncSetMode() {
            const hasSetMode = this._hasSetModeTrigger();
            this._syncSetModeToActions(hasSetMode);
        },

        _hasSetModeTrigger() {
            const container = document.getElementById('periph-exec-triggers');
            if (!container) return false;
            const items = container.querySelectorAll('.periph-exec-config-item');
            for (let i = 0; i < items.length; i++) {
                const triggerType = items[i].querySelector('.pe-trigger-type');
                const operator = items[i].querySelector('.pe-operator');
                if (triggerType && triggerType.value === '0' && operator && operator.value === '1') {
                    return true;
                }
            }
            return false;
        },

        _syncSetModeToActions(isSetMode) {
            const container = document.getElementById('periph-exec-actions');
            if (!container) return;
            container.querySelectorAll('.periph-exec-config-item').forEach(function (item) {
                const checkbox = item.querySelector('.pe-use-received-value');
                const valueInput = item.querySelector('.pe-action-value');
                const recvGroup = item.querySelector('.pe-use-received-value-group');
                const actionType = parseInt(item.querySelector('.pe-action-type')?.value || '0');
                const needsValue = (actionType >= 2 && actionType <= 5);
                const showRecv = isSetMode && needsValue;
                if (recvGroup) {
                    if (showRecv) recvGroup.classList.remove('is-hidden');
                    else recvGroup.classList.add('is-hidden');
                }
                if (checkbox) checkbox.checked = showRecv;
                if (valueInput) {
                    if (showRecv) valueInput.setAttribute('readonly', '');
                    else valueInput.removeAttribute('readonly');
                }
            });
        },

        onPeriphExecTimerModeChangeInBlock(val, index) {
            const block = this._getPeriphExecBlock('periph-exec-triggers', index);
            if (!block) return;
            const intervalFields = block.querySelector('.pe-interval-fields');
            const dailyFields = block.querySelector('.pe-daily-fields');
            this._setSectionVisible(intervalFields, val === '0');
            this._setSectionVisible(dailyFields, val === '1');
        },

        onPeriphExecEventCategoryChangeInBlock(val, index) {
            const block = this._getPeriphExecBlock('periph-exec-triggers', index);
            if (!block) return;
            this._populateEventSelectInBlock(block, val);
        },

        onPeriphExecEventChangeInBlock(val, index) {
            const block = this._getPeriphExecBlock('periph-exec-triggers', index);
            if (!block) return;
            var isDataSource = val && val.indexOf('ds:') === 0;
            var isModbusCtrl = val && val.indexOf('mc:') === 0;
            this._setSectionVisible(block.querySelector('.pe-event-condition-group'), isDataSource);
            this._setSectionVisible(block.querySelector('.pe-event-compare-group'), isDataSource);
            var ctrlPanel = block.querySelector('.pe-event-modbus-ctrl-panel');
            if (ctrlPanel) {
                if (isModbusCtrl) {
                    var devIdx = parseInt(val.substring(3));
                    this._showModbusTriggerCtrlPanel(ctrlPanel, devIdx, '');
                }
                this._setSectionVisible(ctrlPanel, isModbusCtrl);
            }
        },

        onPeriphExecActionTypeChangeInBlock(val, index) {
            const block = this._getPeriphExecBlock('periph-exec-actions', index);
            if (!block) return;
            // 轮询触发模式不处理动作类型变更
            if (this._isPollTriggerActive()) return;
            const actionType = parseInt(val);
            const targetGroup = block.querySelector('.pe-target-group');
            const valueGroup = block.querySelector('.pe-action-value-group');
            const scriptGroup = block.querySelector('.pe-script-group');
            const pollTasksGroup = block.querySelector('.pe-poll-tasks-group');
            const isModbusPoll = actionType === 18;
            const isSensorRead = actionType === 19;
            const showTargetGroup = !((actionType >= 6 && actionType <= 11) || actionType === 15 || isModbusPoll);
            this._setSectionVisible(targetGroup, showTargetGroup);
            const needsValue = (actionType >= 2 && actionType <= 5);
            this._setSectionVisible(valueGroup, needsValue);
            this._setSectionVisible(scriptGroup, actionType === 15);
            if (pollTasksGroup) {
                this._setSectionVisible(pollTasksGroup, isModbusPoll);
                if (isModbusPoll) {
                    this._populateModbusDevicePanel(block.querySelector('.pe-poll-tasks-list'), '');
                }
            }
            const sensorGroup = block.querySelector('.pe-sensor-group');
            this._setSectionVisible(sensorGroup, isSensorRead);
            // 切换动作类型时，隐藏 Modbus 控制面板（ctrlPanel 仅在选择 modbus:xxx 目标时由 _onTargetPeriphChange 管理）
            const ctrlPanel = block.querySelector('.pe-modbus-ctrl-panel');
            this._setSectionVisible(ctrlPanel, false);
            if (isSensorRead) {
                var cat = block.querySelector('.pe-sensor-category')?.value || 'analog';
            }
            if (targetGroup && !targetGroup.classList.contains('is-hidden')) {
                var peSelect = block.querySelector('.pe-target-periph');
                this._populatePeriphSelect(peSelect, peSelect ? peSelect.value : '', this._isPollTriggerActive());
            }
            // 使用接收值: 仅在平台触发+设置+需要数值的动作类型时显示
            const recvGroup = block.querySelector('.pe-use-received-value-group');
            const isSetMode = this._hasSetModeTrigger();
            const showRecv = isSetMode && needsValue;
            this._setSectionVisible(recvGroup, showRecv);
            const checkbox = block.querySelector('.pe-use-received-value');
            if (checkbox) checkbox.checked = showRecv;
            const valueInput = block.querySelector('.pe-action-value');
            if (valueInput) {
                if (showRecv) valueInput.setAttribute('readonly', '');
                else valueInput.removeAttribute('readonly');
            }
        },

        _populateEventCategoriesInBlock(blockEl, eventIdToSet, compareValue) {
            const sel = blockEl.querySelector('.pe-event-category');
            if (!sel) return;
            var isDs = eventIdToSet && String(eventIdToSet).indexOf('ds:') === 0;
            var isMc = eventIdToSet && String(eventIdToSet).indexOf('mc:') === 0;
            apiGet('/api/periph-exec/events/categories').then(res => {
                if (!res || !res.success || !res.data) return;
                let opts = '<option value="">' + i18n.t('periph-exec-select-category') + '</option>';
                res.data.forEach(cat => {
                    const translatedCat = i18n.t('event-cat-' + cat.name) || cat.name;
                    opts += '<option value="' + cat.name + '">' + translatedCat + '</option>';
                });
                sel.innerHTML = opts;
                if (isDs || isMc) {
                    sel.value = 'Modbus子设备';
                    this._populateEventSelectInBlock(blockEl, 'Modbus子设备', eventIdToSet, compareValue);
                } else {
                    this._populateEventSelectInBlock(blockEl, '', eventIdToSet, compareValue);
                }
            });
        },

        _populateEventSelectInBlock(blockEl, categoryFilter, eventIdToSet, compareValue) {
            const sel = blockEl.querySelector('.pe-event');
            if (!sel) return;
            // Modbus子设备类别：从协议配置中提取，不从API加载
            if (categoryFilter === 'Modbus子设备') {
                var sources = this._peDataSources || [];
                var ctrlDevices = this._modbusDevices || [];
                var opts = '<option value="">' + i18n.t('periph-exec-select-event') + '</option>';
                if (sources.length > 0) {
                    var acqCatLabel = i18n.t('event-cat-modbus-acquisition') || '采集设备';
                    opts += '<optgroup label="' + acqCatLabel + '">';
                    sources.forEach(function(ds) {
                        opts += '<option value="ds:' + ds.id + '">' + escapeHtml(ds.label) + '</option>';
                    });
                    opts += '</optgroup>';
                }
                if (ctrlDevices.length > 0) {
                    var ctrlCatLabel = i18n.t('event-cat-modbus-control') || '控制设备';
                    opts += '<optgroup label="' + ctrlCatLabel + '">';
                    ctrlDevices.forEach(function(dev, idx) {
                        if (dev.enabled === false) return;
                        var dt = dev.deviceType || 'relay';
                        if (dt === 'relay' || dt === 'pwm' || dt === 'pid' || dt === 'motor') {
                            opts += '<option value="mc:' + idx + '">' + escapeHtml(dev.name || dt) + '</option>';
                        }
                    });
                    opts += '</optgroup>';
                }
                sel.innerHTML = opts;
                if (eventIdToSet) sel.value = eventIdToSet;
                var isDs = sel.value && sel.value.indexOf('ds:') === 0;
                var isMc = sel.value && sel.value.indexOf('mc:') === 0;
                this._setSectionVisible(blockEl.querySelector('.pe-event-condition-group'), isDs);
                this._setSectionVisible(blockEl.querySelector('.pe-event-compare-group'), isDs);
                var ctrlPanel = blockEl.querySelector('.pe-event-modbus-ctrl-panel');
                if (ctrlPanel) {
                    if (isMc) {
                        var devIdx = parseInt(sel.value.substring(3));
                        this._showModbusTriggerCtrlPanel(ctrlPanel, devIdx, compareValue || '');
                    }
                    this._setSectionVisible(ctrlPanel, isMc);
                }
                return;
            }
            // 顺序加载：先静态事件，再动态事件（避免 ESP32 并发连接限制）
            apiGet('/api/periph-exec/events/static').then(staticRes => {
                return apiGet('/api/periph-exec/events/dynamic').then(dynamicRes => {
                    let allEvents = [];
                    if (staticRes && staticRes.success && staticRes.data) allEvents = allEvents.concat(staticRes.data);
                    if (dynamicRes && dynamicRes.success && dynamicRes.data) allEvents = allEvents.concat(dynamicRes.data);
                    if (categoryFilter) allEvents = allEvents.filter(e => e.category === categoryFilter);
                    const categories = {};
                    allEvents.forEach(e => {
                        if (!categories[e.category]) categories[e.category] = [];
                        categories[e.category].push(e);
                    });
                    let opts = '<option value="">' + i18n.t('periph-exec-select-event') + '</option>';
                    for (const cat in categories) {
                        const translatedCat = i18n.t('event-cat-' + cat) || cat;
                        opts += '<optgroup label="' + translatedCat + '">';
                        categories[cat].forEach(e => {
                            const translatedName = e.isDynamic ? e.name : (i18n.t('event-' + e.id) || e.name);
                            opts += '<option value="' + e.id + '">' + translatedName + '</option>';
                        });
                        opts += '</optgroup>';
                    }
                    // 未选分类时，追加Modbus子设备选项
                    if (!categoryFilter) {
                        var sources = this._peDataSources || [];
                        var ctrlDevices = this._modbusDevices || [];
                        if (sources.length > 0 || ctrlDevices.length > 0) {
                            var dsCatLabel = i18n.t('event-cat-Modbus子设备') || 'Modbus子设备';
                            opts += '<optgroup label="' + dsCatLabel + '">';
                            sources.forEach(function(ds) {
                                opts += '<option value="ds:' + ds.id + '">' + escapeHtml(ds.label) + '</option>';
                            });
                            ctrlDevices.forEach(function(dev, idx) {
                                if (dev.enabled === false) return;
                                var dt = dev.deviceType || 'relay';
                                if (dt === 'relay' || dt === 'pwm' || dt === 'pid' || dt === 'motor') {
                                    opts += '<option value="mc:' + idx + '">' + escapeHtml(dev.name || dt) + '</option>';
                                }
                            });
                            opts += '</optgroup>';
                        }
                    }
                    sel.innerHTML = opts;
                    if (eventIdToSet) sel.value = eventIdToSet;
                    var isDs = sel.value && sel.value.indexOf('ds:') === 0;
                    var isMc = sel.value && sel.value.indexOf('mc:') === 0;
                    this._setSectionVisible(blockEl.querySelector('.pe-event-condition-group'), isDs);
                    this._setSectionVisible(blockEl.querySelector('.pe-event-compare-group'), isDs);
                    var ctrlPanel = blockEl.querySelector('.pe-event-modbus-ctrl-panel');
                    if (ctrlPanel) {
                        if (isMc) {
                            var devIdx = parseInt(sel.value.substring(3));
                            this._showModbusTriggerCtrlPanel(ctrlPanel, devIdx, compareValue || '');
                        }
                        this._setSectionVisible(ctrlPanel, isMc);
                    }
                });
            });
        },

        _collectPeriphExecTriggers() {
            const container = document.getElementById('periph-exec-triggers');
            if (!container) return [];
            const triggers = [];
            container.querySelectorAll('.periph-exec-config-item').forEach(item => {
                const triggerType = item.querySelector('.pe-trigger-type')?.value || '0';
                const trigger = { triggerType: parseInt(triggerType, 10) || 0 };
                if (triggerType === '0') {
                    trigger.triggerPeriphId = item.querySelector('.pe-trigger-periph')?.value || '';
                    trigger.operatorType = parseInt(item.querySelector('.pe-operator')?.value || '0', 10);
                    trigger.compareValue = item.querySelector('.pe-compare-value')?.value?.trim() || '';
                } else if (triggerType === '1') {
                    trigger.timerMode = parseInt(item.querySelector('.pe-timer-mode')?.value || '0', 10);
                    trigger.intervalSec = parseInt(item.querySelector('.pe-interval')?.value || '60', 10);
                    trigger.timePoint = item.querySelector('.pe-timepoint')?.value || '08:00';
                } else if (triggerType === '4') {
                    trigger.eventId = item.querySelector('.pe-event')?.value || '';
                    if (trigger.eventId.indexOf('ds:') === 0) {
                        trigger.operatorType = parseInt(item.querySelector('.pe-event-operator')?.value || '0', 10);
                        trigger.compareValue = item.querySelector('.pe-event-compare')?.value?.trim() || '';
                    } else if (trigger.eventId.indexOf('mc:') === 0) {
                        // Modbus控制设备触发器：从控制面板读取通道/动作配置
                        var ctrlPanel = item.querySelector('.pe-event-modbus-ctrl-panel');
                        if (ctrlPanel) {
                            var devIdx = parseInt(trigger.eventId.substring(3));
                            var ctrl = {d: devIdx};
                            var chSel = ctrlPanel.querySelector('.pe-modbus-channel-select');
                            if (chSel) ctrl.c = parseInt(chSel.value || '0');
                            var actSel = ctrlPanel.querySelector('.pe-modbus-action-select');
                            if (actSel) {
                                ctrl.a = actSel.value || 'on';
                            } else {
                                var paramSel = ctrlPanel.querySelector('.pe-modbus-action-param');
                                if (paramSel) {
                                    ctrl.a = 'pid';
                                    ctrl.p = paramSel.value || 'P';
                                } else {
                                    ctrl.a = 'pwm';
                                }
                            }
                            var valInput = ctrlPanel.querySelector('.pe-modbus-action-value');
                            if (valInput) ctrl.v = parseInt(valInput.value || '0');
                            trigger.compareValue = JSON.stringify({ctrl: [ctrl]});
                        }
                    }
                } else if (triggerType === '5') {
                    trigger.intervalSec = parseInt(item.querySelector('.pe-poll-interval')?.value || '60', 10);
                    trigger.pollResponseTimeout = parseInt(item.querySelector('.pe-poll-timeout')?.value || '1000', 10);
                    trigger.pollMaxRetries = parseInt(item.querySelector('.pe-poll-retries')?.value || '2', 10);
                    trigger.pollInterPollDelay = parseInt(item.querySelector('.pe-poll-inter-delay')?.value || '100', 10);
                }
                triggers.push(trigger);
            });
            return triggers;
        },

        _collectPeriphExecActions() {
            const container = document.getElementById('periph-exec-actions');
            if (!container) return [];
            const actions = [];
            const isPollMode = this._isPollTriggerActive();
            container.querySelectorAll('.periph-exec-config-item').forEach(item => {
                // 轮询触发模式: 从目标外设下拉读取 modbus-task:N，自动生成 ACTION_MODBUS_POLL 动作
                if (isPollMode) {
                    var periphVal = item.querySelector('.pe-target-periph')?.value || '';
                    var jsonObj = {};
                    if (periphVal.indexOf('modbus-task:') === 0) {
                        var taskIdx = parseInt(periphVal.substring(12));
                        if (!isNaN(taskIdx)) jsonObj.poll = [taskIdx];
                    }
                    actions.push({
                        targetPeriphId: periphVal,
                        actionType: 18,
                        actionValue: JSON.stringify(jsonObj),
                        useReceivedValue: false,
                        syncDelayMs: parseInt(item.querySelector('.pe-sync-delay')?.value || '0'),
                        execMode: parseInt(item.querySelector('.pe-exec-mode')?.value || '0')
                    });
                    return;
                }
                const targetPeriphId = item.querySelector('.pe-target-periph')?.value || '';
                const isModbusTarget = targetPeriphId && targetPeriphId.indexOf('modbus:') === 0;
                const actionType = item.querySelector('.pe-action-type')?.value || '0';
                const action = {
                    targetPeriphId: targetPeriphId,
                    actionType: parseInt(actionType, 10) || 0
                };
                if (isModbusTarget) {
                    var dIdx = parseInt(targetPeriphId.substring(7));
                    var ctrl = {d: dIdx};
                    var chSel = item.querySelector('.pe-modbus-channel-select');
                    if (chSel) ctrl.c = parseInt(chSel.value || '0');
                    var actSel = item.querySelector('.pe-modbus-action-select');
                    if (actSel) {
                        ctrl.a = actSel.value || 'on';
                    } else {
                        var paramSel = item.querySelector('.pe-modbus-action-param');
                        if (paramSel) {
                            ctrl.a = 'pid';
                            ctrl.p = paramSel.value || 'P';
                        } else {
                            ctrl.a = 'pwm';
                        }
                    }
                    var valInput = item.querySelector('.pe-modbus-action-value');
                    if (valInput) ctrl.v = parseInt(valInput.value || '0');
                    action.actionType = 18;
                    action.actionValue = JSON.stringify({ctrl: [ctrl]});
                } else if (actionType === '15') {
                    action.actionValue = item.querySelector('.pe-script')?.value || '';
                } else if (actionType === '18') {
                    // Modbus轮询采集：仅支持采集任务，控制设备通过 modbus:xxx 目标外设 + ctrlPanel 配置
                    var devSel = item.querySelector('.pe-modbus-device-select');
                    var devVal = devSel ? devSel.value : '';
                    var jsonObj = {};
                    if (devVal.indexOf('sensor-') === 0) {
                        jsonObj.poll = [parseInt(devVal.split('-')[1])];
                    }
                    action.actionValue = JSON.stringify(jsonObj);
                } else if (actionType === '19') {
                    var sensorCat = item.querySelector('.pe-sensor-category')?.value || 'analog';
                    var sensorPeriphId = item.querySelector('.pe-target-periph')?.value || '';
                    var scaleFactor = parseFloat(item.querySelector('.pe-sensor-scale')?.value);
                    if (isNaN(scaleFactor)) scaleFactor = 1;
                    var sOffset = parseFloat(item.querySelector('.pe-sensor-offset')?.value);
                    if (isNaN(sOffset)) sOffset = 0;
                    var decimals = parseInt(item.querySelector('.pe-sensor-decimals')?.value);
                    if (isNaN(decimals)) decimals = 2;
                    var sensorLabel = item.querySelector('.pe-sensor-label')?.value?.trim() || '';
                    var sUnit = item.querySelector('.pe-sensor-unit')?.value?.trim() || '';
                    action.actionValue = JSON.stringify({
                        periphId: sensorPeriphId,
                        sensorCategory: sensorCat,
                        scaleFactor: scaleFactor,
                        offset: sOffset,
                        decimalPlaces: decimals,
                        sensorLabel: sensorLabel,
                        unit: sUnit
                    });
                } else {
                    action.actionValue = item.querySelector('.pe-action-value')?.value?.trim() || '';
                }
                action.useReceivedValue = item.querySelector('.pe-use-received-value')?.checked || false;
                action.syncDelayMs = parseInt(item.querySelector('.pe-sync-delay')?.value || '0', 10) || 0;
                action.execMode = parseInt(item.querySelector('.pe-exec-mode')?.value || '0', 10) || 0;
                actions.push(action);
            });
            return actions;
        }
    });

    // Part 4: CRUD operations and page loading
    Object.assign(AppState, {

        savePeriphExecRule() {
            const errEl = document.getElementById('periph-exec-error');
            this.clearInlineError(errEl);
            const originalId = document.getElementById('periph-exec-original-id').value;
            const isEdit = originalId !== '' && originalId !== 'null' && originalId !== 'undefined';

            const ruleData = {
                name: document.getElementById('periph-exec-name').value.trim(),
                enabled: document.getElementById('periph-exec-enabled').checked,
                reportAfterExec: document.getElementById('periph-exec-report').checked
            };

            if (!ruleData.name) {
                this.showInlineError(errEl, i18n.t('periph-exec-validate-name'));
                return;
            }

            const triggers = this._collectPeriphExecTriggers();
            ruleData.triggers = triggers;

            const actions = this._collectPeriphExecActions();
            ruleData.actions = actions;
            for (let i = 0; i < actions.length; i++) {
                if (actions[i].actionType === 15) {
                    if (!actions[i].actionValue.trim()) {
                        this.showInlineError(errEl, i18n.t('periph-exec-script-empty'));
                        return;
                    }
                    if (actions[i].actionValue.length > 1024) {
                        this.showInlineError(errEl, i18n.t('periph-exec-script-too-long'));
                        return;
                    }
                }
                if (actions[i].actionType === 19) {
                    var sv = {};
                    try { sv = JSON.parse(actions[i].actionValue); } catch(e) {}
                    if (!sv.periphId) {
                        this.showInlineError(errEl, i18n.t('periph-exec-sensor-no-periph'));
                        return;
                    }
                }
            }

            if (isEdit) ruleData.id = originalId;
            const url = isEdit ? '/api/periph-exec/update' : '/api/periph-exec';
            apiPostJson(url, ruleData)
                .then(res => {
                    if (res && res.success) {
                        Notification.success(i18n.t(isEdit ? 'periph-exec-update-ok' : 'periph-exec-add-ok'), i18n.t('periph-exec-title'));
                        this.closePeriphExecModal();
                        if (this.currentPage === 'periph-exec') this.loadPeriphExecPage();
                    } else {
                        this.showInlineError(errEl, res?.error || i18n.t('periph-exec-save-fail'));
                    }
                })
                .catch(err => {
                    console.error('Save periph exec rule failed:', err);
                    const isNetworkError = err && (
                        err.name === 'TypeError' ||
                        (err.message && (
                            err.message.includes('Failed to fetch') ||
                            err.message.includes('fetch') ||
                            err.message.includes('network')
                        ))
                    );
                    this.showInlineError(errEl, isNetworkError ? i18n.t('device-offline-error') : i18n.t('periph-exec-save-fail'));
                });
        },

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
            if (btn) { btn.disabled = true; btn.innerHTML = '<span class="fb-spin">&#x21bb;</span> 加载中...'; }
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

    // 自动绑定事件
    if (typeof AppState.setupPeriphExecEvents === 'function') {
        AppState.setupPeriphExecEvents();
    }
})();
