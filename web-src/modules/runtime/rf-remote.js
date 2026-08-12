/**
 * 433MHz RF 遥控管理模块（嵌入式面板）
 * 对码（限时学习窗口）、清码（单个/全部）、按键库维护
 * 通过 /api/rf/* 与固件 RfRouteHandler 通信；按键持久化到 /config/rf_keys.json
 * 懒加载：由 peripherals.js 在外设编辑弹窗切换到 RF模块(type=48) 时按需加载
 * 面板嵌入在外设编辑弹窗 #rf-manage-section 内，无独立模态窗
 */
(function() {
    AppState.registerModule('rf-remote', {

        _rfLearnTimer: null,
        _rfCapturedBits: 0,
        _rfCapturedPulse: 0,
        _rfPanelBound: false,
        _rfPanelActive: false,

        // ============ 面板生命周期 ============

        initRfManage() {
            this._rfBindPanelEvents();
            this._rfPanelActive = true;
            this._rfResetLearnUi();
            this._rfLoadStatus();
            this._rfLoadKeys();
        },

        teardownRfManage() {
            if (!this._rfPanelActive) return;
            this._rfPanelActive = false;
            this._rfStopLearnPolling();
            if (typeof apiPostSilent === 'function') {
                apiPostSilent('/api/rf/learn/stop', {});
            }
        },

        // ============ 事件绑定（仅首次） ============

        _rfBindPanelEvents() {
            if (this._rfPanelBound) return;
            var self = this;
            var startBtn = document.getElementById('rf-learn-start-btn');
            if (startBtn) startBtn.addEventListener('click', function() { self._rfStartLearn(); });
            var stopBtn = document.getElementById('rf-learn-stop-btn');
            if (stopBtn) stopBtn.addEventListener('click', function() { self._rfStopLearn(); });
            var saveBtn = document.getElementById('rf-key-save-btn');
            if (saveBtn) saveBtn.addEventListener('click', function() { self._rfSaveKey(); });
            var clearBtn = document.getElementById('rf-keys-clear-btn');
            if (clearBtn) clearBtn.addEventListener('click', function() { self._rfClearKeys(); });
            var tbody = document.getElementById('rf-keys-table-body');
            if (tbody) tbody.addEventListener('click', function(e) { self._rfHandleKeyTableClick(e); });
            this._rfPanelBound = true;
        },

        // ============ 状态 ============

        _rfLoadStatus() {
            var hint = document.getElementById('rf-not-ready');
            var startBtn = document.getElementById('rf-learn-start-btn');
            apiGet('/api/rf/status')
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

        _rfResetLearnUi() {
            this._rfStopLearnPolling();
            this._rfCapturedBits = 0;
            this._rfCapturedPulse = 0;
            var captured = document.getElementById('rf-learn-captured');
            if (captured) captured.style.display = 'none';
            var status = document.getElementById('rf-learn-status');
            if (status) status.textContent = '';
            var startBtn = document.getElementById('rf-learn-start-btn');
            if (startBtn) startBtn.style.display = '';
            var stopBtn = document.getElementById('rf-learn-stop-btn');
            if (stopBtn) stopBtn.style.display = 'none';
            var nameInput = document.getElementById('rf-key-name-input');
            if (nameInput) nameInput.value = '';
            var codeInput = document.getElementById('rf-key-code-input');
            if (codeInput) codeInput.value = '';
            var bitsInput = document.getElementById('rf-key-bits-input');
            if (bitsInput) bitsInput.value = '';
            var pulseInput = document.getElementById('rf-key-pulse-input');
            if (pulseInput) pulseInput.value = '';
        },

        _rfStartLearn() {
            if (!this.guardDeveloperModeAction()) return;
            var self = this;
            var timeoutEl = document.getElementById('rf-learn-timeout');
            var sec = parseInt((timeoutEl && timeoutEl.value) || '10', 10);
            if (isNaN(sec) || sec < 3) sec = 3;
            if (sec > 60) sec = 60;
            this._rfResetLearnUi();
            var startBtn = document.getElementById('rf-learn-start-btn');
            var stopBtn = document.getElementById('rf-learn-stop-btn');
            var status = document.getElementById('rf-learn-status');
            if (startBtn) startBtn.style.display = 'none';
            if (stopBtn) stopBtn.style.display = '';
            if (status) status.textContent = '正在等待 RF 遥控信号...';
            apiPost('/api/rf/learn/start', { timeoutMs: sec * 1000 })
                .then(function(res) {
                    if (!res || !res.success) {
                        if (status) status.textContent = (res && res.error) || '启动学习失败';
                        self._rfResetLearnUi();
                        return;
                    }
                    self._rfStartLearnPolling();
                })
                .catch(function(err) {
                    if (status) status.textContent = (err && err.data && err.data.error) || '启动学习失败';
                    self._rfResetLearnUi();
                });
        },

        _rfStartLearnPolling() {
            var self = this;
            this._rfStopLearnPolling();
            this._rfLearnTimer = setInterval(function() { self._rfPollLearn(); }, 700);
        },

        _rfStopLearnPolling() {
            if (this._rfLearnTimer) {
                clearInterval(this._rfLearnTimer);
                this._rfLearnTimer = null;
            }
        },

        _rfPollLearn() {
            var self = this;
            apiGetSilent('/api/rf/learn/status')
                .then(function(res) {
                    if (!res || !res.success || !res.data) return;
                    var d = res.data;
                    var status = document.getElementById('rf-learn-status');
                    if (d.learnState === 'learning') {
                        var remain = Math.ceil((d.remainingMs || 0) / 1000);
                        if (status) status.textContent = '正在等待 RF 遥控信号...（剩余 ' + remain + ' 秒）';
                    } else if (d.learnState === 'captured') {
                        self._rfStopLearnPolling();
                        self._rfOnCaptured(d.code || '', d.bits || 24, d.pulse || 350);
                    } else if (d.learnState === 'timeout') {
                        self._rfStopLearnPolling();
                        if (status) status.textContent = '学习超时，未捕获到编码，请重试。';
                        self._rfRestoreLearnButtons();
                    } else {
                        self._rfStopLearnPolling();
                        self._rfRestoreLearnButtons();
                    }
                })
                .catch(function() { /* 静默重试 */ });
        },

        _rfRestoreLearnButtons() {
            var startBtn = document.getElementById('rf-learn-start-btn');
            var stopBtn = document.getElementById('rf-learn-stop-btn');
            if (startBtn) startBtn.style.display = '';
            if (stopBtn) stopBtn.style.display = 'none';
        },

        _rfOnCaptured(code, bits, pulse) {
            this._rfCapturedBits = bits;
            this._rfCapturedPulse = pulse;
            var status = document.getElementById('rf-learn-status');
            if (status) status.textContent = '已捕获编码 ' + code + '（' + bits + '位, 脉宽' + pulse + 'μs），请命名并保存。';
            var codeInput = document.getElementById('rf-key-code-input');
            if (codeInput) codeInput.value = code;
            var bitsInput = document.getElementById('rf-key-bits-input');
            if (bitsInput) bitsInput.value = bits;
            var pulseInput = document.getElementById('rf-key-pulse-input');
            if (pulseInput) pulseInput.value = pulse + 'μs';
            var captured = document.getElementById('rf-learn-captured');
            if (captured) captured.style.display = '';
            this._rfRestoreLearnButtons();
            var nameInput = document.getElementById('rf-key-name-input');
            if (nameInput) nameInput.focus();
        },

        _rfStopLearn() {
            var self = this;
            this._rfStopLearnPolling();
            apiPost('/api/rf/learn/stop', {})
                .then(function() { self._rfResetLearnUi(); })
                .catch(function() { self._rfResetLearnUi(); });
        },

        _rfSaveKey() {
            if (!this.guardDeveloperModeAction()) return;
            var self = this;
            var nameInput = document.getElementById('rf-key-name-input');
            var codeInput = document.getElementById('rf-key-code-input');
            var name = (nameInput && nameInput.value || '').trim();
            var code = (codeInput && codeInput.value || '').trim();
            if (!name) {
                Notification.warning('请输入按键名称', 'RF遥控');
                return;
            }
            if (!code) {
                Notification.warning('尚未捕获编码', 'RF遥控');
                return;
            }
            var saveBtn = document.getElementById('rf-key-save-btn');
            if (saveBtn) saveBtn.disabled = true;
            apiPost('/api/rf/keys', { name: name, code: code, bits: this._rfCapturedBits || 24, pulse: this._rfCapturedPulse || 350 })
                .then(function(res) {
                    if (res && res.success) {
                        Notification.success('按键已保存', 'RF遥控');
                        self._rfResetLearnUi();
                        self._rfLoadKeys();
                    } else {
                        Notification.error((res && res.error) || '保存失败', 'RF遥控');
                    }
                })
                .catch(function(err) {
                    Notification.error((err && err.data && err.data.error) || '保存失败', 'RF遥控');
                })
                .finally(function() { if (saveBtn) saveBtn.disabled = false; });
        },

        // ============ 按键库 ============

        _rfLoadKeys() {
            var self = this;
            var tbody = document.getElementById('rf-keys-table-body');
            if (!tbody) return;
            this.renderEmptyTableRow(tbody, 5, '加载中...');
            apiGetFresh('/api/rf/keys')
                .then(function(res) {
                    if (!res || !res.success || !res.data) {
                        self.renderEmptyTableRow(tbody, 5, '加载失败', 'u-empty-cell u-text-danger');
                        return;
                    }
                    var keys = res.data.keys || [];
                    self._rfRenderKeysTable(keys);
                })
                .catch(function() {
                    self.renderEmptyTableRow(tbody, 5, '加载失败', 'u-empty-cell u-text-danger');
                });
        },

        _rfRenderKeysTable(keys) {
            var tbody = document.getElementById('rf-keys-table-body');
            if (!tbody) return;
            if (!keys || keys.length === 0) {
                this.renderEmptyTableRow(tbody, 5, '暂无按键，请先对码学习');
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
                    '<td>' + (k.bits || 24) + '</td>' +
                    '<td>' + (k.pulse || 350) + 'μs</td>' +
                    '<td class="u-cell-nowrap"><div class="u-table-action-row">' +
                    '<button class="fb-btn fb-btn-sm fb-btn-compact fb-btn-danger' + lockClass + '" ' +
                    'data-rf-key-action="delete" data-name="' + name + '"' + delAttr + '>删除</button>' +
                    '</div></td></tr>';
            });
            tbody.innerHTML = html;
        },

        _rfHandleKeyTableClick(event) {
            var button = event.target.closest('[data-rf-key-action]');
            if (!button) return;
            var action = button.getAttribute('data-rf-key-action');
            var name = button.getAttribute('data-name');
            if (action !== 'delete' || !name) return;
            if (!this.guardDeveloperModeAction()) return;
            this._rfDeleteKey(name);
        },

        _rfDeleteKey(name) {
            var self = this;
            if (!confirm('确定删除按键 "' + name + '" 吗？')) return;
            apiPost('/api/rf/keys/delete', { name: name })
                .then(function(res) {
                    if (res && res.success) {
                        Notification.success('按键已删除', 'RF遥控');
                        self._rfLoadKeys();
                    } else {
                        Notification.error((res && res.error) || '删除失败', 'RF遥控');
                    }
                })
                .catch(function(err) {
                    Notification.error((err && err.data && err.data.error) || '删除失败', 'RF遥控');
                });
        },

        _rfClearKeys() {
            if (!this.guardDeveloperModeAction()) return;
            var self = this;
            if (!confirm('确定清除全部 RF 按键吗？该操作不可恢复。')) return;
            apiPost('/api/rf/keys/clear', {})
                .then(function(res) {
                    if (res && res.success) {
                        Notification.success('已清除全部按键', 'RF遥控');
                        self._rfLoadKeys();
                    } else {
                        Notification.error((res && res.error) || '清除失败', 'RF遥控');
                    }
                })
                .catch(function(err) {
                    Notification.error((err && err.data && err.data.error) || '清除失败', 'RF遥控');
                });
        }
    });
})();
