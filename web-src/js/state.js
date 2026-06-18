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
    currentUser: { name: '' },
    developerModeEnabled: true,
    sidebarCollapsed: false,
    _pageNavSeq: 0,

    // SSE 连接管理（属性由 state-sse.js 注册）

    // 页面模块动态加载器 — 映射表保留在 AppState，加载逻辑委托给 PageLoader/ModuleLoader
    _pageMapping: {
        'dashboard-page': 'dashboard',
        'protocol-page': 'protocol',
        'network-page': 'network',
        'device-page': 'device',
        'peripheral-page': 'peripheral',
        'periph-exec-page': 'peripheral',
        'device-control-page': 'peripheral',
        'logs-page': 'logs',
        'data-page': 'admin',
        'users-page': 'admin',
        'rule-script-page': 'rule-script'
    },

    // ============ 委托给 PageLoader ============
    loadPage(pageId) {
        return PageLoader.loadPage(pageId, this._pageMapping);
    },

    _loadModals() {
        var self = this;
        return PageLoader.loadModals().then(function() {
            // 模态窗 DOM 加载后重新绑定事件（setupEvents 调用时这些元素还不存在）
            var closeId = function(id) { self.hideModal(id); };
            ['close-password-modal', 'cancel-password-btn'].forEach(function(id) {
                var el = document.getElementById(id);
                if (el && !el._modalBound) {
                    el._modalBound = true;
                    el.addEventListener('click', function() { closeId('change-password-modal'); });
                }
            });
            var confirmPwd = document.getElementById('confirm-password-btn');
            if (confirmPwd && !confirmPwd._modalBound) {
                confirmPwd._modalBound = true;
                confirmPwd.addEventListener('click', function() { self.changePassword(); });
            }
            // 通知各模块重新绑定模态窗内的事件（模态窗 DOM 此时已就绪）
            self._rebindAllModalEvents();
            self._modalsLoaded = true;
        });
    },

    /**
     * 模态窗 DOM 加载完成后，通知所有已注册模块重新绑定模态内事件。
     * 各模块通过 _registerModalBinder() 注册绑定函数。
     */
    _modalBinders: [],
    _modalsLoaded: false,
    _registerModalBinder(name, fn) {
        if (typeof fn !== 'function') return;
        this._modalBinders.push({ name: name, fn: fn });
        // If modals DOM already loaded, bind immediately to avoid race condition
        // (each binder has its own guard to prevent duplicate binding)
        if (this._modalsLoaded) {
            try { fn.call(this); } catch (e) {
                console.warn('[ModalBind] late-bind ' + name + ' failed:', e);
            }
        }
    },
    _rebindAllModalEvents() {
        var self = this;
        this._modalBinders.forEach(function(binder) {
            try { binder.fn.call(self); } catch (e) {
                console.warn('[ModalBind] ' + binder.name + ' failed:', e);
            }
        });
    },

    // ============ 委托给 ModuleLoader ============
    registerModule(name, methods) {
        ModuleLoader.registerModule(name, methods, this);
    },

    _loadModule(name, callback) {
        ModuleLoader.loadModule(name, callback);
    },

    isDeveloperModeEnabled() {
        return this.developerModeEnabled !== false;
    },

    setDeveloperModeState(enabled) {
        this.developerModeEnabled = enabled !== false;
        document.documentElement.classList.toggle('developer-mode-disabled', !this.developerModeEnabled);
        this.applyDeveloperModeState();
    },

    _refreshDeveloperModeState(options) {
        options = options || {};
        var getter;
        if (options.silent && options.noCache && typeof apiGetSilentFresh === 'function') {
            getter = apiGetSilentFresh;
        } else if (options.noCache && typeof apiGetFresh === 'function') {
            getter = apiGetFresh;
        } else if (options.silent && typeof apiGetSilent === 'function') {
            getter = apiGetSilent;
        } else {
            getter = apiGet;
        }
        return getter('/api/device/config')
            .then((res) => {
                if (res && res.success && res.data) {
                    this.setDeveloperModeState(res.data.developerModeEnabled !== false);
                }
                return this.developerModeEnabled;
            })
            .catch((err) => {
                if (!options.silent) console.warn('[developer-mode] load failed:', err);
                return this.developerModeEnabled;
            });
    },

    getDeveloperModeDisabledText() {
        return '开发环境已禁用，请到设备配置的高级配置中启用后再修改。';
    },

    guardDeveloperModeAction() {
        if (this.isDeveloperModeEnabled()) return true;
        if (typeof Notification !== 'undefined') {
            Notification.warning(this.getDeveloperModeDisabledText(), '开发环境');
        }
        return false;
    },

    applyDeveloperModeState(root) {
        var scope = root && root.querySelectorAll ? root : document;
        var disabled = !this.isDeveloperModeEnabled();
        var tip = this.getDeveloperModeDisabledText();
        [
            '#add-peripheral-btn',
            '#periph-exec-page-add-btn',
            '[data-pe-action="edit"]',
            '[data-pe-action="toggle"]',
            '[data-pe-action="delete"]',
            '[data-peripheral-action="edit"]',
            '[data-peripheral-action="toggle"]',
            '[data-peripheral-action="delete"]',
            '[data-action="_showAddDeviceMenu"]',
            '#modbus-rtu-form button[type="submit"]',
            '[data-action="_saveEditModal"]',
            '[data-action="_saveTaskEditModal"]',
            '[data-action="saveMappingModal"]',
            '[data-action="addMapping"]',
            '.protocol-mapping-remove',
            '.protocol-action-btn[data-protocol-action="edit-device"]',
            '.protocol-action-btn[data-protocol-action="open-mapping"]',
            '.protocol-action-btn[data-protocol-action="delete-device"]',
            '.protocol-action-btn[data-protocol-action="open-edit-modal"]',
            '.protocol-action-btn[data-protocol-action="remove-device"]'
        ].forEach(function(selector) {
            scope.querySelectorAll(selector).forEach(function(el) {
                var resourceLocked = el.getAttribute('data-resource-locked') === 'true';
                el.disabled = disabled || resourceLocked;
                el.classList.toggle('dev-mode-locked', disabled);
                if (disabled) el.title = tip;
                else if (!resourceLocked) el.removeAttribute('title');
            });
        });
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
            const protocolForms = ['mqtt-form', 'modbus-rtu-form'];
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
        // 非 Full 版本默认中文（不覆盖已有偏好）
        if (!localStorage.getItem('language')) {
            localStorage.setItem('language', 'zh-CN');
        }
        if (typeof i18n !== 'undefined') {
            i18n.currentLang = localStorage.getItem('language') || 'zh-CN';
        }

        // 通过 capabilities API 检测是否支持多语言（Full版本）
        var self = this;
        var getter = (typeof apiGetSilentFresh === 'function') ? apiGetSilentFresh
            : (typeof apiGetFresh === 'function' ? apiGetFresh : (typeof apiGet === 'function' ? apiGet : null));
        if (!getter) return;
        getter('/api/system/capabilities').then(function(res) {
            if (!res || !res.success || !res.data || !res.data.i18n) return;
            // Full 版本启用语言切换
            var langSelect = document.getElementById('language-select');
            if (!langSelect) return;
            langSelect.classList.remove('fb-hidden');
            // 恢复用户上次选择的语言
            var savedLang = localStorage.getItem('language') || 'zh-CN';
            langSelect.value = savedLang;
            if (savedLang !== 'zh-CN' && typeof i18n !== 'undefined') {
                i18n.setLanguage(savedLang);
            }
            // 绑定切换事件
            langSelect.addEventListener('change', function() {
                var lang = langSelect.value;
                if (typeof i18n !== 'undefined') {
                    i18n.setLanguage(lang);
                }
            });
        }).catch(function() { /* 静默失败，保持中文 */ });
    },

    _getPageModuleMap() {
        return {
            dashboard: 'dashboard',
            network: 'network',
            device: 'device-config',
            peripheral: 'peripherals',
            'periph-exec': 'periph-exec',
            protocol: 'protocol',
            'device-control': 'device-control',
            logs: 'logs',
            data: 'files',
            users: 'users',
            'rule-script': 'rule-script'
        };
    },

    _getPageLoaders() {
        return {
            dashboard: () => {
                var self = this;
                if (typeof this.bootDashboardPage === 'function') {
                    this.bootDashboardPage();
                } else {
                    this.renderDashboard();
                    setTimeout(function() {
                        self.loadSystemMonitor();
                    }, 120);
                }
            },
            network: () => { this.loadNetworkConfig(); },
            device: () => { this.loadDeviceConfig({ deferHardware: true }); },
            peripheral: () => { this.loadPeripherals(); },
            'periph-exec': () => { this.loadPeriphExecPage(); },
            protocol: () => { this.loadProtocolConfig('mqtt'); },
            logs: () => { this.loadLogsPage({ noCache: true }); },
            data: () => { this.loadFileSystemInfo({ noCache: true }); this.loadFileTree('/', { noCache: true }); },
            users: () => { this.loadUsers({ noCache: true }); },
            'rule-script': () => { this.loadRuleScriptPage({ noCache: true }); },
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
                        loadDiv.textContent = '加载中...';
                        content.appendChild(loadDiv);
                        var self = this;
                        setTimeout(function() {
                            if (typeof self.loadDeviceControlPage === 'function') {
                                self.loadDeviceControlPage();
                            } else {
                                content.innerHTML = '';
                                var errDiv = document.createElement('div');
                                errDiv.className = 'dc-empty u-text-danger';
                                errDiv.textContent = '模块加载失败，请刷新页面重试';
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

    // ============ 配置选项卡 ============
    // 注意：tab 切换已通过 setupGlobalEventDelegation() 中的事件委托处理
    // 此方法保留用于其他静态页面的初始化（目前为空）
    setupConfigTabs() {
        // Tab 切换已移至全局事件委托 (setupGlobalEventDelegation)
        // 协议表单提交、MQTT 按钮等已在各自的模块 JS 中处理
    },
    

    _getProtocolName(formId) {
        const map = { 'modbus-rtu': 'Modbus RTU', mqtt: 'MQTT' };
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
            if (typeof this.loadNetworkStatus === 'function') this.loadNetworkStatus({ noCache: true });
        }
        // 切换到NTP时间tab时自动加载时间
        if (pageId === 'device-page' && tabId === 'dev-ntp') {
            if (typeof this.loadDeviceTime === 'function') this.loadDeviceTime({ noCache: true });
        }
        // 切换到基本信息tab时自动加载硬件信息
        if (pageId === 'device-page' && tabId === 'dev-basic') {
            if (typeof this._loadDeviceHardwareInfo === 'function') this._loadDeviceHardwareInfo({ noCache: true });
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
        const navSeq = (this._pageNavSeq || 0) + 1;
        this._pageNavSeq = navSeq;
        // 取消旧页面排队中的请求，释放 ESP32 资源
        if (typeof window.apiAbortPageRequests === 'function') {
            window.apiAbortPageRequests();
        }

        const pageAlias = { monitor: 'dashboard' };
        const normalizedPage = pageAlias[page] || page;

        // 先确保页面HTML已加载
        await this.loadPage(normalizedPage + '-page');
        if (navSeq !== this._pageNavSeq) return;

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
        var _pageTitleMap = {
            'page-title-dashboard': '设备监控仪表盘',
            'page-title-protocol': '通信协议',
            'page-title-network': '网络设置',
            'page-title-device': '设备配置',
            'page-title-data': '文件管理',
            'page-title-system': '系统监控',
            'page-title-logs': '设备日志',
            'page-title-gpio': 'GPIO配置',
            'page-title-peripheral': '外设配置',
            'page-title-periph-exec': '外设执行',
            'page-title-users': '用户管理',
            'page-title-device-control': '设备大屏'
        };
        if (titleEl) titleEl.textContent = _pageTitleMap[titleKey] || normalizedPage;

        this.currentPage = normalizedPage;

        // 模块名到页面的映射
        const pageModuleMap = this._getPageModuleMap();
        const pageLoaders = this._getPageLoaders();

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
        const runLoader = () => {
            if (navSeq !== this._pageNavSeq || this.currentPage !== normalizedPage) return;
            loader();
        };
        
        if (moduleName && loader) {
            this._loadModule(moduleName, runLoader);
        } else if (loader) {
            runLoader();
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

        // 2. 关闭动态创建的全屏浮层
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
