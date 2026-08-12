// ============================================================
// 功能配置模块 (feature-config)
// 运行时功能开关管理：调用 /api/features/* 端点
// ============================================================
(function() {
    var GROUP_NAMES = {
        peripheral: '外设',
        protocol: '通信协议',
        network: '网络',
        system: '系统'
    };

    function formatRam(bytes) {
        if (!bytes) return '0KB';
        return (bytes / 1024).toFixed(1) + 'KB';
    }

    AppState.registerModule('feature-config', {
        _featureState: null,   // 服务端返回的 features 数组（含本地修改）
        _featureDirty: false,

        loadFeaturesPage() {
            if (!this._featuresEventsBound) {
                this._featuresEventsBound = true;
                var refreshBtn = document.getElementById('features-refresh-btn');
                if (refreshBtn) refreshBtn.addEventListener('click', () => this.loadFeatureStatus());
                var saveBtn = document.getElementById('features-save-btn');
                if (saveBtn) saveBtn.addEventListener('click', () => this.applyFeatureConfig());
            }
            this.loadFeatureStatus();
        },

        loadFeatureStatus() {
            var list = document.getElementById('features-list');
            var summary = document.getElementById('features-summary');
            if (list) list.innerHTML = '<div class="u-fs-13 u-text-secondary">加载中...</div>';
            var self = this;
            return apiGet('/api/features/status')
                .then(function(res) {
                    if (!res || !res.success || !res.data) throw new Error('load failed');
                    self._featureState = res.data.features || [];
                    self._featureDirty = false;
                    self.renderFeatureList();
                    self.updateFeatureSummary(res.data.summary);
                    var saveBtn = document.getElementById('features-save-btn');
                    if (saveBtn) saveBtn.disabled = true;
                })
                .catch(function() {
                    if (list) list.innerHTML = '<div class="message message-error">功能状态加载失败，请刷新重试</div>';
                    if (summary) summary.textContent = '';
                });
        },

        renderFeatureList() {
            var list = document.getElementById('features-list');
            if (!list || !this._featureState) return;
            var groups = {};
            this._featureState.forEach(function(f) {
                var g = f.group || 'system';
                if (!groups[g]) groups[g] = [];
                groups[g].push(f);
            });

            var html = '';
            var self = this;
            Object.keys(groups).sort().forEach(function(g) {
                html += '<h3 class="u-fs-14" style="margin:12px 0 6px;">' + (GROUP_NAMES[g] || g) + '</h3>';
                html += '<table class="fb-table fb-table-bordered"><thead><tr>'
                    + '<th>功能</th><th>Flash</th><th>RAM</th><th>依赖</th><th style="width:70px;">启用</th>'
                    + '</tr></thead><tbody>';
                groups[g].forEach(function(f) {
                    var locked = !!f.locked || !f.compiled;
                    var disabledAttr = locked ? ' disabled' : '';
                    var checked = f.userEnabled ? ' checked' : '';
                    var rowStyle = locked ? ' style="opacity:0.5;"' : '';
                    var dep = '';
                    if (f.hwDep) dep += escapeHtml(f.hwDep);
                    if (f.dependsOn) dep += (dep ? ' / ' : '') + '依赖 ' + escapeHtml(f.dependsOn);
                    html += '<tr' + rowStyle + ' data-feature-key="' + escapeHtml(f.key) + '">'
                        + '<td>' + escapeHtml(f.name)
                        + '<div class="u-fs-12 u-text-secondary">' + escapeHtml(f.key)
                        + (locked ? ' · 未编译入固件' : '') + '</div></td>'
                        + '<td>' + (f.flashKB || 0) + 'KB</td>'
                        + '<td>' + formatRam(f.ramB) + '</td>'
                        + '<td class="u-fs-12">' + (dep || '-') + '</td>'
                        + '<td><label class="toggle-switch">'
                        + '<input type="checkbox" data-feature-toggle="' + escapeHtml(f.key) + '"' + checked + disabledAttr + '>'
                        + '<span class="toggle-slider"></span></label></td>'
                        + '</tr>';
                });
                html += '</tbody></table>';
            });
            list.innerHTML = html;

            list.querySelectorAll('[data-feature-toggle]').forEach(function(input) {
                input.addEventListener('change', function() {
                    self.onFeatureToggle(input.getAttribute('data-feature-toggle'), input.checked);
                });
            });
        },

        onFeatureToggle(key, enabled) {
            var f = this.findFeature(key);
            if (!f) return;
            f.userEnabled = enabled;
            this._featureDirty = true;
            this.applyDependencyPreview();
            this.updateLocalEstimate();
            var saveBtn = document.getElementById('features-save-btn');
            if (saveBtn) saveBtn.disabled = false;
        },

        findFeature(key) {
            if (!this._featureState) return null;
            for (var i = 0; i < this._featureState.length; i++) {
                if (this._featureState[i].key === key) return this._featureState[i];
            }
            return null;
        },

        // 依赖联动预览：父功能关闭时子功能灰显（不修改服务端状态，仅视觉提示）
        applyDependencyPreview() {
            var state = this._featureState || [];
            state.forEach(function(f) {
                if (!f.dependsOn) return;
                var parent = null;
                for (var i = 0; i < state.length; i++) {
                    if (state[i].key === f.dependsOn) { parent = state[i]; break; }
                }
                var input = document.querySelector('[data-feature-toggle="' + f.key + '"]');
                if (!input || !parent) return;
                var depDisabled = !parent.userEnabled;
                input.disabled = depDisabled || !f.compiled;
                var row = input.closest('tr');
                if (row) row.style.opacity = (depDisabled || !f.compiled) ? '0.5' : '';
            });
        },

        updateLocalEstimate() {
            var flashKB = 0, ramB = 0;
            (this._featureState || []).forEach(function(f) {
                if (f.compiled && f.userEnabled) {
                    flashKB += f.flashKB || 0;
                    ramB += f.ramB || 0;
                }
            });
            var summary = document.getElementById('features-summary');
            if (summary) {
                summary.textContent = '预估启用资源: Flash ' + flashKB + 'KB | RAM ' + formatRam(ramB)
                    + (this._baselineSummary || '');
            }
        },

        updateFeatureSummary(summary) {
            if (!summary) return;
            var text = '已启用: Flash ' + (summary.activeFlashKB || 0) + 'KB | RAM '
                + formatRam(summary.activeRamB);
            if (typeof summary.free8KB === 'number') {
                text += '    可用 DRAM: ' + summary.free8KB + 'KB free / '
                    + (summary.largest8KB || 0) + 'KB 最大块';
            }
            this._baselineSummary = '    (当前' + text + ')';
            var el = document.getElementById('features-summary');
            if (el) el.textContent = text;
        },

        applyFeatureConfig() {
            if (!this._featureDirty) return;
            var body = { features: {} };
            (this._featureState || []).forEach(function(f) {
                if (f.compiled) body.features[f.key] = { enabled: !!f.userEnabled };
            });
            var saveBtn = document.getElementById('features-save-btn');
            if (saveBtn) { saveBtn.disabled = true; saveBtn.textContent = '保存中...'; }
            apiPost('/api/features/apply', body)
                .then(function(res) {
                    if (res && res.success) {
                        var secs = (res.data && res.data.rebootIn) || 3;
                        Notification.success('配置已保存，设备将在 ' + secs + ' 秒后重启生效');
                        // 重启后刷新页面加载最新状态
                        setTimeout(function() { location.reload(); }, (secs + 5) * 1000);
                    } else {
                        Notification.error((res && (res.error || res.message)) || '保存失败');
                        if (saveBtn) { saveBtn.disabled = false; saveBtn.textContent = '保存并重启'; }
                    }
                })
                .catch(function() {
                    Notification.error('保存失败，请重试');
                    if (saveBtn) { saveBtn.disabled = false; saveBtn.textContent = '保存并重启'; }
                });
        }
    });
})();
