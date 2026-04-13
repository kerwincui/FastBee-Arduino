/**
 * 规则脚本模块
 * 包含规则脚本的 CRUD 操作
 */
(function() {
    AppState.registerModule('rule-script', {
        _ruleScriptEventsBound: false,

        // ============ 事件绑定 ============
        setupRuleScriptEvents() {
            if (this._ruleScriptEventsBound) return;
            const closeRuleScriptModal = document.getElementById('close-rule-script-modal');
            if (closeRuleScriptModal) closeRuleScriptModal.addEventListener('click', () => this.closeRuleScriptModal());
            const cancelRuleScriptBtn = document.getElementById('cancel-rule-script-btn');
            if (cancelRuleScriptBtn) cancelRuleScriptBtn.addEventListener('click', () => this.closeRuleScriptModal());
            const saveRuleScriptBtn = document.getElementById('save-rule-script-btn');
            if (saveRuleScriptBtn) saveRuleScriptBtn.addEventListener('click', () => this.saveRuleScript());
            const tableBody = document.getElementById('rule-script-table-body');
            if (tableBody) {
                tableBody.addEventListener('click', (event) => this._handleRuleScriptTableClick(event));
            }
            this._ruleScriptEventsBound = true;
        },

        _handleRuleScriptTableClick(event) {
            const button = event.target.closest('[data-rule-script-action]');
            if (!button) return;
            const action = button.getAttribute('data-rule-script-action');
            const id = button.getAttribute('data-id');
            if (!action || !id) return;
            if (action === 'edit') this.editRuleScript(id);
            else if (action === 'toggle') this.toggleRuleScript(id, button.getAttribute('data-enabled') === 'true');
            else if (action === 'delete') this.deleteRuleScript(id);
        },

        _renderRuleScriptStatusBadge(enabled) {
            return enabled
                ? '<span class="badge badge-success">' + i18n.t('periph-exec-status-on') + '</span>'
                : '<span class="badge badge-info">' + i18n.t('periph-exec-status-off') + '</span>';
        },

        _renderRuleScriptActionButton(action, id, label, className, enabledValue) {
            let attrs = 'data-rule-script-action="' + action + '" data-id="' + escapeHtml(id) + '"';
            if (enabledValue !== undefined) attrs += ' data-enabled="' + (enabledValue ? 'true' : 'false') + '"';
            return '<button class="btn btn-sm ' + className + '" ' + attrs + '>' + label + '</button>';
        },

        _renderRuleScriptRow(rule, triggerLabels, protocolLabels) {
            const triggerText = triggerLabels[rule.triggerType] || '?';
            const protocolText = protocolLabels[rule.protocolType] || '-';
            const statsText = i18n.t('periph-exec-stats-count') + ': ' + (rule.triggerCount || 0);
            let html = '<tr>';
            html += '<td>' + escapeHtml(rule.name || '') + '</td>';
            html += '<td>' + this._renderRuleScriptStatusBadge(rule.enabled) + '</td>';
            html += '<td>' + escapeHtml(triggerText) + '</td>';
            html += '<td>' + escapeHtml(protocolText) + '</td>';
            html += '<td class="rule-script-stats-cell">' + escapeHtml(statsText) + '</td>';
            html += '<td class="u-toolbar-sm">';
            html += this._renderRuleScriptActionButton('edit', rule.id, i18n.t('peripheral-edit'), 'btn-edit');
            html += rule.enabled
                ? this._renderRuleScriptActionButton('toggle', rule.id, i18n.t('peripheral-disable'), 'btn-disable', false)
                : this._renderRuleScriptActionButton('toggle', rule.id, i18n.t('peripheral-enable'), 'btn-enable', true);
            html += this._renderRuleScriptActionButton('delete', rule.id, i18n.t('peripheral-delete'), 'btn-delete');
            html += '</td></tr>';
            return html;
        },

        // ============ 规则脚本页面 ============

        loadRuleScriptPage() {
            const tbody = document.getElementById('rule-script-table-body');
            if (!tbody) return;
            apiGet('/api/rule-script').then(res => {
                if (!res || !res.success || !res.data) {
                    this.renderEmptyTableRow(tbody, 6, i18n.t('rule-script-no-data'));
                    return;
                }
                const rules = res.data;
                if (rules.length === 0) {
                    this.renderEmptyTableRow(tbody, 6, i18n.t('rule-script-no-data'));
                    return;
                }
                const triggerLabels = { 0: i18n.t('rule-script-trigger-receive'), 1: i18n.t('rule-script-trigger-report') };
                const protocolLabels = { 0: 'MQTT', 1: 'Modbus RTU', 2: 'Modbus TCP', 3: 'HTTP', 4: 'CoAP', 5: 'TCP' };
                let html = '';
                rules.forEach(r => {
                    html += this._renderRuleScriptRow(r, triggerLabels, protocolLabels);
                });
                tbody.innerHTML = html;
            }).catch(() => {
                this.renderEmptyTableRow(tbody, 6, i18n.t('rule-script-no-data'));
            });
        },

        openRuleScriptModal(editId) {
            const modal = document.getElementById('rule-script-modal');
            if (!modal) return;
            const titleEl = document.getElementById('rule-script-modal-title');
            document.getElementById('rule-script-original-id').value = editId || '';
            this.clearInlineError('rule-script-error');
            if (editId) {
                if (titleEl) titleEl.textContent = i18n.t('rule-script-edit-title');
            } else {
                if (titleEl) titleEl.textContent = i18n.t('rule-script-add-title');
                document.getElementById('rule-script-form').reset();
                document.getElementById('rule-script-protocol-type').value = '0';
                document.getElementById('rule-script-content').value = '';
            }
            this.showModal(modal);
        },

        closeRuleScriptModal() {
            this.hideModal('rule-script-modal');
        },

        saveRuleScript() {
            this.clearInlineError('rule-script-error');
            const originalId = document.getElementById('rule-script-original-id').value;
            const isEdit = originalId !== '';
            const ruleData = {
                name: document.getElementById('rule-script-name').value.trim(),
                enabled: document.getElementById('rule-script-enabled').checked ? '1' : '0',
                triggerType: document.getElementById('rule-script-trigger-type').value,
                protocolType: document.getElementById('rule-script-protocol-type').value,
                scriptContent: document.getElementById('rule-script-content').value
            };
            if (!ruleData.name) {
                this.showInlineError('rule-script-error', i18n.t('periph-exec-validate-name'));
                return;
            }
            if (isEdit) ruleData.id = originalId;
            const url = isEdit ? '/api/rule-script/update' : '/api/rule-script';
            apiPost(url, ruleData).then(res => {
                if (res && res.success) {
                    Notification.success(i18n.t(isEdit ? 'rule-script-update-ok' : 'rule-script-add-ok'), i18n.t('rule-script-title'));
                    this.closeRuleScriptModal();
                    if (this.currentPage === 'rule-script') this.loadRuleScriptPage();
                } else {
                    this.showInlineError('rule-script-error', res?.error || i18n.t('rule-script-save-fail'));
                }
            }).catch(() => {
                this.showInlineError('rule-script-error', i18n.t('rule-script-save-fail'));
            });
        },

        editRuleScript(id) {
            this.openRuleScriptModal(id);
            apiGet('/api/rule-script').then(res => {
                if (!res || !res.success || !res.data) return;
                const rule = res.data.find(r => r.id === id);
                if (!rule) return;
                document.getElementById('rule-script-name').value = rule.name || '';
                document.getElementById('rule-script-enabled').checked = !!rule.enabled;
                document.getElementById('rule-script-trigger-type').value = String(rule.triggerType);
                document.getElementById('rule-script-protocol-type').value = String(rule.protocolType || 0);
                document.getElementById('rule-script-content').value = rule.scriptContent || '';
            });
        },

        toggleRuleScript(id, enable) {
            const url = enable ? '/api/rule-script/enable' : '/api/rule-script/disable';
            apiPost(url, { id: id }).then(res => {
                if (res && res.success) {
                    if (this.currentPage === 'rule-script') this.loadRuleScriptPage();
                }
            });
        },

        deleteRuleScript(id) {
            if (!confirm(i18n.t('periph-exec-confirm-delete'))) return;
            apiDelete('/api/rule-script/', { id: id }).then(res => {
                if (res && res.success) {
                    Notification.success(i18n.t('rule-script-delete-ok'), i18n.t('rule-script-title'));
                    if (this.currentPage === 'rule-script') this.loadRuleScriptPage();
                }
            });
        }
    });

    // 自动绑定事件
    if (typeof AppState.setupRuleScriptEvents === 'function') {
        AppState.setupRuleScriptEvents();
    }
})();
