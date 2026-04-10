/**
 * 规则脚本模块
 * 包含规则脚本的 CRUD 操作
 */
(function() {
    AppState.registerModule('rule-script', {

        // ============ 事件绑定 ============
        setupRuleScriptEvents() {
            const closeRuleScriptModal = document.getElementById('close-rule-script-modal');
            if (closeRuleScriptModal) closeRuleScriptModal.addEventListener('click', () => this.closeRuleScriptModal());
            const cancelRuleScriptBtn = document.getElementById('cancel-rule-script-btn');
            if (cancelRuleScriptBtn) cancelRuleScriptBtn.addEventListener('click', () => this.closeRuleScriptModal());
            const saveRuleScriptBtn = document.getElementById('save-rule-script-btn');
            if (saveRuleScriptBtn) saveRuleScriptBtn.addEventListener('click', () => this.saveRuleScript());
        },

        // ============ 规则脚本页面 ============

        loadRuleScriptPage() {
            const tbody = document.getElementById('rule-script-table-body');
            if (!tbody) return;
            apiGet('/api/rule-script').then(res => {
                if (!res || !res.success || !res.data) {
                    tbody.innerHTML = '<tr><td colspan="6" style="text-align:center;color:#999;">' + i18n.t('rule-script-no-data') + '</td></tr>';
                    return;
                }
                const rules = res.data;
                if (rules.length === 0) {
                    tbody.innerHTML = '<tr><td colspan="6" style="text-align:center;color:#999;">' + i18n.t('rule-script-no-data') + '</td></tr>';
                    return;
                }
                const triggerLabels = { 0: i18n.t('rule-script-trigger-receive'), 1: i18n.t('rule-script-trigger-report') };
                const protocolLabels = { 0: 'MQTT', 1: 'Modbus RTU', 2: 'Modbus TCP', 3: 'HTTP', 4: 'CoAP', 5: 'TCP' };
                let html = '';
                rules.forEach(r => {
                    const statusBadge = r.enabled
                        ? '<span class="badge badge-success">' + i18n.t('periph-exec-status-on') + '</span>'
                        : '<span class="badge badge-info">' + i18n.t('periph-exec-status-off') + '</span>';
                    const triggerText = triggerLabels[r.triggerType] || '?';
                    const protocolText = protocolLabels[r.protocolType] || '-';
                    const statsText = i18n.t('periph-exec-stats-count') + ': ' + (r.triggerCount || 0);
                    html += '<tr>';
                    html += '<td>' + (r.name || '') + '</td>';
                    html += '<td>' + statusBadge + '</td>';
                    html += '<td>' + triggerText + '</td>';
                    html += '<td>' + protocolText + '</td>';
                    html += '<td style="font-size:12px;">' + statsText + '</td>';
                    html += '<td class="u-toolbar-sm">';
                    html += '<button class="btn btn-sm btn-edit" onclick="app.editRuleScript(\'' + r.id + '\')">' + i18n.t('peripheral-edit') + '</button>';
                    if (r.enabled) {
                        html += '<button class="btn btn-sm btn-disable" onclick="app.toggleRuleScript(\'' + r.id + '\',false)">' + i18n.t('peripheral-disable') + '</button>';
                    } else {
                        html += '<button class="btn btn-sm btn-enable" onclick="app.toggleRuleScript(\'' + r.id + '\',true)">' + i18n.t('peripheral-enable') + '</button>';
                    }
                    html += '<button class="btn btn-sm btn-delete" onclick="app.deleteRuleScript(\'' + r.id + '\')">' + i18n.t('peripheral-delete') + '</button>';
                    html += '</td>';
                    html += '</tr>';
                });
                tbody.innerHTML = html;
            }).catch(() => {
                tbody.innerHTML = '<tr><td colspan="6" style="text-align:center;color:#999;">' + i18n.t('rule-script-no-data') + '</td></tr>';
            });
        },

        openRuleScriptModal(editId) {
            const modal = document.getElementById('rule-script-modal');
            if (!modal) return;
            const titleEl = document.getElementById('rule-script-modal-title');
            document.getElementById('rule-script-original-id').value = editId || '';
            document.getElementById('rule-script-error').style.display = 'none';
            if (editId) {
                if (titleEl) titleEl.textContent = i18n.t('rule-script-edit-title');
            } else {
                if (titleEl) titleEl.textContent = i18n.t('rule-script-add-title');
                document.getElementById('rule-script-form').reset();
                document.getElementById('rule-script-protocol-type').value = '0';
                document.getElementById('rule-script-content').value = '';
            }
            modal.style.display = 'flex';
        },

        closeRuleScriptModal() {
            const modal = document.getElementById('rule-script-modal');
            if (modal) modal.style.display = 'none';
        },

        saveRuleScript() {
            const errEl = document.getElementById('rule-script-error');
            errEl.style.display = 'none';
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
                errEl.textContent = i18n.t('periph-exec-validate-name');
                errEl.style.display = 'block';
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
                    errEl.textContent = res?.error || i18n.t('rule-script-save-fail');
                    errEl.style.display = 'block';
                }
            }).catch(() => {
                errEl.textContent = i18n.t('rule-script-save-fail');
                errEl.style.display = 'block';
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
