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
        if (bytes < 1024) return bytes + 'B';
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
            // 应用开发模式状态（按钮禁用/提示显示）
            this.applyDeveloperModeState(document.getElementById('features-page') || document);
        },

        loadFeatureStatus() {
            var list = document.getElementById('features-list');
            if (list) list.innerHTML = '<div class="u-fs-13 u-text-secondary">加载中...</div>';
            var self = this;
            return apiGet('/api/features/status')
                .then(function(res) {
                    if (!res || !res.success || !res.data) throw new Error('load failed');
                    self._featureState = res.data.features || [];
                    self._featureState.forEach(function(f) {
                        f.savedUserEnabled = !!f.userEnabled;
                    });
                    self._featureDirty = false;
                    self.renderFeatureList();
                    self.updateRamMonitor(res.data.summary);
                    var saveBtn = document.getElementById('features-save-btn');
                    if (saveBtn) saveBtn.disabled = true;
                })
                .catch(function() {
                    if (list) list.innerHTML = '<div class="message message-error">功能状态加载失败，请刷新重试</div>';
                });
        },

        renderFeatureList() {
            var list = document.getElementById('features-list');
            if (!list || !this._featureState) return;
            var self = this;

            list.innerHTML = this.renderGroupTables(this._featureState);
            if (window.FB && window.FB.ui && typeof window.FB.ui.enhanceResponsiveTables === 'function') {
                window.FB.ui.enhanceResponsiveTables(list);
            }

            list.querySelectorAll('[data-feature-toggle]').forEach(function(input) {
                input.addEventListener('change', function() {
                    self.onFeatureToggle(input.getAttribute('data-feature-toggle'), input.checked);
                });
            });

            // 初次渲染即应用依赖联动（父功能禁用的子功能行隐藏）
            this.applyDependencyPreview();
            this.applyFeatureDeveloperModeState(list);
        },

        // 按 group 分组渲染功能表格
        renderGroupTables(features) {
            var self = this;
            var groups = {};
            features.forEach(function(f) {
                var g = f.group || 'system';
                if (!groups[g]) groups[g] = [];
                groups[g].push(f);
            });
            var html = '';
            Object.keys(groups).sort().forEach(function(g) {
                html += '<h3 class="feature-group-title">' + (GROUP_NAMES[g] || g) + '</h3>';
                html += '<table class="fb-table fb-table-bordered fb-table-mobile"><thead><tr>'
                    + '<th>功能</th><th>状态</th><th>Flash 估算</th><th>RAM 估算</th><th>依赖</th><th>启用</th>'
                    + '</tr></thead><tbody>';
                groups[g].forEach(function(f) {
                    html += self.buildFeatureRow(f);
                });
                html += '</tbody></table>';
            });
            return html;
        },

        // 渲染单个功能行（支持子功能隐藏）
        buildFeatureRow(f) {
            var attached = this.getAttachedFeatures(f.key);
            var bundleCompiled = attached.every(function(child) { return child.compiled; });
            var bundleEnabled = attached.every(function(child) { return child.userEnabled; });
            var locked = !!f.locked || !f.compiled || !bundleCompiled;
            var disabledAttr = locked ? ' disabled data-feature-locked="true"' : '';
            var checked = f.userEnabled && bundleEnabled ? ' checked' : '';
            var rowClass = locked ? ' class="feature-row-locked"' : '';
            var lockLabel = (!f.lockReason || f.lockReason === 'Not compiled into firmware')
                ? '未编译入固件'
                : '未编译入固件 · ' + f.lockReason;

            // 子功能无独立显示名称，不渲染为独立行
            if (!f.name) {
                return '';
            }

            var dep = '';
            if (f.hwDep) dep += f.hwDep;
            if (f.dependsOn) dep += (dep ? ' / ' : '') + '依赖 ' + f.dependsOn;

            var selected = f.userEnabled && bundleEnabled;
            var pendingSave = typeof f.savedUserEnabled === 'boolean' && f.userEnabled !== f.savedUserEnabled;
            var limitedChild = attached.find(function(child) {
                return child.compiled && child.userEnabled &&
                    (child.runtimeDisabled || child.budgetPassed === false || child.active === false);
            });
            var admissionLabels = {
                waiting_for_network: '等待网络就绪',
                dependency_blocked: '依赖功能受限',
                dependency_missing: '依赖功能缺失',
                dependency_cycle: '依赖关系异常',
                budget_denied: '内存预算不足',
                runtime_disabled: '运行受限',
                restart_required: '重启后启用',
                user_disabled: '未获准启用'
            };
            var statusText = '用户未启用';
            var statusClass = 'status-badge status-badge-neutral';
            if (locked) {
                statusText = '未编译';
                statusClass = 'status-badge status-badge-warning';
            } else if (pendingSave) {
                statusText = selected ? '待保存 · 启用' : '待保存 · 停用';
                statusClass = 'status-badge status-badge-warning';
            } else if (f.requiresRestart && selected !== !!f.bootEnabled) {
                statusText = selected ? '重启后启用' : '重启后停用';
                statusClass = 'status-badge status-badge-warning';
            } else if (f.runtimeDisabled || (selected && f.budgetPassed === false)) {
                statusText = f.budgetPassed === false ? '内存预算不足' : '运行受限';
                statusClass = 'status-badge status-badge-warning';
            } else if (selected && Object.prototype.hasOwnProperty.call(admissionLabels, f.admissionState)) {
                statusText = admissionLabels[f.admissionState];
                statusClass = 'status-badge status-badge-warning';
            } else if (selected && limitedChild) {
                statusText = '附属能力受限';
                statusClass = 'status-badge status-badge-warning';
            } else if (selected && f.active !== false) {
                statusText = f.onDemand ? '已准入 · 按需使用' : '已准入';
            } else if (selected) {
                statusText = '启用状态待确认';
                statusClass = 'status-badge status-badge-warning';
            } else if (f.active) {
                statusText = '停用状态待确认';
                statusClass = 'status-badge status-badge-warning';
            }

            var stateReason = f.runtimeDisableReason || (limitedChild && limitedChild.runtimeDisableReason);
            var statusTitle = ' title="' + escapeHtml(locked ? lockLabel :
                (stateReason ? stateReason + '；' : '') +
                '准入不代表实例已初始化；Flash/RAM 为静态估算，不是实测占用。') + '"';

            return '<tr' + rowClass + ' data-feature-key="' + escapeHtml(f.key) + '">' +
                '<td>' + escapeHtml(f.name) + '</td>'
                + '<td class="feature-state"><span class="' + statusClass + '"' + statusTitle + '>' + statusText + '</span></td>'
                + '<td class="feature-resource" data-feature-resource="flash">' + (f.flashKB || 0) + 'KB</td>'
                + '<td class="feature-resource" data-feature-resource="ram">' + formatRam(f.ramB) + '</td>'
                + '<td class="feature-dependency" data-feature-resource="dependency">' + escapeHtml(dep || '-') + '</td>'
                + '<td><label class="toggle-switch">'
                + '<input type="checkbox" data-feature-toggle="' + escapeHtml(f.key) + '"' + checked + disabledAttr + '>'
                + '<span class="toggle-slider"></span></label></td>'
                + '</tr>';
        },

        onFeatureToggle(key, enabled) {
            var f = this.findFeature(key);
            if (!f) return;
            f.userEnabled = enabled;
            this.getAttachedFeatures(key).forEach(function(child) {
                child.userEnabled = enabled;
            });
            this._featureDirty = true;
            this.renderFeatureList();
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

        // Only required bundle members lock/toggle the parent. Older API
        // payloads without this field retain their existing bundle behavior.
        getAttachedFeatures(key) {
            return (this._featureState || []).filter(function(f) {
                return !f.name && f.dependsOn === key && f.bundleRequired !== false;
            });
        },

        // 依赖联动预览：父功能禁用/未编译时子功能行隐藏（不修改服务端状态，仅视觉提示）
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
                var row = input.closest('tr');
                // 父功能禁用或未编译：依赖未满足，子功能无展示意义，直接隐藏该行
                var parentInactive = !parent.userEnabled || !parent.compiled;
                // 对于无名称的子功能（如 otaFs），跳过隐藏逻辑
                if (!f.name && !parentInactive) {
                    // 作为父功能的附属能力，保持隐藏
                    row.style.display = 'none';
                    return;
                }
                if (row) row.style.display = parentInactive ? 'none' : '';
                // 子功能自身未编译：仍灰显并显示服务端提供的具体原因
                var selfLocked = !f.compiled;
                input.disabled = parentInactive || selfLocked;
                if (row && !parentInactive) row.classList.toggle('feature-row-locked', selfLocked);
            });
        },

        updateRamMonitor(summary) {
            if (!summary) return;
            var free8El = document.getElementById('ram-free8');
            var largest8El = document.getElementById('ram-largest8');
            var activeEl = document.getElementById('ram-active');
            if (free8El && typeof summary.free8KB === 'number') free8El.textContent = summary.free8KB + 'KB';
            if (largest8El && typeof summary.largest8KB === 'number') largest8El.textContent = summary.largest8KB + 'KB';
            if (activeEl && typeof summary.activeRamB === 'number') {
                activeEl.textContent = (summary.activeRamB / 1024).toFixed(1) + 'KB';
                activeEl.title = '已准入功能的静态 RAM 估算，不代表实例实际占用或关闭后的释放量';
            }
        },

        // 功能配置的备份/迁移统一走设备配置页（高级配置）的配置转移模块，
        // 本页不再提供重复的导入/导出入口

        async applyFeatureConfig() {
            if (!this._featureDirty) return;
            
            // WP-91: Web 控制面自锁保护 - 检查是否关闭了 webServer
            const willDisableWebServer = (this._featureState || []).some(f => 
                f.key === 'webServer' && !f.userEnabled
            );
            
            if (willDisableWebServer) {
                const confirmMsg = '警告：关闭 Web 服务后将失去通过浏览器配置设备的能力！\n\n' +
                    '恢复方式：\n' +
                    '1. 访问 http://fastbee.local (mDNS) 或 IP 直连进入 AP 配置模式\n' +
                    '2. 通过外设执行规则 ACTION_FEATURE_ENABLE 重新启用\n' +
                    '3. 或连接串口发送 AT 命令恢复\n\n' +
                    '确定要关闭 Web 服务吗？';
                
                if (!await FBConfirm({ title: '应用功能配置', message: confirmMsg, danger: true })) {
                    var saveBtn = document.getElementById('features-save-btn');
                    if (saveBtn) saveBtn.disabled = false;
                    return;
                }
            }
            
            // 前置 guardDeveloperModeAction 验证 - 防止普通用户误修改核心功能配置
            if (!AppState.guardDeveloperModeAction()) return;
            
            var body = { features: {} };
            (this._featureState || []).forEach(function(f) {
                if (f.compiled) body.features[f.key] = { enabled: !!f.userEnabled };
            });
            var saveBtn = document.getElementById('features-save-btn');
            if (saveBtn) { saveBtn.disabled = true; saveBtn.textContent = '保存中...'; }
            // 后端 features/* POST 通过 onData 回调累积原始 body 并 deserializeJson：
            // 必须用 JSON Content-Type（apiPostJson）；apiPost 的表单编码 body
            // 走参数解析路径不触发 onData，会得到空 body 返回 400 Missing JSON body
            apiPostJson('/api/features/apply', body)
                .then(function(res) {
                    if (res && res.success) {
                        var secs = (res.data && res.data.rebootIn) || 3;
                        Notification.success('配置已保存，设备将在' + ' ' + secs + ' ' + '秒后重启生效');
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
        },

        // ========== 开发模式保护 ==========

        applyFeatureDeveloperModeState(scope) {
            var disabled = !AppState.isDeveloperModeEnabled();
            var tip = AppState.getDeveloperModeDisabledText();
            var root = scope && scope.querySelectorAll ? scope : document;
            
            // 控制保存按钮
            var saveBtn = document.getElementById('features-save-btn');
            if (saveBtn) {
                var isDirty = this._featureDirty || false;
                saveBtn.disabled = disabled || !isDirty;
                saveBtn.classList.toggle('dev-mode-locked', disabled);
                if (disabled) saveBtn.title = tip;
                else saveBtn.removeAttribute('title');
            }
            
            // 控制功能开关复选框
            root.querySelectorAll('[data-feature-toggle]').forEach(function(input) {
                var featureLocked = input.getAttribute('data-feature-locked') === 'true';
                input.disabled = disabled || featureLocked;
                if (input.parentElement) input.parentElement.classList.toggle('dev-mode-locked', disabled);
                if (disabled) input.title = tip;
                else if (!featureLocked) input.removeAttribute('title');
            });
            
            // 显示/隐藏开发模式提示条
            var devHint = document.getElementById('feature-dev-mode-hint');
            if (devHint) devHint.classList.toggle('fb-hidden', !disabled);
        }
    });
})();
