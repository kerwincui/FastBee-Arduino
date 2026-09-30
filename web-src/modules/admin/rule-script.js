/**
 * 规则脚本模块
 * 包含规则脚本的 CRUD 操作
 */
(function() {
    AppState.registerModule('rule-script', {
        _ruleScriptEventsBound: false,

        _updateRuleScriptSummary(rules, errorText) {
            const summary = document.getElementById('rule-script-list-summary');
            if (!summary) return;
            summary.classList.toggle('is-warning', !!errorText);
            if (errorText) {
                summary.textContent = errorText;
                return;
            }
            if (!Array.isArray(rules)) {
                summary.textContent = '加载中...';
                return;
            }
            const enabled = rules.filter(rule => rule && rule.enabled === true).length;
            summary.textContent = '共 ' + rules.length + ' 条规则 · ' + enabled + ' 条已启用';
        },

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
            if (!this.guardDeveloperModeAction()) return;
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
            var locked = !this.isDeveloperModeEnabled();
            var attrs = 'data-rule-script-action="' + action + '" data-id="' + escapeHtml(id) + '"';
            if (enabledValue !== undefined) attrs += ' data-enabled="' + (enabledValue ? 'true' : 'false') + '"';
            if (locked) attrs += ' disabled title="' + escapeHtml(this.getDeveloperModeDisabledText()) + '"';
            var actionClass = action === 'edit' ? ' fb-btn-action-edit' : '';
            var lockClass = locked ? ' dev-mode-locked' : '';
            return '<button class="fb-btn fb-btn-sm ' + className + actionClass + lockClass + '" ' + attrs + '>' + label + '</button>';
        },

        _renderRuleScriptActions(rule) {
            let html = '';
            html += '<div class="rule-script-actions" style="display:inline-flex;gap:6px;flex-wrap:nowrap;">';
            html += this._renderRuleScriptActionButton('edit', rule.id, '编辑', 'fb-btn-primary');
            html += rule.enabled
                ? this._renderRuleScriptActionButton('toggle', rule.id, '禁用', 'fb-btn-warning', false)
                : this._renderRuleScriptActionButton('toggle', rule.id, '启用', 'fb-btn-success', true);
            html += this._renderRuleScriptActionButton('delete', rule.id, '删除', 'fb-btn-danger');
            html += '</div>';
            return html;
        },

        _renderRuleScriptRow(rule, triggerLabels, protocolLabels) {
            const triggerText = triggerLabels[rule.triggerType] || '?';
            const protocolText = protocolLabels[rule.protocolType] || '-';
            const statsText = '触发次数：' + (rule.triggerCount || 0);
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
            html += '<td class="rule-script-stats-cell" style="white-space:nowrap;">' + escapeHtml(statsText) + '</td>';
            html += '<td class="u-toolbar-sm" style="white-space:nowrap;min-width:180px;">';
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
                // 开发模式禁用时禁用新增按钮
                if (!this.isDeveloperModeEnabled()) {
                    addBtn.disabled = true;
                    addBtn.title = this.getDeveloperModeDisabledText();
                    addBtn.classList.add('dev-mode-locked');
                } else {
                    addBtn.disabled = false;
                    addBtn.removeAttribute('title');
                    addBtn.classList.remove('dev-mode-locked');
                }
            }
            // Show/hide developer mode hint banner
            this.applyDeveloperModeState();
            var devHint = document.getElementById('rule-script-dev-mode-hint');
            if (devHint) devHint.style.display = this.isDeveloperModeEnabled() ? 'none' : 'block';

            const tbody = document.getElementById('rule-script-table-body');
            if (!tbody) return;
            this._updateRuleScriptSummary(null);
            var getter = (options && options.noCache === true && typeof apiGetFresh === 'function') ? apiGetFresh : apiGet;
            getter('/api/rule-script').then(res => {
                if (!res || !res.success || !Array.isArray(res.data)) {
                    this._updateRuleScriptSummary(null, '规则列表加载失败');
                    this.renderEmptyTableRow(tbody, 7, '暂无规则脚本');
                    return;
                }
                const rules = res.data;
                this._updateRuleScriptSummary(rules);
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
                this.applyDeveloperModeState(tbody);
            }).catch((err) => {
                if (typeof window.isPageAbortError === 'function' && window.isPageAbortError(err)) return;
                this._updateRuleScriptSummary(null, '规则列表加载失败');
                this.renderEmptyTableRow(tbody, 7, '暂无规则脚本');
            });
        },

        async openRuleScriptModal(editId) {
            if (!this.guardDeveloperModeAction()) return;
            const seq = this._rsEditorSeq = (this._rsEditorSeq || 0) + 1;
            this._rsEditorReady = false;
            const modal = await this._ensureModalForAction('rule-script-modal');
            if (!modal || !this._ruleScriptActive(seq)) return;
            this._rsEditorReady = !editId;
            const saveButton = document.getElementById('save-rule-script-btn');
            if (saveButton) saveButton.disabled = !!editId || !!this._rsSavePending;
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
            var protoSelect = document.getElementById('rule-script-protocol-type');
            if (protoSelect && !protoSelect._ruleScriptBound) {
                if (protoSelect) {
                    protoSelect.addEventListener('change', function() {
                        var topicFields = document.getElementById('rule-script-topic-fields');
                        if (topicFields) topicFields.style.display = (this.value === '0') ? '' : 'none';
                    });
                    protoSelect._ruleScriptBound = true;
                }
            }
            // 初始状态：根据当前协议类型显示/隐藏主题字段
            var protoVal = document.getElementById('rule-script-protocol-type').value;
            var topicFields = document.getElementById('rule-script-topic-fields');
            if (topicFields) topicFields.style.display = (protoVal === '0') ? '' : 'none';
            this.showModal(modal);
            return seq;
        },

        _ruleScriptActive(seq) {
            return this._rsEditorSeq === seq && (!this.currentPage || this.currentPage === 'rule-script');
        },

        closeRuleScriptModal() {
            this._rsEditorSeq = (this._rsEditorSeq || 0) + 1;
            this._rsEditorReady = false;
            this.hideModal('rule-script-modal');
        },

        saveRuleScript() {
            if (!this.guardDeveloperModeAction() || this._rsSavePending || !this._rsEditorReady) return;
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
            const seq = this._rsEditorSeq;
            const button = document.getElementById('save-rule-script-btn');
            this._rsSavePending = true;
            if (button) button.disabled = true;
            return apiPost(url, ruleData).then(res => {
                if (res && res.success && typeof window.apiInvalidateCache === 'function') window.apiInvalidateCache('/api/rule-script');
                if (!this._ruleScriptActive(seq)) return;
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
                if (this._ruleScriptActive(seq)) this.showInlineError('rule-script-error', '保存失败');
            }).finally(() => {
                this._rsSavePending = false;
                const currentButton = document.getElementById('save-rule-script-btn');
                if (currentButton) currentButton.disabled = !this._rsEditorReady;
            });
        },

        async editRuleScript(id) {
            if (!this.guardDeveloperModeAction()) return;
            const seq = await this.openRuleScriptModal(id);
            if (!seq || !this._ruleScriptActive(seq)) return;
            const getter = (typeof apiGetFresh === 'function') ? apiGetFresh : apiGet;
            return getter('/api/rule-script').then(res => {
                if (!this._ruleScriptActive(seq)) return;
                if (!res || !res.success || !Array.isArray(res.data)) throw new Error('规则加载失败');
                const rule = res.data.find(r => r.id === id);
                if (!rule) throw new Error('规则已不存在，请刷新列表');
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
                this._rsEditorReady = true;
                document.getElementById('save-rule-script-btn').disabled = !!this._rsSavePending;
            }).catch(err => {
                if (this._ruleScriptActive(seq)) this.showInlineError('rule-script-error', err.message || '规则加载失败，请关闭后重试');
            });
        },

        _runRuleScriptAction(id, action) {
            if (!this._rsActions) this._rsActions = new Set();
            if (this._rsActions.has(id)) return Promise.resolve(null);
            this._rsActions.add(id);
            return Promise.resolve().then(action).then(res => {
                if (!res || !res.success) throw new Error(res && res.error || '操作失败，请重试');
                return res;
            }).catch(err => {
                Notification.error(err.message || '操作失败，请重试', '规则脚本');
                return null;
            }).finally(() => this._rsActions.delete(id));
        },

        toggleRuleScript(id, enable) {
            if (!this.guardDeveloperModeAction()) return;
            const url = enable ? '/api/rule-script/enable' : '/api/rule-script/disable';
            return this._runRuleScriptAction(id, () => apiPost(url, { id: id })).then(res => {
                if (res && res.success) {
                    if (typeof window.apiInvalidateCache === 'function') {
                        window.apiInvalidateCache('/api/rule-script');
                    }
                    if (this.currentPage === 'rule-script') this.loadRuleScriptPage({ noCache: true });
                }
            });
        },

        deleteRuleScript(id) {
            if (!this.guardDeveloperModeAction()) return;
            if (!confirm('确定要删除此规则吗？')) return;
            return this._runRuleScriptAction(id, () => apiDelete('/api/rule-script/', { id: id })).then(res => {
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
