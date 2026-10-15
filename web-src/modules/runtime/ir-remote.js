/**
 * 红外遥控管理模块（嵌入式面板）
 * 对码（限时学习窗口）、清码（单个/全部）、按键库维护
 * 通过 /api/ir/* 与固件 IrRouteHandler 通信；按键随 /config/periph_exec.json 的 irKeys 数组持久化
 * 懒加载：由 peripherals.js 在外设编辑弹窗切换到红外接收头(type=53)时按需加载
 * 面板嵌入在外设编辑弹窗 #ir-manage-section 内，无独立模态窗
 */
(function() {
    AppState.registerModule('ir-remote', {

        _irLearnTimer: null,
        _irCapturedProtocol: '',
        _irPanelBound: false,
        _irPanelActive: false,

        // ============ 面板生命周期 ============

        /**
         * 初始化红外管理面板（编辑弹窗打开且 type=53 时调用）
         * 绑定事件（仅首次）、重置学习 UI、加载状态和按键库
         */
        initIrManage() {
            this._irBindPanelEvents();
            this._irPanelActive = true;
            this._irResetLearnUi();
            this._irLoadStatus();
            this._irLoadKeys();
        },

        /**
         * 销毁面板状态（编辑弹窗关闭或类型切走时调用）
         * 停止轮询；如仍在学习，通知后端取消，避免残留学习窗口拦截派发
         */
        teardownIrManage() {
            if (!this._irPanelActive) return;
            this._irPanelActive = false;
            this._irStopLearnPolling();
            if (typeof apiPostSilent === 'function') {
                apiPostSilent('/api/ir/learn/stop', {});
            }
        },

        // ============ 事件绑定（仅首次） ============

        _irBindPanelEvents() {
            if (this._irPanelBound) return;
            var self = this;
            var startBtn = document.getElementById('ir-learn-start-btn');
            if (startBtn) startBtn.addEventListener('click', function() { self._irStartLearn(); });
            var stopBtn = document.getElementById('ir-learn-stop-btn');
            if (stopBtn) stopBtn.addEventListener('click', function() { self._irStopLearn(); });
            var saveBtn = document.getElementById('ir-key-save-btn');
            if (saveBtn) saveBtn.addEventListener('click', function() { self._irSaveKey(); });
            var clearBtn = document.getElementById('ir-keys-clear-btn');
            if (clearBtn) clearBtn.addEventListener('click', function() { self._irClearKeys(); });
            var tbody = document.getElementById('ir-keys-table-body');
            if (tbody) tbody.addEventListener('click', function(e) { self._irHandleKeyTableClick(e); });
            this._irPanelBound = true;
        },

        // ============ 状态 ============

        _irLoadStatus() {
            var hint = document.getElementById('ir-remote-not-ready');
            var startBtn = document.getElementById('ir-learn-start-btn');
            apiGet('/api/ir/status')
                .then(function(res) {
                    var ready = res && res.success && res.data && res.data.initialized;
                    if (hint) hint.style.display = ready ? 'none' : 'block';
                    if (startBtn) startBtn.disabled = !ready;
                })
                .catch(function() {
                    if (hint) hint.style.display = 'block';
                    if (startBtn) startBtn.disabled = true;
                });
        },

        // ============ 对码 / 学习 ============

        _irResetLearnUi() {
            this._irStopLearnPolling();
            this._irCapturedProtocol = '';
            var captured = document.getElementById('ir-learn-captured');
            if (captured) captured.style.display = 'none';
            var status = document.getElementById('ir-learn-status');
            if (status) status.textContent = '';
            var startBtn = document.getElementById('ir-learn-start-btn');
            if (startBtn) startBtn.style.display = '';
            var stopBtn = document.getElementById('ir-learn-stop-btn');
            if (stopBtn) stopBtn.style.display = 'none';
            var nameInput = document.getElementById('ir-key-name-input');
            if (nameInput) nameInput.value = '';
            var codeInput = document.getElementById('ir-key-code-input');
            if (codeInput) codeInput.value = '';
        },

        _irStartLearn() {
            if (!this.guardDeveloperModeAction()) return;
            var self = this;
            var timeoutEl = document.getElementById('ir-learn-timeout');
            var sec = parseInt((timeoutEl && timeoutEl.value) || '10', 10);
            if (isNaN(sec) || sec < 3) sec = 3;
            if (sec > 60) sec = 60;
            this._irResetLearnUi();
            var startBtn = document.getElementById('ir-learn-start-btn');
            var stopBtn = document.getElementById('ir-learn-stop-btn');
            var status = document.getElementById('ir-learn-status');
            if (startBtn) startBtn.style.display = 'none';
            if (stopBtn) stopBtn.style.display = '';
            if (status) status.textContent = '正在等待遥控信号...';
            apiPost('/api/ir/learn/start', { timeoutMs: sec * 1000 })
                .then(function(res) {
                    if (!res || !res.success) {
                        if (status) status.textContent = (res && res.error) || '启动学习失败';
                        self._irResetLearnUi();
                        return;
                    }
                    self._irStartLearnPolling();
                })
                .catch(function(err) {
                    if (status) status.textContent = (err && err.data && err.data.error) || '启动学习失败';
                    self._irResetLearnUi();
                });
        },

        _irStartLearnPolling() {
            var self = this;
            this._irStopLearnPolling();
            this._irLearnTimer = setInterval(function() { self._irPollLearn(); }, 700);
        },

        _irStopLearnPolling() {
            if (this._irLearnTimer) {
                clearInterval(this._irLearnTimer);
                this._irLearnTimer = null;
            }
        },

        _irPollLearn() {
            var self = this;
            apiGetSilent('/api/ir/learn/status')
                .then(function(res) {
                    if (!res || !res.success || !res.data) return;
                    var d = res.data;
                    var status = document.getElementById('ir-learn-status');
                    if (d.learnState === 'learning') {
                        var remain = Math.ceil((d.remainingMs || 0) / 1000);
                        if (status) status.textContent = '正在等待遥控信号...（剩余 ' + remain + ' 秒）';
                    } else if (d.learnState === 'captured') {
                        self._irStopLearnPolling();
                        self._irOnCaptured(d.code || '', d.protocol || '');
                    } else if (d.learnState === 'timeout') {
                        self._irStopLearnPolling();
                        if (status) status.textContent = '学习超时，未捕获到编码，请重试。';
                        self._irRestoreLearnButtons();
                    } else {
                        // idle：窗口已被取消
                        self._irStopLearnPolling();
                        self._irRestoreLearnButtons();
                    }
                })
                .catch(function() { /* 静默重试 */ });
        },

        _irRestoreLearnButtons() {
            var startBtn = document.getElementById('ir-learn-start-btn');
            var stopBtn = document.getElementById('ir-learn-stop-btn');
            if (startBtn) startBtn.style.display = '';
            if (stopBtn) stopBtn.style.display = 'none';
        },

        _irOnCaptured(code, protocol) {
            this._irCapturedProtocol = protocol || '';
            var status = document.getElementById('ir-learn-status');
            if (status) status.textContent = '已捕获编码 ' + (protocol ? (protocol + ' ') : '') + code + '，请命名并保存。';
            var codeInput = document.getElementById('ir-key-code-input');
            if (codeInput) codeInput.value = code;
            var captured = document.getElementById('ir-learn-captured');
            if (captured) captured.style.display = '';
            this._irRestoreLearnButtons();
            var nameInput = document.getElementById('ir-key-name-input');
            if (nameInput) nameInput.focus();
        },

        _irStopLearn() {
            var self = this;
            this._irStopLearnPolling();
            apiPost('/api/ir/learn/stop', {})
                .then(function() { self._irResetLearnUi(); })
                .catch(function() { self._irResetLearnUi(); });
        },

        _irSaveKey() {
            if (!this.guardDeveloperModeAction()) return;
            var self = this;
            var nameInput = document.getElementById('ir-key-name-input');
            var codeInput = document.getElementById('ir-key-code-input');
            var name = (nameInput && nameInput.value || '').trim();
            var code = (codeInput && codeInput.value || '').trim();
            if (!name) {
                Notification.warning('请输入按键名称', '红外遥控');
                return;
            }
            if (!code) {
                Notification.warning('尚未捕获编码', '红外遥控');
                return;
            }
            var saveBtn = document.getElementById('ir-key-save-btn');
            if (saveBtn) saveBtn.disabled = true;
            apiPost('/api/ir/keys', { name: name, code: code, protocol: this._irCapturedProtocol || '' })
                .then(function(res) {
                    if (res && res.success) {
                        Notification.success('按键已保存', '红外遥控');
                        self._irResetLearnUi();
                        self._irLoadKeys();
                    } else {
                        Notification.error((res && res.error) || '保存失败', '红外遥控');
                    }
                })
                .catch(function(err) {
                    Notification.error((err && err.data && err.data.error) || '保存失败', '红外遥控');
                })
                .finally(function() { if (saveBtn) saveBtn.disabled = false; });
        },

        // ============ 按键库 ============

        _irLoadKeys() {
            var self = this;
            var tbody = document.getElementById('ir-keys-table-body');
            if (!tbody) return;
            this.renderEmptyTableRow(tbody, 4, '加载中...');
            apiGetFresh('/api/ir/keys')
                .then(function(res) {
                    if (!res || !res.success || !res.data) {
                        self.renderEmptyTableRow(tbody, 4, '加载失败', 'u-empty-cell u-text-danger');
                        return;
                    }
                    var keys = res.data.keys || [];
                    self._irRenderKeysTable(keys);
                })
                .catch(function() {
                    self.renderEmptyTableRow(tbody, 4, '加载失败', 'u-empty-cell u-text-danger');
                });
        },

        _irRenderKeysTable(keys) {
            var tbody = document.getElementById('ir-keys-table-body');
            if (!tbody) return;
            if (!keys || keys.length === 0) {
                this.renderEmptyTableRow(tbody, 4, '暂无按键，请先对码学习');
                return;
            }
            var locked = !this.isDeveloperModeEnabled();
            var delAttr = locked ? ' disabled title="' + escapeHtml(this.getDeveloperModeDisabledText()) + '"' : '';
            var lockClass = locked ? ' dev-mode-locked' : '';
            var html = '';
            keys.forEach(function(k) {
                var name = escapeHtml(k.name || '');
                html += '<tr>' +
                    '<td>' + name + '</td>' +
                    '<td>' + escapeHtml(k.code || '') + '</td>' +
                    '<td>' + escapeHtml(k.protocol || '--') + '</td>' +
                    '<td class="u-cell-nowrap"><div class="u-table-action-row">' +
                    '<button class="fb-btn fb-btn-sm fb-btn-compact fb-btn-danger' + lockClass + '" ' +
                    'data-ir-key-action="delete" data-name="' + name + '"' + delAttr + '>删除</button>' +
                    '</div></td></tr>';
            });
            tbody.innerHTML = html;
        },

        _irHandleKeyTableClick(event) {
            var button = event.target.closest('[data-ir-key-action]');
            if (!button) return;
            var action = button.getAttribute('data-ir-key-action');
            var name = button.getAttribute('data-name');
            if (action !== 'delete' || !name) return;
            if (!this.guardDeveloperModeAction()) return;
            this._irDeleteKey(name);
        },

        async _irDeleteKey(name) {
            var self = this;
            if (!await FBConfirm({ title: '删除按键', message: '确定删除按键 "' + name + '" 吗？', danger: true })) return;
            apiPost('/api/ir/keys/delete', { name: name })
                .then(function(res) {
                    if (res && res.success) {
                        Notification.success('按键已删除', '红外遥控');
                        self._irLoadKeys();
                    } else {
                        Notification.error((res && res.error) || '删除失败', '红外遥控');
                    }
                })
                .catch(function(err) {
                    Notification.error((err && err.data && err.data.error) || '删除失败', '红外遥控');
                });
        },

        async _irClearKeys() {
            if (!this.guardDeveloperModeAction()) return;
            var self = this;
            if (!await FBConfirm({ title: '清除全部按键', message: '确定清除全部红外按键吗？该操作不可恢复。', danger: true, requireCheck: true, checkLabel: '我已确认不再需要任何红外按键配置' })) return;
            apiPost('/api/ir/keys/clear', {})
                .then(function(res) {
                    if (res && res.success) {
                        Notification.success('已清除全部按键', '红外遥控');
                        self._irLoadKeys();
                    } else {
                        Notification.error((res && res.error) || '清除失败', '红外遥控');
                    }
                })
                .catch(function(err) {
                    Notification.error((err && err.data && err.data.error) || '清除失败', '红外遥控');
                });
        }
    });
})();
