// 应用状态管理
const AppState = {
    currentPage: 'dashboard',
    configTab: 'modbus',
    currentUser: { name: '', role: '', canManageFs: false },
    sidebarCollapsed: false,
    _logAutoRefreshTimer: null,  // 日志自动刷新定时器

    // ============ 初始化 ============
    init() {
        this.setupTheme();  // 主题初始化
        this.setupUserDropdown(); // 用户下拉菜单
        this.setupSidebarToggle();
        this.setupLanguage();
        this.setupConfigTabs();
        this.setupEventListeners();
        this.refreshPage();
    },

    // ============ 用户下拉菜单 ============
    setupUserDropdown() {
        const dropdownBtn = document.getElementById('user-dropdown-btn');
        const dropdownMenu = document.getElementById('user-dropdown-menu');
        
        if (!dropdownBtn || !dropdownMenu) return;
        
        // 切换下拉菜单显示
        dropdownBtn.addEventListener('click', (e) => {
            e.stopPropagation();
            const dropdown = dropdownBtn.closest('.user-dropdown');
            dropdown.classList.toggle('open');
        });
        
        // 点击外部关闭下拉菜单
        document.addEventListener('click', (e) => {
            if (!dropdownBtn.contains(e.target) && !dropdownMenu.contains(e.target)) {
                const dropdown = dropdownBtn.closest('.user-dropdown');
                if (dropdown) dropdown.classList.remove('open');
            }
        });
        
        // 点击菜单项后关闭下拉菜单
        dropdownMenu.querySelectorAll('.dropdown-item').forEach(item => {
            item.addEventListener('click', () => {
                const dropdown = dropdownBtn.closest('.user-dropdown');
                if (dropdown) dropdown.classList.remove('open');
            });
        });
    },

    // ============ 主题管理 ============
    setupTheme() {
        // 检测系统主题偏好
        const savedTheme = localStorage.getItem('theme');
        const systemPrefersDark = window.matchMedia('(prefers-color-scheme: dark)').matches;
        
        // 优先级: 用户手动设置 > 系统偏好
        let theme;
        if (savedTheme) {
            theme = savedTheme;
        } else {
            theme = systemPrefersDark ? 'dark' : 'light';
            // 标记为自动模式（未手动设置）
            localStorage.setItem('theme-auto', 'true');
        }
        
        this.setTheme(theme, false);
        this.updateThemeToggleIcon(theme);
        
        // 绑定主题切换按钮 (下拉菜单中的)
        const themeToggleItem = document.getElementById('theme-toggle-item');
        if (themeToggleItem) {
            themeToggleItem.addEventListener('click', () => this.toggleTheme());
        }
        
        // 监听系统主题变化（仅在自动模式下）
        window.matchMedia('(prefers-color-scheme: dark)').addEventListener('change', (e) => {
            const isAutoMode = localStorage.getItem('theme-auto') === 'true';
            if (isAutoMode) {
                const newTheme = e.matches ? 'dark' : 'light';
                this.setTheme(newTheme, false);
                this.updateThemeToggleIcon(newTheme);
            }
        });
    },
    
    setTheme(theme, isManual = true) {
        document.documentElement.setAttribute('data-theme', theme);
        localStorage.setItem('theme', theme);
        
        // 如果是手动设置，清除自动模式标记
        if (isManual) {
            localStorage.removeItem('theme-auto');
        }
        
        this.updateThemeToggleIcon(theme);
    },
    
    toggleTheme() {
        const current = document.documentElement.getAttribute('data-theme');
        const newTheme = current === 'dark' ? 'light' : 'dark';
        this.setTheme(newTheme, true);
    },
    
    updateThemeToggleIcon(theme) {
        const themeToggleItem = document.getElementById('theme-toggle-item');
        if (themeToggleItem) {
            const iconSpan = themeToggleItem.querySelector('.item-icon');
            const textSpan = themeToggleItem.querySelector('span:not(.item-icon)');
            
            if (iconSpan) {
                iconSpan.textContent = theme === 'dark' ? '☀' : '🌙';
            }
            if (textSpan) {
                // 获取当前语言的翻译
                const key = theme === 'dark' ? 'theme-light' : 'theme-dark';
                const translated = window.i18n ? window.i18n.t(key) : (theme === 'dark' ? '浅色模式' : '深色模式');
                textSpan.textContent = translated;
            }
        }
    },

    // ============ 会话验证 ============
    refreshPage() {
        const token = localStorage.getItem('auth_token');

        // 在 API 调用前保存"记住密码"凭据到局部变量
        // （防止 401 处理器清除 localStorage 中的凭据）
        const savedRemember = localStorage.getItem('remember');
        const savedUsername = localStorage.getItem('username');
        const savedPassword = localStorage.getItem('password');

        if (!token) {
            // 没有 token，尝试使用保存的凭据自动登录
            this._tryAutoLogin(savedRemember, savedUsername, savedPassword);
            return;
        }
        apiGet('/api/auth/session')
            .then(res => {
                if (res && res.success && res.data && res.data.sessionValid) {
                    this.currentUser.name = res.data.username || 'Admin';
                    this.currentUser.role = res.data.role || 'VIEWER';
                    this.currentUser.canManageFs = res.data.canManageFs === true;
                    this._showAppPage();
                    this.renderDashboard();
                    this.loadSystemMonitor();  // 加载监控仪表盘数据
                    this.loadUsers();
                } else {
                    // 会话无效，尝试使用保存的凭据重新登录
                    this._tryAutoLogin(savedRemember, savedUsername, savedPassword);
                }
            })
            .catch(() => {
                // 会话验证失败（如 401），尝试自动重新登录
                this._tryAutoLogin(savedRemember, savedUsername, savedPassword);
            });
    },

    // 尝试使用保存的凭据自动登录
    _tryAutoLogin(remember, username, password) {
        if (remember === 'true' && username && password) {
            apiPost('/api/auth/login', { username, password })
                .then(res => {
                    if (res && res.success) {
                        const sid = res.sessionId;
                        localStorage.setItem('auth_token', sid);
                        localStorage.setItem('sessionId', sid);
                        // 恢复"记住密码"凭据（可能已被 401 处理器清除）
                        localStorage.setItem('remember', 'true');
                        localStorage.setItem('username', username);
                        localStorage.setItem('password', password);

                        this.currentUser.name = res.username || username;
                        sessionStorage.setItem('currentUsername', this.currentUser.name);

                        // 获取角色和权限信息
                        apiGet('/api/auth/session').then(sr => {
                            if (sr && sr.success && sr.data) {
                                this.currentUser.role = sr.data.role || 'VIEWER';
                                this.currentUser.canManageFs = sr.data.canManageFs === true;
                            }
                        }).catch(() => {});

                        this._showAppPage();
                        this.renderDashboard();
                        this.loadSystemMonitor();
                        this.loadUsers();
                    } else {
                        // 自动登录失败（如密码已更改），清除无效凭据并显示登录页
                        localStorage.removeItem('password');
                        localStorage.removeItem('remember');
                        this._showLoginPage();
                    }
                })
                .catch(() => {
                    this._showLoginPage();
                });
        } else {
            this._showLoginPage();
        }
    },

    _showLoginPage() {
        document.getElementById('login-page').style.display = 'flex';
        document.getElementById('app-container').style.display = 'none';

        // 预填充已保存的用户名和"记住密码"状态
        const savedUsername = localStorage.getItem('username');
        const savedRemember = localStorage.getItem('remember');
        const usernameInput = document.getElementById('username');
        const rememberCheckbox = document.getElementById('remember');
        if (usernameInput && savedUsername) usernameInput.value = savedUsername;
        if (rememberCheckbox && savedRemember === 'true') rememberCheckbox.checked = true;
    },

    _showAppPage() {
        document.getElementById('login-page').style.display = 'none';
        document.getElementById('app-container').style.display = 'block';
        // 登录成功后将URL从 /login 等路径重定向到根路径 /
        if (location.pathname !== '/' || location.hash) {
            history.replaceState(null, '', '/');
        }
    },

    // ============ 侧边栏 ============
    setupSidebarToggle() {
        const btn = document.getElementById('sidebar-toggle');
        if (btn) btn.addEventListener('click', () => this.toggleSidebar());
        if (localStorage.getItem('sidebarCollapsed') === 'true') this.collapseSidebar();
    },

    toggleSidebar() {
        // 移动端使用 expanded 类，桌面端使用 collapsed 类
        const sidebar = document.getElementById('sidebar');
        if (!sidebar) return;
        
        // 检测是否为移动端
        const isMobile = window.innerWidth <= 768;
        
        if (isMobile) {
            // 移动端：切换 expanded 类
            if (sidebar.classList.contains('expanded')) {
                sidebar.classList.remove('expanded');
                this.sidebarCollapsed = true;
                const btn = document.getElementById('sidebar-toggle');
                if (btn) btn.textContent = '☰';
            } else {
                sidebar.classList.add('expanded');
                this.sidebarCollapsed = false;
                const btn = document.getElementById('sidebar-toggle');
                if (btn) btn.textContent = '✕';
            }
        } else {
            // 桌面端：使用原有逻辑
            this.sidebarCollapsed ? this.expandSidebar() : this.collapseSidebar();
        }
    },

    collapseSidebar() {
        const sidebar = document.getElementById('sidebar');
        if (sidebar) {
            sidebar.classList.add('collapsed');
            sidebar.classList.remove('expanded');
            this.sidebarCollapsed = true;
            localStorage.setItem('sidebarCollapsed', 'true');
            const btn = document.getElementById('sidebar-toggle');
            if (btn) btn.textContent = '☰';
        }
    },

    expandSidebar() {
        const sidebar = document.getElementById('sidebar');
        if (sidebar) {
            sidebar.classList.remove('collapsed');
            this.sidebarCollapsed = false;
            localStorage.setItem('sidebarCollapsed', 'false');
            const btn = document.getElementById('sidebar-toggle');
            if (btn) btn.textContent = '✕';
        }
    },

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
                this.renderDashboard();
                this.loadSystemMonitor();  // 刷新网络状态区域的动态 i18n 内容
                this.loadUsers();
            });
        }
    },

    // ============ 配置选项卡 ============
    setupConfigTabs() {
        ['#protocol-page', '#network-page', '#device-page'].forEach(pageSelector => {
            const page = document.querySelector(pageSelector);
            if (!page) return;
            page.querySelectorAll('.config-tab').forEach(tab => {
                tab.addEventListener('click', () => {
                    this.showConfigTab(pageSelector.replace('#', ''), tab.getAttribute('data-tab'));
                });
            });
        });

        // 协议配置表单提交
        document.querySelectorAll('#protocol-page form').forEach(form => {
            form.addEventListener('submit', e => {
                e.preventDefault();
                this.saveProtocolConfig(form.id);
            });
        });
        
        // MQTT增加主题按钮
        const addMqttTopicBtn = document.getElementById('add-mqtt-topic-btn');
        if (addMqttTopicBtn) {
            addMqttTopicBtn.addEventListener('click', () => this.addMqttPublishTopic());
        }
        
        // MQTT增加订阅按钮
        const addMqttSubscribeBtn = document.getElementById('add-mqtt-subscribe-btn');
        if (addMqttSubscribeBtn) {
            addMqttSubscribeBtn.addEventListener('click', () => this.addMqttSubscribeTopic());
        }

        // MQTT客户端ID或认证方式修改时，清除测试结果提示
        const mqttTestResultEl = document.getElementById('mqtt-test-result');
        if (mqttTestResultEl) {
            const clearTestResult = () => { mqttTestResultEl.textContent = ''; };
            const cidEl = document.getElementById('mqtt-client-id');
            const atEl = document.getElementById('mqtt-auth-type');
            if (cidEl) cidEl.addEventListener('input', clearTestResult);
            if (atEl) atEl.addEventListener('change', clearTestResult);
        }

        // 网络配置表单已在 setupNetworkFormHandlers() 中专门处理，无需通用监听
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
            this.loadProtocolConfig(tabId);
        }
        
        // 切换到设备监控页面时自动加载网络状态
        if (pageId === 'dashboard-page') {
            this.loadNetworkStatus();
        }
        // 切换到NTP时间tab时自动加载时间
        if (pageId === 'device-page' && tabId === 'dev-ntp') {
            this.loadDeviceTime();
        }
        // 切换到基本信息tab时自动加载硬件信息
        if (pageId === 'device-page' && tabId === 'dev-basic') {
            this._loadDeviceHardwareInfo();
        }
        // 切换到热点配置tab时自动加载配网状态
        if (pageId === 'network-page' && tabId === 'ap-config') {
            this.loadProvisionStatus();
        }
        // 切换到蓝牙配网tab时自动加载蓝牙配网状态和配置
        if (pageId === 'device-page' && tabId === 'dev-ble') {
            this.loadBLEProvisionStatus();
            this.loadBLEProvisionConfig();
        }
        // 切换到OTA升级tab时自动加载OTA状态
        if (pageId === 'device-page' && tabId === 'dev-ota') {
            this.loadOtaStatus();
        }
    },

    // ============ 事件绑定 ============
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

        // 添加用户按钮
        const addUserBtn = document.getElementById('add-user-btn');
        if (addUserBtn) addUserBtn.addEventListener('click', () => this.showAddUserModal());

        // 模态窗关闭
        const closeId = (id) => { const el = document.getElementById(id); if (el) el.style.display = 'none'; };
        ['close-password-modal', 'cancel-password-btn'].forEach(id => {
            const el = document.getElementById(id);
            if (el) el.addEventListener('click', () => closeId('change-password-modal'));
        });
        // 关闭用户添加/编辑 modal 时重置状态
        const closeUserModal = () => {
            const modal = document.getElementById('add-user-modal');
            if (modal) {
                modal.style.display = 'none';
                modal.dataset.editMode = 'add';
                modal.dataset.editUsername = '';
            }
            // 重置用户名输入框状态
            const usernameInput = document.getElementById('add-username-input');
            if (usernameInput) usernameInput.disabled = false;
        };
        ['close-add-user-modal', 'cancel-add-user-btn'].forEach(id => {
            const el = document.getElementById(id);
            if (el) el.addEventListener('click', closeUserModal);
        });

        // 确认修改密码
        const confirmPwd = document.getElementById('confirm-password-btn');
        if (confirmPwd) confirmPwd.addEventListener('click', () => this.changePassword());

        // 确认添加用户
        const confirmAdd = document.getElementById('confirm-add-user-btn');
        if (confirmAdd) confirmAdd.addEventListener('click', () => this.addUser());
        
        // ============ 角色管理事件绑定 ============
        
        // 添加角色按钮
        const addRoleBtn = document.getElementById('add-role-btn');
        if (addRoleBtn) addRoleBtn.addEventListener('click', () => this.showAddRoleModal());
        
        // 角色模态窗关闭
        ['close-role-modal', 'cancel-role-btn'].forEach(id => {
            const el = document.getElementById(id);
            if (el) el.addEventListener('click', () => closeId('role-modal'));
        });
        
        // 确认保存角色
        const confirmRole = document.getElementById('confirm-role-btn');
        if (confirmRole) confirmRole.addEventListener('click', () => this.saveRole());
        
        // ============ 日志管理事件绑定 ============
        
        // 刷新日志文件列表按钮
        const refreshLogListBtn = document.getElementById('log-refresh-list-btn');
        if (refreshLogListBtn) refreshLogListBtn.addEventListener('click', () => this.loadLogFileList());
        
        // 刷新日志按钮
        const refreshLogsBtn = document.getElementById('refresh-logs-btn');
        if (refreshLogsBtn) refreshLogsBtn.addEventListener('click', () => this.loadLogs());
        
        // 清空日志按钮
        const clearLogsBtn = document.getElementById('clear-logs-btn');
        if (clearLogsBtn) clearLogsBtn.addEventListener('click', () => this.clearLogs());
        
        // 自动刷新复选框
        const autoRefreshCheckbox = document.getElementById('log-auto-refresh');
        if (autoRefreshCheckbox) {
            autoRefreshCheckbox.addEventListener('change', (e) => {
                if (e.target.checked) {
                    this.startLogAutoRefresh();
                } else {
                    this.stopLogAutoRefresh();
                }
            });
        }
        
        // ============ 文件管理事件绑定 ============
        
        // 刷新文件列表按钮
        const fsRefreshBtn = document.getElementById('fs-refresh-btn');
        if (fsRefreshBtn) fsRefreshBtn.addEventListener('click', () => this.loadFileTree(this._currentDir || '/'));
        
        // 返回上级按钮
        const fsUpBtn = document.getElementById('fs-up-btn');
        if (fsUpBtn) fsUpBtn.addEventListener('click', () => this.navigateUp());
        
        // 保存文件按钮
        const fsSaveBtn = document.getElementById('fs-save-btn');
        if (fsSaveBtn) fsSaveBtn.addEventListener('click', () => this.saveCurrentFile());
        
        // 关闭文件按钮
        const fsCloseBtn = document.getElementById('fs-close-btn');
        if (fsCloseBtn) fsCloseBtn.addEventListener('click', () => this.closeCurrentFile());
        
        // ============ 系统监控事件绑定 ============
        
        // 刷新监控按钮
        const monitorRefreshBtn = document.getElementById('monitor-refresh-btn');
        if (monitorRefreshBtn) monitorRefreshBtn.addEventListener('click', () => this.loadSystemMonitor());
        
        // ============ 网络配置事件绑定 ============
        
        // WiFi扫描按钮
        const wifiScanBtn = document.getElementById('wifi-scan-btn');
        if (wifiScanBtn) wifiScanBtn.addEventListener('click', () => this.scanWifiNetworks());
        
        // 基本配置表单提交
        const wifiForm = document.getElementById('wifi-form');
        if (wifiForm) {
            wifiForm.addEventListener('submit', (e) => {
                e.preventDefault();
                this.saveNetworkConfig();
            });
        }
        
        // 热点配置表单提交
        const apForm = document.getElementById('ap-form');
        if (apForm) {
            apForm.addEventListener('submit', (e) => {
                e.preventDefault();
                this.saveAPConfig();
            });
        }
        
        // 高级配置表单提交
        const advancedForm = document.getElementById('advanced-form');
        if (advancedForm) {
            advancedForm.addEventListener('submit', (e) => {
                e.preventDefault();
                this.saveAdvancedConfig();
            });
        }
        
        // DHCP/静态IP切换
        const wifiDhcp = document.getElementById('wifi-dhcp');
        if (wifiDhcp) {
            wifiDhcp.addEventListener('change', (e) => {
                this._toggleStaticIPFields(e.target.value === '1');
            });
        }
        
        // ============ 外设配置事件绑定 ============
        
        // 新增外设按钮
        const addPeripheralBtn = document.getElementById('add-peripheral-btn');
        if (addPeripheralBtn) addPeripheralBtn.addEventListener('click', () => this.openPeripheralModal());
        
        // 关闭外设模态框
        const closePeripheralModal = document.getElementById('close-peripheral-modal');
        if (closePeripheralModal) closePeripheralModal.addEventListener('click', () => this.closePeripheralModal());
        
        // 取消外设按钮
        const cancelPeripheralBtn = document.getElementById('cancel-peripheral-btn');
        if (cancelPeripheralBtn) cancelPeripheralBtn.addEventListener('click', () => this.closePeripheralModal());
        
        // 保存外设按钮
        const savePeripheralBtn = document.getElementById('save-peripheral-btn');
        if (savePeripheralBtn) savePeripheralBtn.addEventListener('click', () => this.savePeripheralConfig());
        
        // 外设类型选择变化
        const peripheralTypeInput = document.getElementById('peripheral-type-input');
        if (peripheralTypeInput) {
            peripheralTypeInput.addEventListener('change', (e) => this.onPeripheralTypeChange(e.target.value));
        }
        
        // 外设过滤器
        const peripheralFilter = document.getElementById('peripheral-filter-type');
        if (peripheralFilter) {
            peripheralFilter.addEventListener('change', (e) => this.loadPeripherals(e.target.value));
        }
        
        // ============ 兼容旧版：GPIO配置事件绑定 ============
        
        // 新增GPIO按钮
        const addGpioBtn = document.getElementById('add-gpio-btn');
        if (addGpioBtn) addGpioBtn.addEventListener('click', () => this.openPeripheralModal());
        
        // 关闭GPIO模态框
        const closeGpioModal = document.getElementById('close-gpio-modal');
        if (closeGpioModal) closeGpioModal.addEventListener('click', () => this.closePeripheralModal());
        
        // 取消GPIO按钮
        const cancelGpioBtn = document.getElementById('cancel-gpio-btn');
        if (cancelGpioBtn) cancelGpioBtn.addEventListener('click', () => this.closePeripheralModal());
        
        // 保存GPIO按钮
        const saveGpioBtn = document.getElementById('save-gpio-btn');
        if (saveGpioBtn) saveGpioBtn.addEventListener('click', () => this.savePeripheralConfig());
        
        // ============ 外设执行事件绑定 ============
        
        const closePeriphExecModal = document.getElementById('close-periph-exec-modal');
        if (closePeriphExecModal) closePeriphExecModal.addEventListener('click', () => this.closePeriphExecModal());
        
        const cancelPeriphExecBtn = document.getElementById('cancel-periph-exec-btn');
        if (cancelPeriphExecBtn) cancelPeriphExecBtn.addEventListener('click', () => this.closePeriphExecModal());
        
        const savePeriphExecBtn = document.getElementById('save-periph-exec-btn');
        if (savePeriphExecBtn) savePeriphExecBtn.addEventListener('click', () => this.savePeriphExecRule());
        
        // ============ 规则脚本事件绑定 ============
        const closeRuleScriptModal = document.getElementById('close-rule-script-modal');
        if (closeRuleScriptModal) closeRuleScriptModal.addEventListener('click', () => this.closeRuleScriptModal());
        const cancelRuleScriptBtn = document.getElementById('cancel-rule-script-btn');
        if (cancelRuleScriptBtn) cancelRuleScriptBtn.addEventListener('click', () => this.closeRuleScriptModal());
        const saveRuleScriptBtn = document.getElementById('save-rule-script-btn');
        if (saveRuleScriptBtn) saveRuleScriptBtn.addEventListener('click', () => this.saveRuleScript());
        
        // ============ 网络状态事件绑定 ============
        
        // 仪表盘网络状态刷新按钮
        const dashboardNetRefreshBtn = document.getElementById('dashboard-net-refresh-btn');
        if (dashboardNetRefreshBtn) dashboardNetRefreshBtn.addEventListener('click', () => this.loadNetworkStatus());
        
        // ============ 设备配置事件绑定 ============
        
        // 设备基本信息表单提交
        const deviceBasicForm = document.getElementById('device-basic-form');
        if (deviceBasicForm) {
            deviceBasicForm.addEventListener('submit', (e) => {
                e.preventDefault();
                this.saveDeviceBasic();
            });
        }
        
        // NTP表单提交
        const deviceNtpForm = document.getElementById('device-ntp-form');
        if (deviceNtpForm) {
            deviceNtpForm.addEventListener('submit', (e) => {
                e.preventDefault();
                this.saveDeviceNTP();
            });
        }
        
        // 时间刷新按钮 - 触发NTP同步
        const devTimeRefreshBtn = document.getElementById('dev-time-refresh-btn');
        if (devTimeRefreshBtn) devTimeRefreshBtn.addEventListener('click', () => this.syncDeviceTime());
        
        // 重启按钮
        const devRestartBtn = document.getElementById('dev-restart-btn');
        if (devRestartBtn) devRestartBtn.addEventListener('click', () => this.restartDevice());

        // 恢复出厂设置按钮
        const devFactoryBtn = document.getElementById('dev-factory-btn');
        if (devFactoryBtn) devFactoryBtn.addEventListener('click', () => this.factoryReset());

        // AP配网事件绑定（整合到热点配置页面）
        const apProvisionRefreshBtn = document.getElementById('ap-provision-refresh-btn');
        if (apProvisionRefreshBtn) apProvisionRefreshBtn.addEventListener('click', () => this.loadProvisionStatus());
        
        const apProvisionStartBtn = document.getElementById('ap-provision-start-btn');
        if (apProvisionStartBtn) apProvisionStartBtn.addEventListener('click', () => this.startProvision());
        
        const apProvisionStopBtn = document.getElementById('ap-provision-stop-btn');
        if (apProvisionStopBtn) apProvisionStopBtn.addEventListener('click', () => this.stopProvision());

        // 蓝牙配网事件绑定
        const bleProvisionForm = document.getElementById('device-ble-provision-form');
        if (bleProvisionForm) bleProvisionForm.addEventListener('submit', (e) => { e.preventDefault(); this.saveBLEProvisionConfig(); });
        
        const bleProvisionRefreshBtn = document.getElementById('ble-provision-refresh-btn');
        if (bleProvisionRefreshBtn) bleProvisionRefreshBtn.addEventListener('click', () => this.loadBLEProvisionStatus());
        
        const bleProvisionStartBtn = document.getElementById('ble-provision-start-btn');
        if (bleProvisionStartBtn) bleProvisionStartBtn.addEventListener('click', () => this.startBLEProvision());
        
        const bleProvisionStopBtn = document.getElementById('ble-provision-stop-btn');
        if (bleProvisionStopBtn) bleProvisionStopBtn.addEventListener('click', () => this.stopBLEProvision());

        // OTA升级事件绑定
        const otaUrlForm = document.getElementById('ota-url-form');
        if (otaUrlForm) otaUrlForm.addEventListener('submit', (e) => { e.preventDefault(); this.startOtaUrl(); });
        
        const otaUploadForm = document.getElementById('ota-upload-form');
        if (otaUploadForm) otaUploadForm.addEventListener('submit', (e) => { e.preventDefault(); this.startOtaUpload(); });
        
        const otaRefreshBtn = document.getElementById('ota-refresh-btn');
        if (otaRefreshBtn) otaRefreshBtn.addEventListener('click', () => this.loadOtaStatus());
        
        // OTA文件选择更新文件名显示
        const otaFileInput = document.getElementById('ota-file');
        if (otaFileInput) {
            otaFileInput.addEventListener('change', (e) => {
                const fileNameEl = document.getElementById('ota-file-name');
                if (fileNameEl) {
                    const file = e.target.files?.[0];
                    fileNameEl.textContent = file ? file.name : i18n.t('no-file-selected');
                }
            });
        }
    },

    // ============ 登录 ============
    handleLogin() {
        const username = (document.getElementById('username') || {}).value;
        const password = (document.getElementById('password') || {}).value;
        const remember = (document.getElementById('remember') || {}).checked;

        if (!username || !password) {
            Notification.warning(i18n.t('login-empty-warning'), i18n.t('login-fail-title'));
            return;
        }

        const submitBtn = document.querySelector('#login-form button[type="submit"]');
        const originalText = submitBtn ? submitBtn.innerHTML : '';
        if (submitBtn) { submitBtn.innerHTML = i18n.t('login-logging-in-html'); submitBtn.disabled = true; }

        apiPost('/api/auth/login', { username, password })
            .then(res => {
                if (res && res.success) {
                    const sid = res.sessionId;

                    // 保存 token 到 localStorage，fetch-api.js 会自动注入 Authorization 头
                    localStorage.setItem('auth_token', sid);

                    // 记住密码
                    if (remember) {
                        localStorage.setItem('remember', 'true');
                        localStorage.setItem('username', username);
                        localStorage.setItem('password', password);  // 保存密码用于自动填充
                        localStorage.setItem('sessionId', sid);
                    } else {
                        ['remember', 'username', 'password', 'sessionId'].forEach(k => localStorage.removeItem(k));
                    }

                    this.currentUser.name = res.username || username;
                    sessionStorage.setItem('currentUsername', this.currentUser.name);

                    // 获取角色和权限信息
                    apiGet('/api/auth/session').then(sr => {
                        if (sr && sr.success && sr.data) {
                            this.currentUser.role = sr.data.role || 'VIEWER';
                            this.currentUser.canManageFs = sr.data.canManageFs === true;
                        }
                    }).catch(() => {});

                    this._showAppPage();
                    this.renderDashboard();
                    this.loadSystemMonitor();  // 加载监控仪表盘数据
                    this.loadUsers();
                    Notification.success(i18n.t('login-success-msg'), i18n.t('login-welcome-title'));
                } else {
                    Notification.error((res && res.error) || i18n.t('login-fail-title'), i18n.t('login-fail-title'));
                }
            })
            .catch((err) => {
                // 登录失败，显示错误信息
                const errorMsg = (err && err.data && err.data.error) || i18n.t('login-fail-msg');
                Notification.error(errorMsg, i18n.t('login-fail-title'));
            })
            .finally(() => {
                if (submitBtn) { submitBtn.innerHTML = originalText; submitBtn.disabled = false; }
            });
    },

    // ============ 页面切换 ============
    changePage(page) {
        // 兼容别名（历史上 monitor 对应 dashboard）
        const pageAlias = {
            monitor: 'dashboard'
        };
        const normalizedPage = pageAlias[page] || page;
        const target = document.getElementById(`${normalizedPage}-page`);

        // 若目标页面不存在，避免菜单与页面状态错位
        if (!target) {
            console.warn('[changePage] target page not found:', page, '=>', normalizedPage);
            return;
        }

        // 菜单激活态与实际展示页面保持一致
        document.querySelectorAll('.menu-item').forEach(item => {
            item.classList.toggle('active', item.dataset.page === normalizedPage);
        });

        const titleKey = `page-title-${normalizedPage}`;
        const titleEl = document.getElementById('page-title');
        if (titleEl) titleEl.textContent = i18n.t(titleKey);

        document.querySelectorAll('.page').forEach(p => p.classList.remove('active'));
        target.classList.add('active');

        this.currentPage = normalizedPage;

        // 按需加载数据（集中映射，避免菜单与页面逻辑漂移）
        const pageLoaders = {
            dashboard: () => {
                this.renderDashboard();
                this.loadSystemMonitor();
            },
            users: () => this.loadUsers(),
            roles: () => this.loadRoles(),
            peripheral: () => this.loadPeripherals(),
            'periph-exec': () => this.loadPeriphExecPage(),
            'rule-script': () => this.loadRuleScriptPage(),
            data: () => {
                this.loadFileTree('/');
                this.loadFileSystemInfo();
            },
            network: () => this.loadNetworkConfig(),
            device: () => this.loadDeviceConfig(),
            protocol: () => {
                // 切换到协议配置页面时自动加载第一个tab配置(MQTT)
                this.loadProtocolConfig('mqtt');
                this._startMqttStatusPolling();
            },
            logs: () => {
                this._currentLogFile = 'system.log';  // 默认加载 system.log
                const currentSpan = document.getElementById('current-log-file');
                if (currentSpan) currentSpan.textContent = i18n.t('log-current-file-prefix') + this._currentLogFile;
                this.loadLogs();
                const autoRefresh = document.getElementById('log-auto-refresh');
                if (autoRefresh && autoRefresh.checked) {
                    this.startLogAutoRefresh();
                }
            }
        };

        // 非日志页确保关闭日志自动刷新
        if (normalizedPage !== 'logs') {
            this.stopLogAutoRefresh();
        }

        // 非协议页确保停止 MQTT 状态轮询
        if (normalizedPage !== 'protocol') {
            this._stopMqttStatusPolling();
        }

        if (pageLoaders[normalizedPage]) {
            pageLoaders[normalizedPage]();
        } else {
            // 开发期提示：存在页面但没有绑定加载器时提醒，避免菜单和数据加载脱节
            console.warn('[changePage] no page loader mapped for:', normalizedPage);
        }
    },

    // ============ 仪表板（从 /api/system/status 加载实时数据）============
    renderDashboard() {
        apiGet('/api/system/status')
            .then(res => {
                if (!res || !res.success) return;
                const d = res.data || {};
                const freeHeap = d.freeHeap || 0;
                const heapTotal = 327680; // ESP32典型 320KB
                const memPct = Math.round((1 - freeHeap / heapTotal) * 100);

                const set = (id, val) => { const el = document.getElementById(id); if (el) el.textContent = val; };
                set('memory-usage', memPct + '%');
                set('cpu-usage', d.uptime !== undefined ? '—' : '—');
                set('storage-usage', '—');
                set('temperature', '—°C');

                // 在线设备数（暂用模拟，等待设备API）
                const tbody = document.getElementById('device-table-body');
                if (tbody) {
                    tbody.innerHTML = `<tr><td colspan="4" style="text-align:center;color:#888">
                        ${d.networkConnected ? i18n.t('dashboard-connected-prefix') + (d.ipAddress || '') : i18n.t('dashboard-disconnected')}
                    </td></tr>`;
                }
            })
            .catch(() => {
                // 无法获取状态时显示占位内容
                ['cpu-usage', 'memory-usage', 'storage-usage', 'temperature'].forEach(id => {
                    const el = document.getElementById(id); if (el) el.textContent = '—';
                });
            });
    },

    // ============ 系统信息（兼容保留）============
    // @deprecated monitor 独立页面已移除；请使用 dashboard + loadSystemMonitor()
    loadSystemInfo() {
        apiGet('/api/system/info')
            .then(res => {
                if (!res || !res.success) return;
                const d = res.data || {};
                // 若页面有系统信息展示元素则填充
                const set = (id, val) => { const el = document.getElementById(id); if (el) el.textContent = val; };
                set('sys-chip-model', d.chipModel || '—');
                set('sys-cpu-freq', d.cpuFreqMHz ? d.cpuFreqMHz + ' MHz' : '—');
                set('sys-free-heap', d.freeHeap ? Math.round(d.freeHeap / 1024) + ' KB' : '—');
                set('sys-uptime', d.uptime ? Math.round(d.uptime / 1000) + ' s' : '—');
                set('sys-sdk-version', d.sdkVersion || '—');
                set('sys-user-count', d.userCount || '—');
                set('sys-sessions', d.activeSessions || '—');
            })
            .catch(() => {});
    },

    // ============ 用户列表（从 API 加载）============
    loadUsers() {
        apiGet('/api/users', { page: 1, limit: 100 })
            .then(res => {
                if (!res || !res.success) return;
                const users = (res.data && res.data.users) ? res.data.users : [];
                this._renderUsers(users);
            })
            .catch(() => {});
    },

    _renderUsers(users) {
        const tbody = document.getElementById('users-table-body');
        if (!tbody) return;

        tbody.innerHTML = '';
        if (!users || users.length === 0) {
            tbody.innerHTML = `<tr><td colspan="5" style="text-align:center;color:#888">${i18n.t('no-users-data')}</td></tr>`;
            return;
        }

        users.forEach(user => {
            const row = document.createElement('tr');

            const td = (content) => {
                const cell = document.createElement('td');
                if (typeof content === 'string' || typeof content === 'number') {
                    cell.textContent = content;
                } else {
                    cell.appendChild(content);
                }
                return cell;
            };

            // 角色标签本地化
            const roleMap = { admin: i18n.t('admin'), operator: i18n.t('operator'), viewer: i18n.t('viewer') };
            const roleText = roleMap[user.role] || user.role || '—';
            const lastLogin = user.lastLogin ? new Date(user.lastLogin * 1000).toLocaleString() : '—';

            // 状态标志
            const badge = document.createElement('span');
            badge.className = 'badge';
            if (user.isLocked) {
                badge.classList.add('badge-warning');
                badge.textContent = i18n.t('user-locked-badge');
            } else if (!user.enabled) {
                badge.classList.add('badge-danger');
                badge.textContent = i18n.t('user-status-inactive');
            } else {
                badge.classList.add('badge-success');
                badge.textContent = i18n.t('user-status-active');
            }

            // 操作按钮区域
            const actionCell = document.createElement('td');
            actionCell.className = 'u-toolbar-sm';

            const editBtn = document.createElement('button');
            editBtn.className = 'btn btn-sm btn-edit';
            editBtn.textContent = i18n.t('edit-user');
            editBtn.addEventListener('click', () => this.showEditUserModal(user));
            actionCell.appendChild(editBtn);

            const toggleBtn = document.createElement('button');
            if (user.enabled && !user.isLocked) {
                toggleBtn.className = 'btn btn-sm btn-disable';
                toggleBtn.textContent = i18n.t('disable-user');
                toggleBtn.addEventListener('click', () => this.toggleUserStatus(user.username, false));
            } else {
                toggleBtn.className = 'btn btn-sm btn-enable';
                toggleBtn.textContent = i18n.t('enable-user');
                toggleBtn.addEventListener('click', () => this.toggleUserStatus(user.username, true));
            }
            actionCell.appendChild(toggleBtn);

            // 解锁按钮
            if (user.isLocked) {
                const unlockBtn = document.createElement('button');
                unlockBtn.className = 'btn btn-sm btn-enable';
                unlockBtn.textContent = i18n.t('unlock-user');
                unlockBtn.addEventListener('click', () => this.unlockUser(user.username));
                actionCell.appendChild(unlockBtn);
            }

            // 删除按钮（不能删除 admin）
            if (user.username !== 'admin') {
                const delBtn = document.createElement('button');
                delBtn.className = 'btn btn-sm btn-delete';
                delBtn.textContent = i18n.t('delete-user');
                delBtn.addEventListener('click', () => {
                    if (confirm(`${i18n.t('confirm-delete-user-msg')} ${user.username} ${i18n.t('confirm-suffix')}`)) {
                        this.deleteUser(user.username);
                    }
                });
                actionCell.appendChild(delBtn);
            }

            row.appendChild(td(user.username));
            row.appendChild(td(roleText));
            row.appendChild(td(lastLogin));
            row.appendChild(td(badge));
            row.appendChild(actionCell);
            tbody.appendChild(row);
        });
    },

    // ============ 角色管理 ============
    loadRoles() {
        apiGet('/api/roles')
            .then(res => {
                if (!res || !res.success) return;
                const roles = (res.data && res.data.roles) ? res.data.roles : [];
                const permissions = (res.data && res.data.permissions) ? res.data.permissions : [];
                this._permDefs = permissions;  // 保存权限定义
                this._renderRolesList(roles);
            })
            .catch(() => {});
    },

    _renderRolesList(roles) {
        const tbody = document.getElementById('roles-table-body');
        if (!tbody) return;
        tbody.innerHTML = '';

        roles.forEach(role => {
            const row = document.createElement('tr');
            
            // 角色ID
            const tdId = document.createElement('td');
            tdId.textContent = role.id;
            row.appendChild(tdId);
            
            // 角色名称 - 翻译内置角色名
            const tdName = document.createElement('td');
            const _rnm = {'管理员': 'role-admin', '操作员': 'role-operator', '查看者': 'role-viewer'};
            const dName = _rnm[role.name] ? i18n.t(_rnm[role.name]) : role.name;
            tdName.textContent = dName;
            row.appendChild(tdName);
            
            // 描述
            const tdDesc = document.createElement('td');
            tdDesc.textContent = role.description || '—';
            tdDesc.style.maxWidth = '200px';
            tdDesc.style.overflow = 'hidden';
            tdDesc.style.textOverflow = 'ellipsis';
            tdDesc.style.whiteSpace = 'nowrap';
            row.appendChild(tdDesc);
            
            // 权限数
            const tdPermCount = document.createElement('td');
            const permCount = (role.permissions || []).length;
            tdPermCount.innerHTML = `<span style="background: #e6f7ff; color: #1890ff; padding: 2px 8px; border-radius: 10px; font-size: 12px;">${permCount}</span>`;
            row.appendChild(tdPermCount);
            
            // 类型
            const tdType = document.createElement('td');
            if (role.id === 'admin') {
                tdType.innerHTML = `<span style="background: #fff1f0; color: #f5222d; padding: 2px 8px; border-radius: 4px; font-size: 12px;">${i18n.t('role-type-super')}</span>`;
            } else if (role.isBuiltin) {
                tdType.innerHTML = `<span style="background: #f6ffed; color: #52c41a; padding: 2px 8px; border-radius: 4px; font-size: 12px;">${i18n.t('role-type-builtin')}</span>`;
            } else {
                tdType.innerHTML = `<span style="background: #f0f5ff; color: #2f54eb; padding: 2px 8px; border-radius: 4px; font-size: 12px;">${i18n.t('role-type-custom')}</span>`;
            }
            row.appendChild(tdType);
            
            // 操作
            const tdAction = document.createElement('td');
            tdAction.className = 'u-toolbar-sm';
            
            // 查看权限按钮
            const viewBtn = document.createElement('button');
            viewBtn.className = 'btn btn-sm btn-secondary';
            viewBtn.textContent = i18n.t('role-view-perms');
            viewBtn.addEventListener('click', () => this.showRolePermissions(role));
            tdAction.appendChild(viewBtn);
            
            // 仅 admin 角色不可编辑/删除
            if (role.id !== 'admin') {
                // 编辑按钮
                const editBtn = document.createElement('button');
                editBtn.className = 'btn btn-sm btn-edit';
                editBtn.textContent = i18n.t('role-edit');
                editBtn.style.marginRight = '5px';
                editBtn.addEventListener('click', () => this.showEditRoleModal(role.id));
                tdAction.appendChild(editBtn);
                
                // 删除按钮
                const delBtn = document.createElement('button');
                delBtn.className = 'btn btn-sm btn-delete';
                delBtn.textContent = i18n.t('role-delete');
                delBtn.addEventListener('click', () => {
                    const _rnm2 = {'管理员': 'role-admin', '操作员': 'role-operator', '查看者': 'role-viewer'};
                    const dName2 = _rnm2[role.name] ? i18n.t(_rnm2[role.name]) : role.name;
                    this.deleteRole(role.id, dName2);
                });
                tdAction.appendChild(delBtn);
            }
            
            row.appendChild(tdAction);
            tbody.appendChild(row);
        });
    },
    
    // 查看角色权限详情
    showRolePermissions(role) {
        const permDefs = this._permDefs || [];
        const rolePerms = new Set(role.permissions || []);
        
        // 权限组名中文→英文映射key（同时支持中英文）
        const _gpk = {
            '设备':'device', 'Device':'device',
            '网络':'network', 'Network':'network',
            '系统':'system', 'System':'system',
            '用户':'user', 'Users':'user',
            '文件':'file', 'Files':'file',
            '协议':'protocol', 'Protocol':'protocol',
            '审计':'audit', 'Audit':'audit',
            'GPIO':'gpio',
            '外设':'peripheral', 'Peripheral':'peripheral'
        };
        
        // 按分组整理
        const permGroups = {};
        permDefs.forEach(p => {
            if (!permGroups[p.group]) permGroups[p.group] = [];
            permGroups[p.group].push(p);
        });
        
        let html = `<div style="max-height: 400px; overflow-y: auto;">`;
        Object.keys(permGroups).sort().forEach(group => {
            const gKey = _gpk[group] ? i18n.t('perm-group-' + _gpk[group]) : group;
            html += `<div style="margin-bottom: 15px;">`;
            html += `<h4 style="margin: 0 0 8px; font-size: 14px; color: #333; border-bottom: 1px solid #eee; padding-bottom: 5px;">${gKey}</h4>`;
            html += `<div style="display: flex; flex-wrap: wrap; gap: 8px;">`;
            permGroups[group].forEach(perm => {
                const hasPerm = rolePerms.has(perm.id);
                const pName = i18n.t('perm-' + perm.id) !== ('perm-' + perm.id) ? i18n.t('perm-' + perm.id) : perm.name;
                const pDesc = i18n.t('perm-' + perm.id) !== ('perm-' + perm.id) ? i18n.t('perm-' + perm.id) : perm.description;
                html += `<span style="display: inline-flex; align-items: center; padding: 4px 10px; border-radius: 4px; font-size: 12px; ${hasPerm ? 'background: #e6f7ff; color: #1890ff; border: 1px solid #91d5ff;' : 'background: #f5f5f5; color: #999; border: 1px solid #d9d9d9;'}">`;
                html += `<span style="margin-right: 4px;">${hasPerm ? '✓' : '✗'}</span>`;
                html += `<span title="${pDesc}">${pName}</span></span>`;
            });
            html += `</div></div>`;
        });
        html += `</div>`;
        
        // 翻译角色名
        const roleNameMap = {'管理员': 'role-admin', '操作员': 'role-operator', '查看者': 'role-viewer'};
        const displayRoleName = roleNameMap[role.name] ? i18n.t(roleNameMap[role.name]) : role.name;
        
        // 使用简单的 alert 弹窗显示（或可以创建一个模态窗）
        const div = document.createElement('div');
        div.innerHTML = `
            <div style="position: fixed; top: 0; left: 0; right: 0; bottom: 0; background: rgba(0,0,0,0.5); z-index: 9999; display: flex; justify-content: center; align-items: center;" onclick="this.remove()">
                <div style="background: white; border-radius: 8px; max-width: 600px; width: 90%; max-height: 80vh; overflow: hidden;" onclick="event.stopPropagation()">
                    <div style="background: linear-gradient(135deg, #1890ff 0%, #096dd9 100%); color: white; padding: 15px 20px; display: flex; justify-content: space-between; align-items: center;">
                        <h3 style="margin: 0;">${displayRoleName}${i18n.t('role-detail-suffix')}</h3>
                        <button onclick="this.closest('div[style*=position]').remove()" style="background: none; border: none; color: white; font-size: 20px; cursor: pointer;">×</button>
                    </div>
                    <div style="padding: 20px;">${html}</div>
                </div>
            </div>
        `;
        document.body.appendChild(div);
    },
    
    // ============ 角色管理 CRUD ============
    
    showAddRoleModal() {
        const modal = document.getElementById('role-modal');
        if (!modal) return;
        
        // 设置标题
        const title = document.getElementById('role-modal-title');
        if (title) title.textContent = i18n.t('role-add-title');
        
        // 清空输入
        const idInput = document.getElementById('role-id-input');
        const nameInput = document.getElementById('role-name-input');
        const descInput = document.getElementById('role-desc-input');
        if (idInput) { idInput.value = ''; idInput.disabled = false; }
        if (nameInput) nameInput.value = '';
        if (descInput) descInput.value = '';
        
        // 标记为新增模式
        modal.dataset.editMode = 'add';
        modal.dataset.editRoleId = '';
        
        // 渲染权限复选框
        this._renderPermissionCheckboxes([]);
        
        // 清除错误提示
        const errDiv = document.getElementById('role-error');
        if (errDiv) errDiv.style.display = 'none';
        
        modal.style.display = 'flex';
    },
    
    showEditRoleModal(roleId) {
        const modal = document.getElementById('role-modal');
        if (!modal) {
            console.error('[showEditRoleModal] modal not found!');
            return;
        }
        
        console.log('[showEditRoleModal] Opening edit modal for roleId:', roleId);
        
        // 先设置编辑模式，防止API回调前用户点击保存导致调用错误的API
        modal.dataset.editMode = 'edit';
        modal.dataset.editRoleId = roleId;
        
        // 获取角色信息
        apiGet('/api/roles')
            .then(res => {
                if (!res || !res.success) return;
                const roles = (res.data && res.data.roles) ? res.data.roles : [];
                const role = roles.find(r => r.id === roleId);
                if (!role) {
                    Notification.error(i18n.t('role-not-exist'), i18n.t('error-title'));
                    return;
                }
                
                // 设置标题
                const title = document.getElementById('role-modal-title');
                if (title) title.textContent = i18n.t('role-edit-title');
                
                // 填充数据
                const idInput = document.getElementById('role-id-input');
                const nameInput = document.getElementById('role-name-input');
                const descInput = document.getElementById('role-desc-input');
                if (idInput) { idInput.value = role.id; idInput.disabled = true; }
                if (nameInput) nameInput.value = role.name;
                if (descInput) descInput.value = role.description || '';
                
                console.log('[showEditRoleModal] Set editMode:', modal.dataset.editMode, 'editRoleId:', modal.dataset.editRoleId);
                
                // 渲染权限复选框
                this._renderPermissionCheckboxes(role.permissions || []);
                
                // 清除错误提示
                const errDiv = document.getElementById('role-error');
                if (errDiv) errDiv.style.display = 'none';
                
                modal.style.display = 'flex';
            })
            .catch(() => {});
    },
    
    _renderPermissionCheckboxes(selectedPerms) {
        const container = document.getElementById('role-permissions-container');
        if (!container) return;
        container.innerHTML = '';
        
        const permDefs = this._permDefs || [];
        const selectedSet = new Set(selectedPerms);
        
        // 权限组名中文→英文映射key（同时支持中英文）
        const _gpk = {
            '设备':'device', 'Device':'device',
            '网络':'network', 'Network':'network',
            '系统':'system', 'System':'system',
            '用户':'user', 'Users':'user',
            '文件':'file', 'Files':'file',
            '协议':'protocol', 'Protocol':'protocol',
            '审计':'audit', 'Audit':'audit',
            'GPIO':'gpio',
            '外设':'peripheral', 'Peripheral':'peripheral'
        };
        
        // 按分组整理
        const permGroups = {};
        permDefs.forEach(p => {
            if (!permGroups[p.group]) permGroups[p.group] = [];
            permGroups[p.group].push(p);
        });
        
        Object.keys(permGroups).sort().forEach(group => {
            const groupDiv = document.createElement('div');
            groupDiv.style.cssText = 'margin-bottom: 12px;';
            
            const gKey = _gpk[group] ? i18n.t('perm-group-' + _gpk[group]) : group;
            const groupTitle = document.createElement('h5');
            groupTitle.style.cssText = 'margin: 0 0 8px; font-size: 13px; color: #666; border-bottom: 1px solid #eee; padding-bottom: 5px;';
            groupTitle.textContent = gKey;
            groupDiv.appendChild(groupTitle);
            
            const permList = document.createElement('div');
            permList.style.cssText = 'display: flex; flex-wrap: wrap; gap: 10px;';
            
            permGroups[group].forEach(perm => {
                const pName = i18n.t('perm-' + perm.id) !== ('perm-' + perm.id) ? i18n.t('perm-' + perm.id) : perm.name;
                const pDesc = i18n.t('perm-' + perm.id) !== ('perm-' + perm.id) ? i18n.t('perm-' + perm.id) : perm.description;
                const label = document.createElement('label');
                label.style.cssText = 'display: flex; align-items: center; cursor: pointer; font-size: 12px;';
                label.innerHTML = `
                    <input type="checkbox" name="role-perm" value="${perm.id}" ${selectedSet.has(perm.id) ? 'checked' : ''} style="margin-right: 4px;">
                    <span title="${pDesc}">${pName}</span>
                `;
                permList.appendChild(label);
            });
            
            groupDiv.appendChild(permList);
            container.appendChild(groupDiv);
        });
    },
    
    saveRole() {
        const modal = document.getElementById('role-modal');
        if (!modal) {
            Notification.error(i18n.t('role-modal-not-found'), i18n.t('error-title'));
            return;
        }
        
        const isEditMode = modal.dataset.editMode === 'edit';
        const editRoleId = modal.dataset.editRoleId || '';
        
        const idInput = document.getElementById('role-id-input');
        const id = isEditMode ? editRoleId : (idInput ? idInput.value.trim() : '');
        const name = ((document.getElementById('role-name-input') || {}).value || '').trim();
        const description = ((document.getElementById('role-desc-input') || {}).value || '').trim();
        const errDiv = document.getElementById('role-error');
        
        const showErr = (msg) => {
            if (errDiv) { errDiv.textContent = msg; errDiv.style.display = 'block'; }
            Notification.error(msg, isEditMode ? i18n.t('role-fail-edit') : i18n.t('role-fail-add'));
        };
        
        if (!id) return showErr(i18n.t('role-validate-id'));
        if (!name) return showErr(i18n.t('role-validate-name'));
        
        // 获取选中的权限
        const permCheckboxes = document.querySelectorAll('input[name="role-perm"]:checked');
        const permissions = Array.from(permCheckboxes).map(cb => cb.value).join(',');
        
        if (errDiv) errDiv.style.display = 'none';
        
        const btn = document.getElementById('confirm-role-btn');
        if (btn) { btn.disabled = true; btn.textContent = i18n.t('role-saving-text'); }
        
        console.log('[saveRole] modal.dataset:', JSON.stringify(modal.dataset));
        console.log('[saveRole] isEditMode:', isEditMode, 'id:', id, 'editRoleId:', editRoleId);
        console.log('[saveRole] Will call API:', isEditMode ? '/api/roles/update' : '/api/roles');
        
        let apiCall;
        if (isEditMode) {
            // 编辑模式：先更新角色信息，再更新权限
            apiCall = apiPost('/api/roles/update', { id, name, description })
                .then(res => {
                    if (res && res.success) {
                        return apiPost('/api/roles/permissions', { id, permissions });
                    }
                    throw new Error(res.error || i18n.t('role-fail-update-msg'));
                });
        } else {
            // 新增模式
            apiCall = apiPost('/api/roles', { id, name, description })
                .then(res => {
                    if (res && res.success) {
                        return apiPost('/api/roles/permissions', { id, permissions });
                    }
                    throw new Error(res.error || i18n.t('role-fail-create-msg'));
                });
        }
        
        apiCall
            .then(res => {
                if (res && res.success) {
                    Notification.success(`${i18n.t('role-mgmt-title')}: ${name} ${isEditMode ? i18n.t('role-updated') : i18n.t('role-created')}${i18n.t('role-success-suffix')}`, i18n.t('role-mgmt-title'));
                    if (modal) modal.style.display = 'none';
                    this.loadRoles();
                } else {
                    showErr((res && res.error) || i18n.t('role-op-fail'));
                }
            })
            .catch(err => {
                showErr(err.message || i18n.t('role-op-fail'));
            })
            .finally(() => {
                if (btn) { btn.disabled = false; btn.textContent = i18n.t('role-save-text'); }
            });
    },
    
    deleteRole(roleId, roleName) {
        if (!confirm(`${i18n.t('confirm-delete-role-msg')} "${roleName || roleId}" ${i18n.t('confirm-delete-role-suffix')}`)) return;
        
        apiPost('/api/roles/delete', { id: roleId })
            .then(res => {
                if (res && res.success) {
                    Notification.success(`${roleName || roleId} ${i18n.t('role-deleted-msg')}`, i18n.t('delete-success'));
                    this.loadRoles();
                } else {
                    Notification.error((res && res.error) || i18n.t('operation-fail'), i18n.t('operation-fail'));
                }
            })
            .catch(() => {});
    },

    // ============ 修改密码 ============
    showChangePasswordModal() {
        const modal = document.getElementById('change-password-modal');
        if (modal) modal.style.display = 'flex';
        ['current-password-input', 'new-password-input', 'confirm-password-input'].forEach(id => {
            const el = document.getElementById(id); if (el) el.value = '';
        });
        const errDiv = document.getElementById('password-error');
        if (errDiv) errDiv.style.display = 'none';
    },

    changePassword() {
        const oldPwd = (document.getElementById('current-password-input') || {}).value || '';
        const newPwd = (document.getElementById('new-password-input') || {}).value || '';
        const confirmPwd = (document.getElementById('confirm-password-input') || {}).value || '';
        const errDiv = document.getElementById('password-error');

        const showErr = (msg) => {
            if (errDiv) { errDiv.textContent = msg; errDiv.style.display = 'block'; }
            Notification.error(msg, i18n.t('change-pwd-fail'));
        };

        if (!oldPwd || !newPwd || !confirmPwd) return showErr(i18n.t('validate-all-fields'));
        if (newPwd !== confirmPwd) return showErr(i18n.t('password-error') || i18n.t('validate-new-pwd-mismatch'));
        if (newPwd.length < 6) return showErr(i18n.t('validate-new-pwd-len'));

        if (errDiv) errDiv.style.display = 'none';

        const btn = document.getElementById('confirm-password-btn');
        if (btn) { btn.disabled = true; btn.textContent = i18n.t('change-pwd-submitting'); }

        apiPost('/api/auth/change-password', { oldPassword: oldPwd, newPassword: newPwd })
            .then(res => {
                if (res && res.success) {
                    Notification.success(i18n.t('change-pwd-success-msg'), i18n.t('change-pwd-success-title'));
                    const modal = document.getElementById('change-password-modal');
                    if (modal) modal.style.display = 'none';
                    // 修改密码后后端会踢出所有会话，需重新登录
                    setTimeout(() => {
                        localStorage.removeItem('auth_token');
                        this._showLoginPage();
                    }, 1500);
                } else {
                    showErr((res && res.error) || i18n.t('change-pwd-fail-msg'));
                }
            })
            .catch(() => {})
            .finally(() => { if (btn) { btn.disabled = false; btn.textContent = i18n.t('confirm-change-btn'); } });
    },

    // ============ 添加用户 ============
        showAddUserModal() {
        const modal = document.getElementById('add-user-modal');
        if (modal) {
            // 先重置为添加模式，再显示弹窗
            modal.dataset.editMode = 'add';
            modal.dataset.editUsername = '';
            modal.style.display = 'flex';
        }
        
        // 修改标题
        const title = document.getElementById('add-user-title');
        if (title) title.textContent = i18n.t('add-user-modal-title');
        
        // 修改按钮文本
        const confirmBtn = document.getElementById('confirm-add-user-btn');
        if (confirmBtn) confirmBtn.textContent = i18n.t('confirm-add-btn');
        
        // 清空输入框
        ['add-username-input', 'add-password-input', 'add-confirm-password-input'].forEach(id => {
            const el = document.getElementById(id); if (el) el.value = '';
        });
        
        // 启用用户名输入
        const usernameInput = document.getElementById('add-username-input');
        if (usernameInput) usernameInput.disabled = false;
        
        const sel = document.getElementById('add-role-select');
        if (sel) sel.value = 'operator';
        const errDiv = document.getElementById('add-user-error');
        if (errDiv) errDiv.style.display = 'none';
    },

    addUser() {
        const modal = document.getElementById('add-user-modal');
        const isEditMode = modal && modal.dataset.editMode === 'edit';
        const editUsername = modal ? modal.dataset.editUsername : '';
        
        // 调试日志
        console.log('[addUser] modal:', modal);
        console.log('[addUser] modal.dataset:', modal ? JSON.stringify(modal.dataset) : 'null');
        console.log('[addUser] isEditMode:', isEditMode, 'editUsername:', editUsername);
        
        const username = isEditMode ? editUsername : ((document.getElementById('add-username-input') || {}).value || '').trim();
        const password = (document.getElementById('add-password-input') || {}).value || '';
        const confirmPwd = (document.getElementById('add-confirm-password-input') || {}).value || '';
        const role = (document.getElementById('add-role-select') || {}).value || 'operator';
        const errDiv = document.getElementById('add-user-error');

        const showErr = (msg) => {
            if (errDiv) { errDiv.textContent = msg; errDiv.style.display = 'block'; }
            Notification.error(msg, isEditMode ? i18n.t('edit-user-fail') : i18n.t('add-user-fail'));
        };

        if (!username) return showErr(i18n.t('validate-username-empty'));
        
        // 用户名长度验证
        if (username.length < 3 || username.length > 32) return showErr(i18n.t('validate-username-len'));
        
        // 密码验证（添加和编辑模式都要求密码必填）
        if (!password || !confirmPwd) return showErr(i18n.t('validate-pwd-empty'));
        if (password !== confirmPwd) return showErr(i18n.t('validate-pwd-mismatch'));
        if (password.length < 6) return showErr(i18n.t('validate-pwd-len'));

        if (errDiv) errDiv.style.display = 'none';

        const btn = document.getElementById('confirm-add-user-btn');
        if (btn) { btn.disabled = true; btn.textContent = isEditMode ? i18n.t('saving-btn') : i18n.t('adding-btn'); }

        // 根据模式选择 API
        let apiCall;
        if (isEditMode) {
            // 编辑模式：使用 POST /api/users/update
            apiCall = apiPost('/api/users/update', { username, password, role, enabled: 'true' });
        } else {
            // 添加模式
            apiCall = apiPost('/api/users', { username, password, role });
        }
        
        apiCall
            .then(res => {
                if (res && res.success) {
                    Notification.success(`${username} ${isEditMode ? i18n.t('role-updated') : i18n.t('role-created')}${i18n.t('role-success-suffix')}`, isEditMode ? i18n.t('edit-user-success') : i18n.t('add-user-success'));
                    // 关闭 modal 并重置状态
                    if (modal) {
                        modal.style.display = 'none';
                        modal.dataset.editMode = 'add';
                        modal.dataset.editUsername = '';
                    }
                    // 重置用户名输入框状态
                    const usernameInput = document.getElementById('add-username-input');
                    if (usernameInput) usernameInput.disabled = false;
                    this.loadUsers(); // 刷新列表
                } else {
                    showErr((res && res.error) || (isEditMode ? i18n.t('modify-user-fail-msg') : i18n.t('add-user-fail-msg')));
                }
            })
            .catch(() => {})
            .finally(() => { if (btn) { btn.disabled = false; btn.textContent = i18n.t('confirm-add-btn'); } });
    },

    // ============ 编辑用户（复用添加用户弹窗）============
    showEditUserModal(user) {
        console.log('[showEditUserModal] Called with user:', user);
        
        // 复用添加用户的 modal
        const modal = document.getElementById('add-user-modal');
        if (!modal) {
            console.error('[showEditUserModal] Modal not found!');
            Notification.info(`${i18n.t('edit-user-modal-title')}: ${user.username}`, i18n.t('edit-user-modal-title'));
            return;
        }
        
        // 先标记为编辑模式，防止状态丢失
        modal.dataset.editMode = 'edit';
        modal.dataset.editUsername = user.username;
        console.log('[showEditUserModal] Set editMode:', modal.dataset.editMode, 'editUsername:', modal.dataset.editUsername);
        
        // 修改标题
        const title = document.getElementById('add-user-title');
        if (title) title.textContent = i18n.t('edit-user-modal-title');
        
        // 填充用户信息
        const usernameInput = document.getElementById('add-username-input');
        if (usernameInput) {
            usernameInput.value = user.username;
            usernameInput.disabled = true;  // 用户名不可修改
        }
        
        // 清空密码字段（编辑时密码可选）
        const pwdInput = document.getElementById('add-password-input');
        const confirmInput = document.getElementById('add-confirm-password-input');
        if (pwdInput) pwdInput.value = '';
        if (confirmInput) confirmInput.value = '';
        
        // 设置角色
        const roleSelect = document.getElementById('add-role-select');
        if (roleSelect) roleSelect.value = user.role || 'operator';
        
        // 修改确认按钮文本
        const confirmBtn = document.getElementById('confirm-add-user-btn');
        if (confirmBtn) confirmBtn.textContent = i18n.t('confirm-save-btn');
        
        // 显示弹窗
        modal.style.display = 'flex';
        
        // 清除错误提示
        const errDiv = document.getElementById('add-user-error');
        if (errDiv) errDiv.style.display = 'none';
    },

    // ============ 切换用户状态（启用/禁用）============
    toggleUserStatus(username, enable) {
        const action = enable ? i18n.t('enable-user') : i18n.t('disable-user');
        const confirmMsg = enable ? i18n.t('confirm-enable-user') : i18n.t('confirm-disable-user');
        if (!confirm(`${confirmMsg} ${username} ${i18n.t('confirm-suffix')}`)) return;

        apiPost('/api/users/update', { username, enabled: enable ? 'true' : 'false' })
            .then(res => {
                if (res && res.success) {
                    Notification.success(`${username} ${enable ? i18n.t('user-enabled-msg') : i18n.t('user-disabled-msg')}`, i18n.t('user-status-update'));
                    this.loadUsers();
                } else {
                    Notification.error((res && res.error) || i18n.t('operation-fail'), i18n.t('operation-fail'));
                }
            })
            .catch(() => {});
    },

    // ============ 解锁用户 ============
    unlockUser(username) {
        if (!confirm(`${i18n.t('confirm-unlock-user')} ${username} ${i18n.t('confirm-suffix')}`)) return;
        apiPost('/api/users/unlock-account', { username })
            .then(res => {
                if (res && res.success) {
                    Notification.success(`${username} ${i18n.t('user-unlocked-msg')}`, i18n.t('unlock-success'));
                    this.loadUsers();
                } else {
                    Notification.error((res && res.error) || i18n.t('operation-fail'), i18n.t('operation-fail'));
                }
            })
            .catch(() => {});
    },

    // ============ 删除用户 ============
    deleteUser(username) {
        if (!confirm(`${i18n.t('confirm-delete-user-msg')} ${username} ${i18n.t('confirm-suffix')}`)) return;
        
        apiPost('/api/users/delete', { username })
            .then(res => {
                if (res && res.success) {
                    Notification.success(`${username} ${i18n.t('user-deleted-msg')}`, i18n.t('delete-success'));
                    this.loadUsers();
                } else {
                    Notification.error((res && res.error) || i18n.t('operation-fail'), i18n.t('operation-fail'));
                }
            })
            .catch(() => {});
    },

    // ============ 退出登录 ============
    logout() {
        if (!confirm(i18n.t('logout-confirm') || '确定要退出登录吗？')) return;

        this._showLoginPage();
        document.getElementById('login-form') && document.getElementById('login-form').reset();

        apiPost('/api/auth/logout', {})
            .then(() => {})
            .catch(() => {})
            .finally(() => {
                localStorage.removeItem('auth_token');
                localStorage.removeItem('sessionId');
                localStorage.removeItem('password');
                sessionStorage.removeItem('savedPassword');
                sessionStorage.removeItem('currentUsername');
                Notification.success(i18n.t('logout-success'), i18n.t('logout-title'));
            });
    },

    // ============ 关闭所有浮层（token 过期时调用）============
    closeAllOverlays() {
        // 1. 关闭所有 .modal 弹窗
        document.querySelectorAll('.modal').forEach(function (m) {
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
        this.stopLogAutoRefresh();

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
    
    // ============ 日志管理 ============
    
    /**
     * 加载日志文件列表
     */
    loadLogFileList() {
        const listContainer = document.getElementById('log-file-list');
        if (!listContainer) return;
        
        apiGet('/api/logs/list')
            .then(res => {
                if (!res || !res.success) {
                    listContainer.innerHTML = '<div style="color: #f56c6c; padding: 10px;">' + i18n.t('log-load-fail-html') + '</div>';
                    return;
                }
                
                const files = res.data || [];
                if (files.length === 0) {
                    listContainer.innerHTML = '<div style="color: #666; padding: 10px;">' + i18n.t('log-empty-html') + '</div>';
                    return;
                }
                
                // 按文件名排序，system.log 排最前面
                files.sort((a, b) => {
                    if (a.name === 'system.log') return -1;
                    if (b.name === 'system.log') return 1;
                    return b.name.localeCompare(a.name); // 新文件在前
                });
                
                let html = '';
                files.forEach(file => {
                    const sizeStr = file.size < 1024 ? `${file.size} B` : `${(file.size / 1024).toFixed(1)} KB`;
                    const isActive = file.name === this._currentLogFile;
                    const activeStyle = isActive ? 'background: #3a3a3a; color: #fff;' : 'color: #ccc;';
                    const icon = file.current ? '📄' : '📃';
                    html += `<div class="log-file-item" style="padding: 8px 10px; cursor: pointer; border-bottom: 1px solid #333; ${activeStyle}" data-file="${file.name}">
                        <span style="margin-right: 5px;">${icon}</span>
                        <span style="font-size: 12px;">${file.name}</span>
                        <span style="font-size: 11px; color: #888; float: right;">${sizeStr}</span>
                    </div>`;
                });
                
                listContainer.innerHTML = html;
                
                // 绑定点击事件
                listContainer.querySelectorAll('.log-file-item').forEach(item => {
                    item.addEventListener('click', () => {
                        const fileName = item.dataset.file;
                        this._currentLogFile = fileName;
                        // 更新当前文件显示
                        const currentSpan = document.getElementById('current-log-file');
                        if (currentSpan) currentSpan.textContent = i18n.t('log-current-file-prefix') + fileName;
                        // 更新选中状态
                        listContainer.querySelectorAll('.log-file-item').forEach(i => {
                            i.style.background = '';
                            i.style.color = '#ccc';
                        });
                        item.style.background = '#3a3a3a';
                        item.style.color = '#fff';
                        // 加载日志内容
                        this.loadLogs(500, fileName);
                    });
                    
                    // 鼠标悬停效果
                    item.addEventListener('mouseenter', () => {
                        if (item.style.background !== '#3a3a3a') {
                            item.style.background = '#2a2a2a';
                        }
                    });
                    item.addEventListener('mouseleave', () => {
                        if (item.style.background === '#2a2a2a') {
                            item.style.background = '';
                        }
                    });
                });
            })
            .catch(err => {
                console.error('Load log file list failed:', err);
                listContainer.innerHTML = '<div style="color: #f56c6c; padding: 10px;">' + i18n.t('log-load-fail-html') + '</div>';
            });
    },
    
    /**
     * 加载日志内容
     * @param {number} maxLines 最大行数，默认500
     * @param {string} fileName 日志文件名，默认为当前选中的文件
     */
    loadLogs(maxLines = 500, fileName = null) {
        const container = document.getElementById('device-log-container');
        const infoSpan = document.getElementById('log-info');
        
        if (!container) return;
        
        // 使用传入的文件名或当前选中的文件
        const logFile = fileName || this._currentLogFile || 'system.log';
        this._currentLogFile = logFile;
        
        apiGet('/api/logs', { lines: maxLines, file: logFile })
            .then(res => {
                if (!res || !res.success) {
                    container.innerHTML = i18n.t('log-load-fail-html');
                    return;
                }
                
                const data = res.data || {};
                const content = data.content || '';
                const fileSize = data.size || 0;
                const lineCount = data.lines || 0;
                const truncated = data.truncated || false;
                
                // 更新日志信息
                if (infoSpan) {
                    const sizeStr = fileSize < 1024 ? `${fileSize} B` : 
                                   fileSize < 1024 * 1024 ? `${(fileSize / 1024).toFixed(1)} KB` :
                                   `${(fileSize / 1024 / 1024).toFixed(2)} MB`;
                    let infoText = `${lineCount}${i18n.t('log-line-unit')}${sizeStr}`;
                    if (truncated) infoText += i18n.t('log-truncated-suffix');
                    infoSpan.textContent = infoText;
                }
                
                // 格式化并显示日志内容
                if (!content || content.trim() === '') {
                    container.innerHTML = i18n.t('log-empty-html');
                } else {
                    container.innerHTML = this._formatLogContent(content);
                    // 滚动到底部
                    container.scrollTop = container.scrollHeight;
                }
            })
            .catch(err => {
                console.error('Load logs failed:', err);
                container.innerHTML = i18n.t('log-load-fail-html');
            });
    },
    
    /**
     * 格式化日志内容，添加颜色高亮
     * @param {string} content 日志内容
     * @returns {string} 格式化后的HTML
     */
    _formatLogContent(content) {
        const lines = content.split('\n');
        return lines.map(line => {
            // 跳过空行
            if (!line.trim()) return '';
            
            // 根据日志级别设置颜色
            let className = '';
            if (line.includes('[ERROR]') || line.includes('[E]')) {
                className = 'log-error';
            } else if (line.includes('[WARN]') || line.includes('[W]')) {
                className = 'log-warn';
            } else if (line.includes('[INFO]') || line.includes('[I]')) {
                className = 'log-info';
            } else if (line.includes('[DEBUG]') || line.includes('[D]')) {
                className = 'log-debug';
            }
            
            // HTML 转义
            const escaped = line
                .replace(/&/g, '&amp;')
                .replace(/</g, '&lt;')
                .replace(/>/g, '&gt;');
            
            return `<div class="${className}">${escaped}</div>`;
        }).filter(line => line).join('');
    },
    
    /**
     * 清空日志
     */
    clearLogs() {
        if (!confirm(i18n.t('log-clear-confirm-msg'))) return;
        
        const btn = document.getElementById('clear-logs-btn');
        if (btn) {
            btn.disabled = true;
            btn.innerHTML = i18n.t('log-clearing-html');
        }
        
        apiPost('/api/logs/clear', {})
            .then(res => {
                if (res && res.success) {
                    Notification.success(i18n.t('log-cleared-msg'), i18n.t('log-op-title'));
                    this.loadLogs();  // 重新加载
                } else {
                    Notification.error((res && res.error) || i18n.t('log-op-fail'), i18n.t('log-op-fail'));
                }
            })
            .catch(err => {
                console.error('Clear logs failed:', err);
                Notification.error(i18n.t('log-op-fail'), i18n.t('log-op-fail'));
            })
            .finally(() => {
                if (btn) {
                    btn.disabled = false;
                    btn.innerHTML = i18n.t('log-clear-btn-html');
                }
            });
    },
    
    /**
     * 启动日志自动刷新
     * @param {number} interval 刷新间隔（毫秒），默认2000ms
     */
    startLogAutoRefresh(interval = 2000) {
        // 先停止已有的定时器
        this.stopLogAutoRefresh();
        
        // 启动新的定时器
        this._logAutoRefreshTimer = setInterval(() => {
            // 仅在日志页面激活时刷新
            if (this.currentPage === 'logs') {
                this.loadLogs();
            }
        }, interval);
        
        console.log('[Logs] Auto refresh started with interval:', interval);
    },
    
    /**
     * 停止日志自动刷新
     */
    stopLogAutoRefresh() {
        if (this._logAutoRefreshTimer) {
            clearInterval(this._logAutoRefreshTimer);
            this._logAutoRefreshTimer = null;
            console.log('[Logs] Auto refresh stopped');
        }
    },
    
    // ============ 文件管理 ============
    
    /**
     * 加载文件系统信息
     */
    loadFileSystemInfo() {
        apiGet('/api/filesystem')
            .then(res => {
                if (res && res.success) {
                    const d = res.data || {};
                    const infoSpan = document.getElementById('fs-info');
                    if (infoSpan) {
                        const total = d.totalBytes ? (d.totalBytes / 1024 / 1024).toFixed(2) + ' MB' : 'N/A';
                        const used = d.usedBytes ? (d.usedBytes / 1024 / 1024).toFixed(2) + ' MB' : 'N/A';
                        infoSpan.textContent = `${i18n.t('fs-space-prefix')}${total}${i18n.t('fs-space-used-prefix')}${used}`;
                    }
                }
            })
            .catch(() => {});
    },
    
    /**
     * 加载文件树
     * @param {string} path 目录路径
     */
    loadFileTree(path) {
        const treeContainer = document.getElementById('file-tree');
        if (!treeContainer) return;
        
        // 记录当前路径
        this._currentDir = path;
        
        // 更新路径显示
        const pathEl = document.getElementById('current-dir-path');
        if (pathEl) pathEl.textContent = path;
        
        treeContainer.innerHTML = i18n.t('fs-loading-text');
        
        apiGet('/api/files', { path: path })
            .then(res => {
                if (!res || !res.success) {
                    treeContainer.innerHTML = i18n.t('fs-load-fail-text');
                    return;
                }
                
                const data = res.data || {};
                const dirs = data.dirs || [];
                const files = data.files || [];
                
                let html = '';
                
                // 目录
                dirs.forEach(dir => {
                    html += `<div class="file-tree-item" style="padding: 3px 0; cursor: pointer;" data-path="${path}${dir.name}/" data-type="dir">
                        <span style="color: #e6a23c;">📁</span> ${dir.name}
                    </div>`;
                });
                
                // 文件
                files.forEach(file => {
                    const size = file.size < 1024 ? `${file.size} B` : 
                                file.size < 1024 * 1024 ? `${(file.size / 1024).toFixed(1)} KB` :
                                `${(file.size / 1024 / 1024).toFixed(2)} MB`;
                    html += `<div class="file-tree-item" style="padding: 3px 0; cursor: pointer;" data-path="${path}${file.name}" data-type="file">
                        <span style="color: #409eff;">📄</span> ${file.name} <span style="color: #999; font-size: 11px;">(${size})</span>
                    </div>`;
                });
                
                if (dirs.length === 0 && files.length === 0) {
                    html = i18n.t('fs-empty-dir-html');
                }
                
                treeContainer.innerHTML = html;
                
                // 绑定点击事件
                treeContainer.querySelectorAll('.file-tree-item').forEach(item => {
                    item.addEventListener('click', (e) => {
                        const path = e.currentTarget.dataset.path;
                        const type = e.currentTarget.dataset.type;
                        
                        // 移除其他选中状态
                        treeContainer.querySelectorAll('.file-tree-item').forEach(i => {
                            i.style.background = '';
                        });
                        e.currentTarget.style.background = '#e6f7ff';
                        
                        if (type === 'dir') {
                            this.loadFileTree(path);
                        } else {
                            this.openFile(path);
                        }
                    });
                });
            })
            .catch(err => {
                console.error('Load file tree failed:', err);
                treeContainer.innerHTML = i18n.t('fs-load-fail-text');
            });
    },
    
    /**
     * 打开文件
     * @param {string} path 文件路径
     */
    openFile(path) {
        const editor = document.getElementById('file-editor');
        const pathSpan = document.getElementById('current-file-path');
        const saveBtn = document.getElementById('fs-save-btn');
        const closeBtn = document.getElementById('fs-close-btn');
        const statusDiv = document.getElementById('file-status');
        
        if (!editor || !pathSpan) return;
        
        // 检查是否可编辑
        const editable = path.endsWith('.json') || path.endsWith('.txt') || path.endsWith('.log') ||
                        path.endsWith('.html') || path.endsWith('.js') || path.endsWith('.css');
        
        if (!editable) {
            statusDiv.textContent = i18n.t('fs-file-type-unsupported');
            return;
        }
        
        pathSpan.textContent = path;
        statusDiv.textContent = i18n.t('fs-file-loading');
        
        apiGet('/api/files/content', { path: path })
            .then(res => {
                if (!res || !res.success) {
                    statusDiv.textContent = i18n.t('fs-file-load-fail-prefix') + (res.error || i18n.t('fs-file-unknown-error'));
                    return;
                }
                
                const data = res.data || {};
                editor.value = data.content || '';
                editor.disabled = false;
                closeBtn.disabled = false;

                if (!AppState.currentUser.canManageFs) {
                    // 无 fs.manage 权限：保存按钮永久禁用，编辑器只读
                    saveBtn.disabled = true;
                    editor.readOnly = true;
                    editor.title = i18n.t('fs-no-perm-tip');
                    editor.oninput = null;
                } else {
                    // 有权限：打开文件后保存按钮保持禁用，需编辑后才可保存
                    saveBtn.disabled = true;
                    editor.readOnly = false;
                    editor.title = '';
                    // 监听编辑，有改动后才启用保存
                    editor.oninput = () => {
                        saveBtn.disabled = false;
                        this._currentFileModified = true;
                    };
                }
                
                const size = data.size < 1024 ? `${data.size} B` : 
                            data.size < 1024 * 1024 ? `${(data.size / 1024).toFixed(1)} KB` :
                            `${(data.size / 1024 / 1024).toFixed(2)} MB`;
                statusDiv.textContent = `${i18n.t('fs-file-ready-prefix')}${size}${i18n.t('fs-file-ready-suffix')}`;
                
                this._currentFilePath = path;
                this._currentFileModified = false;
            })
            .catch(err => {
                console.error('Open file failed:', err);
                statusDiv.textContent = i18n.t('fs-file-load-fail');
            });
    },
    
    /**
     * 返回上级目录
     */
    navigateUp() {
        const currentPath = this._currentDir || '/';
        
        // 如果当前是根目录，不做任何操作
        if (currentPath === '/') {
            return;
        }
        
        // 移除末尾的斜杠
        let path = currentPath;
        if (path.endsWith('/')) {
            path = path.slice(0, -1);
        }
        
        // 找到最后一个斜杠的位置
        const lastSlashIndex = path.lastIndexOf('/');
        
        if (lastSlashIndex === 0) {
            // 上级是根目录
            this.loadFileTree('/');
        } else if (lastSlashIndex > 0) {
            // 获取上级目录路径
            const parentPath = path.substring(0, lastSlashIndex + 1);
            this.loadFileTree(parentPath);
        } else {
            // 没有斜杠，回到根目录
            this.loadFileTree('/');
        }
    },
    
    /**
     * 保存当前文件
     */
    saveCurrentFile() {
        if (!this._currentFilePath) return;

        // 权限二次校验
        if (!AppState.currentUser.canManageFs) {
            Notification.warning(i18n.t('fs-no-perm-tip'), i18n.t('fs-mgmt-title'));
            return;
        }
        
        const editor = document.getElementById('file-editor');
        const statusDiv = document.getElementById('file-status');
        const saveBtn = document.getElementById('fs-save-btn');
        
        if (!editor || !statusDiv) return;
        
        const content = editor.value;
        statusDiv.textContent = i18n.t('fs-saving-text');
        saveBtn.disabled = true;
        
        apiPost('/api/files/save', { path: this._currentFilePath, content: content })
            .then(res => {
                if (res && res.success) {
                    statusDiv.textContent = i18n.t('fs-save-ok-text');
                    this._currentFileModified = false;
                    Notification.success(i18n.t('fs-save-ok-msg'), i18n.t('fs-mgmt-title'));
                } else {
                    statusDiv.textContent = i18n.t('fs-save-fail-prefix') + (res.error || '');
                    Notification.error(res.error || i18n.t('fs-save-fail-text'), i18n.t('fs-mgmt-title'));
                }
            })
            .catch(err => {
                console.error('Save file failed:', err);
                statusDiv.textContent = i18n.t('fs-save-fail-text');
                Notification.error(i18n.t('fs-save-fail-text'), i18n.t('fs-mgmt-title'));
            })
            .finally(() => {
                saveBtn.disabled = false;
            });
    },
    
    /**
     * 关闭当前文件
     */
    closeCurrentFile() {
        const editor = document.getElementById('file-editor');
        const pathSpan = document.getElementById('current-file-path');
        const saveBtn = document.getElementById('fs-save-btn');
        const closeBtn = document.getElementById('fs-close-btn');
        const statusDiv = document.getElementById('file-status');
        
        if (this._currentFileModified) {
            if (!confirm(i18n.t('fs-modified-confirm'))) {
                return;
            }
            this.saveCurrentFile();
        }
        
        if (editor) {
            editor.value = '';
            editor.disabled = true;
            editor.readOnly = false;
            editor.title = '';
            editor.oninput = null;  // 清除编辑监听
        }
        if (pathSpan) pathSpan.textContent = i18n.t('fs-select-file-text');
        if (saveBtn) saveBtn.disabled = true;
        if (closeBtn) closeBtn.disabled = true;
        if (statusDiv) statusDiv.textContent = '';
        
        this._currentFilePath = null;
        this._currentFileModified = false;
        
        // 取消文件树中的选中状态
        const treeContainer = document.getElementById('file-tree');
        if (treeContainer) {
            treeContainer.querySelectorAll('.file-tree-item').forEach(i => {
                i.style.background = '';
            });
        }
    },
    
    // ============ 系统监控 ============
    
    /**
     * 加载系统监控数据
     */
    loadSystemMonitor() {
        apiGet('/api/system/info')
            .then(res => {
                if (!res || !res.success) return;
                
                const data = res.data || {};
                
                // 设备信息
                const device = data.device || {};
                this._setText('monitor-chip-model', device.chipModel || 'ESP32');
                this._setText('monitor-cpu-freq', device.cpuFreqMHz || '--');
                this._setText('monitor-sdk', device.sdkVersion || '--');
                
                // 运行时间
                const uptime = data.uptime || {};
                this._setText('monitor-uptime', uptime.formatted || '--');
                
                // 网络状态
                const network = data.network || {};
                const netStatus = network.connected ? 
                    `<span style="color: #52c41a;">●</span> ${i18n.t('monitor-connected')} (${network.ssid || 'N/A'})` : 
                    `<span style="color: #f5222d;">●</span> ${i18n.t('monitor-disconnected')}`;
                this._setHtml('monitor-network-status', netStatus);
                this._setText('monitor-ip', network.ipAddress || '--');
                
                // Flash 存储
                const flash = data.flash || {};
                this._setText('monitor-flash-percent', (flash.usagePercent || 0) + '%');
                this._setBar('monitor-flash-bar', flash.usagePercent || 0);
                this._setText('monitor-flash-used', this._formatBytes(flash.used || 0));
                this._setText('monitor-flash-free', this._formatBytes(flash.free || 0));
                this._setText('monitor-flash-total', this._formatBytes(flash.total || 0));
                this._setText('monitor-flash-sketch', this._formatBytes(flash.sketchSize || 0));
                
                // 内存
                const memory = data.memory || {};
                this._setText('monitor-heap-percent', (memory.heapUsagePercent || 0) + '%');
                this._setBar('monitor-heap-bar', memory.heapUsagePercent || 0);
                this._setText('monitor-heap-used', this._formatBytes(memory.heapUsed || 0));
                this._setText('monitor-heap-free', this._formatBytes(memory.heapFree || 0));
                this._setText('monitor-heap-total', this._formatBytes(memory.heapTotal || 0));
                this._setText('monitor-heap-min', this._formatBytes(memory.heapMinFree || 0));
                
                // 文件系统
                const fs = data.filesystem || {};
                this._setText('monitor-fs-percent', (fs.usagePercent || 0) + '%');
                this._setBar('monitor-fs-bar', fs.usagePercent || 0);
                this._setText('monitor-fs-used', this._formatBytes(fs.used || 0));
                this._setText('monitor-fs-free', this._formatBytes(fs.free || 0));
                this._setText('monitor-fs-total', this._formatBytes(fs.total || 0));
                
                // 用户统计
                const users = data.users || {};
                this._setText('monitor-user-total', users.total || 0);
                this._setText('monitor-user-online', users.online || 0);
                this._setText('monitor-user-sessions', users.activeSessions || 0);
                
                // 加载详细网络状态
                this.loadNetworkStatus();
            })
            .catch(err => {
                console.error('Load system monitor failed:', err);
            });
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
     * 格式化字节数
     */
    _formatBytes(bytes) {
        if (bytes === 0) return '0 B';
        const k = 1024;
        const sizes = ['B', 'KB', 'MB', 'GB'];
        const i = Math.floor(Math.log(bytes) / Math.log(k));
        return parseFloat((bytes / Math.pow(k, i)).toFixed(2)) + ' ' + sizes[i];
    },
    
    // ============ 网络配置 ============
    
    /**
     * 加载网络配置
     */
    loadNetworkConfig() {
        apiGet('/api/network/config')
            .then(res => {
                if (!res || !res.success) {
                    Notification.error(i18n.t('net-load-fail'), i18n.t('net-settings-title'));
                    return;
                }
                
                const data = res.data || {};
                const device = data.device || {};
                const network = data.network || {};
                const sta = data.sta || {};
                const ap = data.ap || {};
                const advanced = data.advanced || {};
                
                // ========== 基本配置 ==========
                // 网络模式
                this._setValue('wifi-mode', network.mode !== undefined ? network.mode.toString() : '2');
                
                // STA 配置
                this._setValue('wifi-ssid', sta.ssid || '');
                this._setValue('wifi-password', sta.password || '');
                this._setValue('wifi-security', sta.security || 'wpa');
                
                // ========== 热点配置 ==========
                this._setValue('ap-ssid', ap.ssid || '');
                this._setValue('ap-password', ap.password || '');
                this._setValue('ap-channel', ap.channel !== undefined ? ap.channel.toString() : '1');
                this._setValue('ap-hidden', ap.hidden ? '1' : '0');
                this._setValue('ap-max-connections', ap.maxConnections !== undefined ? ap.maxConnections.toString() : '4');
                
                // ========== 高级配置 ==========
                // IP配置类型 (0=DHCP, 1=静态IP)
                const ipConfigType = network.ipConfigType !== undefined ? network.ipConfigType : 0;
                this._setValue('wifi-dhcp', ipConfigType.toString());
                this._toggleStaticIPFields(ipConfigType === 1);
                
                // 静态IP配置
                this._setValue('static-ip', sta.staticIP || '');
                this._setValue('gateway', sta.gateway || '');
                this._setValue('subnet', sta.subnet || '');
                this._setValue('dns1', sta.dns1 || '');
                this._setValue('dns2', sta.dns2 || '');
                
                // 域名配置
                this._setValue('enable-mdns', network.enableMDNS ? '1' : '0');
                this._setValue('custom-domain', network.customDomain || '');
                
                // 连接设置
                this._setValue('connect-timeout', advanced.connectTimeout !== undefined ? advanced.connectTimeout.toString() : '10000');
                this._setValue('reconnect-interval', advanced.reconnectInterval !== undefined ? advanced.reconnectInterval.toString() : '5000');
                this._setValue('max-reconnect-attempts', advanced.maxReconnectAttempts !== undefined ? advanced.maxReconnectAttempts.toString() : '5');
                this._setValue('conflict-detection', advanced.conflictDetection !== undefined ? advanced.conflictDetection.toString() : '3');
            })
            .catch(err => {
                console.error('Load network config failed:', err);
                Notification.error(i18n.t('net-load-fail'), i18n.t('net-settings-title'));
            });
    },
    
    /**
     * 切换静态IP字段显示/隐藏
     */
    _toggleStaticIPFields(show) {
        const fields = ['static-ip', 'gateway', 'subnet', 'dns1', 'dns2'];
        fields.forEach(id => {
            const el = document.getElementById(id);
            if (el) {
                const group = el.closest('.pure-control-group');
                if (group) {
                    group.style.display = show ? 'block' : 'none';
                }
            }
        });
    },
    
    /**
     * 保存基本网络配置
     */
    saveNetworkConfig() {
        const config = {
            mode: document.getElementById('wifi-mode')?.value || '2',
            staSSID: document.getElementById('wifi-ssid')?.value || '',
            staPassword: document.getElementById('wifi-password')?.value || '',
            staSecurity: document.getElementById('wifi-security')?.value || 'wpa'
        };
        
        // 显示加载状态
        const submitBtn = document.getElementById('wifi-save-btn');
        const submitBtnText = document.getElementById('wifi-save-btn-text');
        const originalText = submitBtnText?.innerHTML;
        
        // 设置按钮为切换中状态
        if (submitBtn && submitBtnText) {
            submitBtn.disabled = true;
            submitBtnText.innerHTML = i18n.t('wifi-saving-mode');
        }
        
        // 隐藏之前的消息
        this._showMessage('wifi-success', false);
        this._showMessage('wifi-error', false);
        
        // 隐藏页面内提醒区域
        const noticeEl = document.getElementById('wifi-mode-notice');
        const noticeTextEl = document.getElementById('wifi-mode-notice-text');
        if (noticeEl) noticeEl.style.display = 'none';
        
        apiPut('/api/network/config', config)
            .then(res => {
                if (res && res.success) {
                    this._showMessage('wifi-success', true);
                    
                    // 构建详细的访问提示
                    const data = res.data || {};
                    let message = i18n.t('wifi-save-ok');
                    
                    // 构建页面内提醒信息
                    let noticeMessage = i18n.t('wifi-mode-notice-title');
                    
                    // 根据网络模式添加访问提示
                    if (data.restartRequired) {
                        message += '<br><small style="color:#888;">' + i18n.t('wifi-restart-hint') + '</small>';
                    }
                    
                    const mode = data.mode;
                    const modeText = data.modeText || '';
                    
                    if (mode === 0 || modeText === 'STA') {
                        // STA 模式
                        if (data.mdnsDomain) {
                            const hint = i18n.t('wifi-mode-sta-hint').replace('{domain}', data.mdnsDomain);
                            message += '<br><small>' + hint + '</small>';
                            noticeMessage += i18n.t('wifi-mode-notice-sta').replace('{domain}', data.mdnsDomain);
                        }
                    } else if (mode === 1 || modeText === 'AP') {
                        // AP 模式
                        const hint = i18n.t('wifi-mode-ap-hint')
                            .replace('{ssid}', data.apSSID || 'fastbee-ap')
                            .replace('{ip}', data.apIP || '192.168.4.1');
                        message += '<br><small>' + hint + '</small>';
                        noticeMessage += i18n.t('wifi-mode-notice-ap')
                            .replace('{ssid}', data.apSSID || 'fastbee-ap')
                            .replace('{ip}', data.apIP || '192.168.4.1');
                    } else if (mode === 2 || modeText === 'AP+STA') {
                        // AP+STA 模式
                        if (data.mdnsDomain && data.apSSID) {
                            const hint = i18n.t('wifi-mode-apsta-hint')
                                .replace('{ssid}', data.apSSID)
                                .replace('{ip}', data.apIP || '192.168.4.1')
                                .replace('{domain}', data.mdnsDomain);
                            message += '<br><small>' + hint + '</small>';
                            noticeMessage += i18n.t('wifi-mode-notice-apsta');
                        } else if (data.apSSID) {
                            const hint = i18n.t('wifi-mode-ap-hint')
                                .replace('{ssid}', data.apSSID)
                                .replace('{ip}', data.apIP || '192.168.4.1');
                            message += '<br><small>' + hint + '</small>';
                            noticeMessage += i18n.t('wifi-mode-notice-ap')
                                .replace('{ssid}', data.apSSID)
                                .replace('{ip}', data.apIP || '192.168.4.1');
                        }
                    }
                    
                    message += '<br><small style="color:#888;">' + i18n.t('wifi-reconnect-hint') + '</small>';
                    
                    // 显示页面内提醒
                    if (noticeEl && noticeTextEl) {
                        noticeTextEl.innerHTML = noticeMessage;
                        noticeEl.style.display = 'block';
                    }
                    
                    Notification.show({
                        type: 'success',
                        title: i18n.t('wifi-mode-changed-title'),
                        message: message,
                        duration: 8000
                    });
                    
                    // 15秒倒计时后恢复按钮
                    let countdown = 15;
                    const countdownInterval = setInterval(() => {
                        countdown--;
                        if (countdown <= 0) {
                            clearInterval(countdownInterval);
                            if (submitBtn && submitBtnText) {
                                submitBtn.disabled = false;
                                submitBtnText.innerHTML = originalText || i18n.t('wifi-save-ready');
                            }
                            // 更新页面内提醒，移除倒计时信息
                            if (noticeTextEl && noticeMessage) {
                                noticeTextEl.innerHTML = noticeMessage;
                            }
                        } else {
                            // 更新按钮文本显示倒计时
                            if (submitBtnText) {
                                const countdownText = i18n.t('wifi-mode-notice-countdown').replace('{seconds}', countdown);
                                submitBtnText.innerHTML = i18n.t('wifi-saving-mode') + ' (' + countdown + 's)';
                            }
                            // 更新页面内提醒添加倒计时
                            if (noticeTextEl) {
                                const countdownText = i18n.t('wifi-mode-notice-countdown').replace('{seconds}', countdown);
                                noticeTextEl.innerHTML = noticeMessage + ' ' + countdownText;
                            }
                        }
                    }, 1000);
                    
                } else {
                    this._showMessage('wifi-error', true);
                    Notification.error(res?.error || i18n.t('net-save-fail'), i18n.t('net-settings-title'));
                    // 错误时恢复按钮
                    if (submitBtn && submitBtnText) {
                        submitBtn.disabled = false;
                        submitBtnText.innerHTML = originalText || i18n.t('wifi-save-ready');
                    }
                }
            })
            .catch(err => {
                console.error('Save network config failed:', err);
                // 网络模式切换时，连接可能会中断，这是正常现象
                // 检查是否是网络切换导致的连接错误
                const isNetworkTransitionError = err && (
                    err.name === 'AbortError' ||
                    err.name === 'TypeError' ||
                    (err.message && (
                        err.message.includes('fetch') ||
                        err.message.includes('network') ||
                        err.message.includes('Failed to fetch')
                    ))
                );
                
                if (isNetworkTransitionError) {
                    // 网络切换导致的错误，显示成功提示（配置已保存，网络正在重启）
                    this._showMessage('wifi-success', true);
                    Notification.show({
                        type: 'success',
                        title: i18n.t('wifi-mode-changed-title'),
                        message: i18n.t('wifi-save-ok') + '<br><small style="color:#888;">' + i18n.t('wifi-restart-hint') + '</small>',
                        duration: 8000
                    });
                } else {
                    this._showMessage('wifi-error', true);
                    Notification.error(i18n.t('net-save-fail'), i18n.t('net-settings-title'));
                    // 错误时恢复按钮
                    if (submitBtn && submitBtnText) {
                        submitBtn.disabled = false;
                        submitBtnText.innerHTML = originalText || i18n.t('wifi-save-ready');
                    }
                }
            });
    },
    
    /**
     * 保存热点配置
     */
    saveAPConfig() {
        const config = {
            apSSID: document.getElementById('ap-ssid')?.value || '',
            apPassword: document.getElementById('ap-password')?.value || '',
            apChannel: document.getElementById('ap-channel')?.value || '1',
            apHidden: document.getElementById('ap-hidden')?.value || '0',
            apMaxConnections: document.getElementById('ap-max-connections')?.value || '4'
        };
        
        // 显示加载状态
        const submitBtn = document.querySelector('#ap-form button[type="submit"]');
        const originalText = submitBtn?.innerHTML;
        if (submitBtn) {
            submitBtn.disabled = true;
            submitBtn.innerHTML = i18n.t('net-saving-html');
        }
        
        apiPut('/api/network/config', config)
            .then(res => {
                if (res && res.success) {
                    this._showMessage('ap-success', true);
                    Notification.success(i18n.t('ap-save-ok'), i18n.t('net-settings-title'));
                } else {
                    Notification.error(res?.error || i18n.t('net-save-fail'), i18n.t('net-settings-title'));
                }
            })
            .catch(err => {
                console.error('Save AP config failed:', err);
                Notification.error(i18n.t('net-save-fail'), i18n.t('net-settings-title'));
            })
            .finally(() => {
                if (submitBtn) {
                    submitBtn.disabled = false;
                    submitBtn.innerHTML = originalText;
                }
            });
    },
    
    /**
     * 保存高级配置
     */
    saveAdvancedConfig() {
        const config = {
            ipConfigType: document.getElementById('wifi-dhcp')?.value || '0',
            staticIP: document.getElementById('static-ip')?.value || '',
            gateway: document.getElementById('gateway')?.value || '',
            subnet: document.getElementById('subnet')?.value || '',
            dns1: document.getElementById('dns1')?.value || '',
            dns2: document.getElementById('dns2')?.value || '',
            enableMDNS: document.getElementById('enable-mdns')?.value || '0',
            customDomain: document.getElementById('custom-domain')?.value || '',
            connectTimeout: document.getElementById('connect-timeout')?.value || '10000',
            reconnectInterval: document.getElementById('reconnect-interval')?.value || '5000',
            maxReconnectAttempts: document.getElementById('max-reconnect-attempts')?.value || '5',
            conflictDetection: document.getElementById('conflict-detection')?.value || '3'
        };
        
        // 显示加载状态
        const submitBtn = document.querySelector('#advanced-form button[type="submit"]');
        const originalText = submitBtn?.innerHTML;
        if (submitBtn) {
            submitBtn.disabled = true;
            submitBtn.innerHTML = i18n.t('net-saving-html');
        }
        
        apiPut('/api/network/config', config)
            .then(res => {
                if (res && res.success) {
                    this._showMessage('advanced-success', true);
                    Notification.success(i18n.t('advanced-save-ok'), i18n.t('net-settings-title'));
                } else {
                    Notification.error(res?.error || i18n.t('net-save-fail'), i18n.t('net-settings-title'));
                }
            })
            .catch(err => {
                console.error('Save advanced config failed:', err);
                Notification.error(i18n.t('net-save-fail'), i18n.t('net-settings-title'));
            })
            .finally(() => {
                if (submitBtn) {
                    submitBtn.disabled = false;
                    submitBtn.innerHTML = originalText;
                }
            });
    },
    
    /**
     * 加载并显示网络状态
     */
    loadNetworkStatus() {
        const refreshBtn = document.getElementById('dashboard-net-refresh-btn');
        if (refreshBtn) {
            refreshBtn.disabled = true;
            refreshBtn.innerHTML = i18n.t('net-refreshing-html');
        }

        const setText = (id, val) => {
            const el = document.getElementById(id);
            if (el) el.textContent = (val !== undefined && val !== null && val !== '') ? val : '--';
        };
        const setHtml = (id, html) => {
            const el = document.getElementById(id);
            if (el) el.innerHTML = html;
        };

        apiGet('/api/network/status')
            .then(res => {
                if (!res || !res.success) {
                    Notification.error(i18n.t('net-status-load-fail'), i18n.t('net-status-title-msg'));
                    return;
                }
                const d = res.data || {};

                // 状态徽章
                const statusMap = {
                    connected:    `<span class="badge badge-success">${i18n.t('net-status-connected')}</span>`,
                    disconnected: `<span class="badge badge-danger">${i18n.t('net-status-disconnected')}</span>`,
                    connecting:   `<span class="badge badge-warning">${i18n.t('net-status-connecting')}</span>`,
                    ap_mode:      `<span class="badge badge-primary">${i18n.t('net-status-ap')}</span>`,
                    failed:       `<span class="badge badge-danger">${i18n.t('net-status-failed')}</span>`,
                };
                setHtml('ns-status', statusMap[d.status] || `<span class="badge badge-info">${d.status || '--'}</span>`);

                // STA 信息
                setText('ns-ssid', d.ssid);
                setText('ns-ip', d.ipAddress);
                setText('ns-gateway', d.gateway);
                setText('ns-subnet', d.subnet);
                setText('ns-dns', d.dnsServer);
                setText('ns-dns2', d.dnsServer2);
                setText('ns-mac', d.macAddress);
                // 连接时长
                if (d.connectedTime !== undefined && d.connectedTime > 0) {
                    const sec = d.connectedTime;
                    const h = Math.floor(sec / 3600);
                    const m = Math.floor((sec % 3600) / 60);
                    const s = sec % 60;
                    setText('ns-conn-time', `${h}h ${m}m ${s}s`);
                } else {
                    setText('ns-conn-time', '--');
                }
                // RSSI
                const rssi = d.rssi;
                if (rssi !== undefined && rssi !== null && rssi !== '') {
                    const pct = d.signalStrength || 0;
                    const color = pct >= 70 ? '#52c41a' : pct >= 40 ? '#faad14' : '#f5222d';
                    setHtml('ns-rssi', `<span style="color:${color};">${rssi} dBm (${pct}%)</span>`);
                } else {
                    setText('ns-rssi', '--');
                }

                // AP 信息
                setText('ns-ap-ssid', d.apSSID);
                setText('ns-ap-ip', d.apIPAddress);
                setText('ns-ap-channel', d.apChannel !== undefined ? `CH ${d.apChannel}` : '--');
                setText('ns-ap-clients', d.apClientCount !== undefined ? d.apClientCount + i18n.t('net-ap-clients-unit') : '--');

                // 连接统计
                const modeLabel = { 
                    STA: i18n.t('net-mode-sta'), 
                    AP: i18n.t('net-mode-ap'), 
                    'AP+STA': i18n.t('net-mode-apsta')
                };
                setText('ns-mode', modeLabel[d.mode] || d.mode || '--');
                setText('ns-mdns', d.enableMDNS ? (d.customDomain ? d.customDomain + '.local' : i18n.t('net-mdns-enabled')) : i18n.t('net-mdns-disabled'));
                setText('ns-reconnect', d.reconnectAttempts !== undefined ? d.reconnectAttempts + i18n.t('net-reconnect-unit') : '--');
                setText('ns-tx-count', d.txCount !== undefined ? d.txCount + i18n.t('net-count-unit') : '--');
                setText('ns-rx-count', d.rxCount !== undefined ? d.rxCount + i18n.t('net-count-unit') : '--');
                setHtml('ns-internet', d.internetAvailable
                    ? `<span class="badge badge-success">${i18n.t('net-accessible')}</span>`
                    : `<span class="badge badge-danger">${i18n.t('net-inaccessible')}</span>`);
                setHtml('ns-conflict', d.conflictDetected
                    ? `<span class="badge badge-danger">${i18n.t('net-conflict-yes')}</span>`
                    : `<span class="badge badge-success">${i18n.t('net-no-conflict')}</span>`);
                setText('ns-uptime', d.uptimeFormatted || '--');
            })
            .catch(err => {
                console.error('Load network status failed:', err);
                Notification.error(i18n.t('net-status-load-fail'), i18n.t('net-status-title-msg'));
            })
            .finally(() => {
                if (refreshBtn) {
                    refreshBtn.disabled = false;
                    refreshBtn.innerHTML = i18n.t('net-refresh-html');
                }
            });
    },

    /**
     * 加载设备配置（NTP+基本信息）
     */
    loadDeviceConfig() {
        apiGet('/api/device/config')
            .then(res => {
                if (!res || !res.success) return;
                const d = res.data || {};
                // 设备编号：显示配置中的值（可能是用户自定义的或系统生成的）
                // 如果配置中没有，后端会返回基于MAC生成的默认值
                this._setValue('dev-id', d.deviceId || '');
                // 产品编号
                this._setValue('dev-product-number', d.productNumber !== undefined ? String(d.productNumber) : '0');
                this._setValue('dev-user-id', d.userId || '');
                this._setValue('dev-name',          d.deviceName   || '');
                const desc = document.getElementById('dev-description');
                if (desc) desc.value = d.description || '';
                this._setValue('dev-ntp-enable',    d.enableNTP ? '1' : '0');
                this._setValue('dev-ntp-server1',   d.ntpServer1   || 'https://iot.fastbee.cn/prod-api/iot/tool/ntp');
                this._setValue('dev-ntp-server2',   d.ntpServer2   || 'time.nist.gov');
                this._setValue('dev-timezone',      d.timezone     || 'CST-8');
                this._setValue('dev-sync-interval', d.syncInterval !== undefined ? String(d.syncInterval) : '3600');
                this._setValue('dev-cache-duration', d.cacheDuration !== undefined ? String(d.cacheDuration) : '86400');
            })
            .catch(err => console.error('Load device config failed:', err));
        // 同时加载硬件信息
        this._loadDeviceHardwareInfo();
    },

    _loadDeviceHardwareInfo() {
        apiGet('/api/system/info')
            .then(res => {
                if (!res || !res.success) return;
                const d = res.data || {};
                const dev = d.device || {};
                const fw  = d.firmware || {};
                const set = (id, val) => { const el = document.getElementById(id); if (el) el.textContent = val || '--'; };
                set('dev-sys-chip',  dev.chipModel);
                set('dev-sys-cpu',   dev.cpuFreqMHz ? dev.cpuFreqMHz + ' MHz' : '--');
                set('dev-sys-heap',  dev.freeHeap   ? Math.round(dev.freeHeap / 1024) + ' KB' : '--');
                set('dev-sys-flash', dev.flashSize  ? Math.round(dev.flashSize / 1024 / 1024) + ' MB' : '--');
                set('dev-sys-sdk',   dev.sdkVersion);
                set('dev-sys-fw',    fw.version || dev.firmwareVersion || '--');
            })
            .catch(() => {});
    },

    saveDeviceBasic() {
        // 获取设备编号（用户可自定义任意格式）
        const deviceIdInput = document.getElementById('dev-id');
        let deviceId = deviceIdInput?.value?.trim() || '';
        // 如果为空，后端会使用基于MAC的默认值
        
        const productNumberVal = document.getElementById('dev-product-number')?.value;
        const config = {
            deviceId:       deviceId,
            deviceName:     document.getElementById('dev-name')?.value || '',
            productNumber:  productNumberVal !== undefined && productNumberVal !== '' ? parseInt(productNumberVal, 10) : 0,
            userId:         document.getElementById('dev-user-id')?.value || '',
            description:    document.getElementById('dev-description')?.value || '',
            ntpServer1:     document.getElementById('dev-ntp-server1')?.value || 'https://iot.fastbee.cn/prod-api/iot/tool/ntp',
            ntpServer2:     document.getElementById('dev-ntp-server2')?.value || 'time.nist.gov',
            timezone:       document.getElementById('dev-timezone')?.value || 'CST-8',
            enableNTP:      document.getElementById('dev-ntp-enable')?.value || '1',
            syncInterval:   document.getElementById('dev-sync-interval')?.value || '3600',
            cacheDuration:  parseInt(document.getElementById('dev-cache-duration')?.value || '86400'),
        };
        apiPut('/api/device/config', config)
            .then(res => {
                if (res && res.success) {
                    this._showMessage('dev-basic-success', true);
                    Notification.success(i18n.t('dev-save-basic-ok'), i18n.t('dev-config-title'));
                } else {
                    Notification.error(res?.error || i18n.t('dev-save-fail'), i18n.t('dev-config-title'));
                }
            })
            .catch(() => Notification.error(i18n.t('dev-save-fail'), i18n.t('dev-config-title')));
    },

    saveCacheDuration() {
        const duration = parseInt(document.getElementById('dev-cache-duration')?.value || '86400');
        apiPut('/api/device/config', { cacheDuration: duration })
            .then(res => {
                if (res && res.success) {
                    Notification.success(i18n.t('dev-cache-save-ok'), i18n.t('dev-cache-title'));
                } else {
                    Notification.error(res?.error || i18n.t('dev-save-fail'), i18n.t('dev-cache-title'));
                }
            })
            .catch(() => Notification.error(i18n.t('dev-save-fail'), i18n.t('dev-cache-title')));
    },

    clearBrowserCache() {
        if ('caches' in window) {
            caches.keys().then(names => {
                names.forEach(name => caches.delete(name));
            });
        }
        // 带时间戳强制刷新，绕过浏览器缓存
        Notification.success(i18n.t('dev-cache-clear-ok'), i18n.t('dev-cache-title'));
        setTimeout(() => {
            window.location.href = window.location.pathname + '?_t=' + Date.now();
        }, 800);
    },

    saveDeviceNTP() {
        // 只保存 NTP 相关配置，不包含设备基本信息
        // 避免覆盖设备名称等基本信息字段
        const config = {
            ntpServer1:   document.getElementById('dev-ntp-server1')?.value || 'https://iot.fastbee.cn/prod-api/iot/tool/ntp',
            ntpServer2:   document.getElementById('dev-ntp-server2')?.value || 'time.nist.gov',
            timezone:     document.getElementById('dev-timezone')?.value || 'CST-8',
            enableNTP:    document.getElementById('dev-ntp-enable')?.value || '1',
            syncInterval: document.getElementById('dev-sync-interval')?.value || '3600',
            // 注意：不发送 deviceName 和 description，避免覆盖基本信息
        };
        apiPut('/api/device/config', config)
            .then(res => {
                if (res && res.success) {
                    this._showMessage('dev-ntp-success', true);
                    Notification.success(i18n.t('dev-save-ntp-ok'), i18n.t('dev-config-title'));
                    this.loadDeviceTime();
                } else {
                    Notification.error(res?.error || i18n.t('dev-save-fail'), i18n.t('dev-ntp-config-title'));
                }
            })
            .catch(() => Notification.error(i18n.t('dev-save-fail'), i18n.t('dev-ntp-config-title')));
    },

    loadDeviceTime() {
        const btn = document.getElementById('dev-time-refresh-btn');
        if (btn) { btn.disabled = true; btn.innerHTML = i18n.t('dev-refreshing-html'); }
        
        // 同时获取时间和网络状态
        Promise.all([
            apiGet('/api/device/time'),
            apiGet('/api/network/status')
        ])
            .then(([timeRes, netRes]) => {
                const timeData = (timeRes && timeRes.success) ? timeRes.data || {} : {};
                const netData = (netRes && netRes.success) ? netRes.data || {} : {};
                const internetAvailable = netData.internetAvailable === true;
                
                this._renderDeviceTime(timeData, internetAvailable);
            })
            .catch(err => console.error('Load device time failed:', err))
            .finally(() => {
                // 按钮状态由 _renderDeviceTime 根据网络状态设置
            });
    },

    syncDeviceTime() {
        const btn = document.getElementById('dev-time-refresh-btn');
        if (btn) { btn.disabled = true; btn.innerHTML = i18n.t('dev-refreshing-html'); }
        apiPost('/api/device/time/sync', {})
            .then(res => {
                if (!res || !res.success) {
                    Notification.warning(i18n.t('dev-time-sync-fail') || 'NTP同步失败', 'NTP');
                    // 同步失败，重新加载状态
                    this.loadDeviceTime();
                    return;
                }
                this._renderDeviceTime(res.data || {}, true);
                if (res.data && res.data.synced) {
                    Notification.success(i18n.t('dev-time-sync-ok') || 'NTP同步成功', 'NTP');
                }
            })
            .catch(err => {
                console.error('Sync device time failed:', err);
                Notification.error(i18n.t('dev-time-sync-fail') || 'NTP同步失败', 'NTP');
                // 同步失败，重新加载状态
                this.loadDeviceTime();
            });
    },

    _renderDeviceTime(d, internetAvailable = true) {
        const setEl   = (id, val)  => { const el = document.getElementById(id); if (el) el.textContent = val || '--'; };
        const setHtml = (id, html) => { const el = document.getElementById(id); if (el) el.innerHTML = html; };
        
        setEl('dev-time-datetime', d.datetime);
        
        // 根据网络状态和同步状态显示不同的提示
        if (!internetAvailable) {
            // 网络不可用
            setHtml('dev-time-synced', i18n.t('dev-time-no-network-html'));
        } else if (d.synced) {
            // 已同步
            setHtml('dev-time-synced', i18n.t('dev-time-synced-html'));
        } else {
            // 未同步但网络可用
            setHtml('dev-time-synced', i18n.t('dev-time-not-synced-html'));
        }
        
        if (d.uptime !== undefined) {
            const ms = d.uptime;
            const h  = Math.floor(ms / 3600000);
            const m  = Math.floor((ms % 3600000) / 60000);
            const s  = Math.floor((ms % 60000) / 1000);
            setEl('dev-time-uptime', `${h}${i18n.t('dev-time-uptime-unit')}${m}${i18n.t('dev-time-uptime-min')}${s}${i18n.t('dev-time-uptime-sec')}`);
        }
        
        // 根据网络状态设置刷新按钮
        const btn = document.getElementById('dev-time-refresh-btn');
        if (btn) {
            if (internetAvailable) {
                btn.disabled = false;
                btn.innerHTML = i18n.t('dev-refresh-html');
                btn.title = '';
            } else {
                btn.disabled = true;
                btn.innerHTML = i18n.t('dev-refresh-disabled-html');
                btn.title = i18n.t('dev-time-no-network-tip');
            }
        }
    },

    restartDevice() {
        const delay = document.getElementById('dev-restart-delay')?.value || '3';
        const btn   = document.getElementById('dev-restart-btn');
        if (!confirm(`${i18n.t('dev-restart-confirm-prefix')}${delay}${i18n.t('dev-restart-confirm-suffix')}`)) return;
        if (btn) { btn.disabled = true; btn.innerHTML = i18n.t('dev-restarting-html'); }

        // 使用专用重启 API（更长超时）
        apiRestart({ delay })
            .then(res => {
                if (res && res.success) {
                    // 重启指令发送成功，显示成功提示
                    Notification.success(`${i18n.t('dev-restart-msg-prefix')}${delay}${i18n.t('dev-restart-msg-suffix')}`, i18n.t('dev-restart-title'));
                } else {
                    // 服务器返回了响应但表示失败
                    if (btn) { btn.disabled = false; btn.innerHTML = i18n.t('dev-restart-btn-html'); }
                    Notification.error(i18n.t('dev-restart-fail'), i18n.t('dev-restart-title'));
                }
            })
            .catch(err => {
                // 重启请求可能会因为连接关闭而失败，这是正常的
                // 检查是否在请求发送后一段时间内（说明服务器已收到请求）
                const isConnectionClosed = err && (
                    err.name === 'AbortError' ||
                    err.name === 'TypeError' ||
                    (err.message && (err.message.includes('fetch') || err.message.includes('network')))
                );

                if (isConnectionClosed) {
                    // 连接关闭可能是正常的，设备可能已经开始重启
                    // 显示成功提示而不是错误提示
                    Notification.success(`${i18n.t('dev-restart-msg-prefix')}${delay}${i18n.t('dev-restart-msg-suffix')}`, i18n.t('dev-restart-title'));
                } else {
                    // 其他错误
                    if (btn) { btn.disabled = false; btn.innerHTML = i18n.t('dev-restart-btn-html'); }
                    Notification.error(i18n.t('dev-restart-fail'), i18n.t('dev-restart-title'));
                }
            });
    },

    /**
     * 恢复出厂设置
     */
    factoryReset() {
        const confirmInput = document.getElementById('dev-factory-confirm');
        const btn = document.getElementById('dev-factory-btn');
        const confirmValue = confirmInput?.value?.toUpperCase().trim();

        // 验证确认文本
        if (confirmValue !== 'RESET') {
            Notification.warning(i18n.t('dev-sys-factory-confirm-error'), i18n.t('dev-sys-factory-title-msg'));
            if (confirmInput) confirmInput.focus();
            return;
        }

        // 二次确认
        if (!confirm(i18n.t('dev-sys-factory-warning'))) {
            if (confirmInput) confirmInput.value = '';
            return;
        }

        // 禁用按钮，显示处理中
        if (btn) { btn.disabled = true; btn.innerHTML = i18n.t('dev-sys-factory-processing'); }

        // 使用专用 API（更长超时）
        apiFactoryReset()
            .then(res => {
                if (res && res.success) {
                    // 恢复出厂设置指令发送成功，显示成功提示
                    Notification.success(i18n.t('dev-sys-factory-success'), i18n.t('dev-sys-factory-title-msg'));
                } else {
                    // 服务器返回了响应但表示失败
                    if (btn) { btn.disabled = false; btn.innerHTML = i18n.t('dev-sys-factory-btn-html'); }
                    Notification.error(i18n.t('dev-sys-factory-fail'), i18n.t('dev-sys-factory-title-msg'));
                }
            })
            .catch(err => {
                // 对于恢复出厂设置操作，连接关闭通常意味着设备已开始执行
                // 这种情况应该视为成功
                const isConnectionClosed = err && (
                    err.name === 'AbortError' ||
                    err.name === 'TypeError' ||
                    (err.message && (err.message.includes('fetch') || err.message.includes('network')))
                );

                if (isConnectionClosed) {
                    // 连接被关闭，说明设备已收到指令并开始执行，显示成功
                    Notification.success(i18n.t('dev-sys-factory-success'), i18n.t('dev-sys-factory-title-msg'));
                } else {
                    // 真正的错误
                    if (btn) { btn.disabled = false; btn.innerHTML = i18n.t('dev-sys-factory-btn-html'); }
                    Notification.error(i18n.t('dev-sys-factory-fail'), i18n.t('dev-sys-factory-title-msg'));
                }
            });
    },

    /**
     * 扫描 WiFi 网络
     */
    scanWifiNetworks() {
        const scanBtn = document.getElementById('wifi-scan-btn');
        const modal = document.getElementById('wifi-modal');
        const modalBody = document.getElementById('wifi-modal-body');
        
        if (!modal || !modalBody) return;
        
        // 打开弹窗，显示扫描中
        modal.style.display = 'flex';
        modalBody.innerHTML = i18n.t('wifi-scanning-result');
        
        if (scanBtn) {
            scanBtn.disabled = true;
            scanBtn.innerHTML = i18n.t('wifi-scanning-html');
        }

        // 绑定关闭按钮（每次打开重新绑，避免重复）
        const closeBtn = document.getElementById('close-wifi-modal');
        if (closeBtn) {
            closeBtn.onclick = () => { modal.style.display = 'none'; };
        }
        modal.onclick = (e) => {
            if (e.target === modal) modal.style.display = 'none';
        };
        
        apiGet('/api/wifi/scan')
            .then(res => {
                if (!res || !res.success) {
                    // 根据错误类型显示不同提示
                    if (res && res.error === 'scan_busy') {
                        modalBody.innerHTML = i18n.t('wifi-scan-busy');
                    } else {
                        // 更友好的错误提示
                        modalBody.innerHTML = `<div style="padding: 30px; text-align: center; color: #f56c6c;">
                            <i class="fas fa-exclamation-circle" style="font-size:24px;"></i>
                            <div style="margin-top:10px;">${i18n.t('wifi-scan-fail-msg')}</div>
                            <div style="margin-top:8px; font-size:12px; color:#999;">${res?.error || 'Unknown error'}</div>
                        </div>`;
                    }
                    return;
                }
                
                const networks = res.data || [];
                
                if (networks.length === 0) {
                    modalBody.innerHTML = i18n.t('wifi-no-network');
                    return;
                }
                
                // 按信号强度排序
                networks.sort((a, b) => b.rssi - a.rssi);
                
                // 两列布局显示WiFi网络
                let html = '<div class="wifi-grid">';
                networks.forEach((net) => {
                    const signalClass = net.rssi > -50 ? 'strong' : net.rssi > -70 ? 'medium' : 'weak';
                    const encryptIcon = net.encryption > 0
                        ? '<i class="fas fa-lock" style="color: #52c41a;"></i>'
                        : '<i class="fas fa-lock-open" style="color: #bbb;"></i>';
                    const securityType = net.encryption > 0 ? 'wpa' : 'none';
                    
                    html += `
                        <div class="wifi-grid-item" data-ssid="${net.ssid}" data-encryption="${securityType}">
                            <div class="wifi-info">
                                <div class="wifi-ssid">${net.ssid}</div>
                                <div class="wifi-meta">
                                    ${encryptIcon} ${net.encryption > 0 ? i18n.t('wifi-encrypted') : i18n.t('wifi-open')}
                                </div>
                            </div>
                            <div class="wifi-signal ${signalClass}">
                                <i class="fas fa-signal"></i> ${net.rssi} dBm
                            </div>
                        </div>
                    `;
                });
                html += '</div>';
                
                modalBody.innerHTML = html;
                
                // 绑定点击选择事件
                modalBody.querySelectorAll('.wifi-grid-item').forEach(item => {
                    item.addEventListener('click', (e) => {
                        const ssid = e.currentTarget.dataset.ssid;
                        const encryption = e.currentTarget.dataset.encryption;
                        
                        // 填充 SSID
                        const ssidInput = document.getElementById('wifi-ssid');
                        if (ssidInput) ssidInput.value = ssid;
                        
                        // 填充安全类型
                        const securitySelect = document.getElementById('wifi-security');
                        if (securitySelect) {
                            securitySelect.value = encryption === 'none' ? '0' : '1';
                        }
                        
                        // 清空密码字段（安全）
                        const passwordInput = document.getElementById('wifi-password');
                        if (passwordInput) passwordInput.value = '';
                        
                        // 关闭弹窗并提示
                        modal.style.display = 'none';
                        Notification.success(`${i18n.t('wifi-selected-prefix')}${ssid}`, i18n.t('wifi-scan-title'));
                    });
                });
            })
            .catch(err => {
                console.error('WiFi scan failed:', err);
                modalBody.innerHTML = `<div style="padding: 30px; text-align: center; color: #f56c6c;">
                    <i class="fas fa-exclamation-circle" style="font-size:24px;"></i>
                    <div style="margin-top:10px;">${i18n.t('wifi-scan-fail-msg')}</div>
                </div>`;
            })
            .finally(() => {
                if (scanBtn) {
                    scanBtn.disabled = false;
                    scanBtn.innerHTML = i18n.t('wifi-scan-btn-html');
                }
            });
    },
    
    /**
     * 设置表单值
     */
    _setValue(id, value) {
        const el = document.getElementById(id);
        if (el) el.value = value;
    },

    /**
     * 设置复选框状态
     */
    _setCheckbox(id, checked) {
        const el = document.getElementById(id);
        if (el) el.checked = checked;
    },
        
    /**
     * 加载MQTT发布主题配置（支持多组）
     */
    _loadMqttPublishTopics(topics) {
        const container = document.getElementById('mqtt-publish-topics');
        if (!container) return;
        container.innerHTML = '';
        if (!topics || topics.length === 0) {
            topics = [{ topic: '', qos: 0, retain: false, enabled: true, autoPrefix: false, topicType: 0 }];
        }
        topics.forEach((topic, index) => {
            this._createMqttPublishTopicElement(topic, index);
        });
    },

    _mqttTopicTypeOptions(selected) {
        const types = [
            { value: 0, key: 'mqtt-topic-type-data-report' },
            { value: 1, key: 'mqtt-topic-type-data-command' },
            { value: 2, key: 'mqtt-topic-type-device-info' },
            { value: 3, key: 'mqtt-topic-type-realtime-mon' },
            { value: 4, key: 'mqtt-topic-type-device-event' },
            { value: 5, key: 'mqtt-topic-type-ota-upgrade' },
            { value: 6, key: 'mqtt-topic-type-ota-binary' },
            { value: 7, key: 'mqtt-topic-type-ntp-sync' }
        ];
        return types.map(t =>
            `<option value="${t.value}" ${Number(selected) === t.value ? 'selected' : ''}>${i18n.t(t.key)}</option>`
        ).join('');
    },

    _createMqttPublishTopicElement(topicData, index) {
        const container = document.getElementById('mqtt-publish-topics');
        if (!container) return;
        const div = document.createElement('div');
        div.className = 'mqtt-topic-item';
        div.dataset.index = index;
        const isEnabled = topicData.enabled !== false;
        const isAutoPrefix = topicData.autoPrefix === true;
        div.innerHTML = `
            <span class="mqtt-topic-index">${index + 1}</span>
            <button type="button" class="mqtt-topic-delete" onclick="app.deleteMqttPublishTopic(${index})">${i18n.t('mqtt-delete-topic-btn')}</button>
            <div class="config-form-grid">
                <div class="pure-control-group">
                    <label>${i18n.t('mqtt-publish-label')}</label>
                    <input type="text" class="pure-input-1 mqtt-topic-input" value="${topicData.topic || ''}" placeholder="/topic/path">
                </div>
                <div class="pure-control-group">
                    <label>${i18n.t('mqtt-topic-type-label')}</label>
                    <select class="pure-input-1 mqtt-topic-type-input">
                        ${this._mqttTopicTypeOptions(topicData.topicType ?? 0)}
                    </select>
                </div>
                <div class="pure-control-group">
                    <label>${i18n.t('mqtt-publish-qos-label')}</label>
                    <select class="pure-input-1 mqtt-qos-input">
                        <option value="0" ${topicData.qos === 0 ? 'selected' : ''}>0</option>
                        <option value="1" ${topicData.qos === 1 ? 'selected' : ''}>1</option>
                        <option value="2" ${topicData.qos === 2 ? 'selected' : ''}>2</option>
                    </select>
                </div>
                <div class="pure-control-group" style="display:flex;align-items:center;padding-top:20px;gap:12px;">
                    <label class="pure-checkbox">
                        <input type="checkbox" class="mqtt-retain-input" ${topicData.retain ? 'checked' : ''}> ${i18n.t('mqtt-publish-retain-label')}
                    </label>
                    <label class="pure-checkbox">
                        <input type="checkbox" class="mqtt-enabled-input" ${isEnabled ? 'checked' : ''}> ${i18n.t('mqtt-topic-enabled-label')}
                    </label>
                    <label class="pure-checkbox">
                        <input type="checkbox" class="mqtt-autoprefix-input" ${isAutoPrefix ? 'checked' : ''}> ${i18n.t('mqtt-auto-prefix-label')}
                    </label>
                </div>
            </div>
        `;
        container.appendChild(div);
    },

    addMqttPublishTopic() {
        const container = document.getElementById('mqtt-publish-topics');
        if (!container) return;
        const index = container.children.length;
        this._createMqttPublishTopicElement({ topic: '', qos: 0, retain: false, enabled: true, autoPrefix: false, topicType: 0 }, index);
    },

    deleteMqttPublishTopic(index) {
        const container = document.getElementById('mqtt-publish-topics');
        if (!container) return;
        const items = container.querySelectorAll('.mqtt-topic-item');
        if (items[index]) { items[index].remove(); }
        const remainingItems = container.querySelectorAll('.mqtt-topic-item');
        remainingItems.forEach((item, idx) => {
            item.dataset.index = idx;
            const indexSpan = item.querySelector('.mqtt-topic-index');
            if (indexSpan) indexSpan.textContent = idx + 1;
            const deleteBtn = item.querySelector('.mqtt-topic-delete');
            if (deleteBtn) deleteBtn.setAttribute('onclick', `app.deleteMqttPublishTopic(${idx})`);
        });
        if (remainingItems.length === 0) {
            this._createMqttPublishTopicElement({ topic: '', qos: 0, retain: false, enabled: true, autoPrefix: false, topicType: 0 }, 0);
        }
    },

    _collectMqttPublishTopics() {
        const container = document.getElementById('mqtt-publish-topics');
        if (!container) return [];
        const topics = [];
        const items = container.querySelectorAll('.mqtt-topic-item');
        items.forEach(item => {
            const topicInput = item.querySelector('.mqtt-topic-input');
            const qosInput = item.querySelector('.mqtt-qos-input');
            const retainInput = item.querySelector('.mqtt-retain-input');
            const enabledInput = item.querySelector('.mqtt-enabled-input');
            const autoPrefixInput = item.querySelector('.mqtt-autoprefix-input');
            const topicTypeInput = item.querySelector('.mqtt-topic-type-input');
            if (topicInput) {
                topics.push({
                    topic: topicInput.value || '',
                    qos: parseInt(qosInput?.value || '0'),
                    retain: retainInput?.checked || false,
                    enabled: enabledInput?.checked !== false,
                    autoPrefix: autoPrefixInput?.checked || false,
                    topicType: parseInt(topicTypeInput?.value || '0')
                });
            }
        });
        return topics;
    },

    // ========== MQTT 订阅主题配置管理 ==========
    
    /**
     * 加载并显示所有MQTT订阅主题配置
     */
    _loadMqttSubscribeTopics(topics) {
        const container = document.getElementById('mqtt-subscribe-topics');
        if (!container) return;
        container.innerHTML = '';
        if (!topics || topics.length === 0) {
            topics = [{ topic: '', qos: 0, enabled: true, autoPrefix: false, topicType: 1 }];
        }
        topics.forEach((topic, index) => {
            this._createMqttSubscribeTopicElement(topic, index);
        });
    },

    _createMqttSubscribeTopicElement(topicData, index) {
        const container = document.getElementById('mqtt-subscribe-topics');
        if (!container) return;
        const div = document.createElement('div');
        div.className = 'mqtt-topic-item mqtt-topic-item-sub';
        div.dataset.index = index;
        const isEnabled = topicData.enabled !== false;
        const isAutoPrefix = topicData.autoPrefix === true;
        div.innerHTML = `
            <span class="mqtt-topic-index mqtt-topic-index-sub">${index + 1}</span>
            <button type="button" class="mqtt-topic-delete" onclick="app.deleteMqttSubscribeTopic(${index})">${i18n.t('mqtt-delete-topic-btn')}</button>
            <div class="config-form-grid">
                <div class="pure-control-group">
                    <label>${i18n.t('mqtt-subscribe-topic-label')}</label>
                    <input type="text" class="pure-input-1 mqtt-sub-topic-input" value="${topicData.topic || ''}" placeholder="/topic/path">
                </div>
                <div class="pure-control-group">
                    <label>${i18n.t('mqtt-subscribe-topictype-label')}</label>
                    <select class="pure-input-1 mqtt-sub-topic-type-input">
                        ${this._mqttTopicTypeOptions(topicData.topicType ?? 1)}
                    </select>
                </div>
                <div class="pure-control-group">
                    <label>${i18n.t('mqtt-subscribe-qos-label')}</label>
                    <select class="pure-input-1 mqtt-sub-qos-input">
                        <option value="0" ${topicData.qos === 0 ? 'selected' : ''}>0</option>
                        <option value="1" ${topicData.qos === 1 ? 'selected' : ''}>1</option>
                        <option value="2" ${topicData.qos === 2 ? 'selected' : ''}>2</option>
                    </select>
                </div>
                <div class="pure-control-group" style="display:flex;align-items:center;padding-top:20px;gap:12px;">
                    <label class="pure-checkbox">
                        <input type="checkbox" class="mqtt-sub-enabled-input" ${isEnabled ? 'checked' : ''}> ${i18n.t('mqtt-topic-enabled-label')}
                    </label>
                    <label class="pure-checkbox">
                        <input type="checkbox" class="mqtt-sub-autoprefix-input" ${isAutoPrefix ? 'checked' : ''}> ${i18n.t('mqtt-auto-prefix-label')}
                    </label>
                </div>
            </div>
        `;
        container.appendChild(div);
    },

    addMqttSubscribeTopic() {
        const container = document.getElementById('mqtt-subscribe-topics');
        if (!container) return;
        const index = container.children.length;
        this._createMqttSubscribeTopicElement({ topic: '', qos: 0, enabled: true, autoPrefix: false, topicType: 1 }, index);
    },

    deleteMqttSubscribeTopic(index) {
        const container = document.getElementById('mqtt-subscribe-topics');
        if (!container) return;
        const items = container.querySelectorAll('.mqtt-topic-item');
        if (items[index]) { items[index].remove(); }
        const remainingItems = container.querySelectorAll('.mqtt-topic-item');
        remainingItems.forEach((item, idx) => {
            item.dataset.index = idx;
            const indexSpan = item.querySelector('.mqtt-topic-index');
            if (indexSpan) indexSpan.textContent = idx + 1;
            const deleteBtn = item.querySelector('.mqtt-topic-delete');
            if (deleteBtn) deleteBtn.setAttribute('onclick', `app.deleteMqttSubscribeTopic(${idx})`);
        });
        if (remainingItems.length === 0) {
            this._createMqttSubscribeTopicElement({ topic: '', qos: 0, enabled: true, autoPrefix: false, topicType: 1 }, 0);
        }
    },

    _collectMqttSubscribeTopics() {
        const container = document.getElementById('mqtt-subscribe-topics');
        if (!container) return [];
        const topics = [];
        const items = container.querySelectorAll('.mqtt-topic-item');
        items.forEach(item => {
            const topicInput = item.querySelector('.mqtt-sub-topic-input');
            const qosInput = item.querySelector('.mqtt-sub-qos-input');
            const enabledInput = item.querySelector('.mqtt-sub-enabled-input');
            const autoPrefixInput = item.querySelector('.mqtt-sub-autoprefix-input');
            const topicTypeInput = item.querySelector('.mqtt-sub-topic-type-input');
            if (topicInput) {
                topics.push({
                    topic: topicInput.value || '',
                    qos: parseInt(qosInput?.value || '0'),
                    enabled: enabledInput?.checked !== false,
                    autoPrefix: autoPrefixInput?.checked || false,
                    topicType: parseInt(topicTypeInput?.value || '1')
                });
            }
        });
        return topics;
    },

    /**
     * 切换折叠面板显示/隐藏
     */
    toggleSection(bodyId) {
        const body = document.getElementById(bodyId);
        const icon = document.getElementById(bodyId + '-icon');
        if (!body) return;
        const isHidden = body.style.display === 'none';
        body.style.display = isHidden ? 'block' : 'none';
        if (icon) {
            icon.classList.toggle('expanded', isHidden);
        }
    },

    /**
     * 显示/隐藏消息
     */
    _showMessage(id, show) {
        const el = document.getElementById(id);
        if (el) el.style.display = show ? 'block' : 'none';
    },
    
    /**
     * 设置元素文本内容
     */
    _setTextContent(id, text) {
        const el = document.getElementById(id);
        if (el) el.textContent = text;
    },
    
    /**
     * 设置复选框状态
     */
    _setChecked(id, checked) {
        const el = document.getElementById(id);
        if (el) el.checked = !!checked;
    },
    
    // ============ 协议配置 ============
    
    /**
     * 协议配置缓存
     */
    _protocolConfig: null,
    
    /**
     * 加载协议配置
     */
    loadProtocolConfig(tabId) {
        // 非 modbus-rtu Tab 时停止自动刷新
        if (tabId !== 'modbus-rtu') {
            this._stopMasterStatusRefresh();
            if (this._coilAutoRefreshTimer) {
                clearInterval(this._coilAutoRefreshTimer);
                this._coilAutoRefreshTimer = null;
            }
        }
        
        // modbus-rtu Tab 总是初始化延时下拉菜单（即使配置缓存存在）
        if (tabId === 'modbus-rtu') {
            this._updateDelayChannelSelect();
        }
        
        // 如果已有缓存，直接填充表单
        if (this._protocolConfig) {
            this._fillProtocolForm(tabId, this._protocolConfig);
            return;
        }
        
        // 从服务器加载配置
        apiGet('/api/protocol/config')
            .then(res => {
                if (!res || !res.success) return;
                this._protocolConfig = res.data || {};
                this._fillProtocolForm(tabId, this._protocolConfig);
            })
            .catch(err => {
                console.error('加载协议配置失败:', err);
            });
    },
    
    /**
     * 填充协议配置表单
     */
    _fillProtocolForm(tabId, config) {
        if (tabId === 'modbus-rtu' && config.modbusRtu) {
            const rtu = config.modbusRtu;
            this._setCheckbox('modbus-rtu-enabled', rtu.enabled ?? false);
            // 加载 UART 外设列表并选中当前配置的 peripheralId
            this._loadUartPeripherals(rtu.peripheralId || '');
            
            // RS485 配置
            this._setValue('rtu-de-pin', rtu.dePin ?? 14);
            this._setValue('rtu-transfer-type', rtu.transferType ?? 0);
            
            // Master 模式（固定）
            this.onModbusModeChange('master');
            
            if (rtu.master) {
                this._masterTasks = rtu.master.tasks || [];
            } else {
                this._masterTasks = [];
            }
            this._renderAllDevices();
            
            this.refreshMasterStatus();
            this._startMasterStatusRefresh();
            
            // 初始化线圈控制面板
            this._updateDelayChannelSelect();
            // 加载多设备管理
            this._loadModbusDevices();
        }
        
        if (tabId === 'modbus-tcp' && config.modbusTcp) {
            const tcp = config.modbusTcp;
            this._setCheckbox('modbus-tcp-enabled', tcp.enabled ?? false);
            this._setValue('tcp-ip', tcp.server || '192.168.1.100');
            this._setValue('tcp-mport', tcp.port || 502);
            this._setValue('tcp-slave-id', tcp.slaveId || 1);
            this._setValue('tcp-mtimeout', tcp.timeout || 5000);
        }
        
        if (tabId === 'mqtt' && config.mqtt) {
            const mqtt = config.mqtt;
            this._setCheckbox('mqtt-enabled', mqtt.enabled ?? true);
            this._setValue('mqtt-broker', mqtt.server || 'iot.fastbee.cn');
            this._setValue('mqtt-port', mqtt.port || 1883);
            this._setValue('mqtt-client-id', mqtt.clientId || '');
            this._setValue('mqtt-username', mqtt.username || '');
            this._setValue('mqtt-password', mqtt.password || '');
            this._setValue('mqtt-alive', mqtt.keepAlive || 60);
            this._setValue('mqtt-conn-timeout', mqtt.connectionTimeout ?? 30000);
            this._setCheckbox('mqtt-auto-reconnect', mqtt.autoReconnect ?? true);
            
            // 遗嘱消息
            this._setValue('mqtt-will-topic', mqtt.willTopic || '');
            this._setValue('mqtt-will-payload', mqtt.willPayload || '');
            this._setValue('mqtt-will-qos', mqtt.willQos ?? 0);
            this._setCheckbox('mqtt-will-retain', mqtt.willRetain ?? false);
            
            // Card 高级配置
            this._setValue('mqtt-longitude', mqtt.longitude ?? 0);
            this._setValue('mqtt-latitude', mqtt.latitude ?? 0);
            this._setValue('mqtt-iccid', mqtt.iccid || '');
            this._setValue('mqtt-card-platform-id', mqtt.cardPlatformId ?? 0);
            this._setValue('mqtt-summary', mqtt.summary || '');
            
            // 认证配置
            this._setValue('mqtt-auth-type', mqtt.authType ?? 0);
            this._setValue('mqtt-secret', mqtt.mqttSecret || 'K451265A72244J79');
            this._setValue('mqtt-auth-code', mqtt.authCode || '');
            
            // 加载发布主题配置（支持多组）
            this._loadMqttPublishTopics(mqtt.publishTopics || []);
            
            // 加载订阅主题配置（支持多组）
            this._loadMqttSubscribeTopics(mqtt.subscribeTopics || []);
        }
        
        if (tabId === 'http' && config.http) {
            const http = config.http;
            this._setCheckbox('http-enabled', http.enabled ?? false);
            this._setValue('http-url', http.url || 'https://api.example.com');
            this._setValue('http-port', http.port || 80);
            this._setValue('http-method', http.method || 'POST');
            this._setValue('http-timeout', http.timeout || 30);
            this._setValue('http-interval', http.interval || 60);
            this._setValue('http-retry', http.retry || 3);
            
            // 认证配置
            this._setValue('http-auth-type', http.authType || 'none');
            this._setValue('http-auth-user', http.authUser || '');
            this._setValue('http-auth-token', http.authToken || '');
            this._setValue('http-content-type', http.contentType || 'application/json');
            this.onHttpAuthTypeChange(http.authType || 'none');
        }
        
        if (tabId === 'coap' && config.coap) {
            const coap = config.coap;
            this._setCheckbox('coap-enabled', coap.enabled ?? false);
            this._setValue('coap-server', coap.server || 'coap://example.com');
            this._setValue('coap-port', coap.port || 5683);
            this._setValue('coap-method', coap.method || 'POST');
            this._setValue('coap-path', coap.path || 'sensors/temperature');
            this._setValue('coap-msg-type', coap.msgType || 'CON');
            this._setValue('coap-retransmit', coap.retransmit ?? 3);
            this._setValue('coap-timeout', coap.timeout ?? 5000);
        }
        
        if (tabId === 'tcp' && config.tcp) {
            const tcp = config.tcp;
            this._setCheckbox('tcp-enabled', tcp.enabled ?? false);
            const tcpMode = tcp.mode || 'client';
            this._setValue('tcp-mode', tcpMode);
            this.onTcpModeChange(tcpMode);
            this._setValue('tcp-server', tcp.server || '192.168.1.200');
            this._setValue('tcp-port', tcp.port || 5000);
            this._setValue('tcp-timeout', tcp.timeout || 5000);
            this._setValue('tcp-keepalive', tcp.keepAlive || 60);
            this._setValue('tcp-retry', tcp.maxRetry || 5);
            this._setValue('tcp-reconnect', tcp.reconnectInterval || 10);
            this._setValue('tcp-local-port', tcp.localPort ?? 8080);
            this._setValue('tcp-max-clients', tcp.maxClients ?? 5);
            this._setValue('tcp-heartbeat-msg', tcp.heartbeatMsg || '\\n');
            this._setValue('tcp-idle-timeout', tcp.idleTimeout ?? 120);
        }
    },
    
    /**
     * 保存协议配置
     */
    saveProtocolConfig(formId) {
        // 收集所有表单数据
        const data = {};
        
        // Modbus RTU
        data.modbusRtu_enabled = document.getElementById('modbus-rtu-enabled')?.checked ? 'true' : 'false';
        data.modbusRtu_peripheralId = document.getElementById('rtu-peripheral-id')?.value || '';
        data.modbusRtu_mode = 'master';
        data.modbusRtu_dePin = document.getElementById('rtu-de-pin')?.value || '14';
        data.modbusRtu_transferType = document.getElementById('rtu-transfer-type')?.value || '0';
        data.modbusRtu_workMode = document.getElementById('rtu-work-mode')?.value || '1';
        
        // 验证：启用 Modbus RTU 时必须选择外设
        if (data.modbusRtu_enabled === 'true' && !data.modbusRtu_peripheralId) {
            Notification.warning(i18n.t('rtu-no-uart-peripherals'));
            return;
        }
        
        // Modbus RTU Master 配置（通信参数已迁移至 PeriphExec，此处只提交任务和子设备）
        data.modbusRtu_master_tasks = JSON.stringify(this._masterTasks || []);
        data.modbusRtu_master_devices = JSON.stringify(this._modbusDevices || []);
        
        // Modbus TCP
        data.modbusTcp_enabled = document.getElementById('modbus-tcp-enabled')?.checked ? 'true' : 'false';
        data.modbusTcp_server = document.getElementById('tcp-ip')?.value || '192.168.1.100';
        data.modbusTcp_port = document.getElementById('tcp-mport')?.value || '502';
        data.modbusTcp_slaveId = document.getElementById('tcp-slave-id')?.value || '1';
        data.modbusTcp_timeout = document.getElementById('tcp-mtimeout')?.value || '5000';
        
        // MQTT
        data.mqtt_enabled = document.getElementById('mqtt-enabled')?.checked ? 'true' : 'false';
        data.mqtt_server = document.getElementById('mqtt-broker')?.value || 'iot.fastbee.cn';
        data.mqtt_port = document.getElementById('mqtt-port')?.value || '1883';
        data.mqtt_clientId = document.getElementById('mqtt-client-id')?.value || '';
        data.mqtt_username = document.getElementById('mqtt-username')?.value || '';
        data.mqtt_password = document.getElementById('mqtt-password')?.value || '';
        data.mqtt_keepAlive = document.getElementById('mqtt-alive')?.value || '60';
        data.mqtt_connectionTimeout = document.getElementById('mqtt-conn-timeout')?.value || '30000';
        data.mqtt_autoReconnect = document.getElementById('mqtt-auto-reconnect')?.checked ?? true;
        // MQTT 认证配置
        data.mqtt_authType = document.getElementById('mqtt-auth-type')?.value || '0';
        data.mqtt_mqttSecret = document.getElementById('mqtt-secret')?.value || '';
        data.mqtt_authCode = document.getElementById('mqtt-auth-code')?.value || '';
        data.mqtt_willTopic = document.getElementById('mqtt-will-topic')?.value || '';
        data.mqtt_willPayload = document.getElementById('mqtt-will-payload')?.value || '';
        data.mqtt_willQos = document.getElementById('mqtt-will-qos')?.value || '0';
        data.mqtt_willRetain = document.getElementById('mqtt-will-retain')?.checked ? 'true' : 'false';
        // Card 高级配置
        data.mqtt_longitude = document.getElementById('mqtt-longitude')?.value || '0';
        data.mqtt_latitude = document.getElementById('mqtt-latitude')?.value || '0';
        data.mqtt_iccid = document.getElementById('mqtt-iccid')?.value || '';
        data.mqtt_cardPlatformId = document.getElementById('mqtt-card-platform-id')?.value || '0';
        data.mqtt_summary = document.getElementById('mqtt-summary')?.value || '';
        // 收集发布主题配置（多组）
        data.mqtt_publishTopics = JSON.stringify(this._collectMqttPublishTopics());
        // 收集订阅主题配置（多组）
        data.mqtt_subscribeTopics = JSON.stringify(this._collectMqttSubscribeTopics());
        
        // HTTP
        data.http_enabled = document.getElementById('http-enabled')?.checked ? 'true' : 'false';
        data.http_url = document.getElementById('http-url')?.value || 'https://api.example.com';
        data.http_port = document.getElementById('http-port')?.value || '80';
        data.http_method = document.getElementById('http-method')?.value || 'POST';
        data.http_timeout = document.getElementById('http-timeout')?.value || '30';
        data.http_interval = document.getElementById('http-interval')?.value || '60';
        data.http_retry = document.getElementById('http-retry')?.value || '3';
        data.http_authType = document.getElementById('http-auth-type')?.value || 'none';
        data.http_authUser = document.getElementById('http-auth-user')?.value || '';
        data.http_authToken = document.getElementById('http-auth-token')?.value || '';
        data.http_contentType = document.getElementById('http-content-type')?.value || 'application/json';
        
        // CoAP
        data.coap_enabled = document.getElementById('coap-enabled')?.checked ? 'true' : 'false';
        data.coap_server = document.getElementById('coap-server')?.value || 'coap://example.com';
        data.coap_port = document.getElementById('coap-port')?.value || '5683';
        data.coap_method = document.getElementById('coap-method')?.value || 'POST';
        data.coap_path = document.getElementById('coap-path')?.value || 'sensors/temperature';
        data.coap_msgType = document.getElementById('coap-msg-type')?.value || 'CON';
        data.coap_retransmit = document.getElementById('coap-retransmit')?.value || '3';
        data.coap_timeout = document.getElementById('coap-timeout')?.value || '5000';
        
        // TCP
        data.tcp_enabled = document.getElementById('tcp-enabled')?.checked ? 'true' : 'false';
        data.tcp_mode = document.getElementById('tcp-mode')?.value || 'client';
        data.tcp_server = document.getElementById('tcp-server')?.value || '192.168.1.200';
        data.tcp_port = document.getElementById('tcp-port')?.value || '5000';
        data.tcp_timeout = document.getElementById('tcp-timeout')?.value || '5000';
        data.tcp_keepAlive = document.getElementById('tcp-keepalive')?.value || '60';
        data.tcp_maxRetry = document.getElementById('tcp-retry')?.value || '5';
        data.tcp_reconnectInterval = document.getElementById('tcp-reconnect')?.value || '10';
        data.tcp_localPort = document.getElementById('tcp-local-port')?.value || '8080';
        data.tcp_maxClients = document.getElementById('tcp-max-clients')?.value || '5';
        data.tcp_heartbeatMsg = document.getElementById('tcp-heartbeat-msg')?.value || '\\n';
        data.tcp_idleTimeout = document.getElementById('tcp-idle-timeout')?.value || '120';
        
        const protocolName = this._getProtocolName(formId);
        
        apiPost('/api/protocol/config', data)
            .then(res => {
                if (res && res.success) {
                    // 清除缓存，下次重新加载
                    this._protocolConfig = null;
                    
                    // 检查MQTT重连结果
                    if (res.data && typeof res.data.mqttReconnected !== 'undefined') {
                        if (res.data.mqttReconnected && res.data.mqttDeferred) {
                            // 延迟连接模式：配置已加载，MQTT将自动连接
                            Notification.success(i18n.t('mqtt-reconnect-ok'), i18n.t('protocol-config-title'));
                        } else if (res.data.mqttReconnected) {
                            Notification.success(i18n.t('mqtt-reconnect-ok'), i18n.t('protocol-config-title'));
                        } else if (res.data.mqttDisconnected) {
                            Notification.success(i18n.t('mqtt-disconnect-ok'), i18n.t('protocol-config-title'));
                        } else if (data.mqtt_enabled === 'true') {
                            const errCode = res.data.mqttError || '';
                            const errMsg = errCode ? this._mqttErrorCodeToText(errCode) : '';
                            Notification.warning(
                                i18n.t('mqtt-reconnect-fail') + (errMsg ? ' (' + errMsg + ')' : ''),
                                i18n.t('protocol-config-title')
                            );
                        }
                    }

                    Notification.success(`${protocolName} ${i18n.t('protocol-save-ok-suffix')}`, i18n.t('protocol-config-title'));
                    
                    // 显示成功消息
                    const form = document.getElementById(formId);
                    const ok = form?.querySelector('.message-success');
                    if (ok) {
                        ok.style.display = 'block';
                        setTimeout(() => { ok.style.display = 'none'; }, 3000);
                    }

                    // 刷新MQTT状态（确保轮询已启动，可能之前因错误被停止）
                    this._startMqttStatusPolling();
                } else {
                    Notification.error(res?.message || i18n.t('protocol-save-fail'), i18n.t('protocol-title'));
                }
            })
            .catch(err => {
                console.error('saveProtocolConfig error:', err);
                Notification.error(i18n.t('protocol-save-fail'), i18n.t('protocol-title'));
            });
    },
    
    // ============ Modbus Master 模式管理 ============
    
    _masterTasks: [],
    
    /**
     * 切换 Master/Slave 模式显示
     */
    onModbusModeChange(mode) {
        // 固定为主站模式，Master配置区始终显示
    },

    onWorkModeChange(mode) {
        var show = (mode === '1') ? '' : 'none';
        var sec = document.getElementById('master-config-section');
        if (sec) sec.style.display = show;
        var st = document.getElementById('master-status-section');
        if (st) st.style.display = show;
    },
    
    /**
     * 渲染统一设备表格（sensor + control）
     */
    _renderAllDevices() {
        var tbody = document.getElementById('all-devices-body');
        if (!tbody) return;
        
        var tasks = this._masterTasks || [];
        var devices = this._modbusDevices || [];
        
        if (tasks.length === 0 && devices.length === 0) {
            tbody.innerHTML = '<tr><td colspan="6" style="text-align:center;color:#999;">' +
                (i18n.t('modbus-no-devices') || '暂无子设备') + '</td></tr>';
            return;
        }
        
        var fcNames = {1: 'FC01', 2: 'FC02', 3: 'FC03', 4: 'FC04'};
        var typeColors = {sensor: '#409EFF', relay: '#67C23A', pwm: '#E6A23C', pid: '#F56C6C'};
        var typeLabels = {
            sensor: i18n.t('modbus-type-sensor') || '采集',
            relay: i18n.t('modbus-type-relay') || '继电器',
            pwm: i18n.t('modbus-type-pwm') || 'PWM',
            pid: i18n.t('modbus-type-pid') || 'PID'
        };
        
        var rows = '';
        
        // Sensor 设备 (来自 _masterTasks)
        for (var i = 0; i < tasks.length; i++) {
            var t = tasks[i];
            var label = t.label || ('Slave ' + (t.slaveAddress || 1));
            var fc = fcNames[t.functionCode] || 'FC03';
            var info = fc + ' @' + (t.startAddress || 0) + ' x' + (t.quantity || 10);
            var mappingCount = (t.mappings && t.mappings.length) || 0;
            if (mappingCount > 0) info += ' [' + mappingCount + (i18n.t('modbus-dev-mappings-suffix') || '映射') + ']';
            var enabledHtml = t.enabled !== false ?
                '<span style="color:#4CAF50;">ON</span>' : '<span style="color:#999;">OFF</span>';
            rows += '<tr>' +
                '<td>' + escapeHtml(label) + '</td>' +
                '<td><span style="background:' + typeColors.sensor + ';color:#fff;padding:1px 6px;border-radius:3px;font-size:11px;">' + typeLabels.sensor + '</span></td>' +
                '<td>' + (t.slaveAddress || 1) + '</td>' +
                '<td><small>' + info + '</small></td>' +
                '<td>' + enabledHtml + '</td>' +
                '<td style="white-space:nowrap;">' +
                    '<button type="button" class="pure-button" style="background:#667eea;color:white;font-size:11px;padding:2px 8px;margin-right:3px;" onclick="AppState._editDevice(\'sensor\',' + i + ')">' + (i18n.t('modbus-task-edit-btn') || '编辑') + '</button>' +
                    '<button type="button" class="pure-button" style="background:#e6a23c;color:white;font-size:11px;padding:2px 8px;margin-right:3px;" onclick="AppState.openMappingModal(' + i + ')">' + (i18n.t('modbus-mapping-btn') || '映射') + '</button>' +
                    '<button type="button" class="pure-button" style="background:#f44336;color:white;font-size:11px;padding:2px 8px;" onclick="AppState._deleteDevice(\'sensor\',' + i + ')">' + (i18n.t('modbus-master-delete-task') || '删除') + '</button>' +
                '</td></tr>';
        }
        
        // Control 设备 (来自 _modbusDevices)
        for (var j = 0; j < devices.length; j++) {
            var d = devices[j];
            var dt = d.deviceType || 'relay';
            var color = typeColors[dt] || '#999';
            var tLabel = typeLabels[dt] || dt;
            var devInfo = (d.channelCount || 2) + 'ch';
            if (dt === 'relay') {
                devInfo += ' ' + (d.controlProtocol === 1 ? 'Reg' : 'Coil') + '@' + (d.coilBase || 0);
            } else if (dt === 'pwm') {
                devInfo += ' Reg@' + (d.pwmRegBase || 0) + ' ' + (d.pwmResolution || 8) + 'bit';
            } else if (dt === 'pid') {
                devInfo += ' PV@' + ((d.pidAddrs && d.pidAddrs[0]) || 0);
            }
            var ctrlEnabled = d.enabled !== false ?
                '<span style="color:#4CAF50;">ON</span>' : '<span style="color:#999;">OFF</span>';
            rows += '<tr>' +
                '<td>' + escapeHtml(d.name || '-') + '</td>' +
                '<td><span style="background:' + color + ';color:#fff;padding:1px 6px;border-radius:3px;font-size:11px;">' + tLabel + '</span></td>' +
                '<td>' + (d.slaveAddress || 1) + '</td>' +
                '<td><small>' + devInfo + '</small></td>' +
                '<td>' + ctrlEnabled + '</td>' +
                '<td style="white-space:nowrap;">' +
                    '<button type="button" class="pure-button" style="background:#667eea;color:white;font-size:11px;padding:2px 8px;margin-right:3px;" onclick="AppState._editDevice(\'control\',' + j + ')">' +
                            (i18n.t('modbus-device-edit-btn') || '编辑') + '</button>' +
                    '<button type="button" class="pure-button" style="background:#e6a23c;color:white;font-size:11px;padding:2px 8px;margin-right:3px;" onclick="AppState._openCtrlModal(' + j + ')">' +
                        (i18n.t('modbus-device-select-btn') || '控制') + '</button>' +
                    '<button type="button" class="pure-button" style="background:#f44336;color:white;font-size:11px;padding:2px 8px;" onclick="AppState._deleteDevice(\'control\',' + j + ')">' +
                        (i18n.t('modbus-master-delete-task') || '删除') + '</button>' +
                '</td></tr>';
        }
        
        tbody.innerHTML = rows;
    },
    
    /**
     * 编辑设备（sensor 打开任务弹窗，control 打开设备弹窗）
     */
    _editDevice(source, idx) {
        if (source === 'sensor') {
            this._openTaskEditModal(idx);
        } else {
            this._openEditModal(idx);
        }
    },
    
    /**
     * 删除设备
     */
    _deleteDevice(source, idx) {
        if (source === 'sensor') {
            if (this._masterTasks) {
                this._masterTasks.splice(idx, 1);
            }
        } else {
            if (this._modbusDevices) {
                this._modbusDevices.splice(idx, 1);
            }
        }
        this._renderAllDevices();
    },
    
    /**
     * 显示/隐藏设备类型下拉菜单
     */
    _showAddDeviceMenu(event) {
        event.stopPropagation();
        var menu = document.getElementById('add-device-menu');
        if (!menu) return;
        var isVisible = menu.style.display !== 'none';
        menu.style.display = isVisible ? 'none' : 'block';
        if (!isVisible) {
            var closeMenu = function(e) {
                if (!menu.contains(e.target)) {
                    menu.style.display = 'none';
                    document.removeEventListener('click', closeMenu);
                }
            };
            setTimeout(function() { document.addEventListener('click', closeMenu); }, 0);
        }
    },

    /**
     * 添加采集(sensor)设备
     */
    _addSensorDevice() {
        document.getElementById('add-device-menu').style.display = 'none';
        if (!this._masterTasks) this._masterTasks = [];
        if (this._masterTasks.length >= 8) {
            Notification.warning('Max 8 sensor devices', i18n.t('modbus-all-devices-title'));
            return;
        }
        this._openTaskEditModal(-1);
    },
    
    /**
     * 添加控制设备
     */
    _addControlDevice() {
        document.getElementById('add-device-menu').style.display = 'none';
        if (!this._modbusDevices) this._modbusDevices = [];
        if (this._modbusDevices.length >= 8) {
            Notification.warning('Max 8 control devices', i18n.t('modbus-all-devices-title'));
            return;
        }
        this._openEditModal(-1);
    },
    
    // ============ 轮询任务编辑弹窗 ============
    _editingTaskIdx: -1,
    
    _openTaskEditModal(idx) {
        var modal = document.getElementById('task-edit-modal');
        if (!modal) return;
        
        this._editingTaskIdx = idx;
        var task;
        if (idx >= 0 && this._masterTasks && this._masterTasks[idx]) {
            task = this._masterTasks[idx];
        } else {
            task = { slaveAddress: 1, functionCode: 3, startAddress: 0, quantity: 10, enabled: true, label: '', mappings: [] };
        }
        
        var f = function(id) { return document.getElementById(id); };
        f('task-edit-slave-addr').value = task.slaveAddress || 1;
        f('task-edit-fc').value = task.functionCode || 3;
        f('task-edit-start-addr').value = task.startAddress || 0;
        f('task-edit-quantity').value = task.quantity || 10;
        f('task-edit-label').value = task.label || '';
        f('task-edit-type').value = task.deviceType || 'holding';
        f('task-edit-enabled').checked = task.enabled !== false;
        
        var titleEl = modal.querySelector('.modal-header h3');
        if (titleEl) titleEl.textContent = idx < 0 ? i18n.t('modbus-task-add-title') : i18n.t('modbus-task-edit-title');
        
        modal.style.display = 'flex';
    },
    
    _closeTaskEditModal() {
        var modal = document.getElementById('task-edit-modal');
        if (modal) modal.style.display = 'none';
        this._editingTaskIdx = -1;
    },

    _onTaskTypeChange(val) {
        var fcMap = { holding: '3', input: '4', coil: '1', discrete: '2' };
        var fcEl = document.getElementById('task-edit-fc');
        if (fcEl && fcMap[val]) fcEl.value = fcMap[val];
    },
    
    _saveTaskEditModal() {
        var f = function(id) { return document.getElementById(id); };
        var task = {
            slaveAddress: parseInt(f('task-edit-slave-addr').value) || 1,
            functionCode: parseInt(f('task-edit-fc').value) || 3,
            startAddress: parseInt(f('task-edit-start-addr').value) || 0,
            quantity: parseInt(f('task-edit-quantity').value) || 10,
            label: f('task-edit-label').value || '',
            deviceType: f('task-edit-type').value || 'holding',
            enabled: f('task-edit-enabled').checked
        };
        
        if (!this._masterTasks) this._masterTasks = [];
        
        if (this._editingTaskIdx >= 0 && this._masterTasks[this._editingTaskIdx]) {
            task.mappings = this._masterTasks[this._editingTaskIdx].mappings || [];
            this._masterTasks[this._editingTaskIdx] = task;
        } else {
            task.mappings = [];
            this._masterTasks.push(task);
        }
        
        this._renderAllDevices();
        this._closeTaskEditModal();
    },
    
    addMasterPollTask() {
        if (!this._masterTasks) this._masterTasks = [];
        if (this._masterTasks.length >= 8) {
            Notification.warning('Max 8 tasks', i18n.t('modbus-master-title'));
            return;
        }
        this._openTaskEditModal(-1);
    },
    
    removeMasterPollTask(idx) {
        if (this._masterTasks) {
            this._masterTasks.splice(idx, 1);
            this._renderAllDevices();
        }
    },
    
    // ============ 寄存器映射管理 ============
    
    _currentMappingTaskIdx: -1,
    _currentMappings: [],
    
    openMappingModal(taskIdx) {
        if (!this._masterTasks || !this._masterTasks[taskIdx]) return;
        this._currentMappingTaskIdx = taskIdx;
        this._currentMappings = JSON.parse(JSON.stringify(this._masterTasks[taskIdx].mappings || []));
        this._renderMappingTable();
        document.getElementById('mapping-modal').style.display = 'flex';
    },
    
    closeMappingModal() {
        document.getElementById('mapping-modal').style.display = 'none';
        this._currentMappingTaskIdx = -1;
        this._currentMappings = [];
    },
    
    saveMappingModal() {
        if (this._currentMappingTaskIdx < 0) return;
        // 从表格收集当前值
        this._collectMappingValues();
        this._masterTasks[this._currentMappingTaskIdx].mappings = this._currentMappings;
        this._renderAllDevices();
        this.closeMappingModal();
    },
    
    addMapping() {
        if (this._currentMappings.length >= 8) {
            Notification.warning(i18n.t('modbus-mapping-max'));
            return;
        }
        this._collectMappingValues();
        this._currentMappings.push({
            regOffset: 0, dataType: 0, scaleFactor: 0.1, decimalPlaces: 1, sensorId: ''
        });
        this._renderMappingTable();
    },
    
    removeMapping(idx) {
        this._collectMappingValues();
        this._currentMappings.splice(idx, 1);
        this._renderMappingTable();
    },
    
    _collectMappingValues() {
        const tbody = document.getElementById('mapping-table-body');
        if (!tbody) return;
        const rows = tbody.querySelectorAll('tr');
        rows.forEach((row, idx) => {
            if (idx >= this._currentMappings.length) return;
            const inputs = row.querySelectorAll('input, select');
            if (inputs.length >= 5) {
                this._currentMappings[idx].regOffset = parseInt(inputs[0].value) || 0;
                this._currentMappings[idx].dataType = parseInt(inputs[1].value) || 0;
                this._currentMappings[idx].scaleFactor = parseFloat(inputs[2].value) || 1.0;
                this._currentMappings[idx].decimalPlaces = parseInt(inputs[3].value) || 1;
                this._currentMappings[idx].sensorId = inputs[4].value || '';
            }
        });
    },
    
    _renderMappingTable() {
        const tbody = document.getElementById('mapping-table-body');
        if (!tbody) return;
        
        const dtOpts = [
            {v: 0, t: 'uint16'}, {v: 1, t: 'int16'},
            {v: 2, t: 'uint32'}, {v: 3, t: 'int32'}, {v: 4, t: 'float32'}
        ];
        
        if (this._currentMappings.length === 0) {
            tbody.innerHTML = '<tr><td colspan="6" style="text-align:center;color:#999;">' + i18n.t('modbus-master-no-tasks') + '</td></tr>';
            return;
        }
        
        tbody.innerHTML = this._currentMappings.map((m, idx) => {
            const dtSelect = dtOpts.map(o =>
                '<option value="' + o.v + '"' + (m.dataType === o.v ? ' selected' : '') + '>' + o.t + '</option>'
            ).join('');
            return '<tr>' +
                '<td><input type="number" value="' + (m.regOffset || 0) + '" min="0" max="124" style="width:50px;"></td>' +
                '<td><select style="width:80px;">' + dtSelect + '</select></td>' +
                '<td><input type="number" value="' + (m.scaleFactor ?? 0.1) + '" step="0.001" style="width:70px;"></td>' +
                '<td><input type="number" value="' + (m.decimalPlaces ?? 1) + '" min="0" max="6" style="width:50px;"></td>' +
                '<td><input type="text" value="' + (m.sensorId || '') + '" maxlength="15" style="width:100px;" placeholder="temperature"></td>' +
                '<td><button type="button" class="pure-button" style="background:#f44336;color:white;font-size:11px;" onclick="AppState.removeMapping(' + idx + ')">X</button></td>' +
            '</tr>';
        }).join('');
    },
    
    /**
     * Master 状态自动刷新定时器
     */
    _masterStatusTimer: null,

    /**
     * 刷新 Master 运行状态
     */
    refreshMasterStatus() {
        apiGet('/api/modbus/status')
            .then(res => {
                if (!res || !res.success || !res.data) {
                    this._setText('master-running-status', i18n.t('modbus-master-status-unavailable'));
                    return;
                }
                const d = res.data;
                this._setText('master-stat-total', d.totalPolls ?? 0);
                this._setText('master-stat-success', d.successPolls ?? 0);
                this._setText('master-stat-failed', d.failedPolls ?? 0);
                this._setText('master-stat-timeout', d.timeoutPolls ?? 0);
                // 显示运行状态文本（只保留 Master - Addr:X）
                var statusRaw = d.status || i18n.t('modbus-master-status-stopped');
                var commaIdx = statusRaw.indexOf(',');
                var statusText = commaIdx > 0 ? statusRaw.substring(0, commaIdx).trim() : statusRaw;
                this._setText('master-running-status', statusText);
                // 渲染采集数据卡片
                if (d.tasks) this._renderMasterDataGrid(d.tasks);
            })
            .catch(() => {
                this._setText('master-running-status', i18n.t('modbus-master-status-fetch-error'));
            });
    },

    /**
     * 启动 Master 状态自动刷新（每5秒）
     */
    _startMasterStatusRefresh() {
        this._stopMasterStatusRefresh();
        this._masterStatusTimer = setInterval(() => {
            this.refreshMasterStatus();
        }, 5000);
    },

    /**
     * 停止 Master 状态自动刷新
     */
    _stopMasterStatusRefresh() {
        if (this._masterStatusTimer) {
            clearInterval(this._masterStatusTimer);
            this._masterStatusTimer = null;
        }
    },

    // ============================================================================
    // 采集数据展示 — 基于轮询任务缓存数据渲染仪表盘卡片
    // ============================================================================

    _renderMasterDataGrid(tasks) {
        var grid = document.getElementById('master-data-grid');
        if (!grid) return;

        var html = '';
        for (var i = 0; i < (tasks || []).length; i++) {
            var t = tasks[i];
            if (!t.enabled || !t.cachedData || !t.cachedData.values) continue;

            if (t.mappings && t.mappings.length > 0) {
                for (var j = 0; j < t.mappings.length; j++) {
                    var m = t.mappings[j];
                    var rawVal = null;
                    if (m.regOffset < t.cachedData.values.length) {
                        switch (m.dataType) {
                            case 0: rawVal = t.cachedData.values[m.regOffset]; break;
                            case 1: rawVal = t.cachedData.values[m.regOffset]; if (rawVal > 32767) rawVal -= 65536; break;
                            case 2: if (m.regOffset + 1 < t.cachedData.values.length) rawVal = (t.cachedData.values[m.regOffset] << 16) | t.cachedData.values[m.regOffset + 1]; break;
                            case 3: if (m.regOffset + 1 < t.cachedData.values.length) { rawVal = (t.cachedData.values[m.regOffset] << 16) | t.cachedData.values[m.regOffset + 1]; if (rawVal > 2147483647) rawVal -= 4294967296; } break;
                            default: rawVal = t.cachedData.values[m.regOffset]; break;
                        }
                    }
                    var displayVal = '--';
                    if (rawVal !== null) {
                        var scaled = rawVal * (m.scaleFactor || 1);
                        displayVal = scaled.toFixed(m.decimalPlaces || 0);
                    }
                    html += '<div class="master-data-item">'
                        + '<span class="master-data-name">' + (m.sensorId || ('R' + (t.startAddress + m.regOffset))) + '</span>'
                        + '<span class="master-data-val">' + displayVal + '</span>'
                        + '</div>';
                }
            } else {
                for (var k = 0; k < t.cachedData.values.length; k++) {
                    html += '<div class="master-data-item">'
                        + '<span class="master-data-name">R' + (t.startAddress + k) + '</span>'
                        + '<span class="master-data-val">' + t.cachedData.values[k] + '</span>'
                        + '</div>';
                }
            }
        }
        grid.innerHTML = html;
        grid.style.display = html ? '' : 'none';
    },

    // ============================================================================
    // Modbus 通用线圈控制
    // ============================================================================

    /** 内部线圈状态 */
    _coilStates: [],
    _coilAutoRefreshTimer: null,

    /** 多设备管理 */
    _modbusDevices: [],
    _activeDeviceId: null,
    _deviceCoilCache: {},
    _devicePwmCache: {},
    _pwmStates: [],
    _pidValues: {},
    _pidAutoRefreshTimer: null,
    _devicePidCache: {},

    /** 当前激活设备索引 */
    _activeDeviceIdx: -1,
    /** 当前编辑弹窗正在编辑的设备索引 (-1 表示新增) */
    _editingDeviceIdx: -1,

    /** 从后端协议配置加载设备列表（含 localStorage 迁移） */
    _loadModbusDevices() {
        var serverDevices = [];
        try {
            var rtu = this._protocolConfig && this._protocolConfig.modbusRtu;
            if (rtu && rtu.master && rtu.master.devices && rtu.master.devices.length > 0) {
                serverDevices = rtu.master.devices;
            }
        } catch(e) {}

        // localStorage 迁移：后端无数据但 localStorage 有旧数据
        if (serverDevices.length === 0) {
            try {
                var raw = localStorage.getItem('modbus_devices');
                if (raw) {
                    var localDevices = JSON.parse(raw);
                    if (localDevices && localDevices.length > 0) {
                        // 转换旧字段名
                        serverDevices = localDevices.map(function(d) {
                            return {
                                name: d.name || 'Device',
                                deviceType: d.deviceType || d.type || 'relay',
                                slaveAddress: d.slaveAddress || 1,
                                channelCount: d.channelCount || 2,
                                coilBase: d.coilBase || 0,
                                ncMode: !!d.ncMode,
                                controlProtocol: d.relayMode === 'register' ? 1 : 0,
                                pwmRegBase: d.pwmRegBase || 0,
                                pwmResolution: d.pwmResolution || 8,
                                pidAddrs: [
                                    d.pidPvAddr || 0, d.pidSvAddr || 1, d.pidOutAddr || 2,
                                    d.pidPAddr || 3, d.pidIAddr || 4, d.pidDAddr || 5
                                ],
                                pidDecimals: d.pidDecimals || 1
                            };
                        });
                    }
                }
            } catch(e) {}
        }

        this._modbusDevices = serverDevices;

        // 向后兼容
        for (var i = 0; i < this._modbusDevices.length; i++) {
            if (!this._modbusDevices[i].deviceType) {
                this._modbusDevices[i].deviceType = this._modbusDevices[i].type || 'relay';
            }
        }

        this._activeDeviceIdx = -1;
        this._renderAllDevices();
    },

    /** 渲染子设备表格（纯展示模式，操作列含编辑/控制/删除） */
    _renderDeviceTable() {
        var tbody = document.getElementById('modbus-devices-body');
        if (!tbody) return;

        if (!this._modbusDevices || this._modbusDevices.length === 0) {
            tbody.innerHTML = '<tr><td colspan="8" style="text-align:center;color:#999;">' +
                (i18n.t('modbus-device-no-devices') || '暂无子设备') + '</td></tr>';
            return;
        }

        var typeLabels = { relay: i18n.t('modbus-ctrl-type-relay') || '继电器', pwm: 'PWM', pid: 'PID' };
        var protLabels = ['Coil', 'Register'];

        tbody.innerHTML = this._modbusDevices.map(function(dev, idx) {
            var dt = dev.deviceType || 'relay';
            var cp = dev.controlProtocol || 0;
            return '<tr>' +
                '<td>' + (dev.name || '-') + '</td>' +
                '<td>' + (typeLabels[dt] || dt) + '</td>' +
                '<td>' + (dev.slaveAddress || 1) + '</td>' +
                '<td>' + (dev.channelCount || 2) + '</td>' +
                '<td>' + (dev.coilBase || 0) + '</td>' +
                '<td>' + (protLabels[cp] || 'Coil') + '</td>' +
                '<td>' + (dev.ncMode ? 'ON' : '-') + '</td>' +
                '<td style="white-space:nowrap;">' +
                    '<button type="button" class="pure-button" style="background:#667eea;color:white;font-size:11px;padding:2px 8px;margin-right:3px;" onclick="AppState._openEditModal(' + idx + ')">' +
                            (i18n.t('modbus-device-edit-btn') || '编辑') + '</button>' +
                    '<button type="button" class="pure-button" style="background:#e6a23c;color:white;font-size:11px;padding:2px 8px;margin-right:3px;" onclick="AppState._openCtrlModal(' + idx + ')">' +
                        (i18n.t('modbus-device-select-btn') || '控制') + '</button>' +
                    '<button type="button" class="pure-button" style="background:#f44336;color:white;font-size:11px;padding:2px 8px;" onclick="AppState._removeDevice(' + idx + ')">' +
                        (i18n.t('modbus-master-delete-task') || '删除') + '</button>' +
                '</td>' +
            '</tr>';
        }).join('');
    },

    /** 更新设备字段 */
    _updateDevice(idx, field, value) {
        if (this._modbusDevices && this._modbusDevices[idx]) {
            this._modbusDevices[idx][field] = value;
            if (field === 'deviceType') {
                // 类型变更：若是当前激活设备，刷新面板；否则仅重绘表格
                if (idx === this._activeDeviceIdx) {
                    this._activateDevice(idx);
                } else {
                    this._renderAllDevices();
                }
            }
        }
    },

    /** 更新当前激活设备的扩展字段 (PWM/PID DOM 表单同步回设备数据) */
    _updateActiveDeviceExt(field, value) {
        if (this._activeDeviceIdx < 0 || !this._modbusDevices) return;
        var dev = this._modbusDevices[this._activeDeviceIdx];
        if (!dev) return;
        if (field.indexOf('pidAddrs.') === 0) {
            var arrIdx = parseInt(field.split('.')[1]);
            if (!dev.pidAddrs) dev.pidAddrs = [0, 1, 2, 3, 4, 5];
            dev.pidAddrs[arrIdx] = value;
        } else {
            dev[field] = value;
        }
    },

    /** 添加新设备 */
    addModbusDevice() {
        if (!this._modbusDevices) this._modbusDevices = [];
        if (this._modbusDevices.length >= 8) {
            Notification.warning(i18n.t('modbus-device-max-reached') || '最多8个子设备');
            return;
        }
        this._openEditModal(-1); // -1 = 新增模式
    },

    /** 删除设备 */
    _removeDevice(idx) {
        if (!this._modbusDevices) return;
        var msg = i18n.t('modbus-device-delete-confirm') || '确定要删除此设备？';
        if (!confirm(msg)) return;
        // 清除缓存
        var devKey = 'dev_' + idx;
        delete this._deviceCoilCache[devKey];
        delete this._devicePwmCache[devKey];
        delete this._devicePidCache[devKey];
        this._modbusDevices.splice(idx, 1);
        if (this._activeDeviceIdx === idx) {
            this._activeDeviceIdx = -1;
        } else if (this._activeDeviceIdx > idx) {
            this._activeDeviceIdx--;
        }
        this._renderAllDevices();
    },

    /** 激活设备（显示控制面板） */
    // ========== 编辑弹窗 ==========

    /** 打开编辑弹窗 */
    _openEditModal(idx) {
        this._editingDeviceIdx = idx;
        var dev = (idx >= 0 && this._modbusDevices && this._modbusDevices[idx])
            ? this._modbusDevices[idx] : null;
        var modal = document.getElementById('modbus-device-edit-modal');
        if (!modal) return;

        // 填充表单
        document.getElementById('mdev-edit-name').value = dev ? (dev.name || '') : ((i18n.t('modbus-ctrl-device-default-name') || '设备') + ((this._modbusDevices ? this._modbusDevices.length : 0) + 1));
        document.getElementById('mdev-edit-type').value = dev ? (dev.deviceType || 'relay') : 'relay';
        document.getElementById('mdev-edit-addr').value = dev ? (dev.slaveAddress || 1) : 1;
        document.getElementById('mdev-edit-ch').value = String(dev ? (dev.channelCount || 2) : 2);
        document.getElementById('mdev-edit-base').value = dev ? (dev.coilBase || 0) : 0;
        document.getElementById('mdev-edit-protocol').value = String(dev ? (dev.controlProtocol || 0) : 0);
        document.getElementById('mdev-edit-nc').value = dev ? (dev.ncMode ? 'true' : 'false') : 'true';
        document.getElementById('mdev-edit-enabled').checked = dev ? (dev.enabled !== false) : true;
        // PWM
        document.getElementById('mdev-edit-pwm-reg-base').value = dev ? (dev.pwmRegBase || 0) : 0;
        document.getElementById('mdev-edit-pwm-resolution').value = String(dev ? (dev.pwmResolution || 8) : 8);
        // PID
        var pidA = dev ? (dev.pidAddrs || [0,1,2,3,4,5]) : [0,1,2,3,4,5];
        var pidFields = ['pv','sv','out','p','i','d'];
        for (var pi = 0; pi < pidFields.length; pi++) {
            var pe = document.getElementById('mdev-edit-pid-' + pidFields[pi]);
            if (pe) pe.value = pidA[pi] || pi;
        }
        document.getElementById('mdev-edit-pid-decimals').value = String(dev ? (dev.pidDecimals || 1) : 1);

        this._onEditTypeChange();

        // 更新标题
        var title = document.getElementById('modbus-edit-modal-title');
        if (title) title.textContent = (idx >= 0)
            ? (i18n.t('modbus-device-edit-title') || '编辑子设备')
            : (i18n.t('modbus-device-add') || '添加设备');

        modal.style.display = 'flex';
    },

    /** 编辑弹窗内类型变更 — 显隐 PWM/PID 扩展区 */
    _onEditTypeChange() {
        var type = document.getElementById('mdev-edit-type').value;
        var pwmSec = document.getElementById('mdev-edit-pwm-section');
        var pidSec = document.getElementById('mdev-edit-pid-section');
        if (pwmSec) pwmSec.style.display = (type === 'pwm') ? '' : 'none';
        if (pidSec) pidSec.style.display = (type === 'pid') ? '' : 'none';
    },

    /** 关闭编辑弹窗 */
    _closeEditModal() {
        var modal = document.getElementById('modbus-device-edit-modal');
        if (modal) modal.style.display = 'none';
        this._editingDeviceIdx = -1;
    },

    /** 保存编辑弹窗 */
    _saveEditModal() {
        var idx = this._editingDeviceIdx;
        var isNew = (idx < 0);

        if (isNew) {
            if (!this._modbusDevices) this._modbusDevices = [];
            if (this._modbusDevices.length >= 8) {
                Notification.warning(i18n.t('modbus-device-max-reached') || '最多8个子设备');
                return;
            }
            this._modbusDevices.push({});
            idx = this._modbusDevices.length - 1;
        }

        var dev = this._modbusDevices[idx];
        dev.name = document.getElementById('mdev-edit-name').value || 'Device';
        dev.deviceType = document.getElementById('mdev-edit-type').value || 'relay';
        dev.slaveAddress = parseInt(document.getElementById('mdev-edit-addr').value) || 1;
        dev.channelCount = parseInt(document.getElementById('mdev-edit-ch').value) || 2;
        dev.coilBase = parseInt(document.getElementById('mdev-edit-base').value) || 0;
        dev.controlProtocol = parseInt(document.getElementById('mdev-edit-protocol').value) || 0;
        dev.ncMode = (document.getElementById('mdev-edit-nc').value === 'true');
        dev.enabled = document.getElementById('mdev-edit-enabled').checked;
        dev.pwmRegBase = parseInt(document.getElementById('mdev-edit-pwm-reg-base').value) || 0;
        dev.pwmResolution = parseInt(document.getElementById('mdev-edit-pwm-resolution').value) || 8;
        var pidFields = ['pv','sv','out','p','i','d'];
        dev.pidAddrs = [];
        for (var pi = 0; pi < pidFields.length; pi++) {
            dev.pidAddrs.push(parseInt(document.getElementById('mdev-edit-pid-' + pidFields[pi]).value) || pi);
        }
        dev.pidDecimals = parseInt(document.getElementById('mdev-edit-pid-decimals').value) || 1;

        this._renderAllDevices();
        this._closeEditModal();
    },

    // ========== 控制弹窗 ==========

    /** 打开控制弹窗 */
    _openCtrlModal(idx) {
        if (!this._modbusDevices || !this._modbusDevices[idx]) return;

        // 缓存当前设备状态
        if (this._activeDeviceIdx >= 0) {
            var cacheKey = 'dev_' + this._activeDeviceIdx;
            if (this._coilStates.length > 0) this._deviceCoilCache[cacheKey] = this._coilStates.slice();
            if (this._pwmStates.length > 0) this._devicePwmCache[cacheKey] = this._pwmStates.slice();
            if (this._pidValues && Object.keys(this._pidValues).length > 0) {
                this._devicePidCache[cacheKey] = JSON.parse(JSON.stringify(this._pidValues));
            }
        }

        this._activeDeviceIdx = idx;
        localStorage.setItem('modbus_active_device', String(idx));

        var dev = this._modbusDevices[idx];
        var type = dev.deviceType || 'relay';

        // 更新弹窗标题
        var title = document.getElementById('modbus-ctrl-modal-title');
        if (title) title.textContent = (dev.name || 'Device') + ' - ' + (i18n.t('modbus-device-ctrl-title') || '设备控制');

        // 显隐类型专属配置面板
        var relayConfig = document.getElementById('modbus-relay-config');
        if (relayConfig) relayConfig.style.display = (type === 'relay') ? '' : 'none';

        // 显隐控制面板
        var relayPanel = document.getElementById('modbus-relay-panel');
        var pwmPanel = document.getElementById('modbus-pwm-panel');
        var pidPanel = document.getElementById('modbus-pid-panel');
        if (relayPanel) relayPanel.style.display = (type === 'relay') ? '' : 'none';
        if (pwmPanel) pwmPanel.style.display = (type === 'pwm') ? '' : 'none';
        if (pidPanel) pidPanel.style.display = (type === 'pid') ? '' : 'none';

        // 更新延时通道下拉
        this._updateDelayChannelSelect();

        // 恢复缓存或刷新
        var newCacheKey = 'dev_' + idx;
        if (type === 'pwm') {
            if (this._devicePwmCache[newCacheKey]) {
                this._pwmStates = this._devicePwmCache[newCacheKey].slice();
                this._renderPwmGrid();
            } else {
                this._pwmStates = [];
                this._renderPwmGrid();
                this.refreshPwmStatus();
            }
        } else if (type === 'pid') {
            if (this._devicePidCache[newCacheKey]) {
                this._pidValues = JSON.parse(JSON.stringify(this._devicePidCache[newCacheKey]));
                this._renderPidGrid();
            } else {
                this._pidValues = {};
                this._renderPidGrid();
                this.refreshPidStatus();
            }
        } else {
            if (this._deviceCoilCache[newCacheKey]) {
                this._coilStates = this._deviceCoilCache[newCacheKey].slice();
                this._renderCoilGrid();
            } else {
                this._coilStates = [];
                this._renderCoilGrid();
                this.refreshCoilStatus();
            }
        }

        // 显示弹窗
        var modal = document.getElementById('modbus-device-ctrl-modal');
        if (modal) modal.style.display = 'flex';
    },

    /** 关闭控制弹窗 */
    _closeCtrlModal() {
        // 停止自动刷新
        if (this._coilAutoRefreshTimer) {
            clearInterval(this._coilAutoRefreshTimer);
            this._coilAutoRefreshTimer = null;
        }
        var autoEl = document.getElementById('modbus-ctrl-auto-refresh');
        if (autoEl) autoEl.checked = false;
        var pidAutoEl = document.getElementById('modbus-ctrl-pid-auto-refresh');
        if (pidAutoEl) pidAutoEl.checked = false;

        var modal = document.getElementById('modbus-device-ctrl-modal');
        if (modal) modal.style.display = 'none';
    },

    /** 旧方法保留兼容: _activateDevice 改为调用 _openCtrlModal */
    _activateDevice(idx) {
        this._openCtrlModal(idx);
    },

    // ============================================================================
    // 设备类型切换与 PWM 控制
    // ============================================================================

    /** 切换设备类型面板显隐 (从当前激活设备读取类型) */
    onDeviceTypeChange() {
        if (this._activeDeviceIdx < 0 || !this._modbusDevices) return;
        var dev = this._modbusDevices[this._activeDeviceIdx];
        if (!dev) return;
        var type = dev.deviceType || 'relay';
        var types = ['relay', 'pwm', 'pid'];
        types.forEach(function(t) {
            var panel = document.getElementById('modbus-' + t + '-panel');
            var config = document.getElementById('modbus-' + t + '-config');
            if (panel) panel.style.display = (t === type) ? '' : 'none';
            if (config) config.style.display = (t === type) ? '' : 'none';
        });
    },

    /** 继电器协议模式切换 — 更新基地址标签提示 */
    onRelayModeChange() {
        if (this._activeDeviceIdx < 0 || !this._modbusDevices) return;
        var dev = this._modbusDevices[this._activeDeviceIdx];
        if (!dev) return;
        var mode = (dev.controlProtocol === 1) ? 'register' : 'coil';
        var hintEl = document.querySelector('#modbus-relay-config .field-hint');
        if (hintEl) {
            hintEl.textContent = (mode === 'register')
                ? (i18n.t('modbus-ctrl-reg-base-hint') || '寄存器起始地址，如M88继电器从0x0008开始')
                : (i18n.t('modbus-ctrl-coil-base-hint') || '线圈起始地址，默认0');
        }
    },

    /** 获取 PWM 参数 */
    _getPwmParams() {
        var dev = (this._activeDeviceIdx >= 0 && this._modbusDevices)
            ? this._modbusDevices[this._activeDeviceIdx] : null;
        var res = dev ? (dev.pwmResolution || 8) : 8;
        return {
            slaveAddress: dev ? (dev.slaveAddress || 1) : 1,
            channelCount: dev ? (dev.channelCount || 4) : 4,
            regBase: dev ? (dev.pwmRegBase || 0) : 0,
            resolution: res,
            maxValue: (1 << res) - 1
        };
    },

    /** 渲染 PWM 通道卡片 */
    _renderPwmGrid() {
        var grid = document.getElementById('pwm-channel-grid');
        if (!grid) return;
        var p = this._getPwmParams();
        var html = '';
        for (var i = 0; i < p.channelCount; i++) {
            var val = i < this._pwmStates.length ? this._pwmStates[i] : 0;
            var pct = p.maxValue > 0 ? Math.round(val / p.maxValue * 100) : 0;
            html += '<div class="pwm-card">'
                + '<div class="pwm-ch">CH' + i + '</div>'
                + '<input type="range" class="pwm-slider" min="0" max="' + p.maxValue + '" value="' + val + '" '
                + 'oninput="AppState._onPwmSliderInput(' + i + ',this.value)" '
                + 'onchange="AppState._onPwmSliderChange(' + i + ',this.value)" data-ch="' + i + '">'
                + '<div class="pwm-value-row">'
                + '<input type="number" class="pwm-num-input" min="0" max="' + p.maxValue + '" value="' + val + '" '
                + 'onchange="AppState._onPwmNumChange(' + i + ',this.value)" data-ch="' + i + '">'
                + '<span class="pwm-pct">' + pct + '%</span>'
                + '</div></div>';
        }
        grid.innerHTML = html;
    },

    /** 读取 PWM 寄存器状态 (FC 0x03) */
    async refreshPwmStatus() {
        var p = this._getPwmParams();
        try {
            var res = await apiGetSilent('/api/modbus/register/read', {
                slaveAddress: p.slaveAddress,
                startAddress: p.regBase,
                quantity: p.channelCount,
                functionCode: 3
            });
            if (res && res.success && res.data && res.data.values) {
                this._pwmStates = res.data.values;
                if (this._activeDeviceId) {
                    this._devicePwmCache[this._activeDeviceId] = this._pwmStates.slice();
                }
                this._renderPwmGrid();
                this._appendDebugLog(res.debug, 'ReadRegs FC03');
            }
        } catch (e) { /* 静默 */ }
    },

    /** 写单个 PWM 寄存器 (FC 0x06) */
    async setPwmChannel(ch, value) {
        var p = this._getPwmParams();
        try {
            var res = await apiPost('/api/modbus/register/write', {
                slaveAddress: p.slaveAddress,
                registerAddress: p.regBase + ch,
                value: value
            });
            if (res && res.success) {
                if (ch < this._pwmStates.length) this._pwmStates[ch] = value;
                this._appendDebugLog(res.debug, 'WriteReg CH' + ch + '=' + value);
            }
        } catch (e) {
            Notification.error(i18n.t('modbus-ctrl-fail'));
        }
    },

    /** 批量 PWM 设置 (FC 0x10) */
    async batchPwm(action) {
        var p = this._getPwmParams();
        var values = [];
        var fillVal = action === 'max' ? p.maxValue : 0;
        for (var i = 0; i < p.channelCount; i++) values.push(fillVal);
        try {
            var res = await apiPost('/api/modbus/register/batch-write', {
                slaveAddress: p.slaveAddress,
                startAddress: p.regBase,
                values: JSON.stringify(values)
            });
            if (res && res.success) {
                this._pwmStates = values;
                this._renderPwmGrid();
                Notification.success(i18n.t('modbus-ctrl-success'));
            }
        } catch (e) {
            Notification.error(i18n.t('modbus-ctrl-fail'));
        }
    },

    /** 滑块拖动时仅更新显示 */
    _onPwmSliderInput(ch, val) {
        var numInput = document.querySelector('.pwm-num-input[data-ch="' + ch + '"]');
        var card = numInput ? numInput.closest('.pwm-card') : null;
        var pctSpan = card ? card.querySelector('.pwm-pct') : null;
        var p = this._getPwmParams();
        if (numInput) numInput.value = val;
        if (pctSpan) pctSpan.textContent = (p.maxValue > 0 ? Math.round(val / p.maxValue * 100) : 0) + '%';
    },

    /** 滑块释放时发送写入 */
    _onPwmSliderChange(ch, val) {
        this.setPwmChannel(ch, parseInt(val));
    },

    /** 数值输入变更 */
    _onPwmNumChange(ch, val) {
        var p = this._getPwmParams();
        val = Math.max(0, Math.min(parseInt(val) || 0, p.maxValue));
        var slider = document.querySelector('.pwm-slider[data-ch="' + ch + '"]');
        if (slider) slider.value = val;
        var card = slider ? slider.closest('.pwm-card') : null;
        var pctSpan = card ? card.querySelector('.pwm-pct') : null;
        if (pctSpan) pctSpan.textContent = (p.maxValue > 0 ? Math.round(val / p.maxValue * 100) : 0) + '%';
        this.setPwmChannel(ch, val);
    },

    // ============================================================================
    // PID 控制器
    // ============================================================================

    _getPidParams() {
        var dev = (this._activeDeviceIdx >= 0 && this._modbusDevices)
            ? this._modbusDevices[this._activeDeviceIdx] : null;
        var pidA = dev ? (dev.pidAddrs || [0,1,2,3,4,5]) : [0,1,2,3,4,5];
        var decimals = dev ? (dev.pidDecimals || 1) : 1;
        return {
            slaveAddress: dev ? (dev.slaveAddress || 1) : 1,
            pvAddr: pidA[0] || 0,
            svAddr: pidA[1] || 1,
            outAddr: pidA[2] || 2,
            pAddr: pidA[3] || 3,
            iAddr: pidA[4] || 4,
            dAddr: pidA[5] || 5,
            decimals: decimals,
            scaleFactor: Math.pow(10, decimals)
        };
    },

    _renderPidGrid() {
        var container = document.getElementById('pid-data-grid');
        if (!container) return;
        var p = this._getPidParams();
        var v = this._pidValues || {};
        var sf = p.scaleFactor;

        var fmtVal = function(raw, dec) {
            if (raw === undefined || raw === null) return '--';
            return (raw / sf).toFixed(dec);
        };
        var fmtPct = function(raw) {
            if (raw === undefined || raw === null) return '--';
            return (raw / sf).toFixed(p.decimals) + '%';
        };

        var cards = [
            { key: 'pv', label: i18n.t('modbus-ctrl-pid-pv-label') || '过程值 PV', value: fmtVal(v.pv, p.decimals), editable: false, big: true },
            { key: 'sv', label: i18n.t('modbus-ctrl-pid-sv-label') || '设定值 SV', value: fmtVal(v.sv, p.decimals), editable: true },
            { key: 'out', label: i18n.t('modbus-ctrl-pid-out-label') || '输出 %', value: fmtPct(v.out), editable: false },
            { key: 'p', label: i18n.t('modbus-ctrl-pid-p-label') || 'P 比例', value: fmtVal(v.p, p.decimals), editable: true },
            { key: 'i', label: i18n.t('modbus-ctrl-pid-i-label') || 'I 积分', value: fmtVal(v.i, p.decimals), editable: true },
            { key: 'd', label: i18n.t('modbus-ctrl-pid-d-label') || 'D 微分', value: fmtVal(v.d, p.decimals), editable: true }
        ];

        var html = '';
        for (var ci = 0; ci < cards.length; ci++) {
            var c = cards[ci];
            var cls = 'pid-card' + (c.big ? ' pid-pv-card' : '') + (c.editable ? ' pid-editable' : '');
            html += '<div class="' + cls + '">';
            html += '<div class="pid-card-label">' + c.label + '</div>';

            if (c.editable) {
                var rawVal = (v[c.key] !== undefined && v[c.key] !== null) ? (v[c.key] / sf).toFixed(p.decimals) : '';
                html += '<div class="pid-card-value">' + c.value + '</div>';
                html += '<div class="pid-edit-row">';
                html += '<input type="number" class="pid-input" step="' + (1 / sf) + '" value="' + rawVal + '" data-param="' + c.key + '">';
                html += '<button type="button" class="btn btn-sm btn-enable pid-set-btn" style="width:64px;" onclick="AppState._onPidInputChange(\'' + c.key + '\', this.parentNode.querySelector(\'.pid-input\').value)">'
                      + (i18n.t('modbus-ctrl-pid-set') || '设置') + '</button>';
                html += '</div>';
            } else {
                html += '<div class="pid-card-value">' + c.value + '</div>';
            }

            html += '</div>';
        }
        container.innerHTML = html;
    },

    refreshPidStatus() {
        var p = this._getPidParams();
        if (!p.slaveAddress) return;

        var addrs = [p.pvAddr, p.svAddr, p.outAddr, p.pAddr, p.iAddr, p.dAddr];
        var minAddr = Math.min.apply(null, addrs);
        var maxAddr = Math.max.apply(null, addrs);
        var quantity = maxAddr - minAddr + 1;

        if (quantity > 125) {
            Notification.warning('PID 寄存器地址跨度过大 (>' + 125 + ')');
            return;
        }

        var self = this;
        apiGetSilent('/api/modbus/register/read', {
            slaveAddress: p.slaveAddress,
            startAddress: minAddr,
            quantity: quantity,
            functionCode: 3
        }).then(function(res) {
            if (res && res.success && res.data && res.data.values) {
                var vals = res.data.values;
                self._pidValues = {
                    pv: vals[p.pvAddr - minAddr],
                    sv: vals[p.svAddr - minAddr],
                    out: vals[p.outAddr - minAddr],
                    p: vals[p.pAddr - minAddr],
                    i: vals[p.iAddr - minAddr],
                    d: vals[p.dAddr - minAddr]
                };
                self._devicePidCache[self._activeDeviceId] = JSON.parse(JSON.stringify(self._pidValues));
                self._renderPidGrid();
            }
            if (res && res.debug) {
                self._appendDebugLog(res.debug.tx ? { tx: res.debug.tx, rx: res.debug.rx } : null, 'PID Read');
            }
        }).catch(function() {});
    },

    setPidRegister(paramName, rawValue) {
        var p = this._getPidParams();
        var addrMap = { sv: p.svAddr, p: p.pAddr, i: p.iAddr, d: p.dAddr };
        var addr = addrMap[paramName];
        if (addr === undefined) return;

        var self = this;
        apiPost('/api/modbus/register/write', {
            slaveAddress: p.slaveAddress,
            registerAddress: addr,
            value: rawValue
        }).then(function(res) {
            if (res && res.success) {
                self._pidValues[paramName] = rawValue;
                self._devicePidCache[self._activeDeviceId] = JSON.parse(JSON.stringify(self._pidValues));
                self._renderPidGrid();
            }
            if (res && res.debug) {
                self._appendDebugLog(res.debug.tx ? { tx: res.debug.tx, rx: res.debug.rx } : null, 'PID Write ' + paramName);
            }
        }).catch(function() {});
    },

    _onPidInputChange(paramName, displayValue) {
        var p = this._getPidParams();
        var val = parseFloat(displayValue);
        if (isNaN(val)) return;
        var rawValue = Math.round(val * p.scaleFactor);
        this.setPidRegister(paramName, rawValue);
    },

    _stopPidAutoRefresh() {
        if (this._pidAutoRefreshTimer) {
            clearInterval(this._pidAutoRefreshTimer);
            this._pidAutoRefreshTimer = null;
        }
    },

    togglePidAutoRefresh() {
        var checked = document.getElementById('modbus-ctrl-pid-auto-refresh')?.checked;
        if (checked) {
            this.refreshPidStatus();
            var self = this;
            this._pidAutoRefreshTimer = setInterval(function() { self.refreshPidStatus(); }, 3000);
        } else {
            this._stopPidAutoRefresh();
        }
    },

    /** 追加 Modbus 调试日志 */
    _appendDebugLog(debug, label) {
        var log = document.getElementById('modbus-debug-log');
        if (!log) return;
        if (!debug && !label) return;
        var time = new Date().toLocaleTimeString();
        var entry = document.createElement('div');
        entry.className = 'modbus-debug-entry';
        var html = '<div class="modbus-debug-time">[' + time + '] ' + (label || '') + '</div>';
        if (debug && debug.tx) html += '<div class="modbus-debug-tx">TX: ' + debug.tx + '</div>';
        if (debug && debug.rx) html += '<div class="modbus-debug-rx">RX: ' + debug.rx + '</div>';
        entry.innerHTML = html;
        log.appendChild(entry);
        log.scrollTop = log.scrollHeight;
        // 限制最多保留100条
        while (log.children.length > 100) log.removeChild(log.firstChild);
    },

    /** 追加错误调试日志 */
    _appendDebugError(msg, debug) {
        var log = document.getElementById('modbus-debug-log');
        if (!log) return;
        var time = new Date().toLocaleTimeString();
        var entry = document.createElement('div');
        entry.className = 'modbus-debug-entry';
        var html = '<div class="modbus-debug-time">[' + time + ']</div>';
        html += '<div class="modbus-debug-err">' + msg + '</div>';
        if (debug && debug.tx) html += '<div class="modbus-debug-tx">TX: ' + debug.tx + '</div>';
        if (debug && debug.rx) html += '<div class="modbus-debug-rx">RX: ' + debug.rx + '</div>';
        entry.innerHTML = html;
        log.appendChild(entry);
        log.scrollTop = log.scrollHeight;
        while (log.children.length > 100) log.removeChild(log.firstChild);
    },

    /** 清除调试日志 */
    clearModbusDebugLog() {
        var log = document.getElementById('modbus-debug-log');
        if (log) log.innerHTML = '';
    },

    /** 从 UI 读取控制参数 */
    _getCoilParams() {
        var dev = (this._activeDeviceIdx >= 0 && this._modbusDevices[this._activeDeviceIdx])
            ? this._modbusDevices[this._activeDeviceIdx] : {};
        return {
            slaveAddress: dev.slaveAddress || 1,
            channelCount: dev.channelCount || 8,
            coilBase: dev.coilBase || 0,
            ncMode: !!dev.ncMode,
            relayMode: (dev.controlProtocol === 1) ? 'register' : 'coil'
        };
    },

    /** 刷新线圈状态（从从站读取） */
    async refreshCoilStatus() {
        const p = this._getCoilParams();
        try {
            const res = await apiGetSilent('/api/modbus/coil/status', {
                slaveAddress: p.slaveAddress,
                channelCount: p.channelCount,
                coilBase: p.coilBase,
                mode: p.relayMode
            });
            if (res && res.success && res.data && res.data.states) {
                this._coilStates = res.data.states;
                // 缓存到当前设备
                if (this._activeDeviceId) {
                    this._deviceCoilCache[this._activeDeviceId] = this._coilStates.slice();
                }
                this._renderCoilGrid();
                this._appendDebugLog(res.debug, 'ReadCoils FC01');
            } else if (res && !res.success) {
                Notification.error(res.error || i18n.t('modbus-ctrl-fail'));
                this._appendDebugError(res.error || 'ReadCoils failed', res.debug);
            }
        } catch (e) {
            // 静默失败（自动刷新时不弹错误）
        }
    },

    /** 渲染线圈网格 */
    _renderCoilGrid() {
        const grid = document.getElementById('coil-status-grid');
        if (!grid) return;

        const p = this._getCoilParams();
        const onText = i18n.t('modbus-ctrl-status-on') || 'ON';
        const offText = i18n.t('modbus-ctrl-status-off') || 'OFF';

        let html = '';
        for (let i = 0; i < p.channelCount; i++) {
            const coilState = i < this._coilStates.length ? this._coilStates[i] : false;
            // NC 模式：线圈断电(false)=NC闭合=设备通电(ON)，需反转显示
            const isOn = p.ncMode ? !coilState : coilState;
            const cls = isOn ? 'coil-on' : 'coil-off';
            html += '<div class="coil-card ' + cls + '" onclick="AppState.toggleCoil(' + i + ')" data-ch="' + i + '">'
                  + '<div class="coil-ch">CH' + i + '</div>'
                  + '<div class="coil-st">' + (isOn ? onText : offText) + '</div>'
                  + '</div>';
        }
        grid.innerHTML = html;
    },

    /** 切换单个线圈 */
    async toggleCoil(ch) {
        const card = document.querySelector('.coil-card[data-ch="' + ch + '"]');
        if (card) card.classList.add('coil-loading');

        const p = this._getCoilParams();
        try {
            const res = await apiPost('/api/modbus/coil/control', {
                slaveAddress: p.slaveAddress,
                channel: ch,
                coilBase: p.coilBase,
                action: 'toggle',
                mode: p.relayMode
            });
            if (res && res.success && res.data) {
                // 更新单个状态
                if (ch < this._coilStates.length) {
                    this._coilStates[ch] = res.data.state;
                }
                this._renderCoilGrid();
                Notification.success(i18n.t('modbus-ctrl-success'));
                this._appendDebugLog(res.debug, 'Toggle CH' + ch);
            } else {
                Notification.error((res && res.error) || i18n.t('modbus-ctrl-fail'));
                this._appendDebugError('Toggle CH' + ch + ' failed', res && res.debug);
            }
        } catch (e) {
            Notification.error(i18n.t('modbus-ctrl-fail'));
        }
        if (card) card.classList.remove('coil-loading');
    },

    /** 批量线圈操作 */
    async batchCoil(action) {
        const p = this._getCoilParams();
        // NC 模式：allOn/allOff 命令反转（NC 接线下，线圈断电=设备通电）
        let modbusAction = action;
        if (p.ncMode) {
            if (action === 'allOn') modbusAction = 'allOff';
            else if (action === 'allOff') modbusAction = 'allOn';
        }
        try {
            const res = await apiPost('/api/modbus/coil/batch', {
                slaveAddress: p.slaveAddress,
                channelCount: p.channelCount,
                coilBase: p.coilBase,
                action: modbusAction,
                mode: p.relayMode
            });
            if (res && res.success && res.data && res.data.states) {
                this._coilStates = res.data.states;
                this._renderCoilGrid();
                Notification.success(i18n.t('modbus-ctrl-success'));
                this._appendDebugLog(res.debug, 'Batch: ' + action);
            } else {
                Notification.error((res && res.error) || i18n.t('modbus-ctrl-fail'));
                this._appendDebugError('Batch ' + action + ' failed', res && res.debug);
            }
        } catch (e) {
            Notification.error(i18n.t('modbus-ctrl-fail'));
        }
    },

    /** 延时控制（NO模式：硬件闪开；NC模式：软件延时断开） */
    async startCoilDelay() {
        const p = this._getCoilParams();
        const ch = parseInt(document.getElementById('modbus-ctrl-delay-ch')?.value || '0');
        const units = parseInt(document.getElementById('modbus-ctrl-delay-units')?.value || '50');

        if (units < 1 || units > 255) {
            Notification.warning(i18n.t('modbus-ctrl-fail') + ': 1-255 (x100ms)');
            return;
        }

        try {
            const params = {
                slaveAddress: p.slaveAddress,
                channel: ch,
                delayBase: 0x0200,
                delayUnits: units,
                ncMode: p.ncMode,
                coilBase: p.coilBase
            };
            const res = await apiPost('/api/modbus/coil/delay', params);
            if (res && res.success) {
                Notification.success(i18n.t('modbus-ctrl-delay-ok'));
                this._appendDebugLog(res.debug, 'Delay CH' + ch + ' ' + units + 'x100ms' + (p.ncMode ? ' (NC)' : ''));
                // NC 软件延时需要等更长时间再刷新状态
                const refreshDelay = p.ncMode ? (units * 100 + 500) : 500;
                setTimeout(() => this.refreshCoilStatus(), refreshDelay);
            } else {
                Notification.error((res && res.error) || i18n.t('modbus-ctrl-fail'));
                this._appendDebugError('Delay CH' + ch + ' failed', res && res.debug);
            }
        } catch (e) {
            Notification.error(i18n.t('modbus-ctrl-fail'));
        }
    },

    /** 通道数变化时更新延时通道下拉 */
    onCoilChannelCountChange() {
        this._updateDelayChannelSelect();
        this._renderCoilGrid();
    },

    _updateDelayChannelSelect() {
        const sel = document.getElementById('modbus-ctrl-delay-ch');
        if (!sel) return;
        var dev = (this._activeDeviceIdx >= 0 && this._modbusDevices)
            ? this._modbusDevices[this._activeDeviceIdx] : null;
        const count = dev ? (dev.channelCount || 8) : 8;
        let html = '';
        for (let i = 0; i < count; i++) {
            html += '<option value="' + i + '">CH' + i + '</option>';
        }
        sel.innerHTML = html;
    },

    /** 自动刷新开关 */
    toggleCoilAutoRefresh() {
        const checked = document.getElementById('modbus-ctrl-auto-refresh')?.checked;
        if (checked) {
            this.refreshCoilStatus();
            this._coilAutoRefreshTimer = setInterval(() => this.refreshCoilStatus(), 3000);
        } else {
            if (this._coilAutoRefreshTimer) {
                clearInterval(this._coilAutoRefreshTimer);
                this._coilAutoRefreshTimer = null;
            }
        }
    },

    /** 读取设备地址 */
    async readDeviceAddress() {
        const slaveAddr = parseInt(document.getElementById('modbus-debug-slave-addr')?.value || '0');
        const addrReg = parseInt(document.getElementById('modbus-ctrl-addr-reg')?.value || '0');
        try {
            const res = await apiPost('/api/modbus/device/address', {
                slaveAddress: slaveAddr,
                addressRegister: addrReg
            });
            const display = document.getElementById('modbus-ctrl-current-addr');
            if (res && res.success && res.data) {
                if (display) display.textContent = 'Current: ' + res.data.currentAddress;
                Notification.success(i18n.t('modbus-ctrl-success'));
                this._appendDebugLog(res.debug, 'ReadAddr reg=' + addrReg);
            } else {
                if (display) display.textContent = '';
                Notification.error((res && res.error) || i18n.t('modbus-ctrl-fail'));
                this._appendDebugError('ReadAddr failed', res && res.debug);
            }
        } catch (e) {
            Notification.error(i18n.t('modbus-ctrl-fail'));
        }
    },

    /** 设置设备地址（广播 FC 0x10） */
    async setDeviceAddress() {
        const slaveAddr = parseInt(document.getElementById('modbus-debug-slave-addr')?.value || '0');
        const addrReg = parseInt(document.getElementById('modbus-ctrl-addr-reg')?.value || '0');
        const newAddr = parseInt(document.getElementById('modbus-ctrl-new-addr')?.value || '0');
        if (newAddr < 1 || newAddr > 255) {
            Notification.warning('Invalid address (1-255)');
            return;
        }
        try {
            const res = await apiPost('/api/modbus/device/address', {
                slaveAddress: slaveAddr,
                addressRegister: addrReg,
                newAddress: newAddr
            });
            if (res && res.success) {
                Notification.success(i18n.t('modbus-ctrl-success'));
                this._appendDebugLog(res.debug, 'SetAddr -> ' + newAddr);
            } else {
                Notification.error((res && res.error) || i18n.t('modbus-ctrl-fail'));
                this._appendDebugError('SetAddr failed', res && res.debug);
            }
        } catch (e) {
            Notification.error(i18n.t('modbus-ctrl-fail'));
        }
    },

    /** 设置波特率（FC 0xB0 专有指令） */
    async setDeviceBaudrate() {
        const slaveAddr = parseInt(document.getElementById('modbus-debug-slave-addr')?.value || '0');
        const baud = parseInt(document.getElementById('modbus-ctrl-baudrate')?.value || '9600');
        try {
            const res = await apiPost('/api/modbus/device/baudrate', {
                slaveAddress: slaveAddr,
                baudRate: baud
            });
            if (res && res.success) {
                Notification.success(i18n.t('modbus-ctrl-success'));
                this._appendDebugLog(res.debug, 'SetBaud -> ' + baud);
            } else {
                Notification.error((res && res.error) || i18n.t('modbus-ctrl-fail'));
                this._appendDebugError('SetBaud failed', res && res.debug);
            }
        } catch (e) {
            Notification.error(i18n.t('modbus-ctrl-fail'));
        }
    },

    /** 读取离散输入 */
    async readDiscreteInputs() {
        const slaveAddr = parseInt(document.getElementById('modbus-debug-slave-addr')?.value || '0');
        const inputCount = parseInt(document.getElementById('modbus-ctrl-input-count')?.value || '4');
        const inputBase = parseInt(document.getElementById('modbus-ctrl-input-base')?.value || '0');
        try {
            const res = await apiGet('/api/modbus/device/inputs', {
                slaveAddress: slaveAddr,
                inputCount: inputCount,
                inputBase: inputBase
            });
            const display = document.getElementById('modbus-ctrl-inputs-display');
            if (res && res.success && res.data && res.data.states) {
                const states = res.data.states;
                let html = '';
                for (let i = 0; i < states.length; i++) {
                    const color = states[i] ? '#4CAF50' : '#909399';
                    html += '<span style="display:inline-block;margin:2px 4px;padding:2px 8px;border-radius:3px;'
                          + 'background:' + (states[i] ? '#e6f7e6' : '#f5f5f5') + ';color:' + color + ';font-size:12px;">'
                          + 'IN' + i + ':' + (states[i] ? 'ON' : 'OFF') + '</span>';
                }
                if (display) display.innerHTML = html;
                Notification.success(i18n.t('modbus-ctrl-success'));
                this._appendDebugLog(res.debug, 'ReadInputs FC02');
            } else {
                if (display) display.innerHTML = '';
                Notification.error((res && res.error) || i18n.t('modbus-ctrl-fail'));
                this._appendDebugError('ReadInputs failed', res && res.debug);
            }
        } catch (e) {
            Notification.error(i18n.t('modbus-ctrl-fail'));
        }
    },
    
    /**
     * 加载已启用的 UART 外设列表（用于 Modbus RTU 外设选择下拉框）
     */
    async _loadUartPeripherals(selectedId) {
        const select = document.getElementById('rtu-peripheral-id');
        if (!select) return;

        try {
            const res = await apiGet('/api/peripherals?pageSize=50');
            if (!res || !res.success) return;

            // 筛选已启用的 UART 外设 (type === 1)
            this._uartPeripherals = (res.data || []).filter(p => p.type === 1 && p.enabled);

            // 清空并重建选项
            select.innerHTML = '<option value="" disabled>' + i18n.t('rtu-peripheral-placeholder') + '</option>';

            if (this._uartPeripherals.length === 0) {
                select.innerHTML += '<option value="" disabled>' + i18n.t('rtu-no-uart-peripherals') + '</option>';
                return;
            }

            this._uartPeripherals.forEach(p => {
                const pinsText = p.pins && p.pins.length >= 2
                    ? ' (RX:' + p.pins[0] + ', TX:' + p.pins[1] + ')'
                    : '';
                const opt = document.createElement('option');
                opt.value = p.id;
                opt.textContent = p.name + pinsText;
                select.appendChild(opt);
            });

            // 设置选中值
            if (selectedId) {
                select.value = selectedId;
                this.onRtuPeripheralChange(selectedId);
            }
        } catch (e) {
            console.error('Failed to load UART peripherals:', e);
        }
    },

    /**
     * UART 外设选择变更回调 — 显示引脚和波特率信息
     */
    onRtuPeripheralChange(peripheralId) {
        const infoDiv = document.getElementById('rtu-peripheral-info');
        if (!infoDiv || !peripheralId) {
            if (infoDiv) infoDiv.style.display = 'none';
            return;
        }

        // 从缓存列表获取基本信息
        const periph = (this._uartPeripherals || []).find(p => p.id === peripheralId);
        if (!periph) {
            infoDiv.style.display = 'none';
            return;
        }

        // 请求完整外设信息（含 baudRate）
        apiGet('/api/peripherals/?id=' + peripheralId).then(res => {
            if (!res || !res.success) return;
            const data = res.data;
            const baudRate = data.params?.baudRate || i18n.t('unknown') || '未知';
            const pins = data.pins || [];
            infoDiv.textContent = 'RX: GPIO' + pins[0] + ', TX: GPIO' + pins[1] + ', ' + i18n.t('uart-baudrate-label') + ': ' + baudRate;
            infoDiv.style.display = 'block';
        }).catch(() => {
            // 仅显示引脚信息
            const pins = periph.pins || [];
            infoDiv.textContent = 'RX: GPIO' + pins[0] + ', TX: GPIO' + pins[1];
            infoDiv.style.display = 'block';
        });
    },

    // ============ 外设接口管理（新版） ============
    
    // 外设列表分页状态
    _periphCurrentPage: 1,
    _periphPageSize: 10,
    _periphTotalCount: 0,
    
    /**
     * 加载外设列表
     */
    loadPeripherals(filterType = '') {
        const tbody = document.getElementById('peripheral-table-body');
        if (!tbody) return;
        
        tbody.innerHTML = '<tr><td colspan="6" style="text-align: center; color: #999;">' + i18n.t('peripheral-loading') + '</td></tr>';
        
        // 构造分页 URL
        let url = '/api/peripherals?page=' + this._periphCurrentPage + '&pageSize=' + this._periphPageSize;
        if (filterType) {
            url += '&category=' + filterType;
        }
        
        apiGet(url)
            .then(res => {
                if (!res || !res.success) {
                    tbody.innerHTML = '<tr><td colspan="6" style="text-align: center; color: #f56c6c;">' + i18n.t('peripheral-load-fail') + '</td></tr>';
                    this._renderPeriphPagination(0, 1, 10);
                    return;
                }
                
                // 获取分页信息
                const total = res.total || 0;
                const page = res.page || 1;
                const pageSize = res.pageSize || 10;
                this._periphTotalCount = total;
                
                const peripherals = res.data || [];
                
                if (peripherals.length === 0) {
                    tbody.innerHTML = '<tr><td colspan="6" style="text-align: center; color: #999;">' + i18n.t('peripheral-empty') + '</td></tr>';
                    this._renderPeriphPagination(total, page, pageSize);
                    return;
                }
                
                // 后端已排序，前端无需再排序
                let html = '';
                peripherals.forEach(periph => {
                    const statusColors = {
                        0: '#999',    // DISABLED
                        1: '#faad14', // ENABLED
                        2: '#52c41a', // INITIALIZED
                        3: '#1890ff', // RUNNING
                        4: '#ff4d4f'  // ERROR
                    };
                    const statusNames = {
                        0: i18n.t('peripheral-status-disabled'),
                        1: i18n.t('peripheral-status-enabled'),
                        2: i18n.t('peripheral-status-initialized'),
                        3: i18n.t('peripheral-status-running'),
                        4: i18n.t('peripheral-status-error')
                    };
                    const statusColor = statusColors[periph.status] || '#999';
                    const statusName = statusNames[periph.status] || i18n.t('peripheral-status-unknown');
                    
                    const pinsStr = periph.pins ? periph.pins.join(', ') : '--';
                    
                    html += `
                        <tr>
                            <td>${periph.id}</td>
                            <td>${periph.name}</td>
                            <td>${periph.typeName || periph.type}</td>
                            <td>${pinsStr}</td>
                            <td style="color: ${statusColor};">${statusName}</td>
                            <td>
                                <button class="btn btn-sm btn-edit" onclick="app.editPeripheral('${periph.id}')">${i18n.t('peripheral-edit')}</button>
                                <button class="btn btn-sm ${periph.enabled ? 'btn-disable' : 'btn-enable'}" onclick="app.togglePeripheral('${periph.id}')">${periph.enabled ? i18n.t('peripheral-disable') : i18n.t('peripheral-enable')}</button>
                                <button class="btn btn-sm btn-delete" onclick="app.deletePeripheral('${periph.id}')">${i18n.t('peripheral-delete')}</button>
                            </td>
                        </tr>
                    `;
                });
                
                tbody.innerHTML = html;
                // 渲染分页导航
                this._renderPeriphPagination(total, page, pageSize);
            })
            .catch(err => {
                console.error('Load peripherals failed:', err);
                tbody.innerHTML = '<tr><td colspan="6" style="text-align: center; color: #f56c6c;">' + i18n.t('peripheral-load-fail') + '</td></tr>';
                this._renderPeriphPagination(0, 1, 10);
            });
    },
        
    /**
     * 渲染外设列表分页导航
     */
    _renderPeriphPagination(total, page, pageSize) {
        const container = document.getElementById('periph-pagination');
        if (!container) return;
        const totalPages = Math.ceil(total / pageSize);
        if (totalPages <= 1) {
            container.innerHTML = '<span style="color:#999;font-size:12px;">' + i18n.t('periph-exec-total') + ': ' + total + '</span>';
            return;
        }
    
        let html = '<div class="pagination" style="display:flex;justify-content:center;align-items:center;gap:5px;flex-wrap:wrap;">';
        // 上一页
        if (page > 1) {
            html += '<button class="btn btn-sm" onclick="app._periphCurrentPage=' + (page-1) + ';app.loadPeripherals(document.getElementById(\'peripheral-filter-type\')?.value || \'\')">&laquo;</button>';
        } else {
            html += '<button class="btn btn-sm" disabled>&laquo;</button>';
        }
        // 页码（最多显示5个）
        const maxVisiblePages = 5;
        let startPage = Math.max(1, page - Math.floor(maxVisiblePages / 2));
        let endPage = Math.min(totalPages, startPage + maxVisiblePages - 1);
        if (endPage - startPage + 1 < maxVisiblePages) {
            startPage = Math.max(1, endPage - maxVisiblePages + 1);
        }
        if (startPage > 1) {
            html += '<button class="btn btn-sm" onclick="app._periphCurrentPage=1;app.loadPeripherals(document.getElementById(\'peripheral-filter-type\')?.value || \'\')">1</button>';
            if (startPage > 2) html += '<span style="padding:0 5px;">...</span>';
        }
        for (let i = startPage; i <= endPage; i++) {
            if (i === page) {
                html += '<button class="btn btn-sm btn-primary" disabled>' + i + '</button>';
            } else {
                html += '<button class="btn btn-sm" onclick="app._periphCurrentPage=' + i + ';app.loadPeripherals(document.getElementById(\'peripheral-filter-type\')?.value || \'\')">' + i + '</button>';
            }
        }
        if (endPage < totalPages) {
            if (endPage < totalPages - 1) html += '<span style="padding:0 5px;">...</span>';
            html += '<button class="btn btn-sm" onclick="app._periphCurrentPage=' + totalPages + ';app.loadPeripherals(document.getElementById(\'peripheral-filter-type\')?.value || \'\')">' + totalPages + '</button>';
        }
        // 下一页
        if (page < totalPages) {
            html += '<button class="btn btn-sm" onclick="app._periphCurrentPage=' + (page+1) + ';app.loadPeripherals(document.getElementById(\'peripheral-filter-type\')?.value || \'\')">&raquo;</button>';
        } else {
            html += '<button class="btn btn-sm" disabled>&raquo;</button>';
        }
        html += ' <span style="color:#999;font-size:12px;margin-left:10px;">' + i18n.t('periph-exec-total') + ': ' + total + '</span>';
        html += '</div>';
        container.innerHTML = html;
    },
    
    /**
     * 打开外设模态框
     */
    openPeripheralModal(isEdit = false, peripheralId = null) {
        const modal = document.getElementById('peripheral-modal');
        const title = document.getElementById('peripheral-modal-title');
        const form = document.getElementById('peripheral-form');
        
        if (!modal) {
            // 如果新模态框不存在，回退到旧GPIO模态框
            this.openGpioModal(isEdit, peripheralId ? { pin: peripheralId } : null);
            return;
        }
        
        form?.reset();
        const errorEl = document.getElementById('peripheral-error');
        if (errorEl) errorEl.style.display = 'none';
        
        // 隐藏所有参数组
        document.querySelectorAll('.peripheral-params-group').forEach(el => el.style.display = 'none');
        
        if (isEdit && peripheralId) {
            title.textContent = i18n.t('peripheral-edit-modal-title');
            this.loadPeripheralForEdit(peripheralId);
        } else {
            title.textContent = i18n.t('peripheral-add-modal-title');
            document.getElementById('peripheral-original-id').value = '';
            document.getElementById('peripheral-id-input').disabled = false;
            // 默认显示GPIO参数
            this.onPeripheralTypeChange('11');
        }

        modal.style.display = 'flex';
    },
    
    /**
     * 关闭外设模态框
     */
    closePeripheralModal() {
        const modal = document.getElementById('peripheral-modal');
        if (modal) modal.style.display = 'none';
        // 同时关闭旧版模态框
        const oldModal = document.getElementById('gpio-modal');
        if (oldModal) oldModal.style.display = 'none';
    },
    
    /**
     * 外设类型改变时更新参数显示
     */
    onPeripheralTypeChange(typeValue) {
        const type = parseInt(typeValue);
        
        // 隐藏所有参数组
        document.querySelectorAll('.peripheral-params-group').forEach(el => el.style.display = 'none');
        // 隐藏 UART 引脚方向提示
        const pinHint = document.getElementById('uart-pin-direction-hint');
        if (pinHint) pinHint.style.display = 'none';
        
        // 根据类型显示对应参数组
        if (type >= 11 && type <= 21) {
            // GPIO类型
            const gpioParams = document.getElementById('gpio-params');
            if (gpioParams) {
                gpioParams.style.display = 'block';
                
                // PWM 相关字段: PWM输出(17) 和 模拟输出(16)
                gpioParams.querySelectorAll('.pwm-only').forEach(el => {
                    el.style.display = (type === 17 || type === 16) ? 'block' : 'none';
                });
                
                // Digital Input 消抖时间(11, 13, 14)
                gpioParams.querySelectorAll('.input-only').forEach(el => {
                    el.style.display = (type === 11 || type === 13 || type === 14) ? 'block' : 'none';
                });
            }
        } else if (type === 1) {
            // UART
            const uartParams = document.getElementById('uart-params');
            if (uartParams) uartParams.style.display = 'block';
            // 显示 UART 引脚方向提示
            if (pinHint) pinHint.style.display = 'block';
        } else if (type === 2) {
            // I2C
            const i2cParams = document.getElementById('i2c-params');
            if (i2cParams) i2cParams.style.display = 'block';
        } else if (type === 3) {
            // SPI
            const spiParams = document.getElementById('spi-params');
            if (spiParams) spiParams.style.display = 'block';
        } else if (type === 26) {
            // ADC
            const adcParams = document.getElementById('adc-params');
            if (adcParams) adcParams.style.display = 'block';
        } else if (type === 27) {
            // DAC
            const dacParams = document.getElementById('dac-params');
            if (dacParams) dacParams.style.display = 'block';
        }
    },
    
    /**
     * 加载外设数据用于编辑
     */
    loadPeripheralForEdit(id) {
        // 使用 apiGet 的标准参数方式调用
        apiGet('/api/peripherals/', { id: id })
            .then(res => {
                console.log('Load peripheral response:', res);  // 调试日志
                
                if (res && res.success && res.data) {
                    const data = res.data;
                    
                    // 安全赋值函数，防止 undefined
                    const safeValue = (val, def = '') => (val !== undefined && val !== null) ? val : def;
                    
                    // 基本字段（带空值保护）
                    document.getElementById('peripheral-original-id').value = safeValue(data.id);
                    document.getElementById('peripheral-id-input').value = safeValue(data.id);
                    document.getElementById('peripheral-id-input').disabled = true;
                    document.getElementById('peripheral-name-input').value = safeValue(data.name);
                    document.getElementById('peripheral-type-input').value = safeValue(data.type, '11');
                    document.getElementById('peripheral-enabled-input').checked = data.enabled ? true : false;
                    
                    // 引脚配置：过滤掉 255（未配置的引脚）
                    if (data.pins && Array.isArray(data.pins)) {
                        const validPins = data.pins.filter(p => p !== 255 && p !== undefined && p !== null);
                        document.getElementById('peripheral-pins-input').value = validPins.join(',');
                    } else {
                        document.getElementById('peripheral-pins-input').value = '';
                    }
                    
                    // 更新类型相关参数显示
                    this.onPeripheralTypeChange(data.type);
                    
                    // 填充类型特定参数
                    if (data.params) {
                        // GPIO参数
                        if (data.params.initialState !== undefined) {
                            document.getElementById('gpio-initial-state').value = data.params.initialState;
                        }
                        if (data.params.pwmFrequency !== undefined) {
                            document.getElementById('gpio-pwm-freq').value = data.params.pwmFrequency;
                        }
                        if (data.params.pwmResolution !== undefined) {
                            document.getElementById('gpio-pwm-resolution').value = data.params.pwmResolution;
                        }
                        
                        // UART参数
                        if (data.params.baudRate !== undefined) {
                            document.getElementById('uart-baudrate').value = data.params.baudRate;
                        }
                        if (data.params.dataBits !== undefined) {
                            document.getElementById('uart-databits').value = data.params.dataBits;
                        }
                        if (data.params.stopBits !== undefined) {
                            document.getElementById('uart-stopbits').value = data.params.stopBits;
                        }
                        if (data.params.parity !== undefined) {
                            document.getElementById('uart-parity').value = data.params.parity;
                        }
                        
                        // I2C参数
                        if (data.params.frequency !== undefined) {
                            document.getElementById('i2c-frequency').value = data.params.frequency;
                        }
                        if (data.params.address !== undefined) {
                            document.getElementById('i2c-address').value = data.params.address;
                        }
                        
                        // SPI参数
                        if (data.params.frequency !== undefined) {
                            document.getElementById('spi-frequency').value = data.params.frequency;
                        }
                        if (data.params.mode !== undefined) {
                            document.getElementById('spi-mode').value = data.params.mode;
                        }
                        
                        // ADC参数
                        if (data.params.resolution !== undefined) {
                            document.getElementById('adc-resolution').value = data.params.resolution;
                        }
                        if (data.params.attenuation !== undefined) {
                            document.getElementById('adc-attenuation').value = data.params.attenuation;
                        }
                        
                        if (data.params.defaultDuty !== undefined) {
                            const el = document.getElementById('gpio-default-duty');
                            if (el) el.value = data.params.defaultDuty;
                        }
                        if (data.params.debounceMs !== undefined) {
                            const el = document.getElementById('gpio-debounce-ms');
                            if (el) el.value = data.params.debounceMs;
                        }
                        
                        // DAC参数
                        if (data.params.defaultValue !== undefined) {
                            const el = document.getElementById('dac-default-value');
                            if (el) el.value = data.params.defaultValue;
                        }
                    }
                } else {
                    console.error('Invalid response format:', res);
                    Notification.error(i18n.t('peripheral-load-fail'), i18n.t('peripheral-title'));
                }
            })
            .catch(err => {
                console.error('Load peripheral for edit failed:', err);
                Notification.error(i18n.t('peripheral-load-fail'), i18n.t('peripheral-title'));
            });
    },
    
    /**
     * 保存外设配置
     */
    savePeripheralConfig() {
        const originalId = document.getElementById('peripheral-original-id').value;
        const id = document.getElementById('peripheral-id-input').value.trim();
        const name = document.getElementById('peripheral-name-input').value.trim();
        const type = document.getElementById('peripheral-type-input').value;
        const enabled = document.getElementById('peripheral-enabled-input').checked ? 1 : 0;
        const pinsStr = document.getElementById('peripheral-pins-input').value.trim();
        const errEl = document.getElementById('peripheral-error');
        
        // 调试日志
        console.log('[savePeripheralConfig] originalId:', originalId, 'id:', id);
        
        if (!name || !type || !pinsStr) {
            errEl.textContent = i18n.t('peripheral-validate-required');
            errEl.style.display = 'block';
            return;
        }
        
        const isEdit = originalId !== '';
        console.log('[savePeripheralConfig] isEdit:', isEdit);
        
        // 编辑模式下确保 id 有值
        const finalId = isEdit ? originalId : (id || undefined);
        console.log('[savePeripheralConfig] finalId:', finalId);
        
        // 构建数据对象
        const data = {
            id: finalId,
            name: name,
            type: type,
            enabled: enabled,
            pins: pinsStr
        };
        
        // 根据类型添加特定参数
        const typeNum = parseInt(type);
        if (typeNum >= 11 && typeNum <= 21) {
            // GPIO参数
            data.initialState = document.getElementById('gpio-initial-state')?.value || '0';
            
            // PWM/Analog Output
            if (typeNum === 17 || typeNum === 16) {
                data.pwmFrequency = document.getElementById('gpio-pwm-freq')?.value || '1000';
                data.pwmResolution = document.getElementById('gpio-pwm-resolution')?.value || '8';
                data.defaultDuty = document.getElementById('gpio-default-duty')?.value || '0';
            }
            
            // 呼吸灯模式的 Digital Output 也需要 PWM 参数
            if (typeNum === 12 && (document.getElementById('gpio-action-mode')?.value === '2')) {
                data.pwmFrequency = document.getElementById('gpio-pwm-freq')?.value || '1000';
                data.pwmResolution = document.getElementById('gpio-pwm-resolution')?.value || '8';
            }
            
            // Digital Input 消抖
            if (typeNum === 11 || typeNum === 13 || typeNum === 14) {
                data.debounceMs = document.getElementById('gpio-debounce-ms')?.value || '50';
            }
        } else if (typeNum === 1) {
            // UART参数
            data.baudRate = document.getElementById('uart-baudrate')?.value || '115200';
            data.dataBits = document.getElementById('uart-databits')?.value || '8';
            data.stopBits = document.getElementById('uart-stopbits')?.value || '1';
            data.parity = document.getElementById('uart-parity')?.value || '0';
        } else if (typeNum === 2) {
            // I2C参数
            data.frequency = document.getElementById('i2c-frequency')?.value || '100000';
            data.address = document.getElementById('i2c-address')?.value || '0';
        } else if (typeNum === 3) {
            // SPI参数
            data.frequency = document.getElementById('spi-frequency')?.value || '1000000';
            data.mode = document.getElementById('spi-mode')?.value || '0';
        } else if (typeNum === 26) {
            // ADC参数
            data.resolution = document.getElementById('adc-resolution')?.value || '12';
            data.attenuation = document.getElementById('adc-attenuation')?.value || '3';
        } else if (typeNum === 27) {
            // DAC参数
            data.defaultValue = document.getElementById('dac-default-value')?.value || '0';
        }
        
        // 安全起见：禁用按钮防止重复提交
        const saveBtn = document.getElementById('save-peripheral-btn');
        const origText = saveBtn?.textContent;
        if (saveBtn) {
            saveBtn.disabled = true;
            saveBtn.textContent = i18n.t('peripheral-saving-text');
        }
        
        const url = isEdit ? '/api/peripherals/update' : '/api/peripherals';
        const method = apiPost;  // 统一使用 POST
        
        method(url, data)
            .then(res => {
                if (res && res.success) {
                    this.closePeripheralModal();
                    this.loadPeripherals(document.getElementById('peripheral-filter-type')?.value || '');
                    Notification.success(isEdit ? i18n.t('peripheral-update-ok') : i18n.t('peripheral-add-ok'), i18n.t('peripheral-title'));
                } else {
                    errEl.textContent = res?.error || i18n.t('peripheral-save-fail');
                    errEl.style.display = 'block';
                }
            })
            .catch(err => {
                console.error('Save peripheral failed:', err);
                errEl.textContent = i18n.t('peripheral-save-fail');
                errEl.style.display = 'block';
            })
            .finally(() => {
                if (saveBtn) {
                    saveBtn.disabled = false;
                    saveBtn.textContent = origText;
                }
            });
    },
    
    /**
     * 编辑外设
     */
    editPeripheral(id) {
        this.openPeripheralModal(true, id);
    },
    
    /**
     * 删除外设
     */
    deletePeripheral(id) {
        if (!confirm(i18n.t('peripheral-confirm-delete') + id + i18n.t('peripheral-confirm-suffix'))) return;
        
        apiDelete('/api/peripherals/?id=' + encodeURIComponent(id))
            .then(res => {
                if (res && res.success) {
                    Notification.success(i18n.t('peripheral-deleted'), i18n.t('peripheral-title'));
                    this.loadPeripherals(document.getElementById('peripheral-filter-type')?.value || '');
                } else {
                    Notification.error(res?.error || i18n.t('peripheral-delete-fail'), i18n.t('peripheral-title'));
                }
            })
            .catch(err => {
                console.error('Delete peripheral failed:', err);
                Notification.error(i18n.t('peripheral-delete-fail'), i18n.t('peripheral-title'));
            });
    },
    
    /**
     * 启用/禁用外设
     */
    togglePeripheral(id) {
        // 先获取当前状态（使用标准参数方式）
        apiGet('/api/peripherals/status', { id: id })
            .then(res => {
                if (res && res.success && res.data) {
                    const isEnabled = res.data.enabled;
                    const url = isEnabled ? '/api/peripherals/disable' : '/api/peripherals/enable';
                    
                    apiPost(url, { id: id })
                        .then(res2 => {
                            if (res2 && res2.success) {
                                Notification.success(isEnabled ? i18n.t('peripheral-disabled') : i18n.t('peripheral-enabled'), i18n.t('peripheral-title'));
                                this.loadPeripherals(document.getElementById('peripheral-filter-type')?.value || '');
                            } else {
                                Notification.error(res2?.error || i18n.t('peripheral-toggle-fail'), i18n.t('peripheral-title'));
                            }
                        })
                        .catch(err => {
                            console.error('Toggle peripheral failed:', err);
                            Notification.error(i18n.t('peripheral-toggle-fail'), i18n.t('peripheral-title'));
                        });
                }
            })
            .catch(err => {
                console.error('Get peripheral status failed:', err);
                Notification.error(i18n.t('peripheral-toggle-fail'), i18n.t('peripheral-title'));
            });
    },

    // ============ 外设执行管理 ============

    openPeriphExecModal(editId) {
        const modal = document.getElementById('periph-exec-modal');
        if (!modal) return;
        const titleEl = document.getElementById('periph-exec-modal-title');
        var safeId = (editId && editId !== 'null' && editId !== 'undefined') ? editId : '';
        document.getElementById('periph-exec-original-id').value = safeId;
        document.getElementById('periph-exec-error').style.display = 'none';
        if (safeId) {
            if (titleEl) titleEl.textContent = i18n.t('periph-exec-edit-modal-title');
        } else {
            if (titleEl) titleEl.textContent = i18n.t('periph-exec-add-modal-title');
            document.getElementById('periph-exec-form').reset();
        }
        // Clear dynamic containers
        const triggersContainer = document.getElementById('periph-exec-triggers');
        const actionsContainer = document.getElementById('periph-exec-actions');
        if (triggersContainer) triggersContainer.innerHTML = '';
        if (actionsContainer) actionsContainer.innerHTML = '';
        // For edit mode, editPeriphExecRule handles peripheral loading and block creation
        if (safeId) {
            modal.style.display = 'flex';
            return;
        }
        // Load peripherals and protocol data sources
        this._pePeripherals = [];
        this._peDataSources = [];
        // 改为串行请求，避免并发导致 ESP32 内存不足
        apiGet('/api/peripherals?pageSize=50').then(res => {
            if (res && res.success && res.data) {
                this._pePeripherals = res.data.filter(p => p.enabled);
            }
            return apiGet('/api/protocol/config');
        }).then(protoRes => {
            if (protoRes && protoRes.success && protoRes.data) {
                const protoData = protoRes.data;
                this._peDataSources = this._extractDataSources(protoData);
                // 加载 Modbus 子设备数据（供 type=6 触发器使用）
                if (protoData.modbusRtu && protoData.modbusRtu.master) {
                    this._masterTasks = protoData.modbusRtu.master.tasks || [];
                    this._modbusDevices = protoData.modbusRtu.master.devices || [];
                }
            }
            // 新建模式不需要加载规则列表，只需要创建默认触发器和动作
            this._createPeriphExecTriggerElement({}, 0);
            this._createPeriphExecActionElement({}, 0);
        }).catch(err => {
            console.error('Failed to load periph exec data:', err);
            // 即使失败也创建默认元素
            this._createPeriphExecTriggerElement({}, 0);
            this._createPeriphExecActionElement({}, 0);
        });
        modal.style.display = 'flex';
    },

    openPeriphExecModalStandalone() {
        this.openPeriphExecModal();
    },

    closePeriphExecModal() {
        const modal = document.getElementById('periph-exec-modal');
        if (modal) modal.style.display = 'none';
        this._pePeripherals = [];
        this._peDataSources = [];
    },

    // ============ 外设执行 - 动态触发/动作配置块 ============

    _pePeripherals: [],
    _peDataSources: [],
    _peCurrentPage: 1,
    _pePageSize: 10,
    _peTotalRules: 0,

    _populatePeriphSelect(selectEl, selectedValue) {
        if (!selectEl) return;
        selectEl.innerHTML = '<option value="">' + i18n.t('periph-exec-select-periph') + '</option>';
        (this._pePeripherals || []).forEach(p => {
            const opt = document.createElement('option');
            opt.value = p.id;
            opt.textContent = p.name + ' (' + p.id + ')';
            selectEl.appendChild(opt);
        });
        if (selectedValue) selectEl.value = selectedValue;
    },

    _populateTriggerPeriphSelect(selectEl, selectedValue) {
        if (!selectEl) return;
        selectEl.innerHTML = '<option value="">' + i18n.t('periph-exec-select-periph') + '</option>';
        // 数据源分组 (Modbus sensorId 等)
        if (this._peDataSources && this._peDataSources.length > 0) {
            const grp = document.createElement('optgroup');
            grp.label = i18n.t('periph-exec-datasource-group') || '数据源';
            this._peDataSources.forEach(ds => {
                const opt = document.createElement('option');
                opt.value = ds.id;
                opt.textContent = ds.label + ' (' + ds.id + ')';
                grp.appendChild(opt);
            });
            selectEl.appendChild(grp);
        }
        // 硬件外设分组
        if (this._pePeripherals && this._pePeripherals.length > 0) {
            const grp = document.createElement('optgroup');
            grp.label = i18n.t('periph-exec-periph-group') || '硬件外设';
            this._pePeripherals.forEach(p => {
                const opt = document.createElement('option');
                opt.value = p.id;
                opt.textContent = p.name + ' (' + p.id + ')';
                grp.appendChild(opt);
            });
            selectEl.appendChild(grp);
        }
        if (selectedValue) selectEl.value = selectedValue;
    },

    _extractDataSources(protocolConfig) {
        const sources = [];
        if (!protocolConfig) return sources;
        // Modbus RTU
        const rtu = protocolConfig.modbusRtu;
        if (rtu && rtu.enabled && rtu.master && rtu.master.tasks) {
            rtu.master.tasks.forEach(task => {
                if (!task.enabled || !task.mappings) return;
                task.mappings.forEach(m => {
                    if (m.sensorId) {
                        sources.push({ id: m.sensorId, label: (task.label || 'Modbus') + '/' + m.sensorId });
                    }
                });
            });
        }
        // Modbus TCP
        const tcp = protocolConfig.modbusTcp;
        if (tcp && tcp.enabled && tcp.master && tcp.master.tasks) {
            tcp.master.tasks.forEach(task => {
                if (!task.enabled || !task.mappings) return;
                task.mappings.forEach(m => {
                    if (m.sensorId) {
                        sources.push({ id: m.sensorId, label: 'TCP/' + m.sensorId });
                    }
                });
            });
        }
        return sources;
    },

    _createPeriphExecTriggerElement(data, index) {
        const container = document.getElementById('periph-exec-triggers');
        if (!container) return;
        const div = document.createElement('div');
        div.className = 'periph-exec-config-item';
        div.dataset.index = index;
        const triggerType = String(data.triggerType ?? 0);
        const showPlatform = triggerType === '0';
        const showTimer = triggerType === '1';
        const showEvent = triggerType === '4';
        const showPoll = triggerType === '5';
        const timerMode = String(data.timerMode ?? 0);
        const showInterval = timerMode === '0';
        const showDaily = timerMode === '1';
        const opVal = String(data.operatorType ?? 0);
        // 向后兼容：旧数据 operatorType >= 2 映射为 "匹配"(0)
        const mappedOp = (opVal === '0' || opVal === '1') ? opVal : '0';
        const showCompareValue = showPlatform && mappedOp !== '1';

        div.innerHTML = `
            <span class="mqtt-topic-index">${index + 1}</span>
            <button type="button" class="mqtt-topic-delete" onclick="app.deletePeriphExecTrigger(${index})">${i18n.t('peripheral-delete')}</button>
            <div class="config-form-grid">
                <div class="pure-control-group">
                    <label>${i18n.t('periph-exec-trigger-type-label')}</label>
                    <select class="pure-input-1 pe-trigger-type" onchange="app.onPeriphExecTriggerTypeChangeInBlock(this.value, ${index})">
                        <option value="0" ${triggerType === '0' ? 'selected' : ''}>${i18n.t('periph-exec-trigger-platform')}</option>
                        <option value="1" ${triggerType === '1' ? 'selected' : ''}>${i18n.t('periph-exec-trigger-timer')}</option>
                        <option value="4" ${triggerType === '4' ? 'selected' : ''}>${i18n.t('periph-exec-trigger-event')}</option>
                        <option value="5" ${triggerType === '5' ? 'selected' : ''}>${i18n.t('periph-exec-trigger-poll')}</option>
                    </select>
                </div>
                <div class="pure-control-group pe-trigger-periph-group" style="display:${(showPlatform || showPoll) ? 'block' : 'none'}">
                    <label>${i18n.t('periph-exec-trigger-periph-label')}</label>
                    <select class="pure-input-1 pe-trigger-periph">
                        <option value="">${i18n.t('periph-exec-select-periph')}</option>
                    </select>
                </div>
            </div>
            <div class="pe-platform-condition" style="display:${showPlatform ? 'block' : 'none'}">
                <div class="config-form-grid">
                    <div class="pure-control-group">
                        <label>${i18n.t('periph-exec-operator-label')}</label>
                        <select class="pure-input-1 pe-operator" onchange="app.onPeriphExecOperatorChangeInBlock(this.value, ${index})">
                            <option value="0" ${mappedOp === '0' ? 'selected' : ''}>${i18n.t('periph-exec-handle-match')}</option>
                            <option value="1" ${mappedOp === '1' ? 'selected' : ''}>${i18n.t('periph-exec-handle-set')}</option>
                        </select>
                    </div>
                    <div class="pure-control-group pe-compare-value-group" style="display:${showCompareValue ? 'block' : 'none'}">
                        <label>${i18n.t('periph-exec-compare-label')}</label>
                        <input type="text" class="pure-input-1 pe-compare-value" value="${escapeHtml(data.compareValue)}" placeholder="${escapeHtml(i18n.t('periph-exec-compare-hint'))}">
                    </div>
                </div>
            </div>
            <div class="pe-poll-params" style="display:${showPoll ? 'block' : 'none'}">
                <div class="config-form-grid">
                    <div class="pure-control-group">
                        <label>${i18n.t('periph-exec-poll-interval-label')}</label>
                        <input type="number" class="pure-input-1 pe-poll-interval" value="${data.intervalSec || 60}" min="5" max="86400">
                    </div>
                    <div class="pure-control-group">
                        <label>${i18n.t('periph-exec-poll-timeout-label')}</label>
                        <input type="number" class="pure-input-1 pe-poll-timeout" value="${data.pollResponseTimeout || 1000}" min="100" max="30000">
                    </div>
                    <div class="pure-control-group">
                        <label>${i18n.t('periph-exec-poll-retries-label')}</label>
                        <input type="number" class="pure-input-1 pe-poll-retries" value="${data.pollMaxRetries ?? 2}" min="0" max="10">
                    </div>
                    <div class="pure-control-group">
                        <label>${i18n.t('periph-exec-poll-delay-label')}</label>
                        <input type="number" class="pure-input-1 pe-poll-inter-delay" value="${data.pollInterPollDelay || 100}" min="10" max="5000">
                    </div>
                </div>
            </div>
            <div class="pe-timer-config" style="display:${showTimer ? 'block' : 'none'}">
                <div class="config-form-grid">
                    <div class="pure-control-group">
                        <label>${i18n.t('periph-exec-timer-mode-label')}</label>
                        <select class="pure-input-1 pe-timer-mode" onchange="app.onPeriphExecTimerModeChangeInBlock(this.value, ${index})">
                            <option value="0" ${timerMode === '0' ? 'selected' : ''}>${i18n.t('periph-exec-timer-interval')}</option>
                            <option value="1" ${timerMode === '1' ? 'selected' : ''}>${i18n.t('periph-exec-timer-daily')}</option>
                        </select>
                    </div>
                    <div class="pure-control-group pe-interval-fields" style="display:${showInterval ? 'block' : 'none'}">
                        <label>${i18n.t('periph-exec-interval-label')}</label>
                        <input type="number" class="pure-input-1 pe-interval" value="${data.intervalSec || 60}" min="1" max="86400">
                    </div>
                    <div class="pure-control-group pe-daily-fields" style="display:${showDaily ? 'block' : 'none'}">
                        <label>${i18n.t('periph-exec-timepoint-label')}</label>
                        <input type="time" class="pure-input-1 pe-timepoint" value="${data.timePoint || '08:00'}">
                    </div>
                </div>
            </div>
            <div class="pe-event-group" style="display:${showEvent ? 'block' : 'none'}">
                <div class="config-form-grid">
                    <div class="pure-control-group">
                        <label>${i18n.t('periph-exec-event-category-label')}</label>
                        <select class="pure-input-1 pe-event-category" onchange="app.onPeriphExecEventCategoryChangeInBlock(this.value, ${index})">
                            <option value="">${i18n.t('periph-exec-select-category')}</option>
                        </select>
                    </div>
                    <div class="pure-control-group">
                        <label>${i18n.t('periph-exec-event-label')}</label>
                        <select class="pure-input-1 pe-event">
                            <option value="">${i18n.t('periph-exec-select-event')}</option>
                        </select>
                    </div>
                </div>
            </div>
        `;

        container.appendChild(div);
        // Populate trigger peripheral select with optgroups
        this._populateTriggerPeriphSelect(div.querySelector('.pe-trigger-periph'), data.triggerPeriphId || data.sourcePeriphId || '');
        // If event trigger, populate event categories
        if (showEvent) {
            this._populateEventCategoriesInBlock(div, data.eventId);
        }
    },

    _createPeriphExecActionElement(data, index) {
        const container = document.getElementById('periph-exec-actions');
        if (!container) return;
        const div = document.createElement('div');
        div.className = 'periph-exec-config-item';
        div.dataset.index = index;
        const actionType = String(data.actionType ?? 0);
        const actionTypeInt = parseInt(actionType);
        const isModbusPoll = actionTypeInt === 18;
        const isSensorRead = actionTypeInt === 19;
        const showPeriphGroup = !((actionTypeInt >= 6 && actionTypeInt <= 11) || actionTypeInt === 15 || isModbusPoll);
        const needsValue = (actionTypeInt >= 2 && actionTypeInt <= 5);
        const isScript = actionTypeInt === 15;
        const sel = (v) => actionType === String(v) ? 'selected' : '';

        // Parse sensor read config from actionValue for edit mode
        var sensorCfg = {sensorCategory:'analog',periphId:'',scaleFactor:1,offset:0,decimalPlaces:2,sensorLabel:'',unit:''};
        if (isSensorRead && data.actionValue) {
            try { Object.assign(sensorCfg, JSON.parse(data.actionValue)); } catch(e) {}
        }
        const selCat = (v) => sensorCfg.sensorCategory === v ? 'selected' : '';

        const curExecMode = document.getElementById('periph-exec-exec-mode')?.value || '0';
        const selMode = (v) => curExecMode === String(v) ? 'selected' : '';

        div.innerHTML = `
            <span class="mqtt-topic-index">${index + 1}</span>
            <button type="button" class="mqtt-topic-delete" onclick="app.deletePeriphExecAction(${index})">${i18n.t('peripheral-delete')}</button>
            <div class="pe-action-grid">
                <div class="pure-control-group pe-action-type-group">
                    <label>${i18n.t('periph-exec-action-type-label')}</label>
                    <select class="pure-input-1 pe-action-type" onchange="app.onPeriphExecActionTypeChangeInBlock(this.value, ${index})">
                        <optgroup label="${i18n.t('periph-exec-action-cat-gpio')}">
                            <option value="0" ${sel(0)}>${i18n.t('periph-exec-action-high')}</option>
                            <option value="1" ${sel(1)}>${i18n.t('periph-exec-action-low')}</option>
                            <option value="2" ${sel(2)}>${i18n.t('periph-exec-action-blink')}</option>
                            <option value="3" ${sel(3)}>${i18n.t('periph-exec-action-breathe')}</option>
                        </optgroup>
                        <optgroup label="${i18n.t('periph-exec-action-cat-analog')}">
                            <option value="4" ${sel(4)}>${i18n.t('periph-exec-action-pwm')}</option>
                            <option value="5" ${sel(5)}>${i18n.t('periph-exec-action-dac')}</option>
                        </optgroup>
                        <optgroup label="${i18n.t('periph-exec-action-cat-system')}">
                            <option value="6" ${sel(6)}>${i18n.t('periph-exec-action-restart')}</option>
                            <option value="7" ${sel(7)}>${i18n.t('periph-exec-action-factory')}</option>
                            <option value="8" ${sel(8)}>${i18n.t('periph-exec-action-ntp')}</option>
                            <option value="9" ${sel(9)}>${i18n.t('periph-exec-action-ota')}</option>
                            <option value="10" ${sel(10)}>${i18n.t('periph-exec-action-ap')}</option>
                            <option value="11" ${sel(11)}>${i18n.t('periph-exec-action-ble')}</option>
                        </optgroup>
                        <optgroup label="${i18n.t('periph-exec-action-cat-advanced')}">
                            <option value="15" ${sel(15)}>${i18n.t('periph-exec-action-script')}</option>
                            <option value="18" ${sel(18)}>${i18n.t('periph-exec-action-modbus-poll')}</option>
                            <option value="19" ${sel(19)}>${i18n.t('periph-exec-action-sensor-read')}</option>
                        </optgroup>
                    </select>
                </div>
                <div class="pure-control-group pe-target-group" style="display:${showPeriphGroup ? 'block' : 'none'}">
                    <label>${i18n.t('periph-exec-target-periph-label')}</label>
                    <select class="pure-input-1 pe-target-periph">
                        <option value="">${i18n.t('periph-exec-select-periph')}</option>
                    </select>
                </div>
                <div class="pure-control-group pe-action-value-group" style="display:${needsValue ? 'block' : 'none'}; grid-column: 1 / -1;">
                    <label>${i18n.t('periph-exec-action-value-label')}</label>
                    <input type="text" class="pure-input-1 pe-action-value" value="${isScript ? '' : escapeHtml(data.actionValue)}" placeholder="${escapeHtml(i18n.t('periph-exec-action-value-hint'))}" ${data.useReceivedValue ? 'readonly' : ''}>
                    <small style="color: #999;">${i18n.t('periph-exec-action-value-help')}</small>
                </div>
                <div class="pure-control-group pe-use-received-value-group">
                    <label class="pe-checkbox-label">
                        <input type="checkbox" class="pe-use-received-value" ${data.useReceivedValue ? 'checked' : ''} onchange="app.onPeriphExecUseRecvChangeInBlock(this, ${index})">
                        <span>${i18n.t('periph-exec-use-received-value')}</span>
                    </label>
                    <small style="color: #999;">${i18n.t('periph-exec-use-received-value-help')}</small>
                </div>
                <div class="pure-control-group pe-sync-delay-group">
                    <label>${i18n.t('periph-exec-sync-delay-label')}</label>
                    <input type="number" class="pure-input-1 pe-sync-delay" min="0" max="10000" step="100" value="${data.syncDelayMs || 0}" placeholder="0">
                    <small style="color: #999;">${i18n.t('periph-exec-sync-delay-help')}</small>
                </div>
                <div class="pure-control-group pe-script-group" style="display:${isScript ? 'block' : 'none'}; grid-column: 1 / -1;">
                    <label>${i18n.t('periph-exec-script-label')}</label>
                    <textarea class="pure-input-1 pe-script" rows="6" style="font-family: monospace; font-size: 13px; resize: vertical;" maxlength="1024" placeholder="${escapeHtml(i18n.t('periph-exec-script-placeholder'))}">${isScript ? escapeHtml(data.actionValue) : ''}</textarea>
                    <small style="color: #999;">${i18n.t('periph-exec-script-help')}</small>
                </div>
                <div class="pure-control-group pe-poll-tasks-group" style="display:${isModbusPoll ? 'block' : 'none'}; grid-column: 1 / -1;">
                    <label>${i18n.t('periph-exec-poll-tasks-label')}</label>
                    <div class="pe-poll-tasks-list" style="border:1px solid #ddd;border-radius:4px;padding:8px;max-height:200px;overflow-y:auto;background:#fafafa;">
                    </div>
                    <small style="color: #999;">${i18n.t('periph-exec-poll-tasks-help')}</small>
                </div>
                <div class="pure-control-group pe-sensor-group" style="display:${isSensorRead ? 'block' : 'none'}; grid-column: 1 / -1;">
                    <div class="pe-sensor-config-grid">
                        <div class="pure-control-group">
                            <label>${i18n.t('periph-exec-sensor-category')}</label>
                            <select class="pure-input-1 pe-sensor-category" onchange="app.onSensorCategoryChange(this, ${index})">
                                <option value="analog" ${selCat('analog')}>${i18n.t('periph-exec-sensor-cat-analog')}</option>
                                <option value="digital" ${selCat('digital')}>${i18n.t('periph-exec-sensor-cat-digital')}</option>
                                <option value="pulse" ${selCat('pulse')}>${i18n.t('periph-exec-sensor-cat-pulse')}</option>
                            </select>
                        </div>
                        <div class="pure-control-group">
                            <label>${i18n.t('periph-exec-sensor-scale')}</label>
                            <input type="number" class="pure-input-1 pe-sensor-scale" step="any" value="${sensorCfg.scaleFactor}">
                        </div>
                        <div class="pure-control-group">
                            <label>${i18n.t('periph-exec-sensor-offset')}</label>
                            <input type="number" class="pure-input-1 pe-sensor-offset" step="any" value="${sensorCfg.offset}">
                        </div>
                        <div class="pure-control-group">
                            <label>${i18n.t('periph-exec-sensor-decimals')}</label>
                            <input type="number" class="pure-input-1 pe-sensor-decimals" min="0" max="6" value="${sensorCfg.decimalPlaces}">
                        </div>
                        <div class="pure-control-group">
                            <label>${i18n.t('periph-exec-sensor-label')}</label>
                            <input type="text" class="pure-input-1 pe-sensor-label" value="${escapeHtml(sensorCfg.sensorLabel)}" placeholder="${i18n.t('periph-exec-sensor-label')}">
                        </div>
                        <div class="pure-control-group">
                            <label>${i18n.t('periph-exec-sensor-unit')}</label>
                            <input type="text" class="pure-input-1 pe-sensor-unit" value="${escapeHtml(sensorCfg.unit)}" maxlength="8" placeholder="°C, %, V...">
                        </div>
                    </div>
                </div>
            </div>
        `;

        container.appendChild(div);
        // Populate target peripheral select
        // Populate target peripheral select
        if (isSensorRead) {
            // For sensor read, filter peripherals by category
            this._populateSensorPeriphSelect(div, sensorCfg.sensorCategory || 'analog', sensorCfg.periphId || data.targetPeriphId || '');
        } else {
            this._populatePeriphSelect(div.querySelector('.pe-target-periph'), data.targetPeriphId || '');
        }
        if (isModbusPoll) {
            this._populateModbusDevicePanel(div.querySelector('.pe-poll-tasks-list'), data.actionValue || '');
        }
    },

    _populateSensorPeriphSelect(blockEl, category, selectedValue) {
        var sel = blockEl.querySelector('.pe-target-periph');
        if (!sel) return;
        var periphs = this._pePeripherals || [];
        var analogTypes = [15, 26];
        var digitalTypes = [11, 13, 14];
        var pulseTypes = [46];
        var allowedTypes;
        if (category === 'digital') allowedTypes = digitalTypes;
        else if (category === 'pulse') allowedTypes = pulseTypes;
        else allowedTypes = analogTypes;
        var prev = selectedValue || sel.value;
        sel.innerHTML = '<option value="">--</option>';
        periphs.filter(function(p) { return allowedTypes.indexOf(p.type) >= 0; }).forEach(function(p) {
            var opt = document.createElement('option');
            opt.value = p.id;
            var pinInfo = (p.pins && p.pins.length > 0) ? (' (Pin ' + p.pins[0] + ')') : '';
            opt.textContent = p.name + pinInfo;
            sel.appendChild(opt);
        });
        if (prev) sel.value = prev;
    },

    onSensorCategoryChange(selectEl, index) {
        var container = document.getElementById('periph-exec-actions');
        if (!container) return;
        var block = container.querySelectorAll('.periph-exec-config-item')[index];
        if (!block) return;
        this._populateSensorPeriphSelect(block, selectEl.value);
    },

    _populateModbusDevicePanel(container, actionValue) {
        if (!container) return;
        var tasks = this._masterTasks || [];
        var devices = this._modbusDevices || [];
        
        if (tasks.length === 0 && devices.length === 0) {
            container.innerHTML = '<span style="color:#999;font-size:12px;">' + (i18n.t('modbus-no-devices') || '暂无子设备') + '</span>';
            return;
        }
        
        // 解析 actionValue: JSON 格式或旧逗号分隔格式
        var selDevice = '';
        var selChannel = '';
        var selAction = '';
        var selValue = '';
        var selParam = '';
        if (actionValue) {
            if (actionValue.charAt(0) === '{') {
                try {
                    var parsed = JSON.parse(actionValue);
                    if (parsed.poll && parsed.poll.length > 0) {
                        selDevice = 'sensor-' + parsed.poll[0];
                    } else if (parsed.ctrl && parsed.ctrl.length > 0) {
                        var c = parsed.ctrl[0];
                        selDevice = 'control-' + c.d;
                        selChannel = String(c.c || 0);
                        selAction = c.a || '';
                        selValue = String(c.v || 0);
                        selParam = c.p || '';
                    }
                } catch(e) {}
            } else {
                // 旧格式: 逗号分隔任务索引，取第一个
                var parts = actionValue.split(',');
                if (parts.length > 0 && parts[0].trim()) selDevice = 'sensor-' + parts[0].trim();
            }
        }
        
        var fcNames = {1: 'FC01', 2: 'FC02', 3: 'FC03', 4: 'FC04'};
        var typeLabels = {relay: i18n.t('modbus-type-relay') || '继电器', pwm: i18n.t('modbus-type-pwm') || 'PWM', pid: i18n.t('modbus-type-pid') || 'PID'};
        
        // 设备选择下拉框
        var html = '<div class="pe-modbus-select-flow">';
        html += '<div class="pure-control-group" style="margin-bottom:8px;">';
        html += '<label style="font-size:12px;font-weight:bold;">' + (i18n.t('periph-exec-select-device') || '选择子设备') + '</label>';
        html += '<select class="pure-input-1 pe-modbus-device-select" onchange="AppState._onPeriphDeviceSelect(this)" style="font-size:13px;">';
        html += '<option value="">--</option>';
        
        // 采集设备组
        if (tasks.length > 0) {
            html += '<optgroup label="' + (i18n.t('modbus-type-sensor') || '采集设备') + '">';
            for (var i = 0; i < tasks.length; i++) {
                var t = tasks[i];
                var label = t.label || ('Slave ' + (t.slaveAddress || 1));
                var desc = (fcNames[t.functionCode] || 'FC03') + ' @' + (t.startAddress || 0) + ' x' + (t.quantity || 10);
                var val = 'sensor-' + i;
                html += '<option value="' + val + '"' + (val === selDevice ? ' selected' : '') + '>' + escapeHtml(label) + ' (' + desc + ')</option>';
            }
            html += '</optgroup>';
        }
        
        // 控制设备组
        if (devices.length > 0) {
            html += '<optgroup label="' + (i18n.t('modbus-type-control') || '控制设备') + '">';
            for (var j = 0; j < devices.length; j++) {
                var d = devices[j];
                var dt = d.deviceType || 'relay';
                var devName = d.name || ('Device ' + (j + 1));
                var tl = typeLabels[dt] || dt;
                var val2 = 'control-' + j;
                html += '<option value="' + val2 + '"' + (val2 === selDevice ? ' selected' : '') + '>' + escapeHtml(devName) + ' (' + tl + ', Addr ' + (d.slaveAddress || 1) + ')</option>';
            }
            html += '</optgroup>';
        }
        html += '</select></div>';
        
        // 通道选择（仅控制设备时显示）
        var isCtrl = selDevice.indexOf('control-') === 0;
        var chDisplay = isCtrl ? 'block' : 'none';
        html += '<div class="pure-control-group pe-modbus-channel-group" style="display:' + chDisplay + ';margin-bottom:8px;">';
        html += '<label style="font-size:12px;font-weight:bold;">' + (i18n.t('periph-exec-select-channel') || '选择通道') + '</label>';
        html += '<select class="pure-input-1 pe-modbus-channel-select" onchange="AppState._onPeriphChannelSelect(this)" style="font-size:13px;">';
        if (isCtrl) {
            var cidx = parseInt(selDevice.split('-')[1]);
            var cdev = devices[cidx];
            if (cdev) {
                for (var ch = 0; ch < (cdev.channelCount || 2); ch++) {
                    html += '<option value="' + ch + '"' + (String(ch) === selChannel ? ' selected' : '') + '>CH' + ch + '</option>';
                }
            }
        }
        html += '</select></div>';
        
        // 动作选择（仅控制设备+已选通道时显示）
        var actDisplay = isCtrl ? 'block' : 'none';
        html += '<div class="pure-control-group pe-modbus-action-group" style="display:' + actDisplay + ';margin-bottom:4px;">';
        html += '<label style="font-size:12px;font-weight:bold;">' + (i18n.t('periph-exec-select-action') || '子设备动作') + '</label>';
        html += '<div class="pe-modbus-action-content">';
        if (isCtrl) {
            var aidx = parseInt(selDevice.split('-')[1]);
            var adev = devices[aidx];
            if (adev) html += this._buildPeriphActionUI(adev, selAction, selChannel, selValue, selParam);
        }
        html += '</div></div>';
        
        html += '</div>';
        container.innerHTML = html;
    },
    
    /** 构建动作选择 UI（根据设备类型） */
    _buildPeriphActionUI(dev, action, channel, value, param) {
        var dt = dev.deviceType || 'relay';
        var html = '<div style="display:flex;gap:8px;align-items:center;flex-wrap:wrap;">';
        if (dt === 'relay') {
            html += '<select class="pure-input-1 pe-modbus-action-select" style="max-width:120px;font-size:13px;">' +
                '<option value="on"' + (action === 'off' ? '' : ' selected') + '>' + (i18n.t('periph-exec-ctrl-on') || '打开') + '</option>' +
                '<option value="off"' + (action === 'off' ? ' selected' : '') + '>' + (i18n.t('periph-exec-ctrl-off') || '关闭') + '</option>' +
            '</select>';
        } else if (dt === 'pwm') {
            var maxPwm = dev.pwmResolution === 10 ? 1023 : (dev.pwmResolution === 16 ? 65535 : 255);
            html += '<label style="font-size:12px;">' + (i18n.t('periph-exec-ctrl-duty') || '占空比') + ': ';
            html += '<input type="number" class="pe-modbus-action-value" min="0" max="' + maxPwm + '" value="' + (value || 0) + '" style="width:80px;font-size:12px;padding:2px 4px;">';
            html += '</label>';
        } else if (dt === 'pid') {
            html += '<label style="font-size:12px;">' + (i18n.t('periph-exec-ctrl-pid-param') || '参数') + ': ';
            html += '<select class="pe-modbus-action-param" style="font-size:12px;padding:2px 4px;">' +
                '<option value="P"' + (param === 'I' || param === 'D' ? '' : ' selected') + '>P</option>' +
                '<option value="I"' + (param === 'I' ? ' selected' : '') + '>I</option>' +
                '<option value="D"' + (param === 'D' ? ' selected' : '') + '>D</option>' +
            '</select></label>';
            html += '<label style="font-size:12px;">' + (i18n.t('periph-exec-ctrl-value') || '值') + ': ';
            html += '<input type="number" class="pe-modbus-action-value" value="' + (value || 0) + '" style="width:80px;font-size:12px;padding:2px 4px;">';
            html += '</label>';
        }
        html += '</div>';
        return html;
    },
    
    /** 设备下拉框变化 */
    _onPeriphDeviceSelect(selectEl) {
        var flow = selectEl.closest('.pe-modbus-select-flow');
        if (!flow) return;
        var val = selectEl.value;
        var chGroup = flow.querySelector('.pe-modbus-channel-group');
        var actGroup = flow.querySelector('.pe-modbus-action-group');
        
        if (!val || val.indexOf('sensor-') === 0) {
            // sensor 或空选择：隐藏通道和动作
            if (chGroup) chGroup.style.display = 'none';
            if (actGroup) actGroup.style.display = 'none';
            return;
        }
        
        // 控制设备：填充通道下拉框
        var idx = parseInt(val.split('-')[1]);
        var devices = this._modbusDevices || [];
        var dev = devices[idx];
        if (!dev) return;
        
        var chSelect = flow.querySelector('.pe-modbus-channel-select');
        if (chSelect) {
            var chHtml = '';
            for (var ch = 0; ch < (dev.channelCount || 2); ch++) {
                chHtml += '<option value="' + ch + '">CH' + ch + '</option>';
            }
            chSelect.innerHTML = chHtml;
        }
        if (chGroup) chGroup.style.display = 'block';
        
        // 填充动作
        var actContent = flow.querySelector('.pe-modbus-action-content');
        if (actContent) actContent.innerHTML = this._buildPeriphActionUI(dev, '', '', '', '');
        if (actGroup) actGroup.style.display = 'block';
    },
    
    /** 通道下拉框变化 */
    _onPeriphChannelSelect(selectEl) {
        // 通道变化时无需额外操作，动作UI已显示
    },


    
    _onCtrlDeviceToggle(checkbox) {
        var panel = checkbox.closest('.pe-ctrl-device-item').querySelector('.pe-ctrl-options');
        if (panel) panel.style.display = checkbox.checked ? 'block' : 'none';
    },

    addPeriphExecTrigger() {
        const container = document.getElementById('periph-exec-triggers');
        if (!container) return;
        const index = container.children.length;
        this._createPeriphExecTriggerElement({}, index);
    },

    deletePeriphExecTrigger(index) {
        const container = document.getElementById('periph-exec-triggers');
        if (!container) return;
        const items = container.querySelectorAll('.periph-exec-config-item');
        if (items.length <= 1) return; // Keep at least one trigger
        if (items[index]) items[index].remove();
        this._reindexPeriphExecBlocks(container, 'Trigger');
    },

    addPeriphExecAction() {
        const container = document.getElementById('periph-exec-actions');
        if (!container) return;
        const index = container.children.length;
        // 如果当前有"设置"模式触发，新动作块自动继承该状态
        const hasSet = this._hasSetModeTrigger();
        this._createPeriphExecActionElement(hasSet ? { useReceivedValue: true } : {}, index);
    },

    deletePeriphExecAction(index) {
        const container = document.getElementById('periph-exec-actions');
        if (!container) return;
        const items = container.querySelectorAll('.periph-exec-config-item');
        if (items.length <= 1) return; // Keep at least one action
        if (items[index]) items[index].remove();
        this._reindexPeriphExecBlocks(container, 'Action');
    },

    _reindexPeriphExecBlocks(container, type) {
        const items = container.querySelectorAll('.periph-exec-config-item');
        const fnPrefix = type === 'Trigger' ? 'deletePeriphExecTrigger' : 'deletePeriphExecAction';
        items.forEach((item, idx) => {
            item.dataset.index = idx;
            const indexSpan = item.querySelector('.mqtt-topic-index');
            if (indexSpan) indexSpan.textContent = idx + 1;
            const deleteBtn = item.querySelector('.mqtt-topic-delete');
            if (deleteBtn) deleteBtn.setAttribute('onclick', `app.${fnPrefix}(${idx})`);
            // Update onchange handlers with new index
            if (type === 'Trigger') {
                const triggerTypeSel = item.querySelector('.pe-trigger-type');
                if (triggerTypeSel) triggerTypeSel.setAttribute('onchange', `app.onPeriphExecTriggerTypeChangeInBlock(this.value, ${idx})`);
                const timerModeSel = item.querySelector('.pe-timer-mode');
                if (timerModeSel) timerModeSel.setAttribute('onchange', `app.onPeriphExecTimerModeChangeInBlock(this.value, ${idx})`);
                const eventCatSel = item.querySelector('.pe-event-category');
                if (eventCatSel) eventCatSel.setAttribute('onchange', `app.onPeriphExecEventCategoryChangeInBlock(this.value, ${idx})`);
                const operatorSel = item.querySelector('.pe-operator');
                if (operatorSel) operatorSel.setAttribute('onchange', `app.onPeriphExecOperatorChangeInBlock(this.value, ${idx})`);
            } else {
                const actionTypeSel = item.querySelector('.pe-action-type');
                if (actionTypeSel) actionTypeSel.setAttribute('onchange', `app.onPeriphExecActionTypeChangeInBlock(this.value, ${idx})`);
                const useRecvCb = item.querySelector('.pe-use-received-value');
                if (useRecvCb) useRecvCb.setAttribute('onchange', `app.onPeriphExecUseRecvChangeInBlock(this, ${idx})`);
            }
        });
    },

    onPeriphExecTriggerTypeChangeInBlock(val, index) {
        const container = document.getElementById('periph-exec-triggers');
        if (!container) return;
        const block = container.querySelectorAll('.periph-exec-config-item')[index];
        if (!block) return;
        const triggerPeriphGroup = block.querySelector('.pe-trigger-periph-group');
        const platformCondition = block.querySelector('.pe-platform-condition');
        const pollParams = block.querySelector('.pe-poll-params');
        const timerConfig = block.querySelector('.pe-timer-config');
        const eventGroup = block.querySelector('.pe-event-group');
        if (triggerPeriphGroup) triggerPeriphGroup.style.display = (val === '0' || val === '5') ? 'block' : 'none';
        if (platformCondition) platformCondition.style.display = (val === '0') ? 'block' : 'none';
        if (pollParams) pollParams.style.display = (val === '5') ? 'block' : 'none';
        if (timerConfig) timerConfig.style.display = (val === '1') ? 'block' : 'none';
        if (eventGroup) {
            eventGroup.style.display = (val === '4') ? 'block' : 'none';
            if (val === '4') {
                this._populateEventCategoriesInBlock(block);
                this._populateEventSelectInBlock(block);
            }
        }
        if (val === '5') {
            this._populateTriggerPeriphSelect(block.querySelector('.pe-trigger-periph'), '');
        }
        // 切换离开平台触发时，检查是否还有其他平台触发处于"设置"模式
        if (val !== '0') {
            this._checkAndSyncSetMode();
        }
    },

    // 处理方式（匹配/设置）切换
    onPeriphExecOperatorChangeInBlock(val, index) {
        const container = document.getElementById('periph-exec-triggers');
        if (!container) return;
        const block = container.querySelectorAll('.periph-exec-config-item')[index];
        if (!block) return;
        // 显示/隐藏匹配值字段
        const compareGroup = block.querySelector('.pe-compare-value-group');
        if (compareGroup) compareGroup.style.display = (val === '1') ? 'none' : 'block';
        // 同步设置模式到所有动作块
        this._syncSetModeToActions(val === '1');
    },

    // "使用接收值"复选框手动切换
    onPeriphExecUseRecvChangeInBlock(checkbox, index) {
        const block = checkbox.closest('.periph-exec-config-item');
        if (!block) return;
        const valueInput = block.querySelector('.pe-action-value');
        if (valueInput) {
            if (checkbox.checked) {
                valueInput.setAttribute('readonly', '');
            } else {
                valueInput.removeAttribute('readonly');
            }
        }
    },

    // 检查所有触发块中是否有"设置"模式的平台触发，并同步到动作块
    _checkAndSyncSetMode() {
        const hasSetMode = this._hasSetModeTrigger();
        this._syncSetModeToActions(hasSetMode);
    },

    // 检测是否有任意平台触发处于"设置"模式
    _hasSetModeTrigger() {
        const container = document.getElementById('periph-exec-triggers');
        if (!container) return false;
        const items = container.querySelectorAll('.periph-exec-config-item');
        for (let i = 0; i < items.length; i++) {
            const triggerType = items[i].querySelector('.pe-trigger-type');
            const operator = items[i].querySelector('.pe-operator');
            if (triggerType && triggerType.value === '0' && operator && operator.value === '1') {
                return true;
            }
        }
        return false;
    },

    // 同步"设置"模式状态到所有动作块
    _syncSetModeToActions(isSetMode) {
        const container = document.getElementById('periph-exec-actions');
        if (!container) return;
        container.querySelectorAll('.periph-exec-config-item').forEach(function (item) {
            const checkbox = item.querySelector('.pe-use-received-value');
            const valueInput = item.querySelector('.pe-action-value');
            if (checkbox) checkbox.checked = isSetMode;
            if (valueInput) {
                if (isSetMode) {
                    valueInput.setAttribute('readonly', '');
                } else {
                    valueInput.removeAttribute('readonly');
                }
            }
        });
    },

    onPeriphExecTimerModeChangeInBlock(val, index) {
        const container = document.getElementById('periph-exec-triggers');
        if (!container) return;
        const block = container.querySelectorAll('.periph-exec-config-item')[index];
        if (!block) return;
        const intervalFields = block.querySelector('.pe-interval-fields');
        const dailyFields = block.querySelector('.pe-daily-fields');
        if (intervalFields) intervalFields.style.display = (val === '0') ? 'block' : 'none';
        if (dailyFields) dailyFields.style.display = (val === '1') ? 'block' : 'none';
    },

    onPeriphExecEventCategoryChangeInBlock(val, index) {
        const container = document.getElementById('periph-exec-triggers');
        if (!container) return;
        const block = container.querySelectorAll('.periph-exec-config-item')[index];
        if (!block) return;
        this._populateEventSelectInBlock(block, val);
    },

    onPeriphExecActionTypeChangeInBlock(val, index) {
        const container = document.getElementById('periph-exec-actions');
        if (!container) return;
        const block = container.querySelectorAll('.periph-exec-config-item')[index];
        if (!block) return;
        const actionType = parseInt(val);
        const targetGroup = block.querySelector('.pe-target-group');
        const valueGroup = block.querySelector('.pe-action-value-group');
        const scriptGroup = block.querySelector('.pe-script-group');
        const pollTasksGroup = block.querySelector('.pe-poll-tasks-group');
        const isModbusPoll = actionType === 18;
        const isSensorRead = actionType === 19;
        if (targetGroup) targetGroup.style.display = ((actionType >= 6 && actionType <= 11) || actionType === 15 || isModbusPoll) ? 'none' : 'block';
        const needsValue = (actionType >= 2 && actionType <= 5);
        if (valueGroup) valueGroup.style.display = needsValue ? 'block' : 'none';
        if (scriptGroup) scriptGroup.style.display = actionType === 15 ? 'block' : 'none';
        if (pollTasksGroup) {
            pollTasksGroup.style.display = isModbusPoll ? 'block' : 'none';
            if (isModbusPoll) {
                this._populateModbusDevicePanel(block.querySelector('.pe-poll-tasks-list'), '');
            }
        }
        const sensorGroup = block.querySelector('.pe-sensor-group');
        if (sensorGroup) {
            sensorGroup.style.display = isSensorRead ? 'block' : 'none';
        }
        // For sensor read: filter peripherals by category; for others: restore full list
        if (isSensorRead) {
            var cat = block.querySelector('.pe-sensor-category')?.value || 'analog';
            this._populateSensorPeriphSelect(block, cat);
        } else if (targetGroup && targetGroup.style.display !== 'none') {
            this._populatePeriphSelect(block.querySelector('.pe-target-periph'), '');
        }
    },

    onPeriphExecModeChangeInBlock(val) {
        // Sync hidden input
        const hidden = document.getElementById('periph-exec-exec-mode');
        if (hidden) hidden.value = val;
        // Sync all blocks
        const container = document.getElementById('periph-exec-actions');
        if (!container) return;
        container.querySelectorAll('.pe-exec-mode').forEach(s => { s.value = val; });
    },

    _populateEventCategoriesInBlock(blockEl, eventIdToSet) {
        const sel = blockEl.querySelector('.pe-event-category');
        if (!sel) return;
        apiGet('/api/periph-exec/events/categories').then(res => {
            if (!res || !res.success || !res.data) return;
            let opts = '<option value="">' + i18n.t('periph-exec-select-category') + '</option>';
            res.data.forEach(cat => {
                const translatedCat = i18n.t('event-cat-' + cat.name) || cat.name;
                opts += '<option value="' + cat.name + '">' + translatedCat + '</option>';
            });
            sel.innerHTML = opts;
            if (eventIdToSet) {
                this._populateEventSelectInBlock(blockEl, '', eventIdToSet);
            }
        });
    },

    _populateEventSelectInBlock(blockEl, categoryFilter, eventIdToSet) {
        const sel = blockEl.querySelector('.pe-event');
        if (!sel) return;
        Promise.all([
            apiGet('/api/periph-exec/events/static'),
            apiGet('/api/periph-exec/events/dynamic')
        ]).then(([staticRes, dynamicRes]) => {
            let allEvents = [];
            if (staticRes && staticRes.success && staticRes.data) allEvents = allEvents.concat(staticRes.data);
            if (dynamicRes && dynamicRes.success && dynamicRes.data) allEvents = allEvents.concat(dynamicRes.data);
            if (categoryFilter) allEvents = allEvents.filter(e => e.category === categoryFilter);
            const categories = {};
            allEvents.forEach(e => {
                if (!categories[e.category]) categories[e.category] = [];
                categories[e.category].push(e);
            });
            let opts = '<option value="">' + i18n.t('periph-exec-select-event') + '</option>';
            for (const cat in categories) {
                const translatedCat = i18n.t('event-cat-' + cat) || cat;
                opts += '<optgroup label="' + translatedCat + '">';
                categories[cat].forEach(e => {
                    const translatedName = e.isDynamic ? e.name : (i18n.t('event-' + e.id) || e.name);
                    opts += '<option value="' + e.id + '">' + translatedName + '</option>';
                });
                opts += '</optgroup>';
            }
            sel.innerHTML = opts;
            if (eventIdToSet) sel.value = eventIdToSet;
        });
    },

    _collectPeriphExecTriggers() {
        const container = document.getElementById('periph-exec-triggers');
        if (!container) return [];
        const triggers = [];
        container.querySelectorAll('.periph-exec-config-item').forEach(item => {
            const triggerType = item.querySelector('.pe-trigger-type')?.value || '0';
            const trigger = { triggerType: parseInt(triggerType, 10) || 0 };
            if (triggerType === '0') {
                trigger.triggerPeriphId = item.querySelector('.pe-trigger-periph')?.value || '';
                trigger.operatorType = parseInt(item.querySelector('.pe-operator')?.value || '0', 10);
                trigger.compareValue = item.querySelector('.pe-compare-value')?.value?.trim() || '';
            } else if (triggerType === '1') {
                trigger.timerMode = parseInt(item.querySelector('.pe-timer-mode')?.value || '0', 10);
                trigger.intervalSec = parseInt(item.querySelector('.pe-interval')?.value || '60', 10);
                trigger.timePoint = item.querySelector('.pe-timepoint')?.value || '08:00';
            } else if (triggerType === '4') {
                trigger.eventId = item.querySelector('.pe-event')?.value || '';
            } else if (triggerType === '5') {
                trigger.triggerPeriphId = item.querySelector('.pe-trigger-periph')?.value || '';
                trigger.intervalSec = parseInt(item.querySelector('.pe-poll-interval')?.value || '60', 10);
                trigger.pollResponseTimeout = parseInt(item.querySelector('.pe-poll-timeout')?.value || '1000', 10);
                trigger.pollMaxRetries = parseInt(item.querySelector('.pe-poll-retries')?.value || '2', 10);
                trigger.pollInterPollDelay = parseInt(item.querySelector('.pe-poll-inter-delay')?.value || '100', 10);
            }
            triggers.push(trigger);
        });
        return triggers;
    },

    _collectPeriphExecActions() {
        const container = document.getElementById('periph-exec-actions');
        if (!container) return [];
        const actions = [];
        container.querySelectorAll('.periph-exec-config-item').forEach(item => {
            const actionType = item.querySelector('.pe-action-type')?.value || '0';
            const action = {
                targetPeriphId: item.querySelector('.pe-target-periph')?.value || '',
                actionType: parseInt(actionType, 10) || 0
            };
            if (actionType === '15') {
                action.actionValue = item.querySelector('.pe-script')?.value || '';
            } else if (actionType === '18') {
                // Modbus: 从级联下拉框收集 JSON actionValue
                var devSel = item.querySelector('.pe-modbus-device-select');
                var devVal = devSel ? devSel.value : '';
                var jsonObj = {};
                if (devVal.indexOf('sensor-') === 0) {
                    jsonObj.poll = [parseInt(devVal.split('-')[1])];
                } else if (devVal.indexOf('control-') === 0) {
                    var dIdx = parseInt(devVal.split('-')[1]);
                    var ctrl = {d: dIdx};
                    var chSel = item.querySelector('.pe-modbus-channel-select');
                    if (chSel) ctrl.c = parseInt(chSel.value || '0');
                    var actSel = item.querySelector('.pe-modbus-action-select');
                    if (actSel) {
                        ctrl.a = actSel.value || 'on';
                    } else {
                        var paramSel = item.querySelector('.pe-modbus-action-param');
                        if (paramSel) {
                            ctrl.a = 'pid';
                            ctrl.p = paramSel.value || 'P';
                        } else {
                            ctrl.a = 'pwm';
                        }
                    }
                    var valInput = item.querySelector('.pe-modbus-action-value');
                    if (valInput) ctrl.v = parseInt(valInput.value || '0');
                    jsonObj.ctrl = [ctrl];
                }
                action.actionValue = JSON.stringify(jsonObj);
            } else if (actionType === '19') {
                // Sensor read: collect JSON actionValue
                var sensorCat = item.querySelector('.pe-sensor-category')?.value || 'analog';
                var sensorPeriphId = item.querySelector('.pe-target-periph')?.value || '';
                var scaleFactor = parseFloat(item.querySelector('.pe-sensor-scale')?.value);
                if (isNaN(scaleFactor)) scaleFactor = 1;
                var sOffset = parseFloat(item.querySelector('.pe-sensor-offset')?.value);
                if (isNaN(sOffset)) sOffset = 0;
                var decimals = parseInt(item.querySelector('.pe-sensor-decimals')?.value);
                if (isNaN(decimals)) decimals = 2;
                var sensorLabel = item.querySelector('.pe-sensor-label')?.value?.trim() || '';
                var sUnit = item.querySelector('.pe-sensor-unit')?.value?.trim() || '';
                action.actionValue = JSON.stringify({
                    periphId: sensorPeriphId,
                    sensorCategory: sensorCat,
                    scaleFactor: scaleFactor,
                    offset: sOffset,
                    decimalPlaces: decimals,
                    sensorLabel: sensorLabel,
                    unit: sUnit
                });
            } else {
                action.actionValue = item.querySelector('.pe-action-value')?.value?.trim() || '';
            }
            action.useReceivedValue = item.querySelector('.pe-use-received-value')?.checked || false;
            action.syncDelayMs = parseInt(item.querySelector('.pe-sync-delay')?.value || '0', 10) || 0;
            actions.push(action);
        });
        return actions;
    },

    savePeriphExecRule() {
        const errEl = document.getElementById('periph-exec-error');
        errEl.style.display = 'none';
        const originalId = document.getElementById('periph-exec-original-id').value;
        const isEdit = originalId !== '' && originalId !== 'null' && originalId !== 'undefined';

        const ruleData = {
            name: document.getElementById('periph-exec-name').value.trim(),
            enabled: document.getElementById('periph-exec-enabled').checked,
            execMode: parseInt(document.getElementById('periph-exec-exec-mode').value, 10) || 0,
            reportAfterExec: document.getElementById('periph-exec-report').value === 'true'
        };

        if (!ruleData.name) {
            errEl.textContent = i18n.t('periph-exec-validate-name');
            errEl.style.display = 'block';
            return;
        }

        // Collect triggers
        const triggers = this._collectPeriphExecTriggers();
        ruleData.triggers = triggers;

        // Collect actions
        const actions = this._collectPeriphExecActions();
        ruleData.actions = actions;
        // Validate script actions
        for (let i = 0; i < actions.length; i++) {
            if (actions[i].actionType === 15) {
                if (!actions[i].actionValue.trim()) {
                    errEl.textContent = i18n.t('periph-exec-script-empty');
                    errEl.style.display = 'block';
                    return;
                }
                if (actions[i].actionValue.length > 1024) {
                    errEl.textContent = i18n.t('periph-exec-script-too-long');
                    errEl.style.display = 'block';
                    return;
                }
            }
            // Validate sensor read actions
            if (actions[i].actionType === 19) {
                var sv = {};
                try { sv = JSON.parse(actions[i].actionValue); } catch(e) {}
                if (!sv.periphId) {
                    errEl.textContent = i18n.t('periph-exec-sensor-no-periph');
                    errEl.style.display = 'block';
                    return;
                }
            }
        }

        if (isEdit) ruleData.id = originalId;
        const url = isEdit ? '/api/periph-exec/update' : '/api/periph-exec';
        apiPostJson(url, ruleData)
            .then(res => {
                if (res && res.success) {
                    Notification.success(i18n.t(isEdit ? 'periph-exec-update-ok' : 'periph-exec-add-ok'), i18n.t('periph-exec-title'));
                    this.closePeriphExecModal();
                    if (this.currentPage === 'periph-exec') this.loadPeriphExecPage();
                } else {
                    errEl.textContent = res?.error || i18n.t('periph-exec-save-fail');
                    errEl.style.display = 'block';
                }
            })
            .catch(err => {
                console.error('Save periph exec rule failed:', err);
                // 检测网络错误
                const isNetworkError = err && (
                    err.name === 'TypeError' ||
                    (err.message && (
                        err.message.includes('Failed to fetch') ||
                        err.message.includes('fetch') ||
                        err.message.includes('network')
                    ))
                );
                if (isNetworkError) {
                    errEl.textContent = i18n.t('device-offline-error');
                } else {
                    errEl.textContent = i18n.t('periph-exec-save-fail');
                }
                errEl.style.display = 'block';
            });
    },

    editPeriphExecRule(id) {
        this.openPeriphExecModal(id);
        // 使用按ID查询获取单条规则，避免加载全部规则导致内存不足
        Promise.all([apiGet('/api/periph-exec?id=' + id), apiGet('/api/peripherals?pageSize=50'), apiGet('/api/protocol/config')])
            .then(([execRes, periphRes, protoRes]) => {
                if (periphRes && periphRes.success && periphRes.data) {
                    this._pePeripherals = periphRes.data.filter(p => p.enabled);
                }
                this._peDataSources = [];
                if (protoRes && protoRes.success && protoRes.data) {
                    const protoData = protoRes.data;
                    this._peDataSources = this._extractDataSources(protoData);
                    // 加载 Modbus 子设备数据（供 type=6 触发器使用）
                    if (protoData.modbusRtu && protoData.modbusRtu.master) {
                        this._masterTasks = protoData.modbusRtu.master.tasks || [];
                        this._modbusDevices = protoData.modbusRtu.master.devices || [];
                    }
                }
                if (!execRes || !execRes.success || !execRes.data) return;
                // 按ID查询返回的是单条规则对象，不再是数组
                const rule = execRes.data;
                if (!rule) return;

                // Basic config
                document.getElementById('periph-exec-name').value = rule.name || '';
                document.getElementById('periph-exec-enabled').checked = !!rule.enabled;
                document.getElementById('periph-exec-exec-mode').value = String(rule.execMode || 0);
                document.getElementById('periph-exec-report').value = rule.reportAfterExec !== false ? 'true' : 'false';

                // Build triggers array from rule data (backward compat)
                let triggers = rule.triggers || [];
                if (triggers.length === 0) {
                    triggers = [{
                        triggerType: String(rule.triggerType ?? 0),
                        triggerPeriphId: rule.triggerPeriphId || rule.sourcePeriphId || '',
                        operatorType: String(rule.operatorType ?? 0),
                        compareValue: rule.compareValue || '',
                        timerMode: String(rule.timerMode ?? 0),
                        intervalSec: rule.intervalSec || 60,
                        timePoint: rule.timePoint || '08:00',
                        eventId: rule.eventId || ''
                    }];
                }

                // Build actions array from rule data (backward compat)
                let actions = rule.actions || [];
                if (actions.length === 0) {
                    actions = [{
                        targetPeriphId: rule.targetPeriphId || '',
                        actionType: String(rule.actionType ?? 0),
                        actionValue: rule.actionValue || ''
                    }];
                }

                // Render trigger blocks
                const triggersContainer = document.getElementById('periph-exec-triggers');
                if (triggersContainer) triggersContainer.innerHTML = '';
                triggers.forEach((t, i) => this._createPeriphExecTriggerElement(t, i));

                // Render action blocks
                const actionsContainer = document.getElementById('periph-exec-actions');
                if (actionsContainer) actionsContainer.innerHTML = '';
                actions.forEach((a, i) => this._createPeriphExecActionElement(a, i));

                // 编辑模式加载完成后，检查是否有"设置"模式触发并同步到动作块
                if (this._hasSetModeTrigger()) {
                    this._syncSetModeToActions(true);
                }
            });
    },

    deletePeriphExecRule(id) {
        if (!confirm(i18n.t('periph-exec-confirm-delete'))) return;
        apiDelete('/api/periph-exec/', { id: id })
            .then(res => {
                if (res && res.success) {
                    Notification.success(i18n.t('periph-exec-delete-ok'), i18n.t('periph-exec-title'));
                    if (this.currentPage === 'periph-exec') this.loadPeriphExecPage();
                } else {
                    Notification.error(res?.error || i18n.t('periph-exec-delete-fail'), i18n.t('periph-exec-title'));
                }
            })
            .catch(err => {
                console.error('Delete periph exec rule failed:', err);
                // 检测网络错误
                const isNetworkError = err && (
                    err.name === 'TypeError' ||
                    (err.message && (
                        err.message.includes('Failed to fetch') ||
                        err.message.includes('fetch') ||
                        err.message.includes('network')
                    ))
                );
                if (isNetworkError) {
                    Notification.error(i18n.t('device-offline-error'), i18n.t('periph-exec-title'));
                } else {
                    Notification.error(i18n.t('periph-exec-delete-fail'), i18n.t('periph-exec-title'));
                }
            });
    },

    togglePeriphExecRule(id, enable) {
        const url = enable ? '/api/periph-exec/enable' : '/api/periph-exec/disable';
        apiPost(url, { id: id })
            .then(res => {
                if (res && res.success) {
                    if (this.currentPage === 'periph-exec') this.loadPeriphExecPage();
                } else {
                    Notification.error(res?.error || i18n.t('periph-exec-toggle-fail'), i18n.t('periph-exec-title'));
                }
            })
            .catch(err => {
                console.error('Toggle periph exec rule failed:', err);
                // 检测网络错误
                const isNetworkError = err && (
                    err.name === 'TypeError' ||
                    (err.message && (
                        err.message.includes('Failed to fetch') ||
                        err.message.includes('fetch') ||
                        err.message.includes('network')
                    ))
                );
                if (isNetworkError) {
                    Notification.error(i18n.t('device-offline-error'), i18n.t('periph-exec-title'));
                } else {
                    Notification.error(i18n.t('periph-exec-toggle-fail'), i18n.t('periph-exec-title'));
                }
            });
    },

    runPeriphExecOnce(id) {
        apiPost('/api/periph-exec/run', { id: id })
            .then(res => {
                if (res && res.success) {
                    Notification.success(i18n.t('periph-exec-run-submitted'), i18n.t('periph-exec-title'));
                    if (this.currentPage === 'periph-exec') this.loadPeriphExecPage();
                } else {
                    Notification.error(res?.error || i18n.t('periph-exec-run-fail'), i18n.t('periph-exec-title'));
                }
            })
            .catch(err => {
                console.error('Run periph exec rule failed:', err);
                // 检测网络错误
                const isNetworkError = err && (
                    err.name === 'TypeError' ||
                    (err.message && (
                        err.message.includes('Failed to fetch') ||
                        err.message.includes('fetch') ||
                        err.message.includes('network')
                    ))
                );
                if (isNetworkError) {
                    Notification.error(i18n.t('device-offline-error'), i18n.t('periph-exec-title'));
                } else {
                    Notification.error(i18n.t('periph-exec-run-fail'), i18n.t('periph-exec-title'));
                }
            });
    },

    // ============ 外设执行独立页面 ============

    loadPeriphExecPage() {
        const tbody = document.getElementById('periph-exec-table-body');
        if (!tbody) return;
        const filterSel = document.getElementById('periph-exec-filter-periph');
        const filterPeriphId = filterSel ? filterSel.value : '';

        this._populatePeriphExecFilter();

        // 使用分页参数调用 API
        const apiUrl = '/api/periph-exec?page=' + this._peCurrentPage + '&pageSize=' + this._pePageSize;
        Promise.all([apiGet(apiUrl), apiGet('/api/peripherals?pageSize=50')])
            .then(([execRes, periphRes]) => {
                const periphMap = {};
                if (periphRes && periphRes.success && periphRes.data) {
                    periphRes.data.forEach(p => { periphMap[p.id] = p.name; });
                }
                // 保存分页信息
                this._peTotalRules = execRes && execRes.total ? execRes.total : 0;
                const currentPage = execRes && execRes.page ? execRes.page : 1;
                const currentPageSize = execRes && execRes.pageSize ? execRes.pageSize : 10;
                
                if (!execRes || !execRes.success || !execRes.data || execRes.data.length === 0) {
                    tbody.innerHTML = '<tr><td colspan="7" style="text-align:center;color:#999;">' + i18n.t('periph-exec-no-data') + '</td></tr>';
                    this._renderPeriphExecPagination(this._peTotalRules, currentPage, currentPageSize);
                    return;
                }
                let rules = execRes.data;
                if (filterPeriphId) {
                    rules = rules.filter(r => r.targetPeriphId === filterPeriphId);
                }
                // 排序：启用的规则优先，同状态按名称字母序
                rules.sort((a, b) => {
                    if (a.enabled !== b.enabled) return a.enabled ? -1 : 1;
                    return (a.name || '').localeCompare(b.name || '', 'zh');
                });
                if (rules.length === 0) {
                    tbody.innerHTML = '<tr><td colspan="7" style="text-align:center;color:#999;">' + i18n.t('periph-exec-no-data') + '</td></tr>';
                    this._renderPeriphExecPagination(this._peTotalRules, currentPage, currentPageSize);
                    return;
                }
                const triggerLabels = {
                    0: i18n.t('periph-exec-trigger-platform'),
                    1: i18n.t('periph-exec-trigger-timer'),
                    4: i18n.t('periph-exec-trigger-event'),
                    5: i18n.t('periph-exec-trigger-poll'),
                    6: i18n.t('periph-exec-trigger-periph-exec')
                };
                const actionLabels = {
                    0: i18n.t('periph-exec-action-high'), 1: i18n.t('periph-exec-action-low'),
                    2: i18n.t('periph-exec-action-blink'), 3: i18n.t('periph-exec-action-breathe'),
                    4: i18n.t('periph-exec-action-pwm'), 5: i18n.t('periph-exec-action-dac'),
                    6: i18n.t('periph-exec-action-restart'), 7: i18n.t('periph-exec-action-factory'),
                    8: i18n.t('periph-exec-action-ntp'), 9: i18n.t('periph-exec-action-ota'),
                    10: i18n.t('periph-exec-action-ap'), 11: i18n.t('periph-exec-action-ble'),
                    12: i18n.t('periph-exec-action-call-periph'),
                    15: i18n.t('periph-exec-action-script')
                };
                const opLabels = [i18n.t('periph-exec-handle-match'), i18n.t('periph-exec-handle-set')];
                let html = '';
                rules.forEach(r => {
                    const statusBadge = r.enabled
                        ? '<span class="badge badge-success">' + i18n.t('periph-exec-status-on') + '</span>'
                        : '<span class="badge badge-info">' + i18n.t('periph-exec-status-off') + '</span>';
                    // Build trigger display: use triggers array or root-level fields
                    let triggerText = '';
                    const triggers = r.triggers && r.triggers.length > 0 ? r.triggers : [r];
                    triggers.forEach((t, ti) => {
                        const tt = Number(t.triggerType);
                        let line = triggerLabels[tt] || '?';
                        if (tt === 0) {
                            const opIdx = Number(t.operatorType);
                            const opText = (opIdx === 0 || opIdx === 1) ? (opLabels[opIdx] || '') : opLabels[0];
                            line += ': ' + opText;
                            if (opIdx !== 1 && t.compareValue) line += ' ' + t.compareValue;
                        } else if (tt === 1) {
                            line += ': ' + (Number(t.timerMode) === 0 ? i18n.t('periph-exec-every') + ' ' + t.intervalSec + 's' : i18n.t('periph-exec-daily') + ' ' + (t.timePoint || ''));
                        } else if (tt === 4) {
                            line += ': ' + (t.eventId || '?');
                        } else if (tt === 5) {
                            if (t.pollInterval) line += ': ' + t.pollInterval + 'ms';
                        }
                        if (ti > 0) triggerText += '<br>';
                        triggerText += line;
                    });
                    // Build action display: use actions array or root-level fields
                    const actions = r.actions && r.actions.length > 0 ? r.actions : [r];
                    let actionDisplay = '';
                    let periphNames = [];
                    actions.forEach((a, ai) => {
                        const at = Number(a.actionType);
                        let line = actionLabels[at] || '?';
                        if (at === 15 && a.actionValue) {
                            const lineCount = a.actionValue.split('\n').filter(l => l.trim() && !l.trim().startsWith('#')).length;
                            line += ' (' + lineCount + i18n.t('periph-exec-script-lines') + ')';
                        } else if (a.actionValue) {
                            line += ' (' + a.actionValue + ')';
                        }
                        if (ai > 0) actionDisplay += '<br>';
                        actionDisplay += line;
                        const pid = a.targetPeriphId;
                        if (pid) periphNames.push(periphMap[pid] || pid);
                    });
                    const periphName = periphNames.length > 0 ? periphNames.join(', ') : '-';
                    const statsText = i18n.t('periph-exec-stats-count') + ': ' + (r.triggerCount || 0);
                    html += '<tr>';
                    html += '<td>' + (r.name || r.id) + '</td>';
                    html += '<td>' + statusBadge + '</td>';
                    html += '<td style="font-size:12px;">' + triggerText + '</td>';
                    html += '<td>' + periphName + '</td>';
                    html += '<td style="font-size:12px;">' + actionDisplay + '</td>';
                    html += '<td style="font-size:12px;">' + statsText + '</td>';
                    html += '<td style="white-space:nowrap;">';
                    html += '<button class="btn btn-sm btn-run" onclick="app.runPeriphExecOnce(\'' + r.id + '\')">' + i18n.t('periph-exec-run-once') + '</button> ';
                    html += '<button class="btn btn-sm btn-edit" onclick="app.editPeriphExecRule(\'' + r.id + '\')">' + i18n.t('peripheral-edit') + '</button> ';
                    html += '<button class="btn btn-sm ' + (r.enabled ? 'btn-disable' : 'btn-enable') + '" onclick="app.togglePeriphExecRule(\'' + r.id + '\', ' + (r.enabled ? 'false' : 'true') + ')">' + (r.enabled ? i18n.t('peripheral-disable') : i18n.t('peripheral-enable')) + '</button> ';
                    html += '<button class="btn btn-sm btn-delete" onclick="app.deletePeriphExecRule(\'' + r.id + '\')">' + i18n.t('peripheral-delete') + '</button>';
                    html += '</td>';
                    html += '</tr>';
                });
                tbody.innerHTML = html;
                // 渲染分页导航
                this._renderPeriphExecPagination(this._peTotalRules, currentPage, currentPageSize);
            })
            .catch(() => {
                tbody.innerHTML = '<tr><td colspan="7" style="text-align:center;color:#999;">' + i18n.t('periph-exec-no-data') + '</td></tr>';
            });
    },

    _renderPeriphExecPagination(total, page, pageSize) {
        const container = document.getElementById('periph-exec-pagination');
        if (!container) return;
        const totalPages = Math.ceil(total / pageSize);
        if (totalPages <= 1) {
            container.innerHTML = '<span style="color:#999;font-size:12px;">' + i18n.t('periph-exec-total') + ': ' + total + '</span>';
            return;
        }

        let html = '<div class="pagination" style="display:flex;justify-content:center;align-items:center;gap:5px;flex-wrap:wrap;">';
        // 上一页
        if (page > 1) {
            html += '<button class="btn btn-sm" onclick="app._peCurrentPage=' + (page-1) + ';app.loadPeriphExecPage()">«</button>';
        } else {
            html += '<button class="btn btn-sm" disabled>«</button>';
        }
        // 页码（最多显示5个）
        const maxVisiblePages = 5;
        let startPage = Math.max(1, page - Math.floor(maxVisiblePages / 2));
        let endPage = Math.min(totalPages, startPage + maxVisiblePages - 1);
        if (endPage - startPage + 1 < maxVisiblePages) {
            startPage = Math.max(1, endPage - maxVisiblePages + 1);
        }
        if (startPage > 1) {
            html += '<button class="btn btn-sm" onclick="app._peCurrentPage=1;app.loadPeriphExecPage()">1</button>';
            if (startPage > 2) html += '<span style="padding:0 5px;">...</span>';
        }
        for (let i = startPage; i <= endPage; i++) {
            if (i === page) {
                html += '<button class="btn btn-sm btn-primary" disabled>' + i + '</button>';
            } else {
                html += '<button class="btn btn-sm" onclick="app._peCurrentPage=' + i + ';app.loadPeriphExecPage()">' + i + '</button>';
            }
        }
        if (endPage < totalPages) {
            if (endPage < totalPages - 1) html += '<span style="padding:0 5px;">...</span>';
            html += '<button class="btn btn-sm" onclick="app._peCurrentPage=' + totalPages + ';app.loadPeriphExecPage()">' + totalPages + '</button>';
        }
        // 下一页
        if (page < totalPages) {
            html += '<button class="btn btn-sm" onclick="app._peCurrentPage=' + (page+1) + ';app.loadPeriphExecPage()">»</button>';
        } else {
            html += '<button class="btn btn-sm" disabled>»</button>';
        }
        html += ' <span style="color:#999;font-size:12px;margin-left:10px;">' + i18n.t('periph-exec-total') + ': ' + total + '</span>';
        html += '</div>';
        container.innerHTML = html;
    },

    _populatePeriphExecFilter() {
        const sel = document.getElementById('periph-exec-filter-periph');
        if (!sel || sel._populated) return;
        apiGet('/api/peripherals?pageSize=50').then(res => {
            if (!res || !res.success || !res.data) return;
            const currentVal = sel.value;
            let opts = '<option value="">' + i18n.t('periph-exec-filter-all') + '</option>';
            res.data.forEach(p => {
                opts += '<option value="' + p.id + '">' + p.name + ' (' + p.id + ')' + '</option>';
            });
            sel.innerHTML = opts;
            if (currentVal) sel.value = currentVal;
            sel._populated = true;
        });
    },

    // ============ 规则脚本页面 ============

    loadRuleScriptPage() {
        const tbody = document.getElementById('rule-script-table-body');
        if (!tbody) return;
        apiGet('/api/rule-script').then(res => {
            if (!res || !res.success || !res.data) {
                tbody.innerHTML = '<tr><td colspan="6" style="text-align:center;color:#999;">' + i18n.t('rule-script-no-data') + '</td></tr>';
                return;
            }
            const rules = res.data;
            if (rules.length === 0) {
                tbody.innerHTML = '<tr><td colspan="6" style="text-align:center;color:#999;">' + i18n.t('rule-script-no-data') + '</td></tr>';
                return;
            }
            const triggerLabels = { 0: i18n.t('rule-script-trigger-receive'), 1: i18n.t('rule-script-trigger-report') };
            const protocolLabels = { 0: 'MQTT', 1: 'Modbus RTU', 2: 'Modbus TCP', 3: 'HTTP', 4: 'CoAP', 5: 'TCP' };
            let html = '';
            rules.forEach(r => {
                const statusBadge = r.enabled
                    ? '<span class="badge badge-success">' + i18n.t('periph-exec-status-on') + '</span>'
                    : '<span class="badge badge-info">' + i18n.t('periph-exec-status-off') + '</span>';
                const triggerText = triggerLabels[r.triggerType] || '?';
                const protocolText = protocolLabels[r.protocolType] || '-';
                const statsText = i18n.t('periph-exec-stats-count') + ': ' + (r.triggerCount || 0);
                html += '<tr>';
                html += '<td>' + (r.name || '') + '</td>';
                html += '<td>' + statusBadge + '</td>';
                html += '<td>' + triggerText + '</td>';
                html += '<td>' + protocolText + '</td>';
                html += '<td style="font-size:12px;">' + statsText + '</td>';
                html += '<td class="u-toolbar-sm">';
                html += '<button class="btn btn-sm btn-edit" onclick="app.editRuleScript(\'' + r.id + '\')">' + i18n.t('peripheral-edit') + '</button>';
                if (r.enabled) {
                    html += '<button class="btn btn-sm btn-disable" onclick="app.toggleRuleScript(\'' + r.id + '\',false)">' + i18n.t('peripheral-disable') + '</button>';
                } else {
                    html += '<button class="btn btn-sm btn-enable" onclick="app.toggleRuleScript(\'' + r.id + '\',true)">' + i18n.t('peripheral-enable') + '</button>';
                }
                html += '<button class="btn btn-sm btn-delete" onclick="app.deleteRuleScript(\'' + r.id + '\')">' + i18n.t('peripheral-delete') + '</button>';
                html += '</td>';
                html += '</tr>';
            });
            tbody.innerHTML = html;
        }).catch(() => {
            tbody.innerHTML = '<tr><td colspan="6" style="text-align:center;color:#999;">' + i18n.t('rule-script-no-data') + '</td></tr>';
        });
    },

    openRuleScriptModal(editId) {
        const modal = document.getElementById('rule-script-modal');
        if (!modal) return;
        const titleEl = document.getElementById('rule-script-modal-title');
        document.getElementById('rule-script-original-id').value = editId || '';
        document.getElementById('rule-script-error').style.display = 'none';
        if (editId) {
            if (titleEl) titleEl.textContent = i18n.t('rule-script-edit-title');
        } else {
            if (titleEl) titleEl.textContent = i18n.t('rule-script-add-title');
            document.getElementById('rule-script-form').reset();
            document.getElementById('rule-script-protocol-type').value = '0';
            document.getElementById('rule-script-content').value = '';
        }
        modal.style.display = 'flex';
    },

    closeRuleScriptModal() {
        const modal = document.getElementById('rule-script-modal');
        if (modal) modal.style.display = 'none';
    },

    saveRuleScript() {
        const errEl = document.getElementById('rule-script-error');
        errEl.style.display = 'none';
        const originalId = document.getElementById('rule-script-original-id').value;
        const isEdit = originalId !== '';
        const ruleData = {
            name: document.getElementById('rule-script-name').value.trim(),
            enabled: document.getElementById('rule-script-enabled').checked ? '1' : '0',
            triggerType: document.getElementById('rule-script-trigger-type').value,
            protocolType: document.getElementById('rule-script-protocol-type').value,
            scriptContent: document.getElementById('rule-script-content').value
        };
        if (!ruleData.name) {
            errEl.textContent = i18n.t('periph-exec-validate-name');
            errEl.style.display = 'block';
            return;
        }
        if (isEdit) ruleData.id = originalId;
        const url = isEdit ? '/api/rule-script/update' : '/api/rule-script';
        apiPost(url, ruleData).then(res => {
            if (res && res.success) {
                Notification.success(i18n.t(isEdit ? 'rule-script-update-ok' : 'rule-script-add-ok'), i18n.t('rule-script-title'));
                this.closeRuleScriptModal();
                if (this.currentPage === 'rule-script') this.loadRuleScriptPage();
            } else {
                errEl.textContent = res?.error || i18n.t('rule-script-save-fail');
                errEl.style.display = 'block';
            }
        }).catch(() => {
            errEl.textContent = i18n.t('rule-script-save-fail');
            errEl.style.display = 'block';
        });
    },

    editRuleScript(id) {
        this.openRuleScriptModal(id);
        apiGet('/api/rule-script').then(res => {
            if (!res || !res.success || !res.data) return;
            const rule = res.data.find(r => r.id === id);
            if (!rule) return;
            document.getElementById('rule-script-name').value = rule.name || '';
            document.getElementById('rule-script-enabled').checked = !!rule.enabled;
            document.getElementById('rule-script-trigger-type').value = String(rule.triggerType);
            document.getElementById('rule-script-protocol-type').value = String(rule.protocolType || 0);
            document.getElementById('rule-script-content').value = rule.scriptContent || '';
        });
    },

    toggleRuleScript(id, enable) {
        const url = enable ? '/api/rule-script/enable' : '/api/rule-script/disable';
        apiPost(url, { id: id }).then(res => {
            if (res && res.success) {
                if (this.currentPage === 'rule-script') this.loadRuleScriptPage();
            }
        });
    },

    deleteRuleScript(id) {
        if (!confirm(i18n.t('periph-exec-confirm-delete'))) return;
        apiDelete('/api/rule-script/', { id: id }).then(res => {
            if (res && res.success) {
                Notification.success(i18n.t('rule-script-delete-ok'), i18n.t('rule-script-title'));
                if (this.currentPage === 'rule-script') this.loadRuleScriptPage();
            }
        });
    },

    // ============ AP配网功能 ============
    
    /**
     * 加载配网状态
     */
    loadProvisionStatus() {
        apiGet('/api/provision/status')
            .then(res => {
                if (!res || !res.success) return;
                const d = res.data || {};
                
                // 更新状态显示
                const statusEl = document.getElementById('ap-provision-status');
                if (statusEl) {
                    if (d.active) {
                        statusEl.textContent = i18n.t('provision-active');
                        statusEl.className = 'badge badge-success';
                    } else {
                        statusEl.textContent = i18n.t('provision-inactive');
                        statusEl.className = 'badge badge-info';
                    }
                }
                
                this._setTextContent('ap-provision-ap-name', d.apSSID || '--');
                this._setTextContent('ap-provision-clients', d.clients || '0');
                
                // 更新按钮状态
                const startBtn = document.getElementById('ap-provision-start-btn');
                const stopBtn = document.getElementById('ap-provision-stop-btn');
                if (startBtn) startBtn.disabled = d.active;
                if (stopBtn) stopBtn.disabled = !d.active;
            })
            .catch(err => {
                console.error('Load provision status failed:', err);
            });
    },

    /**
     * 加载AP配网配置
     */
    loadProvisionConfig() {
        apiGet('/api/provision/config')
            .then(res => {
                if (!res || !res.success) return;
                const d = res.data || {};
                
                this._setValue('provision-ssid', d.provisionSSID || '');
                this._setValue('provision-password', d.provisionPassword || '');
                this._setValue('provision-timeout', d.provisionTimeout || 300);
                this._setValue('provision-user-id', d.provisionUserId || '');
                this._setValue('provision-product-id', d.provisionProductId || '');
                this._setValue('provision-auth-code', d.provisionAuthCode || '');
                this._setValue('provision-ip', d.provisionIP || '192.168.4.1');
                this._setValue('provision-gateway', d.provisionGateway || '192.168.4.1');
                this._setValue('provision-subnet', d.provisionSubnet || '255.255.255.0');
            })
            .catch(err => {
                console.error('Load provision config failed:', err);
            });
    },

    /**
     * 保存AP配网配置
     */
    saveProvisionConfig() {
        const data = {
            provisionSSID:     document.getElementById('provision-ssid')?.value || '',
            provisionPassword: document.getElementById('provision-password')?.value || '',
            provisionTimeout:  document.getElementById('provision-timeout')?.value || '300',
            provisionUserId:   document.getElementById('provision-user-id')?.value || '',
            provisionProductId: document.getElementById('provision-product-id')?.value || '',
            provisionAuthCode: document.getElementById('provision-auth-code')?.value || '',
            provisionIP:       document.getElementById('provision-ip')?.value || '192.168.4.1',
            provisionGateway:  document.getElementById('provision-gateway')?.value || '192.168.4.1',
            provisionSubnet:   document.getElementById('provision-subnet')?.value || '255.255.255.0'
        };

        apiPut('/api/provision/config', data)
            .then(res => {
                if (res && res.success) {
                    this._showMessage('provision-success', true);
                    Notification.success(i18n.t('provision-save-ok'), i18n.t('provision-title'));
                } else {
                    Notification.error(res?.message || i18n.t('dev-save-fail'), i18n.t('provision-title'));
                }
            })
            .catch(err => {
                Notification.error(i18n.t('provision-start-fail') + ': ' + (err.message || err), i18n.t('provision-title'));
            });
    },

    /**
     * 启动AP配网
     */
    startProvision() {
        const startBtn = document.getElementById('ap-provision-start-btn');
        if (startBtn) {
            startBtn.disabled = true;
            startBtn.innerHTML = i18n.t('provision-starting-html');
        }

        apiPost('/api/provision/start', {})
            .then(res => {
                if (res && res.success) {
                    Notification.success(`${i18n.t('provision-start-ok-prefix')}${res.data?.apSSID || ''}`, i18n.t('provision-title'));
                    this.loadProvisionStatus();
                } else {
                    Notification.error(res?.message || i18n.t('provision-start-fail'), i18n.t('provision-title'));
                }
            })
            .catch(err => {
                Notification.error(i18n.t('provision-start-fail') + ': ' + (err.message || err), i18n.t('provision-title'));
            })
            .finally(() => {
                if (startBtn) {
                    startBtn.innerHTML = i18n.t('provision-start-html');
                    // 按钮状态由 loadProvisionStatus 更新
                }
            });
    },

    /**
     * 停止AP配网
     */
    stopProvision() {
        const stopBtn = document.getElementById('ap-provision-stop-btn');
        if (stopBtn) {
            stopBtn.disabled = true;
            stopBtn.innerHTML = i18n.t('provision-stopping-html');
        }

        apiPost('/api/provision/stop', {})
            .then(res => {
                if (res && res.success) {
                    Notification.success(i18n.t('provision-stop-ok'), i18n.t('provision-title'));
                    this.loadProvisionStatus();
                } else {
                    Notification.error(res?.message || i18n.t('provision-stop-fail'), i18n.t('provision-title'));
                }
            })
            .catch(err => {
                Notification.error(i18n.t('provision-stop-fail') + ': ' + (err.message || err), i18n.t('provision-title'));
            })
            .finally(() => {
                if (stopBtn) {
                    stopBtn.innerHTML = i18n.t('provision-stop-html');
                }
            });
    },

    // ============ 蓝牙配网 ============

    /**
     * 加载蓝牙配网状态
     */
    loadBLEProvisionStatus() {
        apiGet('/api/ble/provision/status')
            .then(res => {
                if (!res || !res.success) return;
                const d = res.data || {};
                
                const badge = document.getElementById('ble-provision-status-badge');
                const deviceName = document.getElementById('ble-provision-device-name');
                const remainingWrap = document.getElementById('ble-provision-remaining-wrap');
                const remaining = document.getElementById('ble-provision-remaining');
                const startBtn = document.getElementById('ble-provision-start-btn');
                const stopBtn = document.getElementById('ble-provision-stop-btn');

                if (d.active) {
                    if (badge) { badge.className = 'status-badge status-online'; badge.textContent = i18n.t('ble-active'); }
                    if (deviceName) deviceName.textContent = d.deviceName || '--';
                    if (remainingWrap) {
                        remainingWrap.classList.remove('is-hidden');
                    }
                    if (remaining) remaining.textContent = (d.remainingTime || 0) + i18n.t('ble-remaining-unit');
                    if (startBtn) startBtn.classList.add('is-hidden');
                    if (stopBtn) { stopBtn.classList.remove('is-hidden'); stopBtn.disabled = false; }
                } else {
                    if (badge) { badge.className = 'status-badge status-offline'; badge.textContent = i18n.t('ble-inactive'); }
                    if (deviceName) deviceName.textContent = '--';
                    if (remainingWrap) {
                        remainingWrap.classList.add('is-hidden');
                    }
                    if (startBtn) { startBtn.classList.remove('is-hidden'); startBtn.disabled = false; }
                    if (stopBtn) stopBtn.classList.add('is-hidden');
                }
            })
            .catch(err => {
                console.error('加载蓝牙配网状态失败:', err);
            });
    },

    /**
     * 加载蓝牙配网配置
     */
    loadBLEProvisionConfig() {
        apiGet('/api/ble/provision/config')
            .then(res => {
                if (!res || !res.success) return;
                const d = res.data || {};
                
                this._setValue('ble-device-name', d.bleName || 'FBDevice');
                this._setValue('ble-timeout', d.bleTimeout || 300);
                this._setChecked('ble-auto-start', d.bleAutoStart || false);
                this._setValue('ble-service-uuid', d.bleServiceUUID || '6E400001-B5A3-F393-E0A9-E50E24DCCA9F');
                this._setValue('ble-rx-uuid', d.bleRxUUID || '6E400002-B5A3-F393-E0A9-E50E24DCCA9F');
                this._setValue('ble-tx-uuid', d.bleTxUUID || '6E400003-B5A3-F393-E0A9-E50E24DCCA9F');
            })
            .catch(err => {
                console.error('加载蓝牙配网配置失败:', err);
            });
    },

    /**
     * 保存蓝牙配网配置
     */
    saveBLEProvisionConfig() {
        const data = {
            bleName:        document.getElementById('ble-device-name')?.value || 'FBDevice',
            bleTimeout:     document.getElementById('ble-timeout')?.value || '300',
            bleAutoStart:   document.getElementById('ble-auto-start')?.checked ? 'true' : 'false',
            bleServiceUUID: document.getElementById('ble-service-uuid')?.value || '6E400001-B5A3-F393-E0A9-E50E24DCCA9F',
            bleRxUUID:      document.getElementById('ble-rx-uuid')?.value || '6E400002-B5A3-F393-E0A9-E50E24DCCA9F',
            bleTxUUID:      document.getElementById('ble-tx-uuid')?.value || '6E400003-B5A3-F393-E0A9-E50E24DCCA9F'
        };

        apiPut('/api/ble/provision/config', data)
            .then(res => {
                if (res && res.success) {
                    this._showMessage('ble-provision-success', true);
                    Notification.success(i18n.t('ble-save-ok'), i18n.t('ble-title'));
                } else {
                    Notification.error(res?.message || i18n.t('dev-save-fail'), i18n.t('ble-title'));
                }
            })
            .catch(err => {
                Notification.error(i18n.t('ble-start-fail') + ': ' + (err.message || err), i18n.t('ble-title'));
            });
    },

    /**
     * 启动蓝牙配网
     */
    startBLEProvision() {
        const startBtn = document.getElementById('ble-provision-start-btn');
        if (startBtn) {
            startBtn.disabled = true;
            startBtn.innerHTML = i18n.t('ble-starting-html');
        }

        apiPost('/api/ble/provision/start', {})
            .then(res => {
                if (res && res.success) {
                    Notification.success(i18n.t('ble-start-ok-prefix') + (res.data?.deviceName || ''), i18n.t('ble-title'));
                    this.loadBLEProvisionStatus();
                } else {
                    Notification.error(res?.message || i18n.t('ble-start-fail'), i18n.t('ble-title'));
                }
            })
            .catch(err => {
                Notification.error(i18n.t('ble-start-fail') + ': ' + (err.message || err), i18n.t('ble-title'));
            })
            .finally(() => {
                if (startBtn) {
                    startBtn.innerHTML = i18n.t('ble-start-html');
                }
            });
    },

    /**
     * 停止蓝牙配网
     */
    stopBLEProvision() {
        const stopBtn = document.getElementById('ble-provision-stop-btn');
        if (stopBtn) {
            stopBtn.disabled = true;
            stopBtn.innerHTML = i18n.t('ble-stopping-html');
        }

        apiPost('/api/ble/provision/stop', {})
            .then(res => {
                if (res && res.success) {
                    Notification.success(i18n.t('ble-stop-ok'), i18n.t('ble-title'));
                    this.loadBLEProvisionStatus();
                } else {
                    Notification.error(res?.message || i18n.t('ble-stop-fail'), i18n.t('ble-title'));
                }
            })
            .catch(err => {
                Notification.error(i18n.t('ble-stop-fail') + ': ' + (err.message || err), i18n.t('ble-title'));
            })
            .finally(() => {
                if (stopBtn) {
                    stopBtn.innerHTML = i18n.t('ble-stop-html');
                }
            });
    },

    // ============ OTA升级 ============

    /**
     * 加载OTA状态
     */
    loadOtaStatus() {
        // 同时获取OTA状态和网络状态
        Promise.all([
            apiGet('/api/ota/status'),
            apiGet('/api/network/status'),
            apiGet('/api/system/info')
        ])
            .then(([otaRes, netRes, sysRes]) => {
                const internetAvailable = (netRes && netRes.success && netRes.data) ? netRes.data.internetAvailable === true : false;
                
                // OTA状态处理
                if (otaRes) {
                    const badge = document.getElementById('ota-status-badge');
                    const progressWrap = document.getElementById('ota-progress-wrap');
                    const progressBar = document.getElementById('ota-progress-bar');
                    const progressText = document.getElementById('ota-progress-text');
                    
                    if (otaRes.status === 'OTA ready') {
                        if (badge) { badge.className = 'status-badge status-online'; badge.textContent = i18n.t('ota-ready'); }
                        if (progressWrap) progressWrap.classList.add('is-hidden');
                    } else if (otaRes.progress > 0 && otaRes.progress < 100) {
                        if (badge) { badge.className = 'status-badge status-warning'; badge.textContent = i18n.t('ota-in-progress'); }
                        if (progressWrap) progressWrap.classList.remove('is-hidden');
                        if (progressBar) progressBar.style.width = otaRes.progress + '%';
                        if (progressText) progressText.textContent = otaRes.progress + '%';
                    }
                }
                
                // 根据网络状态设置URL升级按钮
                const urlBtn = document.getElementById('ota-url-btn');
                const urlInput = document.getElementById('ota-url');
                const urlHint = document.getElementById('ota-url-hint');
                if (urlBtn) {
                    if (internetAvailable) {
                        urlBtn.disabled = false;
                        urlBtn.title = '';
                        if (urlInput) urlInput.disabled = false;
                        if (urlHint) urlHint.classList.add('is-hidden');
                    } else {
                        urlBtn.disabled = true;
                        urlBtn.title = i18n.t('ota-no-network-tip');
                        if (urlInput) urlInput.disabled = true;
                        if (urlHint) {
                            urlHint.classList.remove('is-hidden');
                            urlHint.innerHTML = `<span class="badge badge-danger">${i18n.t('ota-no-network-msg')}</span>`;
                        }
                    }
                }
                
                // 系统信息处理
                if (sysRes && sysRes.success) {
                    const d = sysRes.data || {};
                    
                    this._setValue('ota-current-version', d.firmwareVersion || '--');
                    
                    const flashSize = d.flashChipSize || 0;
                    const freeSketch = d.freeSketchSpace || 0;
                    
                    const flashSizeEl = document.getElementById('ota-flash-size');
                    const freeSpaceEl = document.getElementById('ota-free-space');
                    
                    if (flashSizeEl) flashSizeEl.textContent = flashSize > 0 ? (flashSize / 1024 / 1024).toFixed(2) + ' MB' : '--';
                    if (freeSpaceEl) freeSpaceEl.textContent = freeSketch > 0 ? (freeSketch / 1024).toFixed(0) + ' KB' : '--';
                }
            })
            .catch(err => {
                console.error('加载OTA状态失败:', err);
            });
    },

    /**
     * 通过URL在线升级
     */
    startOtaUrl() {
        const url = document.getElementById('ota-url')?.value || '';
        
        if (!url) {
            Notification.error(i18n.t('ota-url-empty'), i18n.t('ota-title'));
            return;
        }
        
        if (!url.startsWith('http://') && !url.startsWith('https://')) {
            Notification.error(i18n.t('ota-url-invalid'), i18n.t('ota-title'));
            return;
        }
        
        const btn = document.getElementById('ota-url-btn');
        if (btn) {
            btn.disabled = true;
            btn.innerHTML = i18n.t('ota-downloading-html');
        }
        
        // 显示进度条
        const progressWrap = document.getElementById('ota-progress-wrap');
        if (progressWrap) progressWrap.classList.remove('is-hidden');
        
        apiPost('/api/ota/url', { url })
            .then(res => {
                if (res && res.success) {
                    Notification.success(i18n.t('ota-start-ok'), i18n.t('ota-title'));
                    // 开始轮询状态
                    this._pollOtaProgress();
                } else {
                    Notification.error(res?.message || i18n.t('ota-start-fail'), i18n.t('ota-title'));
                    if (progressWrap) progressWrap.classList.add('is-hidden');
                }
            })
            .catch(err => {
                Notification.error(i18n.t('ota-start-fail') + ': ' + (err.message || err), i18n.t('ota-title'));
                if (progressWrap) progressWrap.classList.add('is-hidden');
            })
            .finally(() => {
                if (btn) {
                    btn.disabled = false;
                    btn.innerHTML = i18n.t('ota-start-url-html');
                }
            });
    },

    /**
     * 本地文件上传升级
     */
    startOtaUpload() {
        const fileInput = document.getElementById('ota-file');
        const file = fileInput?.files?.[0];
        
        if (!file) {
            Notification.error(i18n.t('ota-file-empty'), i18n.t('ota-title'));
            return;
        }
        
        if (!file.name.endsWith('.bin')) {
            Notification.error(i18n.t('ota-file-invalid'), i18n.t('ota-title'));
            return;
        }
        
        const btn = document.getElementById('ota-upload-btn');
        if (btn) {
            btn.disabled = true;
            btn.innerHTML = i18n.t('ota-uploading-html');
        }
        
        // 显示进度条
        const progressWrap = document.getElementById('ota-progress-wrap');
        const progressBar = document.getElementById('ota-progress-bar');
        const progressText = document.getElementById('ota-progress-text');
        if (progressWrap) progressWrap.classList.remove('is-hidden');
        
        const formData = new FormData();
        formData.append('firmware', file);
        
        const xhr = new XMLHttpRequest();
        
        xhr.upload.addEventListener('progress', (e) => {
            if (e.lengthComputable) {
                const percent = Math.round((e.loaded / e.total) * 100);
                if (progressBar) progressBar.style.width = percent + '%';
                if (progressText) progressText.textContent = percent + '%';
            }
        });
        
        xhr.addEventListener('load', () => {
            if (xhr.status === 200) {
                try {
                    const res = JSON.parse(xhr.responseText);
                    if (res.success) {
                        Notification.success(i18n.t('ota-upload-ok'), i18n.t('ota-title'));
                        // 设备会重启，等待后刷新页面
                        setTimeout(() => {
                            window.location.reload();
                        }, 5000);
                    } else {
                        Notification.error(res.message || i18n.t('ota-upload-fail'), i18n.t('ota-title'));
                    }
                } catch (e) {
                    Notification.success(i18n.t('ota-upload-ok2'), i18n.t('ota-title'));
                }
            } else {
                Notification.error(i18n.t('ota-upload-fail-prefix') + xhr.status, i18n.t('ota-title'));
            }
            
            if (btn) {
                btn.disabled = false;
                btn.innerHTML = i18n.t('ota-upload-btn-html');
            }
        });
        
        xhr.addEventListener('error', () => {
            Notification.error(i18n.t('ota-upload-network-fail'), i18n.t('ota-title'));
            if (btn) {
                btn.disabled = false;
                btn.innerHTML = i18n.t('ota-upload-btn-html');
            }
            if (progressWrap) progressWrap.classList.add('is-hidden');
        });
        
        xhr.open('POST', '/api/ota/upload');
        xhr.setRequestHeader('Authorization', 'Bearer ' + localStorage.getItem('token'));
        xhr.send(formData);
    },

    /**
     * 轮询OTA进度
     */
    _pollOtaProgress() {
        const progressBar = document.getElementById('ota-progress-bar');
        const progressText = document.getElementById('ota-progress-text');
        const badge = document.getElementById('ota-status-badge');
        
        const poll = () => {
            apiGet('/api/ota/status')
                .then(res => {
                    if (!res) return;
                    
                    const progress = res.progress || 0;
                    if (progressBar) progressBar.style.width = progress + '%';
                    if (progressText) progressText.textContent = progress + '%';
                    
                    if (progress < 100 && res.status !== 'OTA ready') {
                        if (badge) { badge.className = 'status-badge status-warning'; badge.textContent = i18n.t('ota-in-progress'); }
                        setTimeout(poll, 1000);
                    } else if (progress >= 100) {
                        if (badge) { badge.className = 'status-badge status-online'; badge.textContent = i18n.t('ota-done'); }
                        Notification.success(i18n.t('ota-complete-msg'), i18n.t('ota-title'));
                    }
                })
                .catch(err => {
                    console.error(i18n.t('ota-progress-fail'), err);
                });
        };
        
        poll();
    },

    // ============ 协议配置增强方法 ============
    
    /**
     * 测试MQTT连接
     */
    testMqttConnection() {
        const resultEl = document.getElementById('mqtt-test-result');
        const btn = document.querySelector('#mqtt-form .mqtt-test-btn');
        
        const server = document.getElementById('mqtt-broker')?.value || '';
        const port = document.getElementById('mqtt-port')?.value || '1883';
        const clientId = document.getElementById('mqtt-client-id')?.value || '';
        const username = document.getElementById('mqtt-username')?.value || '';
        const password = document.getElementById('mqtt-password')?.value || '';
        const authCode = document.getElementById('mqtt-auth-code')?.value || '';
        const mqttSecret = document.getElementById('mqtt-secret')?.value || '';
        const authType = document.getElementById('mqtt-auth-type')?.value || '0';
        
        if (!server) {
            if (resultEl) {
                resultEl.textContent = i18n.t('mqtt-test-no-server');
                resultEl.style.color = '#f56c6c';
            }
            return;
        }

        // 客户端ID前缀与认证方式匹配提示（不阻止操作）
        // FastBee平台约定: 简单认证(authType=0)客户端ID以S&开头，加密认证(authType=1)以E&开头
        if (clientId) {
            if (authType === '0' && !clientId.startsWith('S&')) {
                if (resultEl) {
                    resultEl.textContent = i18n.t('mqtt-test-clientid-simple-prefix');
                    resultEl.style.color = '#e6a23c';
                }
            }
            if (authType === '1' && !clientId.startsWith('E&')) {
                if (resultEl) {
                    resultEl.textContent = i18n.t('mqtt-test-clientid-encrypted-prefix');
                    resultEl.style.color = '#e6a23c';
                }
            }
        }
        
        if (btn) {
            btn.disabled = true;
            btn.textContent = i18n.t('mqtt-test-testing');
            btn.classList.remove('mqtt-test-success', 'mqtt-test-fail');
        }
        if (resultEl) {
            resultEl.textContent = i18n.t('mqtt-test-testing');
            resultEl.style.color = '#909399';
        }
        
        // 传递 authType 让后端知道当前 UI 选择的认证方式
        apiMqttTest({ server, port, clientId, username, password, authCode, authType, mqttSecret })
            .then(res => {
                if (res && res.success && res.data) {
                    // 加密认证：后端使用非阻塞延迟连接，需轮询状态
                    if (res.data.deferred) {
                        if (resultEl) {
                            resultEl.textContent = i18n.t('mqtt-test-deferred') || 'MQTT connecting asynchronously, check status...';
                            resultEl.style.color = '#e6a23c';
                        }
                        if (btn) btn.classList.add('mqtt-test-success');
                        // 延迟轮询实际状态
                        let pollCount = 0;
                        const pollInterval = setInterval(() => {
                            pollCount++;
                            this._loadMqttStatus();
                            const badge = document.getElementById('mqtt-status-badge');
                            if (badge && badge.classList.contains('mqtt-status-online')) {
                                clearInterval(pollInterval);
                                if (resultEl) {
                                    resultEl.textContent = i18n.t('mqtt-test-success');
                                    resultEl.style.color = '#67c23a';
                                }
                            }
                            if (pollCount >= 15) {
                                clearInterval(pollInterval);
                                if (resultEl && !resultEl.textContent.includes(i18n.t('mqtt-test-success'))) {
                                    resultEl.textContent = i18n.t('mqtt-test-deferred-timeout') || 'Connection timeout, check logs';
                                    resultEl.style.color = '#f56c6c';
                                    if (btn) {
                                        btn.classList.remove('mqtt-test-success');
                                        btn.classList.add('mqtt-test-fail');
                                    }
                                }
                            }
                        }, 2000);
                        return;
                    }

                    if (res.data.connected) {
                        // 测试通过，检查实际连接结果
                        if (res.data.realConnected) {
                            if (resultEl) {
                                resultEl.textContent = i18n.t('mqtt-test-success');
                                resultEl.style.color = '#67c23a';
                            }
                            if (btn) btn.classList.add('mqtt-test-success');
                        } else {
                            // 临时测试通过但实际重连失败
                            const realErr = res.data.realError;
                            const errMsg = realErr ? this._mqttErrorCodeToText(realErr) : '';
                            if (resultEl) {
                                resultEl.textContent = i18n.t('mqtt-test-ok-real-fail') + (errMsg ? ' (' + errMsg + ')' : '');
                                resultEl.style.color = '#e6a23c';
                            }
                            if (btn) btn.classList.add('mqtt-test-fail');
                        }
                    } else {
                        const errCode = res.data.error || 'Unknown';
                        let errMsg = this._mqttErrorCodeToText(errCode);
                        // AES 认证下，Bad credentials 通常是密钥/授权码/账号信息不匹配
                        if (authType === '1' && String(errCode) === '4') {
                            const hint = i18n.t('mqtt-test-aes-bad-credentials-hint') ||
                                'AES认证失败，请检查用户名、密码、产品密钥(mqttSecret)和授权码(authCode)是否与平台一致';
                            errMsg = errMsg + ' - ' + hint;
                        }
                        if (resultEl) {
                            resultEl.textContent = i18n.t('mqtt-test-fail-prefix') + errMsg;
                            resultEl.style.color = '#f56c6c';
                        }
                        if (btn) btn.classList.add('mqtt-test-fail');
                    }
                } else {
                    if (resultEl) {
                        resultEl.textContent = i18n.t('mqtt-test-error');
                        resultEl.style.color = '#f56c6c';
                    }
                    if (btn) btn.classList.add('mqtt-test-fail');
                }
            })
            .catch(err => {
                console.error('MQTT test failed:', err);
                if (resultEl) {
                    // 区分超时、401和其他错误
                    const isTimeout = err && (err.name === 'AbortError' ||
                        (err.message && err.message.includes('abort')));
                    const isUnauthorized = err && err.status === 401;
                                
                    if (isUnauthorized) {
                        // 401错误：可能是认证问题，但不提示"会话过期"避免误导用户
                        resultEl.textContent = i18n.t('mqtt-test-error') || '测试请求失败，请稍后重试';
                    } else if (isTimeout) {
                        resultEl.textContent = i18n.t('mqtt-test-timeout') || '测试超时，请检查Broker地址是否正确';
                    } else {
                        // 优先显示后端返回的错误信息
                        const errData = err && err.data;
                        const errMsg = (errData && errData.error) ? errData.error : i18n.t('mqtt-test-error');
                        resultEl.textContent = errMsg;
                    }
                    resultEl.style.color = '#f56c6c';
                }
                if (btn) btn.classList.add('mqtt-test-fail');
            })
            .finally(() => {
                if (btn) {
                    btn.disabled = false;
                    btn.textContent = i18n.t('mqtt-test-btn-text');
                }
                // 刷新实际MQTT状态（由轮询接口获取真实连接状态）
                setTimeout(() => this._loadMqttStatus(), 1000);
                // 3秒后恢复按钮颜色
                setTimeout(() => {
                    if (btn) btn.classList.remove('mqtt-test-success', 'mqtt-test-fail');
                }, 3000);
            });
    },

    /**
     * MQTT断开连接
     */
    disconnectMqtt() {
        const btn = document.querySelector('#mqtt-form .mqtt-disconnect-btn');
        if (btn) {
            btn.disabled = true;
            btn.textContent = i18n.t('mqtt-disconnecting');
        }

        // 先停止状态轮询，避免断开过程中状态不一致
        this._stopMqttStatusPolling();

        // 使用静默模式，避免触发全局401错误处理
        apiPostSilent('/api/mqtt/disconnect', {})
            .then(res => {
                if (res && res.success) {
                    Notification.success(i18n.t('mqtt-disconnect-ok'), 'MQTT');
                    // 更新状态面板
                    const badge = document.getElementById('mqtt-status-badge');
                    if (badge) {
                        badge.className = 'mqtt-status-badge mqtt-status-offline';
                        badge.textContent = i18n.t('mqtt-status-disconnected');
                    }
                    // 更新测试结果显示区域
                    const resultEl = document.getElementById('mqtt-test-result');
                    if (resultEl) {
                        resultEl.textContent = i18n.t('mqtt-disconnect-ok');
                        resultEl.style.color = '#909399';
                    }
                } else {
                    Notification.error(i18n.t('mqtt-disconnect-fail'), 'MQTT');
                    // 断开失败，恢复状态轮询
                    this._startMqttStatusPolling();
                }
            })
            .catch(err => {
                console.error('MQTT disconnect failed:', err);
                Notification.error(i18n.t('mqtt-disconnect-fail'), 'MQTT');
                // 断开失败，恢复状态轮询
                this._startMqttStatusPolling();
            })
            .finally(() => {
                if (btn) {
                    btn.disabled = false;
                    btn.textContent = i18n.t('mqtt-disconnect-btn');
                }
                // 断开成功后延迟刷新状态
                setTimeout(() => this._loadMqttStatus(), 500);
            });
    },

    /**
     * MQTT NTP 时间同步
     */
    mqttNtpSync() {
        const btn = document.querySelector('#mqtt-form .mqtt-ntp-sync-btn');
        const resultEl = document.getElementById('mqtt-test-result');
        if (btn) {
            btn.disabled = true;
            btn.textContent = i18n.t('mqtt-ntp-syncing');
        }
        if (resultEl) {
            resultEl.textContent = '';
        }

        apiPost('/api/mqtt/ntp-sync', {})
            .then(res => {
                if (res && res.success) {
                    Notification.success(i18n.t('mqtt-ntp-sync-ok'), 'MQTT');
                    if (resultEl) {
                        resultEl.style.color = '#67c23a';
                        resultEl.textContent = i18n.t('mqtt-ntp-sync-ok');
                    }
                } else {
                    const errMsg = (res && res.error) ? res.error : i18n.t('mqtt-ntp-sync-fail');
                    Notification.error(errMsg, 'MQTT');
                    if (resultEl) {
                        resultEl.style.color = '#f56c6c';
                        resultEl.textContent = errMsg;
                    }
                }
            })
            .catch(err => {
                console.error('MQTT NTP sync failed:', err);
                Notification.error(i18n.t('mqtt-ntp-sync-fail'), 'MQTT');
                if (resultEl) {
                    resultEl.style.color = '#f56c6c';
                    resultEl.textContent = i18n.t('mqtt-ntp-sync-fail');
                }
            })
            .finally(() => {
                if (btn) {
                    btn.disabled = false;
                    btn.textContent = i18n.t('mqtt-ntp-sync-btn');
                }
                // 3秒后清除结果提示
                setTimeout(() => {
                    if (resultEl) resultEl.textContent = '';
                }, 3000);
            });
    },

    /**
     * MQTT错误码转可读文本
     */
    _mqttErrorCodeToText(code) {
        const map = {
            '-4': 'MQTT_CONNECTION_TIMEOUT',
            '-3': 'MQTT_CONNECTION_LOST',
            '-2': 'MQTT_CONNECT_FAILED',
            '-1': 'MQTT_DISCONNECTED',
            '1': 'MQTT_BAD_PROTOCOL',
            '2': 'MQTT_BAD_CLIENT_ID',
            '3': 'MQTT_UNAVAILABLE',
            '4': 'MQTT_BAD_CREDENTIALS',
            '5': 'MQTT_UNAUTHORIZED'
        };
        return map[String(code)] || ('Error ' + code);
    },

    /**
     * MQTT 状态轮询定时器
     */
    _mqttStatusTimer: null,

    /**
     * 启动MQTT状态轮询
     */
    _startMqttStatusPolling() {
        this._stopMqttStatusPolling();
        this._loadMqttStatus();
        this._mqttStatusTimer = setInterval(() => this._loadMqttStatus(), 5000);
    },

    /**
     * 停止MQTT状态轮询
     */
    _stopMqttStatusPolling() {
        if (this._mqttStatusTimer) {
            clearInterval(this._mqttStatusTimer);
            this._mqttStatusTimer = null;
        }
    },

    /**
     * 加载MQTT实时状态
     */
    _loadMqttStatus() {
        apiGetSilent('/api/mqtt/status')
            .then(res => {
                if (!res || !res.success) return;
                const d = res.data || {};
                const badge = document.getElementById('mqtt-status-badge');
                const serverEl = document.getElementById('mqtt-status-server');
                const clientEl = document.getElementById('mqtt-status-clientid');
                const reconnEl = document.getElementById('mqtt-status-reconnects');

                if (badge) {
                    if (!d.initialized) {
                        badge.className = 'mqtt-status-badge mqtt-status-offline';
                        badge.textContent = i18n.t('mqtt-status-uninit');
                    } else if (d.connected) {
                        badge.className = 'mqtt-status-badge mqtt-status-online';
                        badge.textContent = i18n.t('mqtt-status-connected');
                    } else {
                        badge.className = 'mqtt-status-badge mqtt-status-offline';
                        badge.textContent = i18n.t('mqtt-status-disconnected');
                    }
                }
                if (serverEl) serverEl.textContent = d.server ? (d.server + ':' + d.port) : '--';
                if (clientEl) clientEl.textContent = d.clientId || '--';
                if (reconnEl) reconnEl.textContent = d.reconnectCount ?? 0;
            })
            .catch(err => {
                // 401 表示会话失效或权限不足，停止轮询并给出可见提示，避免“无响应”感知
                if (err && err.status === 401) {
                    this._stopMqttStatusPolling();
                    const badge = document.getElementById('mqtt-status-badge');
                    if (badge) {
                        badge.className = 'mqtt-status-badge mqtt-status-offline';
                        badge.textContent = i18n.t('mqtt-status-auth-fail') || 'Unauthorized';
                    }
                }
            });
    },

    /**
     * 手动更新MQTT状态面板（用于测试连接后即时反馈）
     * @param {boolean} connected 是否连接成功
     * @param {string} server 服务器地址
     * @param {string|number} port 端口
     * @param {string} clientId 客户端ID
     */
    _updateMqttStatusPanel(connected, server, port, clientId) {
        const badge = document.getElementById('mqtt-status-badge');
        const serverEl = document.getElementById('mqtt-status-server');
        const clientEl = document.getElementById('mqtt-status-clientid');

        if (badge) {
            if (connected) {
                badge.className = 'mqtt-status-badge mqtt-status-online';
                badge.textContent = i18n.t('mqtt-status-connected');
            } else {
                badge.className = 'mqtt-status-badge mqtt-status-offline';
                badge.textContent = i18n.t('mqtt-status-disconnected');
            }
        }
        if (serverEl) serverEl.textContent = server ? (server + ':' + port) : '--';
        if (clientEl) clientEl.textContent = clientId || '--';
    },
    
    /**
     * HTTP认证类型切换
     */
    onHttpAuthTypeChange(type) {
        const userGroup = document.getElementById('http-auth-user')?.closest('.pure-control-group');
        const tokenGroup = document.getElementById('http-auth-token')?.closest('.pure-control-group');
        
        if (userGroup) {
            userGroup.style.display = (type === 'basic') ? 'block' : 'none';
        }
        if (tokenGroup) {
            tokenGroup.style.display = (type === 'basic' || type === 'bearer') ? 'block' : 'none';
        }
    },
    
    /**
     * TCP模式切换
     */
    onTcpModeChange(mode) {
        const clientConfig = document.getElementById('tcp-client-config');
        const serverConfig = document.getElementById('tcp-server-config');
        
        if (clientConfig) {
            clientConfig.style.display = (mode === 'client') ? 'block' : 'none';
        }
        if (serverConfig) {
            serverConfig.style.display = (mode === 'server') ? 'block' : 'none';
        }
    },
    
    /**
     * 导出协议配置
     */
    exportProtocolConfig() {
        apiGet('/api/protocol/config')
            .then(res => {
                if (!res || !res.success) {
                    Notification.error(i18n.t('protocol-export-fail'), i18n.t('protocol-config-title'));
                    return;
                }
                const jsonStr = JSON.stringify(res.data || {}, null, 2);
                const blob = new Blob([jsonStr], { type: 'application/json' });
                const url = URL.createObjectURL(blob);
                const a = document.createElement('a');
                a.href = url;
                a.download = 'protocol-config.json';
                document.body.appendChild(a);
                a.click();
                document.body.removeChild(a);
                URL.revokeObjectURL(url);
                Notification.success(i18n.t('protocol-export-ok'), i18n.t('protocol-config-title'));
            })
            .catch(err => {
                console.error('Export protocol config failed:', err);
                Notification.error(i18n.t('protocol-export-fail'), i18n.t('protocol-config-title'));
            });
    },
    
    /**
     * 导入协议配置
     */
    importProtocolConfig() {
        const input = document.createElement('input');
        input.type = 'file';
        input.accept = '.json';
        input.onchange = (e) => {
            const file = e.target.files[0];
            if (!file) return;
            
            const reader = new FileReader();
            reader.onload = (evt) => {
                try {
                    const config = JSON.parse(evt.target.result);
                    if (!confirm(i18n.t('protocol-import-confirm'))) return;
                    
                    // 将导入的JSON转换为flat参数格式并提交
                    const data = {};
                    const flatten = (obj, prefix) => {
                        for (const key in obj) {
                            const val = obj[key];
                            const flatKey = prefix ? prefix + '_' + key : key;
                            if (val !== null && typeof val === 'object' && !Array.isArray(val)) {
                                flatten(val, flatKey);
                            } else {
                                data[flatKey] = typeof val === 'object' ? JSON.stringify(val) : String(val);
                            }
                        }
                    };
                    flatten(config, '');
                    
                    apiPost('/api/protocol/config', data)
                        .then(res => {
                            if (res && res.success) {
                                this._protocolConfig = null;
                                Notification.success(i18n.t('protocol-import-ok'), i18n.t('protocol-config-title'));
                                // 重新加载当前tab
                                const activeTab = document.querySelector('#protocol-page .config-tab.active');
                                if (activeTab) {
                                    const tabId = activeTab.getAttribute('data-tab');
                                    this.loadProtocolConfig(tabId);
                                }
                            } else {
                                Notification.error(i18n.t('protocol-import-fail'), i18n.t('protocol-config-title'));
                            }
                        })
                        .catch(() => {
                            Notification.error(i18n.t('protocol-import-fail'), i18n.t('protocol-config-title'));
                        });
                } catch (parseErr) {
                    Notification.error(i18n.t('protocol-import-invalid'), i18n.t('protocol-config-title'));
                }
            };
            reader.readAsText(file);
        };
        input.click();
    }
};
