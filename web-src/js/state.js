// ============================================================
// AppState: 核心应用状态管理
// 依赖: request-governor.js, notification.js, page-loader.js, module-loader.js
// 子模块: state-theme.js, state-session.js, state-sse.js, state-ui.js
// ============================================================

// escapeHtml 已由 utils.js 全局提供 (window.escapeHtml)，无需重复定义

// 应用状态管理
var AppState = typeof AppState !== 'undefined' ? AppState : {
    currentPage: 'dashboard',
    configTab: 'modbus',
    currentUser: { name: '', role: '', canManageFs: false },
    sidebarCollapsed: false,
    _logAutoRefreshTimer: null,  // 日志自动刷新定时器

    // SSE 连接管理（属性由 state-sse.js 注册）

    // 页面模块动态加载器 — 映射表保留在 AppState，加载逻辑委托给 PageLoader/ModuleLoader
    _pageMapping: {
        'dashboard-page': 'dashboard',
        'protocol-page': 'protocol',
        'network-page': 'network',
        'device-page': 'device',
        'data-page': 'admin',
        'users-page': 'admin',
        'roles-page': 'admin',
        'logs-page': 'admin',
        'peripheral-page': 'peripheral',
        'periph-exec-page': 'peripheral',
        'device-control-page': 'peripheral',
        'rule-script-page': 'rule-script'
    },

    // ============ 委托给 PageLoader ============
    loadPage(pageId) {
        return PageLoader.loadPage(pageId, this._pageMapping);
    },

    _loadModals() {
        return PageLoader.loadModals();
    },

    // ============ 委托给 ModuleLoader ============
    registerModule(name, methods) {
        ModuleLoader.registerModule(name, methods, this);
    },

    _loadModule(name, callback) {
        ModuleLoader.loadModule(name, callback);
    },

    // ============ URL 参数处理 ============
    _handleUrlParams() {
        var params = new URLSearchParams(window.location.search);
        var targetPage = params.get('page');
        var goFullscreen = params.get('fullscreen') === '1';
        if (targetPage) {
            this._pendingUrlPage = targetPage;
            this._pendingUrlFullscreen = goFullscreen;
            // 清除 URL 参数，避免刷新时重复触发
            var cleanUrl = window.location.pathname;
            window.history.replaceState({}, '', cleanUrl);
        }
    },

    // 在页面加载后执行 URL 参数指定的跳转和全屏
    _applyUrlParams() {
        if (!this._pendingUrlPage) return;
        var page = this._pendingUrlPage;
        var goFullscreen = this._pendingUrlFullscreen;
        delete this._pendingUrlPage;
        delete this._pendingUrlFullscreen;
        // 跳转到指定页面
        this.changePage(page);
        // 延迟进入全屏（等待页面和模块加载完成）
        if (goFullscreen && page === 'device-control') {
            setTimeout(function() {
                if (typeof AppState._dcToggleFullscreen === 'function') {
                    AppState._dcToggleFullscreen();
                }
            }, 1500);
        }
    },

    // ============ 初始化 ============
    init() {
        this.setupTheme();  // 主题初始化
        this.setupUserDropdown(); // 用户下拉菜单
        this.setupSidebarToggle();
        this.setupLanguage();
        this.setupConfigTabs();
        this.setupEventListeners();
        this.setupGlobalEventDelegation(); // 全局事件委托
        this._handleUrlParams(); // 处理 URL 参数（新标签页全屏等）
        this.refreshPage();
    },

    // ============ 全局事件委托 ============
    setupGlobalEventDelegation() {
        const self = this;
        // 全局事件委托 - 替代内联 onclick
        document.addEventListener('click', (e) => {
            // 处理 config-tab 点击（动态加载页面的 tab 切换）
            var tab = e.target.closest('.config-tab');
            if (tab) {
                // 禁用态 Tab 不可切换
                if (tab.classList.contains('tab-disabled')) return;
                var pageEl = tab.closest('[id$="-page"]');
                if (pageEl) {
                    var tabId = tab.getAttribute('data-tab');
                    self.showConfigTab(pageEl.id, tabId);
                }
                return;
            }
    
            const el = e.target.closest('[data-action]');
            if (!el) return;
    
            const action = el.dataset.action;
            const args = el.dataset.args ? el.dataset.args.split(',') : [];
    
            // 优先在 AppState 上查找方法，如果不存在则在 app 对象上查找
            if (typeof this[action] === 'function') {
                e.preventDefault();
                this[action](...args);
            } else if (typeof window.app !== 'undefined' && typeof window.app[action] === 'function') {
                e.preventDefault();
                window.app[action](...args);
            } else {
                // 方法尚未注册（模块可能还在加载中），延迟重试一次
                e.preventDefault();
                setTimeout(function() {
                    if (typeof self[action] === 'function') {
                        self[action](...args);
                    } else if (typeof window.app !== 'undefined' && typeof window.app[action] === 'function') {
                        window.app[action](...args);
                    }
                }, 800);
            }
        });

        // 全局 change 事件委托 - 替代内联 onchange
        document.addEventListener('change', (e) => {
            const el = e.target.closest('[data-change-action]');
            if (!el) return;
    
            const action = el.dataset.changeAction;
            const value = el.value;
    
            // 优先在 AppState 上查找方法，如果不存在则在 app 对象上查找
            if (typeof this[action] === 'function') {
                this[action](value);
            } else if (typeof window.app !== 'undefined' && typeof window.app[action] === 'function') {
                window.app[action](value);
            }
        });
    
        // 全局 submit 事件委托 - 处理协议页面表单提交
        document.addEventListener('submit', (e) => {
            const form = e.target.closest('form');
            if (!form) return;
    
            // 仅处理协议页面的表单
            const protocolForms = ['mqtt-form', 'modbus-rtu-form', 'modbus-tcp-form', 'http-form', 'coap-form', 'tcp-form'];
            if (protocolForms.includes(form.id)) {
                e.preventDefault();
                if (typeof self.saveProtocolConfig === 'function') {
                    self.saveProtocolConfig(form.id);
                }
            }
        });
    
        // 全局 input/change 事件委托 - 清除 MQTT 测试结果提示
        ['input', 'change'].forEach(function(eventType) {
            document.addEventListener(eventType, function(e) {
                const targetId = e.target.id;
                if (targetId === 'mqtt-client-id' || targetId === 'mqtt-auth-type') {
                    var resultEl = document.getElementById('mqtt-test-result');
                    if (resultEl) resultEl.textContent = '';
                }
            });
        });
    },

    // 触发 OTA 文件选择
    triggerOtaFileSelect() {
        const fileInput = document.getElementById('ota-file');
        if (fileInput) fileInput.click();
    },

    // ============ UI 工具方法（由 state-ui.js 注册） ============
    // getEl, showElement, hideElement, showModal, hideModal,
    // showInlineError, clearInlineError, renderEmptyTableRow, toggleVisible,
    // setLoading, restoreButton, renderBadge, renderPagination,
    // setExclusiveActive, setupUserDropdown, toggleSection,
    // setupSidebarToggle, toggleSidebar, collapseSidebar, expandSidebar

    // ============ 主题管理（由 state-theme.js 注册） ============

    // ============ 会话管理（由 state-session.js 注册） ============

    // ============ 语言 ============
    setupLanguage() {
        // 初始化时立即应用 i18n 到登录页（在登录前就需要正确显示）
        i18n.updatePageText();

        // 登录页语言切换
        const loginLangSelect = document.getElementById('login-language-select');
        if (loginLangSelect) {
            loginLangSelect.value = i18n.currentLang;
            loginLangSelect.addEventListener('change', e => {
                i18n.setLanguage(e.target.value);
                // 同步主应用的语言选择器
                const mainSelect = document.getElementById('language-select');
                if (mainSelect) mainSelect.value = e.target.value;
            });
        }

        // 主应用语言切换
        const langSelect = document.getElementById('language-select');
        if (langSelect) {
            langSelect.value = i18n.currentLang;
            langSelect.addEventListener('change', e => {
                i18n.setLanguage(e.target.value);
                // 同步登录页的语言选择器
                if (loginLangSelect) loginLangSelect.value = e.target.value;
                // 仅刷新当前页的动态 i18n 内容，避免额外预加载其它模块
                this._refreshCurrentPageLocalizedContent();
            });
        }
    },

    _getPageModuleMap() {
        return {
            dashboard: 'dashboard',
            network: 'network',
            device: 'device-config',
            users: 'users',
            roles: 'roles',
            peripheral: 'peripherals',
            'periph-exec': 'periph-exec',
            protocol: 'protocol',
            'rule-script': 'rule-script',
            data: 'files',
            logs: 'logs',
            'device-control': 'device-control'
        };
    },

    _getPageLoaders() {
        return {
            dashboard: () => { this.renderDashboard(); this.loadSystemMonitor(); },
            network: () => { this.loadNetworkConfig(); },
            device: () => { this.loadDeviceConfig(); },
            users: () => { this.loadUsers(); },
            roles: () => { this.loadRoles(); },
            peripheral: () => { this.loadPeripherals(); },
            'periph-exec': () => { this.loadPeriphExecPage(); },
            protocol: () => { this.loadProtocolConfig('mqtt'); if (this._startMqttStatusPolling) this._startMqttStatusPolling(); },
            'rule-script': () => { this.loadRuleScriptPage(); },
            data: () => { if (typeof this.setupFilesEvents === 'function' && !this._filesEventsBound) { this.setupFilesEvents(); this._filesEventsBound = true; } this.loadFileTree(this._currentDir || '/'); this.loadFileSystemInfo(); },
            logs: () => {
                if (!this._currentLogFile) {
                    this._currentLogFile = 'system.log';
                }
                const currentSpan = document.getElementById('current-log-file');
                if (currentSpan) currentSpan.textContent = i18n.t('log-current-file-prefix') + this._currentLogFile;
                var self = this;
                // 延迟初始加载，让 ESP32 完成静态文件传输
                setTimeout(function() {
                    self.loadLogFileList();
                    self.loadLogs();
                    var autoRefresh = document.getElementById('log-auto-refresh');
                    if (autoRefresh && autoRefresh.checked) {
                        self.startLogAutoRefresh();
                    }
                }, 500);
            },
            'device-control': () => {
                if (typeof this.loadDeviceControlPage === 'function') {
                    this.loadDeviceControlPage();
                } else {
                    console.warn('[changePage] loadDeviceControlPage not available, module may not be loaded');
                    var content = document.getElementById('dc-content');
                    if (content) {
                        content.innerHTML = '';
                        var loadDiv = document.createElement('div');
                        loadDiv.className = 'dc-empty';
                        loadDiv.textContent = i18n.t('loading');
                        content.appendChild(loadDiv);
                        var self = this;
                        setTimeout(function() {
                            if (typeof self.loadDeviceControlPage === 'function') {
                                self.loadDeviceControlPage();
                            } else {
                                content.innerHTML = '';
                                var errDiv = document.createElement('div');
                                errDiv.className = 'dc-empty u-text-danger';
                                errDiv.textContent = i18n.t('module-load-fail') || '模块加载失败，请刷新页面重试';
                                content.appendChild(errDiv);
                            }
                        }, 1000);
                    } else {
                        console.error('[changePage] dc-content element not found!');
                    }
                }
            }
        };
    },

    _refreshCurrentPageLocalizedContent() {
        const loader = this._getPageLoaders()[this.currentPage];
        if (typeof loader === 'function') {
            loader();
        }
    },

    // ============ 配置选项卡 ============
    // 注意：tab 切换已通过 setupGlobalEventDelegation() 中的事件委托处理
    // 此方法保留用于其他静态页面的初始化（目前为空）
    setupConfigTabs() {
        // Tab 切换已移至全局事件委托 (setupGlobalEventDelegation)
        // 协议表单提交、MQTT 按钮等已在各自的模块 JS 中处理
    },
    

    _getProtocolName(formId) {
        const map = { 'modbus-rtu': 'Modbus RTU', 'modbus-tcp': 'Modbus TCP', mqtt: 'MQTT', http: 'HTTP', coap: 'CoAP' };
        for (const key of Object.keys(map)) {
            if (formId.includes(key)) return map[key];
        }
        return 'TCP';
    },

    showConfigTab(pageId, tabId) {
        const page = document.getElementById(pageId);
        if (!page) return;
        page.querySelectorAll('.config-tab').forEach(t => {
            t.classList.toggle('active', t.getAttribute('data-tab') === tabId);
        });
        page.querySelectorAll('.config-content').forEach(c => c.classList.remove('active'));
        const target = page.querySelector(`#${tabId}`);
        if (target) target.classList.add('active');
        
        // 切换到协议配置页面时自动加载配置
        if (pageId === 'protocol-page') {
            if (typeof this.loadProtocolConfig === 'function') this.loadProtocolConfig(tabId);
        }
        
        // 切换到设备监控页面时自动加载网络状态
        if (pageId === 'dashboard-page') {
            if (typeof this.loadNetworkStatus === 'function') this.loadNetworkStatus();
        }
        // 切换到NTP时间tab时自动加载时间
        if (pageId === 'device-page' && tabId === 'dev-ntp') {
            if (typeof this.loadDeviceTime === 'function') this.loadDeviceTime();
        }
        // 切换到基本信息tab时自动加载硬件信息
        if (pageId === 'device-page' && tabId === 'dev-basic') {
            if (typeof this._loadDeviceHardwareInfo === 'function') this._loadDeviceHardwareInfo();
        }
        // 切换到OTA升级tab时自动加载OTA状态
        if (pageId === 'device-page' && tabId === 'dev-ota') {
            var selfOta = this;
            PageLoader.loadFragment('dev-ota-fragment-container', 'device-ota', function() {
                if (typeof selfOta.loadOtaStatus === 'function') selfOta.loadOtaStatus();
            });
        }
    },

    // ============ 事件绑定（仅通用事件） ============
    setupEventListeners() {
        // 登录表单
        const loginForm = document.getElementById('login-form');
        if (loginForm) loginForm.addEventListener('submit', e => { e.preventDefault(); this.handleLogin(); });

        // 菜单
        document.querySelectorAll('.menu-item').forEach(item => {
            item.addEventListener('click', e => { e.preventDefault(); this.changePage(item.dataset.page); });
        });

        // 修改密码
        const changePwdBtn = document.getElementById('change-password-btn');
        if (changePwdBtn) changePwdBtn.addEventListener('click', () => this.showChangePasswordModal());

        // 退出登录
        const logoutBtn = document.getElementById('logout-btn');
        if (logoutBtn) logoutBtn.addEventListener('click', () => this.logout());

        // 模态窗关闭
        const closeId = (id) => { this.hideModal(id); };
        ['close-password-modal', 'cancel-password-btn'].forEach(id => {
            const el = document.getElementById(id);
            if (el) el.addEventListener('click', () => closeId('change-password-modal'));
        });

        // 确认修改密码
        const confirmPwd = document.getElementById('confirm-password-btn');
        if (confirmPwd) confirmPwd.addEventListener('click', () => this.changePassword());

        // 移动端侧边栏：点击菜单项后自动收起
        document.querySelectorAll('.menu-item').forEach(item => {
            item.addEventListener('click', () => {
                const sidebar = document.getElementById('sidebar');
                if (sidebar && window.innerWidth <= 768 && sidebar.classList.contains('expanded')) {
                    sidebar.classList.remove('expanded');
                    this.sidebarCollapsed = true;
                    const btn = document.getElementById('sidebar-toggle');
                    if (btn) btn.textContent = '☰';
                }
            });
        });
    },

    // ============ 登录（由 state-session.js 注册） ============

    // ============ 页面切换 ============
    async changePage(page) {
        // 取消旧页面排队中的请求，释放 ESP32 资源
        if (typeof window.apiAbortPageRequests === 'function') {
            window.apiAbortPageRequests();
        }

        const pageAlias = { monitor: 'dashboard' };
        const normalizedPage = pageAlias[page] || page;

        // 先确保页面HTML已加载
        await this.loadPage(normalizedPage + '-page');

        // 对新加载的页面应用 i18n 翻译
        if (typeof i18n !== 'undefined' && i18n.updatePageText) {
            i18n.updatePageText();
        }

        document.querySelectorAll('.page').forEach(p => p.classList.remove('active'));
        const target = document.getElementById(normalizedPage + '-page');
        if (!target) {
            console.warn('[changePage] target page not found:', page, '=>', normalizedPage);
            return;
        }
        target.classList.add('active');

        document.querySelectorAll('.menu-item').forEach(item => {
            item.classList.toggle('active', item.dataset.page === normalizedPage);
        });

        const titleKey = `page-title-${normalizedPage}`;
        const titleEl = document.getElementById('page-title');
        if (titleEl) titleEl.textContent = i18n.t(titleKey);

        this.currentPage = normalizedPage;

        // 模块名到页面的映射
        const pageModuleMap = this._getPageModuleMap();
        const pageLoaders = this._getPageLoaders();

        if (normalizedPage !== 'logs' && this._logAutoRefreshTimer) {
            clearInterval(this._logAutoRefreshTimer);
            this._logAutoRefreshTimer = null;
        }

        if (normalizedPage !== 'device-control' && typeof this._dcStopAllAutoRefresh === 'function') {
            this._dcStopAllAutoRefresh();
        }

        // 停止协议页面的所有轮询（如果离开协议页面）
        if (normalizedPage !== 'protocol') {
            // 停止 MQTT 状态轮询
            if (typeof this._stopMqttStatusPolling === 'function') {
                this._stopMqttStatusPolling();
            }
            // 停止 Modbus 主站状态轮询
            if (typeof this._stopMasterStatusRefresh === 'function') {
                this._stopMasterStatusRefresh();
            }
            // 停止 Modbus 线圈自动刷新
            if (this._coilAutoRefreshTimer) {
                clearInterval(this._coilAutoRefreshTimer);
                this._coilAutoRefreshTimer = null;
            }
            // 停止 PID 自动刷新
            if (typeof this._stopPidAutoRefresh === 'function') {
                this._stopPidAutoRefresh();
            }
        }

        const moduleName = pageModuleMap[normalizedPage];
        const loader = pageLoaders[normalizedPage];
        
        if (moduleName && loader) {
            this._loadModule(moduleName, loader);
        } else if (loader) {
            loader();
        } else {
            console.warn('[changePage] no page loader mapped for:', normalizedPage);
        }
    },

    // ============ 修改密码 / 退出登录（由 state-session.js 注册） ============

    // ============ 关闭所有浮层（token 过期时调用）============
    closeAllOverlays() {
        // 1. 关闭所有 .modal 弹窗并清理事件
        document.querySelectorAll('.modal').forEach(function (m) {
            // 清理遮罩点击事件
            if (m._modalOverlayHandler) {
                m.removeEventListener('click', m._modalOverlayHandler);
                m._modalOverlayHandler = null;
            }
            // 清理关闭按钮事件
            const closeBtn = m.querySelector('.modal-close, .modal-close-btn');
            if (closeBtn && closeBtn._modalCloseHandler) {
                closeBtn.removeEventListener('click', closeBtn._modalCloseHandler);
                closeBtn._modalCloseHandler = null;
            }
            m.style.display = 'none';
        });

        // 2. 关闭动态创建的全屏浮层（如角色权限详情弹窗）
        document.querySelectorAll('div[style*="position: fixed"][style*="z-index"]').forEach(function (el) {
            // 排除 Notification 容器等非弹窗元素
            if (el.querySelector('.modal-content, [onclick*="remove"]')) {
                el.remove();
            }
        });

        // 3. 关闭用户下拉菜单
        document.querySelectorAll('.user-dropdown.open').forEach(function (d) {
            d.classList.remove('open');
        });

        // 4. 停止日志自动刷新定时器
        if (typeof this.stopLogAutoRefresh === 'function') this.stopLogAutoRefresh();

        // 5. 停止 MQTT 状态轮询（如有）
        if (typeof this._stopMqttStatusPolling === 'function') {
            this._stopMqttStatusPolling();
        }
    },

    // ============ 工具方法 ============
    getAuthHeader() {
        const token = localStorage.getItem('auth_token');
        return token ? { 'Authorization': `Bearer ${token}` } : {};
    },

    /**
     * 设置文本内容
     */
    _setText(id, text) {
        const el = document.getElementById(id);
        if (el) el.textContent = text;
    },

    /**
     * 设置 HTML 内容
     */
    _setHtml(id, html) {
        const el = document.getElementById(id);
        if (el) el.innerHTML = html;
    },

    /**
     * 设置进度条宽度
     */
    _setBar(id, percent) {
        const el = document.getElementById(id);
        if (el) el.style.width = Math.min(percent, 100) + '%';
    },

    /**
     * 格式化字节数（委托给 utils.js 全局 formatBytes）
     */
    _formatBytes(bytes) {
        return window.formatBytes(bytes);
    },

    /**
     * 设置表单元素的值（支持 input, select, textarea）
     */
    _setValue(id, value) {
        const el = document.getElementById(id);
        if (el) el.value = value;
    },

    /**
     * 设置复选框的选中状态
     */
    _setCheckbox(id, checked) {
        const el = document.getElementById(id);
        if (el) el.checked = !!checked;
    },

    /**
     * 设置复选框的选中状态（_setCheckbox 的别名）
     */
    _setChecked(id, checked) {
        this._setCheckbox(id, checked);
    },

    /**
     * 显示/隐藏消息提示元素
     */
    _showMessage(id, show) {
        const el = document.getElementById(id);
        if (!el) return;
        el.classList.toggle('is-hidden', !show);
        el.style.display = show ? '' : 'none';
    },

    /**
     * 设置元素的文本内容（与 _setText 功能相同，提供兼容性别名）
     */
    _setTextContent(id, text) {
        this._setText(id, text);
    }

    // ============ SSE 连接管理（由 state-sse.js 注册） ============
};
