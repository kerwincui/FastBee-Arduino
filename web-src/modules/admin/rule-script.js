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
            const tableBody = document.getElementById('rule-script-table-body');
            if (tableBody) {
                tableBody.addEventListener('click', (event) => this._handleRuleScriptTableClick(event));
            }
            // 刷新按钮
            const rsRefreshBtn = document.getElementById('rule-script-refresh-btn');
            if (rsRefreshBtn) rsRefreshBtn.addEventListener('click', () => this._refreshRuleScriptList());
            // 注册模态窗事件绑定器
            if (typeof this._registerModalBinder === 'function') {
                this._registerModalBinder('rule-script', () => this._bindRuleScriptModalEvents());
            }
            this._ruleScriptEventsBound = true;
        },

        /**
         * 绑定模态窗内的事件（模态窗 DOM 已就绪后调用）
         */
        _bindRuleScriptModalEvents() {
            if (this._ruleScriptModalBound) return;
            const closeRuleScriptModal = document.getElementById('close-rule-script-modal');
            if (closeRuleScriptModal) closeRuleScriptModal.addEventListener('click', () => this.closeRuleScriptModal());
            const cancelRuleScriptBtn = document.getElementById('cancel-rule-script-btn');
            if (cancelRuleScriptBtn) cancelRuleScriptBtn.addEventListener('click', () => this.closeRuleScriptModal());
            const saveRuleScriptBtn = document.getElementById('save-rule-script-btn');
            if (saveRuleScriptBtn) saveRuleScriptBtn.addEventListener('click', () => this.saveRuleScript());
            this._ruleScriptModalBound = true;
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
                ? '<span class="badge badge-success">已启用</span>'
                : '<span class="badge badge-info">已禁用</span>';
        },

        _renderRuleScriptActionButton(action, id, label, className, enabledValue) {
            let attrs = 'data-rule-script-action="' + action + '" data-id="' + escapeHtml(id) + '"';
            if (enabledValue !== undefined) attrs += ' data-enabled="' + (enabledValue ? 'true' : 'false') + '"';
            const actionClass = action === 'edit' ? ' fb-btn-action-edit' : '';
            return '<button class="fb-btn fb-btn-sm ' + className + actionClass + '" ' + attrs + '>' + label + '</button>';
        },

        _renderRuleScriptActions(rule) {
            let html = '';
            html += this._renderRuleScriptActionButton('edit', rule.id, '编辑', 'fb-btn-primary');
            html += rule.enabled
                ? this._renderRuleScriptActionButton('toggle', rule.id, '禁用', 'fb-btn-warning', false)
                : this._renderRuleScriptActionButton('toggle', rule.id, '启用', 'fb-btn-success', true);
            html += this._renderRuleScriptActionButton('delete', rule.id, '删除', 'fb-btn-danger');
            return html;
        },

        _renderRuleScriptRow(rule, triggerLabels, protocolLabels) {
            const triggerText = triggerLabels[rule.triggerType] || '?';
            const protocolText = protocolLabels[rule.protocolType] || '-';
            const statsText = '触发次数: ' + (rule.triggerCount || 0);
            // 主题映射显示
            let topicText = '-';
            if (rule.protocolType === 0) { // MQTT
                const src = rule.sourceTopic || '*';
                const tgt = rule.targetTopic || '';
                topicText = tgt ? (src + ' → ' + tgt) : src;
            }
            let html = '<tr>';
            html += '<td>' + escapeHtml(rule.name || '') + '</td>';
            html += '<td>' + this._renderRuleScriptStatusBadge(rule.enabled) + '</td>';
            html += '<td>' + escapeHtml(triggerText) + '</td>';
            html += '<td>' + escapeHtml(protocolText) + '</td>';
            html += '<td>' + escapeHtml(topicText) + '</td>';
            html += '<td class="rule-script-stats-cell">' + escapeHtml(statsText) + '</td>';
            html += '<td class="u-toolbar-sm">';
            html += this._renderRuleScriptActions(rule);
            html += '</td></tr>';
            return html;
        },

        // ============ 规则脚本页面 ============

        _refreshRuleScriptList() {
            var btn = document.getElementById('rule-script-refresh-btn');
            if (btn && btn.disabled) return;
            if (btn) { btn.disabled = true; btn.textContent = '加载中...'; }
            if (typeof window.apiInvalidateCache === 'function') {
                window.apiInvalidateCache('/api/rule-script');
            }
            this.loadRuleScriptPage({ noCache: true });
            setTimeout(function() {
                if (btn) { btn.disabled = false; btn.innerHTML = '&#x21bb; 刷新'; }
            }, 2000);
        },

        loadRuleScriptPage(options) {
            // 新增规则按钮
            const addBtn = document.querySelector('[data-action="openRuleScriptModal"]');
            if (addBtn) {
                addBtn.style.display = '';
            }
            // Show/hide developer mode hint banner
            this.applyDeveloperModeState();
            var devHint = document.getElementById('rule-script-dev-mode-hint');
            if (devHint) devHint.style.display = this.isDeveloperModeEnabled() ? 'none' : 'block';

            const tbody = document.getElementById('rule-script-table-body');
            if (!tbody) return;
            var getter = (options && options.noCache === true && typeof apiGetFresh === 'function') ? apiGetFresh : apiGet;
            getter('/api/rule-script').then(res => {
                if (!res || !res.success || !res.data) {
                    this.renderEmptyTableRow(tbody, 7, '暂无规则脚本');
                    return;
                }
                const rules = res.data;
                if (rules.length === 0) {
                    this.renderEmptyTableRow(tbody, 7, '暂无规则脚本');
                    return;
                }
                const triggerLabels = { 0: '数据接收', 1: '数据上报' };
                const protocolLabels = { 0: 'MQTT', 1: 'Modbus RTU' };
                let html = '';
                rules.forEach(r => {
                    html += this._renderRuleScriptRow(r, triggerLabels, protocolLabels);
                });
                tbody.innerHTML = html;
            }).catch(() => {
                this.renderEmptyTableRow(tbody, 6, '暂无规则脚本');
            });
        },

        openRuleScriptModal(editId) {
            const modal = document.getElementById('rule-script-modal');
            if (!modal) return;
            const titleEl = document.getElementById('rule-script-modal-title');
            document.getElementById('rule-script-original-id').value = editId || '';
            this.clearInlineError('rule-script-error');
            if (editId) {
                if (titleEl) titleEl.textContent = '编辑规则脚本';
            } else {
                if (titleEl) titleEl.textContent = '新增规则脚本';
                document.getElementById('rule-script-form').reset();
                document.getElementById('rule-script-protocol-type').value = '0';
                document.getElementById('rule-script-content').value = '';
                document.getElementById('rule-script-source-topic').value = '';
                document.getElementById('rule-script-target-topic').value = '';
            }
            // 绑定协议类型切换事件（仅一次）
            if (!this._ruleScriptProtocolBound) {
                var protoSelect = document.getElementById('rule-script-protocol-type');
                if (protoSelect) {
                    protoSelect.addEventListener('change', function() {
                        var topicFields = document.getElementById('rule-script-topic-fields');
                        if (topicFields) topicFields.style.display = (this.value === '0') ? '' : 'none';
                    });
                    this._ruleScriptProtocolBound = true;
                }
            }
            // 初始状态：根据当前协议类型显示/隐藏主题字段
            var protoVal = document.getElementById('rule-script-protocol-type').value;
            var topicFields = document.getElementById('rule-script-topic-fields');
            if (topicFields) topicFields.style.display = (protoVal === '0') ? '' : 'none';
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
                scriptContent: document.getElementById('rule-script-content').value,
                sourceTopic: document.getElementById('rule-script-source-topic').value.trim(),
                targetTopic: document.getElementById('rule-script-target-topic').value.trim()
            };
            if (!ruleData.name) {
                this.showInlineError('rule-script-error', '请输入规则名称');
                return;
            }
            if (isEdit) ruleData.id = originalId;
            const url = isEdit ? '/api/rule-script/update' : '/api/rule-script';
            apiPost(url, ruleData).then(res => {
                if (res && res.success) {
                    Notification.success(isEdit ? '规则脚本更新成功' : '规则脚本添加成功', '规则脚本');
                    this.closeRuleScriptModal();
                    if (typeof window.apiInvalidateCache === 'function') {
                        window.apiInvalidateCache('/api/rule-script');
                    }
                    if (this.currentPage === 'rule-script') this.loadRuleScriptPage({ noCache: true });
                } else {
                    this.showInlineError('rule-script-error', res?.error || '保存失败');
                }
            }).catch(() => {
                this.showInlineError('rule-script-error', '保存失败');
            });
        },

        editRuleScript(id) {
            this.openRuleScriptModal(id);
            const getter = (typeof apiGetFresh === 'function') ? apiGetFresh : apiGet;
            getter('/api/rule-script').then(res => {
                if (!res || !res.success || !res.data) return;
                const rule = res.data.find(r => r.id === id);
                if (!rule) return;
                document.getElementById('rule-script-name').value = rule.name || '';
                document.getElementById('rule-script-enabled').checked = !!rule.enabled;
                document.getElementById('rule-script-trigger-type').value = String(rule.triggerType);
                document.getElementById('rule-script-protocol-type').value = String(rule.protocolType || 0);
                document.getElementById('rule-script-content').value = rule.scriptContent || '';
                document.getElementById('rule-script-source-topic').value = rule.sourceTopic || '';
                document.getElementById('rule-script-target-topic').value = rule.targetTopic || '';
                // 触发协议类型切换以更新主题字段可见性
                var protoVal = String(rule.protocolType || 0);
                var topicFields = document.getElementById('rule-script-topic-fields');
                if (topicFields) topicFields.style.display = (protoVal === '0') ? '' : 'none';
            });
        },

        toggleRuleScript(id, enable) {
            const url = enable ? '/api/rule-script/enable' : '/api/rule-script/disable';
            apiPost(url, { id: id }).then(res => {
                if (res && res.success) {
                    if (typeof window.apiInvalidateCache === 'function') {
                        window.apiInvalidateCache('/api/rule-script');
                    }
                    if (this.currentPage === 'rule-script') this.loadRuleScriptPage({ noCache: true });
                }
            });
        },

        deleteRuleScript(id) {
            if (!confirm('确定要删除此规则吗？')) return;
            apiDelete('/api/rule-script/', { id: id }).then(res => {
                if (res && res.success) {
                    Notification.success('规则脚本已删除', '规则脚本');
                    if (typeof window.apiInvalidateCache === 'function') {
                        window.apiInvalidateCache('/api/rule-script');
                    }
                    if (this.currentPage === 'rule-script') this.loadRuleScriptPage({ noCache: true });
                }
            });
        }
    });

    // 自动绑定事件
    if (typeof AppState.setupRuleScriptEvents === 'function') {
        AppState.setupRuleScriptEvents();
    }
})();
