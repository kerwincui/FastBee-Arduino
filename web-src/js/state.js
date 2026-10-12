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
        'screen-panel-page': 'screen-panel',
        'device-control-page': 'peripheral',
        'logs-page': 'logs',
        'users-page': 'admin',
        'rule-script-page': 'rule-script',
        'features-page': 'features'
    },

    // ============ 委托给 PageLoader ============
    loadPage(pageId, options) {
        return PageLoader.loadPage(pageId, this._pageMapping, options);
    },

    _loadModals() {
        return PageLoader.loadModals().then(function() {
            AppState._modalsLoaded = true;
        }).catch(function(error) {
            AppState._modalsLoaded = false;
            throw error;
        });
    },

    ensureModal(modalId) {
        var existing = document.getElementById(modalId);
        if (existing) return existing;
        if (typeof PageLoader === 'undefined' || typeof PageLoader.mountModal !== 'function') return null;
        var modal = PageLoader.mountModal(modalId);
        if (modal) this._bindMountedModal(modalId);
        return modal;
    },

    async ensureModalAsync(modalId, expectedNavSeq) {
        var modal = this.ensureModal(modalId);
        if (modal) return modal;
        if (typeof PageLoader === 'undefined' || typeof PageLoader.loadModals !== 'function') {
            throw new Error('Modal loader unavailable: #' + modalId);
        }
        var modalNavSeq = expectedNavSeq === undefined ? (this._pageNavSeq || 0) : expectedNavSeq;
        await this._loadModals();
        // The template request may outlive the page that initiated it. Keep the
        // cached template, but never mount a modal into a newer page context.
        if (modalNavSeq !== (this._pageNavSeq || 0)) return null;
        modal = this.ensureModal(modalId);
        if (!modal) throw new Error('Modal template unavailable: #' + modalId);
        return modal;
    },

    async _ensureModalForAction(modalId) {
        var actionNavSeq = this._pageNavSeq || 0;
        try {
            return await this.ensureModalAsync(modalId, actionNavSeq);
        } catch (error) {
            console.error('[Modal] Failed to load #' + modalId + ':', error);
            if (typeof Notification !== 'undefined' && Notification.error) {
                Notification.error('弹窗加载失败，请稍后重试', '页面加载');
            }
            return null;
        }
    },

    _bindMountedModal(modalId) {
        this._resetModalBindingState(modalId);
        if (modalId === 'change-password-modal') {
            var self = this;
            ['close-password-modal', 'cancel-password-btn'].forEach(function(id) {
                var el = document.getElementById(id);
                if (el) el.addEventListener('click', function() { self.hideModal(modalId); });
            });
            var confirmPwd = document.getElementById('confirm-password-btn');
            if (confirmPwd) confirmPwd.addEventListener('click', function() { self.changePassword(); });
        }
        this._rebindAllModalEvents();
        if (/^(modbus-device-|task-edit-|mapping-)/.test(modalId) &&
            typeof this.setupProtocolEvents === 'function') {
            this.setupProtocolEvents();
        }
    },

    _resetModalBindingState(modalId) {
        if (modalId === 'peripheral-modal' || modalId === 'gpio-modal') this._periphModalBound = false;
        if (modalId === 'periph-exec-modal') this._peModalBound = false;
        if (modalId === 'rule-script-modal') {
            this._ruleScriptModalBound = false;
            this._ruleScriptProtocolBound = false;
        }
        if (modalId === 'add-user-modal') this._userModalBound = false;
    },

    _destroyMountedModals() {
        var self = this;
        Array.prototype.slice.call(document.querySelectorAll('.modal')).forEach(function(modal) {
            self.hideModal(modal);
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
        this._modalBinders = this._modalBinders.filter(function(binder) { return binder.name !== name; });
        this._modalBinders.push({ name: name, fn: fn });
        // Late-loaded modules may register after their modal has been mounted.
        if (this._modalsLoaded && document.querySelector('.modal')) {
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

    // ============ 页面生命周期注册（WP-13/73）============
    // 模块经 onPageHide/onPageShow 注册切页回调（如定时器清理），
    // changePage 时统一触发。同名重复注册覆盖旧回调。
    _pageHideHandlers: [],
    _pageShowHandlers: [],

    onPageHide(name, fn) {
        if (typeof fn !== 'function') return;
        this._pageHideHandlers = this._pageHideHandlers.filter(h => h.name !== name);
        this._pageHideHandlers.push({ name: name, fn: fn });
    },

    onPageShow(name, fn) {
        if (typeof fn !== 'function') return;
        this._pageShowHandlers = this._pageShowHandlers.filter(h => h.name !== name);
        this._pageShowHandlers.push({ name: name, fn: fn });
    },

    _firePageLifecycle(handlers, tag) {
        const self = this;
        handlers.forEach(function (h) {
            try { h.fn.call(self); } catch (e) {
                console.warn('[' + tag + '] ' + h.name + ' handler failed:', e);
            }
        });
    },

    _preparePageMount(page) {
        var bindingFlags = {
            peripheral: ['_periphEventsBound'],
            'periph-exec': ['_peEventsBound'],
            'screen-panel': ['_screenPanelEventsBound'],
            protocol: ['_protocolEventsBound'],
            'device-control': ['_eventsBound'],
            logs: ['_logsEventsBound'],
            users: ['_usersEventsBound'],
            'rule-script': ['_ruleScriptEventsBound'],
            features: ['_featuresEventsBound']
        };
        (bindingFlags[page] || []).forEach((flag) => { this[flag] = false; });
    },

    _rebindMountedPageEvents(page) {
        var setupMethods = {
            dashboard: 'setupDashboardEvents',
            network: 'setupNetworkEvents',
            device: 'setupDeviceConfigEvents',
            peripheral: 'setupPeripheralsEvents',
            'periph-exec': 'setupPeriphExecEvents',
            'screen-panel': '_spSetupEvents',
            protocol: 'setupProtocolEvents',
            'device-control': 'setupDeviceControlEvents',
            logs: 'setupLogsEvents',
            users: 'setupUsersEvents',
            'rule-script': 'setupRuleScriptEvents'
        };
        var method = setupMethods[page];
        if (method && typeof this[method] === 'function') this[method]();
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
        return (typeof i18n !== 'undefined' && i18n.t) ? i18n.t('dc-dev-mode-disabled-tip') : '开发环境已禁用，请到设备配置的高级配置中启用后再修改。';
    },

    guardDeveloperModeAction() {
        if (this.isDeveloperModeEnabled()) return true;
        if (typeof Notification !== 'undefined') {
            var title = (typeof i18n !== 'undefined' && i18n.t) ? i18n.t('dc-dev-mode-title') : '开发环境功能';
            Notification.warning(this.getDeveloperModeDisabledText(), title);
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
            '.remote-resource-write',
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
        if (typeof this.applyFeatureDeveloperModeState === 'function') {
            this.applyFeatureDeveloperModeState(scope);
        }
    },

    // ============ URL 参数处理 ============
    _handleUrlParams() {
        var params = new URLSearchParams(window.location.search);
        var targetPage = params.get('page');
        this._pendingHistoryTab = targetPage === 'screen-panel' && params.get('tab') === 'history';
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
        var navigation = this.changePage(page);
        if (this._pendingHistoryTab) {
            delete this._pendingHistoryTab;
            Promise.resolve(navigation).then(() => {
                if (this.currentPage !== 'screen-panel' || !this._spSwitchTopTab) return;
                this._spSwitchTopTab('sp-config');
                document.querySelector('.sp-history-settings')?.scrollIntoView({ block: 'center' });
            });
        }
        // 页面和功能门控完成后再进入全屏，避免禁用直达页的旧定时器误触发。
        if (goFullscreen && page === 'device-control') {
            var urlNavSeq = this._pageNavSeq;
            Promise.resolve(navigation).then(function() {
                if (urlNavSeq !== AppState._pageNavSeq ||
                    AppState.currentPage !== 'device-control' ||
                    !AppState._featureActive || AppState._featureActive.deviceControl !== true) return;
                AppState._dcFullscreenLaunchTimer = setTimeout(function() {
                    AppState._dcFullscreenLaunchTimer = null;
                    if (urlNavSeq === AppState._pageNavSeq &&
                        AppState.currentPage === 'device-control' &&
                        AppState._featureActive && AppState._featureActive.deviceControl === true &&
                        typeof AppState._dcToggleFullscreen === 'function') {
                        AppState._dcToggleFullscreen();
                    }
                }, 1500);
            });
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
        // Web UI 统一使用中文硬编码，不加载语言包或请求多语言能力。
        if (typeof localStorage !== 'undefined') localStorage.setItem('language', 'zh-CN');
    },

    _getPageModuleMap() {
        return {
            dashboard: 'dashboard',
            network: 'network',
            device: 'device-config',
            peripheral: 'peripherals',
            'periph-exec': 'periph-exec',
            'screen-panel': 'screen-panel',
            protocol: 'protocol',
            'device-control': 'device-control',
            logs: 'logs',
            users: 'users',
            'rule-script': 'rule-script',
            features: 'feature-config'
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
            'screen-panel': () => { this.loadScreenPanelPage(); },
            protocol: () => {
                // 协议页每次进入都刷新轻量功能状态，避免启动期请求失败后
                // 把临时的待定状态长期当成 Modbus 已禁用。
                this.applyProtocolFeatureGate();
                const featureStatus = this.applyFeatureMenuVisibility();
                this.loadProtocolConfig('mqtt');
                return featureStatus;
            },
            logs: () => { this.loadLogsPage({ noCache: true }); },
            users: () => { this.loadUsers({ noCache: true }); },
            'rule-script': () => { this.loadRuleScriptPage({ noCache: true }); },
            features: () => { this.loadFeaturesPage(); },
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
                        loadDiv.textContent = (typeof i18n !== 'undefined' && i18n.t) ? i18n.t('common-loading') : '加载中...';
                        content.appendChild(loadDiv);
                        var self = this;
                        setTimeout(function() {
                            if (typeof self.loadDeviceControlPage === 'function') {
                                self.loadDeviceControlPage();
                            } else {
                                content.innerHTML = '';
                                var errDiv = document.createElement('div');
                                errDiv.className = 'dc-empty u-text-danger';
                                errDiv.textContent = (typeof i18n !== 'undefined' && i18n.t) ? i18n.t('state-module-load-fail') : '模块加载失败，请刷新页面重试';
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
            const syncNtp = () => {
                if (typeof this.syncDeviceTime === 'function') {
                    // 进入页面即触发一次真实 NTP 同步；成功/失败提示由用户主动刷新时显示。
                    this.syncDeviceTime({ silent: true });
                } else if (typeof this.loadDeviceTime === 'function') {
                    this.loadDeviceTime({ noCache: true });
                }
            };
            // 页面 HTML 与运行时模块是异步加载的，快速点击 Tab 时把动作排到模块加载完成后。
            if (typeof this.syncDeviceTime === 'function' || typeof this.loadDeviceTime === 'function') {
                syncNtp();
            } else if (typeof this._loadModule === 'function') {
                this._loadModule('device-config', syncNtp);
            }
        }
        // 切换到基本信息tab时自动加载硬件信息
        if (pageId === 'device-page' && tabId === 'dev-basic') {
            const loadHardware = () => {
                if (typeof this._loadDeviceHardwareInfo === 'function') {
                    this._loadDeviceHardwareInfo({ noCache: true });
                }
            };
            if (typeof this._loadDeviceHardwareInfo === 'function') {
                loadHardware();
            } else if (typeof this._loadModule === 'function') {
                this._loadModule('device-config', loadHardware);
            }
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

    // ============ 功能配置与菜单联动 ============
    // 功能 key → 菜单 data-page 映射：功能被禁用时隐藏对应菜单项
    // （核心页面如监控/网络/外设配置不受功能开关影响，不在此表）
    _featureMenuMap: {
        ruleScript: 'rule-script',
        deviceControl: 'device-control',   // 设备大屏
        screenPanel: 'screen-panel',       // 运行降级或缺屏时仍按用户启用意图保留入口
        logViewer: 'logs',                 // 设备日志
        userAdmin: 'users'                 // 用户管理
    },

    // 最近一次功能菜单状态缓存（key → active）
    _featureActive: null,
    _featureStatusReady: false,
    _featureStatusPromise: null,

    /**
     * 登录后拉取功能状态，按启用情况显示/隐藏左侧菜单项
     * 完整功能状态在 C3/经典 ESP32 登录资源并发窗口内可能返回 501/503。
     * 菜单优先读取轻量 system/status 的运行态与屏幕用户启用意图；
     * 仅在轻量端点不可用或旧固件缺少意图字段时回退完整接口。
     */
    applyFeatureMenuVisibility() {
        if (typeof apiGet !== 'function') return;
        if (this._featureStatusPromise) return this._featureStatusPromise;
        const map = this._featureMenuMap;
        const self = this;
        const featureKeys = Object.keys(map);
        const statusKeys = featureKeys.concat(['modbus']);
        const menuPages = featureKeys.map(function(key) { return map[key]; });
        const pageToFeature = {};
        featureKeys.forEach(function(key) { pageToFeature[map[key]] = key; });
        const setMenuVisible = function(page, visible) {
            const item = document.querySelector('.menu-item[data-page="' + page + '"]');
            if (!item) return;
            item.style.display = visible ? '' : 'none';
            item.setAttribute('aria-hidden', visible ? 'false' : 'true');
        };
        const applyMenuVisibility = function(active) {
            menuPages.forEach(function(page) {
                const key = pageToFeature[page];
                setMenuVisible(page, active[key] === true);
            });
            // 设备配置是所有设备都必须保留的基础入口，不受可选功能门控。
            setMenuVisible('device', true);
        };
        const applyMenuState = function(active) {
            applyMenuVisibility(active);
            self._featureActive = active;
            self._featureStatusReady = true;
            self.applyProtocolFeatureGate();
        };
        const featureGetter = typeof apiGetSilentFresh === 'function'
            ? apiGetSilentFresh
            : apiGet;

        // 首次状态请求完成前隐藏可选项，但不要把待定值写入功能状态缓存。
        // 否则协议页会把一次启动期等待永久解释为 Modbus 已禁用。
        const pending = {};
        Object.keys(map).forEach(function(key) { pending[key] = false; });
        if (!this._featureStatusReady) {
            applyMenuVisibility(pending);
            this.applyProtocolFeatureGate();
        }

        const applyFeatureResponse = function(res) {
            if (!res || !res.success || !res.data || !Array.isArray(res.data.features)) {
                throw new Error('feature status unavailable');
            }
            const active = {};
            statusKeys.forEach(function(key) {
                active[key] = false;
            });
            res.data.features.forEach(function(f) {
                if (f && f.key) {
                    active[f.key] = f.key === 'screenPanel'
                        ? (f.compiled !== false && f.userEnabled === true)
                        : (f.compiled !== false && f.active === true);
                }
            });
            applyMenuState(active);
            return res;
        };

        const applySystemStatusFallback = function(res) {
            const states = res && res.success && res.data && res.data.features;
            if (!states || typeof states !== 'object') {
                throw new Error('system feature status unavailable');
            }
            const active = {};
            Object.keys(states).forEach(function(key) {
                active[key] = states[key] === 'active';
            });
            statusKeys.forEach(function(key) {
                if (!Object.prototype.hasOwnProperty.call(active, key)) {
                    active[key] = false;
                }
            });
            // Preserve the panel entry during runtime degradation without
            // loading the full feature catalog on current firmware. Older or
            // incomplete responses still use the bounded compatibility path.
            if (Object.prototype.hasOwnProperty.call(map, 'screenPanel')) {
                const panelIntent = res.data.featureIntent && res.data.featureIntent.screenPanel;
                if (panelIntent && typeof panelIntent.compiled === 'boolean' &&
                    typeof panelIntent.userEnabled === 'boolean') {
                    active.screenPanel = panelIntent.compiled && panelIntent.userEnabled;
                    applyMenuState(active);
                    return res;
                }
                return featureGetter('/api/features/status')
                    .then(function(fullResponse) {
                        const features = fullResponse && fullResponse.success &&
                            fullResponse.data && fullResponse.data.features;
                        if (!Array.isArray(features)) throw new Error('feature status unavailable');
                        const panel = features.find(function(feature) {
                            return feature && feature.key === 'screenPanel';
                        });
                        active.screenPanel = !!panel && panel.compiled !== false && panel.userEnabled === true;
                        applyMenuState(active);
                        return res;
                    })
                    .catch(function() {
                        applyMenuState(active);
                        return res;
                    });
            }
            applyMenuState(active);
            return res;
        };

        const request = featureGetter('/api/system/status')
            .then(applySystemStatusFallback)
            .catch(function() {
                return featureGetter('/api/features/status')
                    .then(applyFeatureResponse)
                    .catch(function() {
                        // 首次请求失败时保持 fail-closed；刷新失败则保留上一份
                        // 已确认状态，协议页下次进入仍会重新尝试轻量请求。
                        if (!self._featureStatusReady) {
                            applyMenuVisibility(pending);
                            self.applyProtocolFeatureGate();
                        }
                    });
            });
        const tracked = request.finally(function() {
            if (self._featureStatusPromise === tracked) self._featureStatusPromise = null;
        });
        this._featureStatusPromise = tracked;
        return tracked;
    },

    /**
     * Modbus 功能禁用时隐藏协议页的 Modbus RTU 子项（仅保留已启用协议入口）。
     * 协议页为懒加载 DOM，需在页面加载后与功能状态就绪后各调用一次。
     */
    applyProtocolFeatureGate() {
        const active = this._featureActive;
        // The protocol status may be temporarily unavailable on constrained
        // boards. Keep the Modbus tab fail-closed until the backend explicitly
        // reports the feature as active; restore it when a later fallback
        // status call confirms that Modbus is running.
        const ready = this._featureStatusReady === true;
        const enabled = ready && !!active && active.modbus === true;
        const tab = document.querySelector('#protocol-page .config-tab[data-tab="modbus-rtu"]');
        if (tab) {
            tab.style.display = enabled ? '' : 'none';
            tab.setAttribute('aria-hidden', enabled ? 'false' : 'true');
            tab.setAttribute('data-feature-state', ready ? (enabled ? 'active' : 'disabled') : 'pending');
        }
        const content = document.getElementById('modbus-rtu');
        if (content) content.style.display = enabled ? '' : 'none';
    },

    // ============ 页面切换 ============
    async changePage(page) {
        const navSeq = (this._pageNavSeq || 0) + 1;
        this._pageNavSeq = navSeq;
        if (this._dcFullscreenLaunchTimer) {
            clearTimeout(this._dcFullscreenLaunchTimer);
            this._dcFullscreenLaunchTimer = null;
        }
        // 取消旧页面排队中的请求，释放 ESP32 资源
        if (typeof window.apiAbortPageRequests === 'function') {
            window.apiAbortPageRequests();
        }
        // 通知旧页面模块清理（定时器/轮询），避免切走后继续请求设备
        this._firePageLifecycle(this._pageHideHandlers, 'page-hide');
        this._destroyMountedModals();

        const pageAlias = { monitor: 'dashboard' };
        const normalizedPage = pageAlias[page] || page;
        const featureEntry = Object.keys(this._featureMenuMap || {}).find((key) =>
            this._featureMenuMap[key] === normalizedPage) || null;
        if (featureEntry && this._featureActive === null &&
            typeof this.applyFeatureMenuVisibility === 'function') {
            await this.applyFeatureMenuVisibility();
            if (navSeq !== this._pageNavSeq) return;
        }
        const featureUnavailable = !!featureEntry &&
            (!this._featureActive || this._featureActive[featureEntry] !== true);

        // Cleanup has completed. Release the old page DOM before mounting the
        // next template so repeated navigation cannot accumulate hidden pages.
        if (typeof PageLoader !== 'undefined' && typeof PageLoader.unmountPages === 'function') {
            PageLoader.unmountPages();
        }
        this._preparePageMount(normalizedPage);

        let target = null;
        if (featureUnavailable && typeof PageLoader !== 'undefined' &&
            typeof PageLoader._renderUnavailablePage === 'function') {
            const disabledError = new Error('Feature disabled: ' + featureEntry);
            disabledError.code = 'FEATURE_DISABLED';
            PageLoader._renderUnavailablePage(normalizedPage + '-page', disabledError);
            target = document.getElementById(normalizedPage + '-page');
        } else {
            // 先确保页面HTML已加载
            await this.loadPage(normalizedPage + '-page', { mount: false });
            if (navSeq !== this._pageNavSeq) return;
            target = (typeof PageLoader !== 'undefined' && typeof PageLoader.mountPage === 'function')
                ? PageLoader.mountPage(normalizedPage + '-page')
                : document.getElementById(normalizedPage + '-page');
            if (!target) {
                // 首次加载失败（登录后设备繁忙期静态资源响应可能被截断），
                // 等待设备恢复后重试一次，避免用户需手动二次点击
                await new Promise(resolve => setTimeout(resolve, 1200));
                if (navSeq !== this._pageNavSeq) return;
                await this.loadPage(normalizedPage + '-page', { mount: false });
                if (navSeq !== this._pageNavSeq) return;
                target = (typeof PageLoader !== 'undefined' && typeof PageLoader.mountPage === 'function')
                    ? PageLoader.mountPage(normalizedPage + '-page')
                    : document.getElementById(normalizedPage + '-page');
            }
        }
        if (!target) {
            console.warn('[changePage] target page not found:', page, '=>', normalizedPage);
            return;
        }

        const pageModuleMap = this._getPageModuleMap();
        const pageLoaders = this._getPageLoaders();
        const moduleName = pageModuleMap[normalizedPage];
        const loader = pageLoaders[normalizedPage];
        const moduleWasLoaded = !!(moduleName && typeof ModuleLoader !== 'undefined' &&
            typeof ModuleLoader.isLoaded === 'function' && ModuleLoader.isLoaded(moduleName));
        // Page HTML can arrive before its runtime module on constrained devices.
        // Keep visible controls inert until their handlers are registered so an
        // eager click cannot be silently lost.
        if (moduleName && loader && !moduleWasLoaded) {
            target.setAttribute('inert', '');
            target.setAttribute('aria-busy', 'true');
            target.setAttribute('data-module-loading', 'true');
        }
        target.classList.add('active');

        document.querySelectorAll('.menu-item').forEach(item => {
            item.classList.toggle('active', item.dataset.page === normalizedPage);
        });

        const titleKey = `page-title-${normalizedPage}`;
        const titleEl = document.getElementById('page-title');
        var _t = (typeof i18n !== 'undefined' && i18n.t) ? i18n.t.bind(i18n) : function(k) { return k; };
        var _pageTitleMap = {
            'page-title-dashboard': _t('state-page-dashboard') !== 'state-page-dashboard' ? _t('state-page-dashboard') : '设备监控仪表盘',
            'page-title-protocol': _t('state-page-protocol') !== 'state-page-protocol' ? _t('state-page-protocol') : '通信协议',
            'page-title-network': _t('state-page-network') !== 'state-page-network' ? _t('state-page-network') : '网络设置',
            'page-title-device': _t('state-page-device') !== 'state-page-device' ? _t('state-page-device') : '设备配置',
            'page-title-system': _t('state-page-system') !== 'state-page-system' ? _t('state-page-system') : '系统监控',
            'page-title-logs': _t('state-page-logs') !== 'state-page-logs' ? _t('state-page-logs') : '设备日志',
            'page-title-gpio': _t('state-page-gpio') !== 'state-page-gpio' ? _t('state-page-gpio') : 'GPIO配置',
            'page-title-peripheral': _t('state-page-peripherals') !== 'state-page-peripherals' ? _t('state-page-peripherals') : '外设配置',
            'page-title-periph-exec': _t('state-page-periph-exec') !== 'state-page-periph-exec' ? _t('state-page-periph-exec') : '外设执行',
            'page-title-screen-panel': '设备屏幕',
            'page-title-users': _t('state-page-users') !== 'state-page-users' ? _t('state-page-users') : '用户管理',
            'page-title-device-control': _t('state-page-fullscreen') !== 'state-page-fullscreen' ? _t('state-page-fullscreen') : '设备大屏',
            'page-title-rule-script': _t('state-page-rule-scripts') !== 'state-page-rule-scripts' ? _t('state-page-rule-scripts') : '规则脚本',
            'page-title-admin': _t('state-page-users') !== 'state-page-users' ? _t('state-page-users') : '用户管理',
            'page-title-features': _t('state-page-features') !== 'state-page-features' ? _t('state-page-features') : '功能配置'
        };
        if (titleEl) titleEl.textContent = _pageTitleMap[titleKey] || normalizedPage;

        this.currentPage = normalizedPage;
        // 通知新页面模块进入（与 _pageHideHandlers 对称的生命周期钩子）
        this._firePageLifecycle(this._pageShowHandlers, 'page-show');

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

        // 禁用功能的直达页面只保留轻量提示，不加载运行模块或业务接口。
        // This path never runs the module loader's finally block, so it must
        // release the temporary loading gate itself. Otherwise the visible
        // explanation remains inert and disappears from the accessibility tree.
        if (featureUnavailable) {
            target.removeAttribute('inert');
            target.removeAttribute('aria-busy');
            target.removeAttribute('data-module-loading');
            return;
        }

        const runLoader = () => {
            if (navSeq !== this._pageNavSeq || this.currentPage !== normalizedPage) return;
            try {
                if (moduleWasLoaded) this._rebindMountedPageEvents(normalizedPage);
                loader();
            } finally {
                if (target && target.isConnected) {
                    target.removeAttribute('inert');
                    target.removeAttribute('aria-busy');
                    target.removeAttribute('data-module-loading');
                }
            }
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
        // 1. 关闭并销毁所有已挂载模态框
        this._destroyMountedModals();

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
