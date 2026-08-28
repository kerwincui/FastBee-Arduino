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
            this.applyDeveloperModeState();
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
            var self = this;

            // 分层展示：ramB===0 的功能运行时不占 DRAM（字模/文案存于 Flash），
            // 归入折叠的"零 DRAM 可选区"，避免与内存敏感功能混排造成干扰
            var mainFeatures = [];
            var zeroRamFeatures = [];
            this._featureState.forEach(function(f) {
                if (!f.ramB) zeroRamFeatures.push(f);
                else mainFeatures.push(f);
            });

            var html = this.renderGroupTables(mainFeatures);
            if (zeroRamFeatures.length > 0) {
                html += this.buildZeroRamSection(zeroRamFeatures);
            }
            list.innerHTML = html;

            // 全功能统一固件（所有功能均已编入）时，Flash 占用已固定，
            // 开关只影响运行时 RAM，给出说明避免误解
            var allCompiled = (this._featureState || []).every(function(f) { return f.compiled; });
            var hint = document.getElementById('features-unified-hint');
            if (!hint) {
                hint = document.createElement('p');
                hint.id = 'features-unified-hint';
                hint.className = 'u-fs-12 u-text-secondary';
                hint.style.marginTop = '8px';
                list.parentNode.insertBefore(hint, list.nextSibling);
            }
            hint.style.display = allCompiled ? '' : 'none';
            hint.textContent = allCompiled
                ? '当前固件已编入全部功能（统一镜像）：Flash 占用已固定，表中 Flash 列仅为编译参考；开关功能只影响运行时 RAM 占用。'
                : '';

            list.querySelectorAll('[data-feature-toggle]').forEach(function(input) {
                input.addEventListener('change', function() {
                    self.onFeatureToggle(input.getAttribute('data-feature-toggle'), input.checked);
                });
            });

            // 绑定零 DRAM 折叠区展开/收起
            var collapseHeader = list.querySelector('[data-zero-ram-toggle]');
            if (collapseHeader && AppState.toggleSection) {
                collapseHeader.addEventListener('click', function() {
                    AppState.toggleSection('features-zero-ram-body');
                });
            }

            // 初次渲染即应用依赖联动（父功能禁用的子功能行隐藏）
            this.applyDependencyPreview();
        },

        // 按 group 分组渲染功能表格（主表与零 DRAM 折叠区共用）
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
                html += '<h3 class="u-fs-14" style="margin:12px 0 6px;">' + (GROUP_NAMES[g] || g) + '</h3>';
                html += '<table class="fb-table fb-table-bordered"><thead><tr>'
                    + '<th>功能</th><th>Flash</th><th>RAM</th><th>依赖</th><th style="width:70px;">启用</th>'
                    + '</tr></thead><tbody>';
                groups[g].forEach(function(f) {
                    html += self.buildFeatureRow(f);
                });
                html += '</tbody></table>';
            });
            return html;
        },

        // 渲染单个功能行
        buildFeatureRow(f) {
            var locked = !!f.locked || !f.compiled;
            var disabledAttr = locked ? ' disabled' : '';
            var checked = f.userEnabled ? ' checked' : '';
            var rowStyle = locked ? ' style="opacity:0.5;"' : '';
            var dep = '';
            if (f.hwDep) dep += escapeHtml(f.hwDep);
            if (f.dependsOn) dep += (dep ? ' / ' : '') + '依赖 ' + escapeHtml(f.dependsOn);
            return '<tr' + rowStyle + ' data-feature-key="' + escapeHtml(f.key) + '">'
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
        },

        // 零 DRAM 可选功能折叠区（运行时不占内存，开关仅影响行为/渲染）
        buildZeroRamSection(features) {
            return '<div class="mqtt-collapsible-section" style="margin-top:16px;">'
                + '<div class="mqtt-collapsible-header" data-zero-ram-toggle>'
                + '<h4 style="margin:0;font-size:14px;">可选功能 · 零 DRAM 占用（' + features.length + '）</h4>'
                + '<span class="mqtt-collapse-icon" id="features-zero-ram-body-icon">&#9654;</span>'
                + '</div>'
                + '<div id="features-zero-ram-body" class="mqtt-collapsible-body fb-hidden">'
                + '<p class="u-fs-12 u-text-secondary" style="margin:8px 0;">以下功能运行时不占用 DRAM（字模/文案存储于 Flash），开关仅影响功能行为或渲染方式，不影响内存水位，可按需启用。</p>'
                + this.renderGroupTables(features)
                + '</div>'
                + '</div>';
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
                if (row) row.style.display = parentInactive ? 'none' : '';
                // 子功能自身未编译：仍灰显提示"未编译入固件"
                var selfLocked = !f.compiled;
                input.disabled = parentInactive || selfLocked;
                if (row && !parentInactive) row.style.opacity = selfLocked ? '0.5' : '';
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
                summary.textContent = '预估启用资源: Flash' + ' ' + flashKB + 'KB | RAM ' + formatRam(ramB)
                    + (this._baselineSummary || '');
            }
        },

        updateFeatureSummary(summary) {
            if (!summary) return;
            var text = '已启用: Flash' + ' ' + (summary.activeFlashKB || 0) + 'KB | RAM '
                + formatRam(summary.activeRamB);
            if (typeof summary.free8KB === 'number') {
                text += '    ' + '可用 DRAM:' + ' ' + summary.free8KB + ' KB free /' + '\n'
                    + (summary.largest8KB || 0) + ' KB 最大块';
            }
            this._baselineSummary = '    (当前' + text + ')';
            var el = document.getElementById('features-summary');
            if (el) el.textContent = text;
            // 填充 RAM 监控卡片
            var free8El = document.getElementById('ram-free8');
            var largest8El = document.getElementById('ram-largest8');
            var activeEl = document.getElementById('ram-active');
            if (free8El && typeof summary.free8KB === 'number') free8El.textContent = summary.free8KB + 'KB';
            if (largest8El && typeof summary.largest8KB === 'number') largest8El.textContent = summary.largest8KB + 'KB';
            if (activeEl && typeof summary.activeRamB === 'number') {
                activeEl.textContent = (summary.activeRamB / 1024).toFixed(1) + 'KB';
            }
        },

        // 功能配置的备份/迁移统一走设备配置页（高级配置）的配置转移模块，
        // 本页不再提供重复的导入/导出入口

        applyFeatureConfig() {
            if (!this._featureDirty) return;
            
            // WP-91: Web 控制面自锁保护 - 检查是否关闭了 webServer
            const willDisableWebServer = (this._featureState || []).some(f => 
                f.key === 'webServer' && !f.userEnabled
            );
            
            if (willDisableWebServer) {
                const confirmMsg = t('fc-webserver-disable-warning', 
                    '警告：关闭 Web 服务后将失去通过浏览器配置设备的能力！\n\n' +
                    '恢复方式：\n' +
                    '1. 访问 http://fastbee.local (mDNS) 或 IP 直连进入 AP 配置模式\n' +
                    '2. 通过外设执行规则 ACTION_FEATURE_ENABLE 重新启用\n' +
                    '3. 或连接串口发送 AT 命令恢复\n\n' +
                    '确定要关闭 Web 服务吗？');
                
                if (!confirm(confirmMsg)) {
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
            if (saveBtn) { saveBtn.disabled = true; saveBtn.textContent = t('fc-saving', '保存中...'); }
            // 后端 features/* POST 通过 onData 回调累积原始 body 并 deserializeJson：
            // 必须用 JSON Content-Type（apiPostJson）；apiPost 的表单编码 body
            // 走参数解析路径不触发 onData，会得到空 body 返回 400 Missing JSON body
            apiPostJson('/api/features/apply', body)
                .then(function(res) {
                    if (res && res.success) {
                        var secs = (res.data && res.data.rebootIn) || 3;
                        Notification.success(t('fc-save-success', '配置已保存，设备将在') + ' ' + secs + ' ' + t('fc-save-success-suffix', '秒后重启生效'));
                        // 重启后刷新页面加载最新状态
                        setTimeout(function() { location.reload(); }, (secs + 5) * 1000);
                    } else {
                        Notification.error((res && (res.error || res.message)) || t('common-save-fail', '保存失败'));
                        if (saveBtn) { saveBtn.disabled = false; saveBtn.textContent = t('fc-save-reboot', '保存并重启'); }
                    }
                })
                .catch(function() {
                    Notification.error(t('common-save-retry', '保存失败，请重试'));
                    if (saveBtn) { saveBtn.disabled = false; saveBtn.textContent = t('fc-save-reboot', '保存并重启'); }
                });
        },

        // ========== 开发模式保护 ==========

        applyDeveloperModeState(scope) {
            var disabled = !AppState.isDeveloperModeEnabled();
            var tip = AppState.getDeveloperModeDisabledText();
            
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
            document.querySelectorAll('[data-feature-toggle]').forEach(function(input) {
                input.disabled = disabled;
                input.parentElement.classList.toggle('dev-mode-locked', disabled);
                if (disabled) input.title = tip;
                else input.removeAttribute('title');
            });
            
            // 显示/隐藏开发模式提示条
            var devHint = document.getElementById('feature-dev-mode-hint');
            if (devHint) devHint.style.display = disabled ? 'block' : 'none';
        }
    });
})();
