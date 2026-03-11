// 应用状态管理
const AppState = {
    currentPage: 'dashboard',
    configTab: 'modbus',
    currentUser: { name: '', role: '', canManageFs: false },
    sidebarCollapsed: false,
    _logAutoRefreshTimer: null,  // 日志自动刷新定时器

    // ============ 初始化 ============
    init() {
        this.setupSidebarToggle();
        this.setupLanguage();
        this.setupConfigTabs();
        this.setupEventListeners();
        this.refreshPage();
    },

    // ============ 会话验证 ============
    refreshPage() {
        const token = localStorage.getItem('auth_token');
        if (!token) {
            this._showLoginPage();
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
                    this._showLoginPage();
                }
            })
            .catch(() => {
                this._showLoginPage();
            });
    },

    _showLoginPage() {
        document.getElementById('login-page').style.display = 'flex';
        document.getElementById('app-container').style.display = 'none';
    },

    _showAppPage() {
        document.getElementById('login-page').style.display = 'none';
        document.getElementById('app-container').style.display = 'block';
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
        const langSelect = document.getElementById('language-select');
        if (langSelect) {
            langSelect.value = i18n.currentLang;
            langSelect.addEventListener('change', e => {
                i18n.setLanguage(e.target.value);
                this.renderDashboard();
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
        
        // 网络配置表单提交（已有单独处理）
        document.querySelectorAll('#network-page form').forEach(form => {
            form.addEventListener('submit', e => {
                e.preventDefault();
                const protocolName = this._getProtocolName(form.id);
                Notification.success(`${protocolName} ${i18n.t('protocol-save-ok-suffix')}`);
                const ok = form.querySelector('.message-success');
                if (ok) {
                    ok.style.display = 'block';
                    setTimeout(() => { ok.style.display = 'none'; }, 3000);
                }
            });
        });
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
        // 切换到AP配网tab时自动加载配网状态和配置
        if (pageId === 'device-page' && tabId === 'dev-provision') {
            this.loadProvisionStatus();
            this.loadProvisionConfig();
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

        // AP配网事件绑定
        const provisionForm = document.getElementById('device-provision-form');
        if (provisionForm) provisionForm.addEventListener('submit', (e) => { e.preventDefault(); this.saveProvisionConfig(); });
        
        const provisionRefreshBtn = document.getElementById('provision-refresh-btn');
        if (provisionRefreshBtn) provisionRefreshBtn.addEventListener('click', () => this.loadProvisionStatus());
        
        const provisionStartBtn = document.getElementById('provision-start-btn');
        if (provisionStartBtn) provisionStartBtn.addEventListener('click', () => this.startProvision());
        
        const provisionStopBtn = document.getElementById('provision-stop-btn');
        if (provisionStopBtn) provisionStopBtn.addEventListener('click', () => this.stopProvision());

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
        document.querySelectorAll('.menu-item').forEach(item => {
            item.classList.toggle('active', item.dataset.page === page);
        });
        const titleKey = `page-title-${page}`;
        const titleEl = document.getElementById('page-title');
        if (titleEl) titleEl.textContent = i18n.t(titleKey);

        document.querySelectorAll('.page').forEach(p => p.classList.remove('active'));
        const target = document.getElementById(`${page}-page`);
        if (target) target.classList.add('active');

        this.currentPage = page;

        // 按需加载数据
        if (page === 'dashboard') this.renderDashboard();
        if (page === 'users') this.loadUsers();
        if (page === 'roles') this.loadRoles();
        if (page === 'peripheral') this.loadPeripherals();
        if (page === 'monitor') this.loadSystemInfo();
        if (page === 'logs') {
            this.loadLogs();
            // 如果自动刷新复选框已选中，启动定时刷新
            const autoRefresh = document.getElementById('log-auto-refresh');
            if (autoRefresh && autoRefresh.checked) {
                this.startLogAutoRefresh();
            }
        } else {
            // 离开日志页面时停止自动刷新
            this.stopLogAutoRefresh();
        }
        
        if (page === 'data') {
            this.loadFileTree('/');
            this.loadFileSystemInfo();
        }
        
        if (page === 'dashboard') {
            this.loadSystemMonitor();
        }
        
        if (page === 'network') {
            this.loadNetworkConfig();
        }
        
        if (page === 'device') {
            this.loadDeviceConfig();
        }
        
        // 切换到协议配置页面时自动加载第一个tab的配置(MQTT)
        if (page === 'protocol') {
            this.loadProtocolConfig('mqtt');
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

    // ============ 系统信息（monitor 页面）============
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

            const editBtn = document.createElement('button');
            editBtn.className = 'pure-button pure-button-small pure-button-primary';
            editBtn.textContent = i18n.t('edit-user');
            editBtn.addEventListener('click', () => this.showEditUserModal(user));
            actionCell.appendChild(editBtn);

            const toggleBtn = document.createElement('button');
            toggleBtn.className = 'pure-button pure-button-small';
            if (user.enabled && !user.isLocked) {
                toggleBtn.classList.add('pure-button-warning');
                toggleBtn.textContent = i18n.t('disable-user');
                toggleBtn.addEventListener('click', () => this.toggleUserStatus(user.username, false));
            } else {
                toggleBtn.classList.add('pure-button-success');
                toggleBtn.textContent = i18n.t('enable-user');
                toggleBtn.addEventListener('click', () => this.toggleUserStatus(user.username, true));
            }
            actionCell.appendChild(toggleBtn);

            // 解锁按钮
            if (user.isLocked) {
                const unlockBtn = document.createElement('button');
                unlockBtn.className = 'pure-button pure-button-small pure-button-primary';
                unlockBtn.textContent = i18n.t('unlock-user');
                unlockBtn.addEventListener('click', () => this.unlockUser(user.username));
                actionCell.appendChild(unlockBtn);
            }

            // 删除按钮（不能删除 admin）
            if (user.username !== 'admin') {
                const delBtn = document.createElement('button');
                delBtn.className = 'pure-button pure-button-small pure-button-error';
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
            
            // 角色名称
            const tdName = document.createElement('td');
            tdName.innerHTML = `<strong>${role.name}</strong>`;
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
            
            // 查看权限按钮
            const viewBtn = document.createElement('button');
            viewBtn.className = 'pure-button pure-button-small';
            viewBtn.textContent = i18n.t('role-view-perms');
            viewBtn.style.marginRight = '5px';
            viewBtn.addEventListener('click', () => this.showRolePermissions(role));
            tdAction.appendChild(viewBtn);
            
            // 仅 admin 角色不可编辑/删除
            if (role.id !== 'admin') {
                // 编辑按钮
                const editBtn = document.createElement('button');
                editBtn.className = 'pure-button pure-button-small pure-button-primary';
                editBtn.textContent = i18n.t('role-edit');
                editBtn.style.marginRight = '5px';
                editBtn.addEventListener('click', () => this.showEditRoleModal(role.id));
                tdAction.appendChild(editBtn);
                
                // 删除按钮
                const delBtn = document.createElement('button');
                delBtn.className = 'pure-button pure-button-small pure-button-error';
                delBtn.textContent = i18n.t('role-delete');
                delBtn.addEventListener('click', () => this.deleteRole(role.id, role.name));
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
        
        // 按分组整理
        const permGroups = {};
        permDefs.forEach(p => {
            if (!permGroups[p.group]) permGroups[p.group] = [];
            permGroups[p.group].push(p);
        });
        
        let html = `<div style="max-height: 400px; overflow-y: auto;">`;
        Object.keys(permGroups).sort().forEach(group => {
            html += `<div style="margin-bottom: 15px;">`;
            html += `<h4 style="margin: 0 0 8px; font-size: 14px; color: #333; border-bottom: 1px solid #eee; padding-bottom: 5px;">${group}</h4>`;
            html += `<div style="display: flex; flex-wrap: wrap; gap: 8px;">`;
            permGroups[group].forEach(perm => {
                const hasPerm = rolePerms.has(perm.id);
                html += `<span style="display: inline-flex; align-items: center; padding: 4px 10px; border-radius: 4px; font-size: 12px; ${hasPerm ? 'background: #e6f7ff; color: #1890ff; border: 1px solid #91d5ff;' : 'background: #f5f5f5; color: #999; border: 1px solid #d9d9d9;'}">`;
                html += `<span style="margin-right: 4px;">${hasPerm ? '✓' : '✗'}</span>`;
                html += `<span title="${perm.description}">${perm.name}</span></span>`;
            });
            html += `</div></div>`;
        });
        html += `</div>`;
        
        // 使用简单的 alert 弹窗显示（或可以创建一个模态窗）
        const div = document.createElement('div');
        div.innerHTML = `
            <div style="position: fixed; top: 0; left: 0; right: 0; bottom: 0; background: rgba(0,0,0,0.5); z-index: 9999; display: flex; justify-content: center; align-items: center;" onclick="this.remove()">
                <div style="background: white; border-radius: 8px; max-width: 600px; width: 90%; max-height: 80vh; overflow: hidden;" onclick="event.stopPropagation()">
                    <div style="background: linear-gradient(135deg, #1890ff 0%, #096dd9 100%); color: white; padding: 15px 20px; display: flex; justify-content: space-between; align-items: center;">
                        <h3 style="margin: 0;">${role.name}${i18n.t('role-detail-suffix')}</h3>
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
                
                // 标记为编辑模式
                modal.dataset.editMode = 'edit';
                modal.dataset.editRoleId = roleId;
                
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
        
        // 按分组整理
        const permGroups = {};
        permDefs.forEach(p => {
            if (!permGroups[p.group]) permGroups[p.group] = [];
            permGroups[p.group].push(p);
        });
        
        Object.keys(permGroups).sort().forEach(group => {
            const groupDiv = document.createElement('div');
            groupDiv.style.cssText = 'margin-bottom: 12px;';
            
            const groupTitle = document.createElement('h5');
            groupTitle.style.cssText = 'margin: 0 0 8px; font-size: 13px; color: #666; border-bottom: 1px solid #eee; padding-bottom: 5px;';
            groupTitle.textContent = group;
            groupDiv.appendChild(groupTitle);
            
            const permList = document.createElement('div');
            permList.style.cssText = 'display: flex; flex-wrap: wrap; gap: 10px;';
            
            permGroups[group].forEach(perm => {
                const label = document.createElement('label');
                label.style.cssText = 'display: flex; align-items: center; cursor: pointer; font-size: 12px;';
                label.innerHTML = `
                    <input type="checkbox" name="role-perm" value="${perm.id}" ${selectedSet.has(perm.id) ? 'checked' : ''} style="margin-right: 4px;">
                    <span title="${perm.description}">${perm.name}</span>
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
            modal.style.display = 'flex';
            // 重置为添加模式
            modal.dataset.editMode = 'add';
            modal.dataset.editUsername = '';
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
        
        // 添加模式时验证
        if (!isEditMode) {
            if (username.length < 3 || username.length > 32) return showErr(i18n.t('validate-username-len'));
            if (!password || !confirmPwd) return showErr(i18n.t('validate-pwd-empty'));
        }
        
        // 密码验证（编辑模式密码可选，但如果填写了就要验证）
        if (password || confirmPwd) {
            if (password !== confirmPwd) return showErr(i18n.t('validate-pwd-mismatch'));
            if (password.length < 6) return showErr(i18n.t('validate-pwd-len'));
        }

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
        // 复用添加用户的 modal
        const modal = document.getElementById('add-user-modal');
        if (!modal) {
            Notification.info(`${i18n.t('edit-user-modal-title')}: ${user.username}`, i18n.t('edit-user-modal-title'));
            return;
        }
        
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
        
        // 标记为编辑模式
        modal.dataset.editMode = 'edit';
        modal.dataset.editUsername = user.username;
        
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

    // ============ 工具方法 ============
    getAuthHeader() {
        const token = localStorage.getItem('auth_token');
        return token ? { 'Authorization': `Bearer ${token}` } : {};
    },
    
    // ============ 日志管理 ============
    
    /**
     * 加载日志内容
     * @param {number} maxLines 最大行数，默认500
     */
    loadLogs(maxLines = 500) {
        const container = document.getElementById('device-log-container');
        const infoSpan = document.getElementById('log-info');
        
        if (!container) return;
        
        apiGet('/api/logs', { lines: maxLines })
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
            
            return `<div class="${className}" style="${this._getLogLineStyle(className)}">${escaped}</div>`;
        }).filter(line => line).join('');
    },
    
    /**
     * 获取日志行样式
     * @param {string} className 日志级别类名
     * @returns {string} CSS样式字符串
     */
    _getLogLineStyle(className) {
        switch (className) {
            case 'log-error':
                return 'color: #f56c6c;';  // 红色
            case 'log-warn':
                return 'color: #e6a23c;';  // 橙色
            case 'log-info':
                return 'color: #67c23a;';  // 绿色
            case 'log-debug':
                return 'color: #909399;';  // 灰色
            default:
                return 'color: #d4d4d4;';  // 默认浅灰色
        }
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
        apiGet('/api/system/fs-info')
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
        
        apiGet('/api/fs/list', { path: path })
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
        
        apiGet('/api/fs/read', { path: path })
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
        
        apiPost('/api/fs/save', { path: this._currentFilePath, content: content })
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
        const submitBtn = document.querySelector('#wifi-form button[type="submit"]');
        const originalText = submitBtn?.innerHTML;
        if (submitBtn) {
            submitBtn.disabled = true;
            submitBtn.innerHTML = i18n.t('net-saving-html');
        }
        
        apiPut('/api/network/config', config)
            .then(res => {
                if (res && res.success) {
                    this._showMessage('wifi-success', true);
                    this._showMessage('wifi-error', false);
                    Notification.success(i18n.t('wifi-save-ok'), i18n.t('net-settings-title'));
                } else {
                    this._showMessage('wifi-success', false);
                    this._showMessage('wifi-error', true);
                    Notification.error(res?.error || i18n.t('net-save-fail'), i18n.t('net-settings-title'));
                }
            })
            .catch(err => {
                console.error('Save network config failed:', err);
                this._showMessage('wifi-success', false);
                this._showMessage('wifi-error', true);
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
     * 保存热点配置
     */
    saveAPConfig() {
        const config = {
            apSSID: document.getElementById('ap-ssid')?.value || '',
            apPassword: document.getElementById('ap-password')?.value || '',
            apChannel: document.getElementById('ap-channel')?.value || '1',
            apHidden: document.getElementById('ap-hidden')?.value || '0'
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
                this._setValue('dev-name',          d.deviceName   || '');
                const desc = document.getElementById('dev-description');
                if (desc) desc.value = d.description || '';
                this._setValue('dev-ntp-enable',    d.enableNTP ? '1' : '0');
                this._setValue('dev-ntp-server1',   d.ntpServer1   || 'https://iot.fastbee.cn/prod-api/iot/tool/ntp');
                this._setValue('dev-ntp-server2',   d.ntpServer2   || 'time.nist.gov');
                this._setValue('dev-timezone',      d.timezone     || 'CST-8');
                this._setValue('dev-sync-interval', d.syncInterval !== undefined ? String(d.syncInterval) : '3600');
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
            description:    document.getElementById('dev-description')?.value || '',
            ntpServer1:     document.getElementById('dev-ntp-server1')?.value || 'https://iot.fastbee.cn/prod-api/iot/tool/ntp',
            ntpServer2:     document.getElementById('dev-ntp-server2')?.value || 'time.nist.gov',
            timezone:       document.getElementById('dev-timezone')?.value || 'CST-8',
            enableNTP:      document.getElementById('dev-ntp-enable')?.value || '1',
            syncInterval:   document.getElementById('dev-sync-interval')?.value || '3600',
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
        apiGet('/api/device/time')
            .then(res => {
                if (!res || !res.success) return;
                this._renderDeviceTime(res.data || {});
            })
            .catch(err => console.error('Load device time failed:', err))
            .finally(() => {
                if (btn) { btn.disabled = false; btn.innerHTML = i18n.t('dev-refresh-html'); }
            });
    },

    syncDeviceTime() {
        const btn = document.getElementById('dev-time-refresh-btn');
        if (btn) { btn.disabled = true; btn.innerHTML = i18n.t('dev-refreshing-html'); }
        apiPost('/api/device/time/sync', {})
            .then(res => {
                if (!res || !res.success) {
                    Notification.warning(i18n.t('dev-time-sync-fail') || 'NTP同步失败', 'NTP');
                    return;
                }
                this._renderDeviceTime(res.data || {});
                if (res.data && res.data.synced) {
                    Notification.success(i18n.t('dev-time-sync-ok') || 'NTP同步成功', 'NTP');
                }
            })
            .catch(err => {
                console.error('Sync device time failed:', err);
                Notification.error(i18n.t('dev-time-sync-fail') || 'NTP同步失败', 'NTP');
            })
            .finally(() => {
                if (btn) { btn.disabled = false; btn.innerHTML = i18n.t('dev-refresh-html'); }
            });
    },

    _renderDeviceTime(d) {
        const setEl   = (id, val)  => { const el = document.getElementById(id); if (el) el.textContent = val || '--'; };
        const setHtml = (id, html) => { const el = document.getElementById(id); if (el) el.innerHTML = html; };
        setEl('dev-time-datetime', d.datetime);
        setHtml('dev-time-synced', d.synced
            ? i18n.t('dev-time-synced-html')
            : i18n.t('dev-time-not-synced-html'));
        if (d.uptime !== undefined) {
            const ms = d.uptime;
            const h  = Math.floor(ms / 3600000);
            const m  = Math.floor((ms % 3600000) / 60000);
            const s  = Math.floor((ms % 60000) / 1000);
            setEl('dev-time-uptime', `${h}${i18n.t('dev-time-uptime-unit')}${m}${i18n.t('dev-time-uptime-min')}${s}${i18n.t('dev-time-uptime-sec')}`);
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
                    modalBody.innerHTML = i18n.t('wifi-scan-fail');
                    return;
                }
                
                const networks = res.data || [];
                
                if (networks.length === 0) {
                    modalBody.innerHTML = i18n.t('wifi-no-network');
                    return;
                }
                
                // 按信号强度排序
                networks.sort((a, b) => b.rssi - a.rssi);
                
                let html = '<div class="wifi-list-container">';
                networks.forEach((net) => {
                    const signalClass = net.rssi > -50 ? 'strong' : net.rssi > -70 ? 'medium' : 'weak';
                    const encryptIcon = net.encryption > 0
                        ? '<i class="fas fa-lock" style="color: #52c41a;"></i>'
                        : '<i class="fas fa-lock-open" style="color: #bbb;"></i>';
                    const securityType = net.encryption > 0 ? 'wpa' : 'none';
                    
                    html += `
                        <div class="wifi-item" data-ssid="${net.ssid}" data-encryption="${securityType}">
                            <div class="wifi-info">
                                <div class="wifi-ssid">${net.ssid}</div>
                                <div class="wifi-meta">
                                    ${encryptIcon} ${net.encryption > 0 ? i18n.t('wifi-encrypted') : i18n.t('wifi-open')} | ${i18n.t('wifi-channel-prefix')}${net.channel}
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
                modalBody.querySelectorAll('.wifi-item').forEach(item => {
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
                modalBody.innerHTML = i18n.t('wifi-scan-fail');
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
            
        // 如果没有配置，添加一个默认空组
        if (!topics || topics.length === 0) {
            topics = [{ topic: '', qos: 0, retain: false, content: '' }];
        }
            
        topics.forEach((topic, index) => {
            this._createMqttPublishTopicElement(topic, index);
        });
    },
        
    /**
     * 创建单个MQTT发布主题配置元素
     */
    _createMqttPublishTopicElement(topicData, index) {
        const container = document.getElementById('mqtt-publish-topics');
        if (!container) return;
            
        const div = document.createElement('div');
        div.className = 'mqtt-publish-topic-item';
        div.dataset.index = index;
            
        div.innerHTML = `
            <span class="mqtt-publish-topic-index">${index + 1}</span>
            <button type="button" class="mqtt-publish-topic-delete" onclick="app.deleteMqttPublishTopic(${index})">${i18n.t('mqtt-delete-topic-btn')}</button>
            <div class="mqtt-publish-topic-grid">
                <div class="mqtt-publish-topic-left">
                    <div class="pure-control-group">
                        <label>${i18n.t('mqtt-publish-label')}</label>
                        <input type="text" class="pure-input-1 mqtt-topic-input" value="${topicData.topic || ''}" placeholder="/topic/path">
                    </div>
                    <div class="pure-control-group">
                        <label>${i18n.t('mqtt-publish-qos-label')}</label>
                        <select class="pure-input-1 mqtt-qos-input">
                            <option value="0" ${topicData.qos === 0 ? 'selected' : ''}>0 - ${i18n.t('mqtt-qos-0') || '最多一次'}</option>
                            <option value="1" ${topicData.qos === 1 ? 'selected' : ''}>1 - ${i18n.t('mqtt-qos-1') || '至少一次'}</option>
                            <option value="2" ${topicData.qos === 2 ? 'selected' : ''}>2 - ${i18n.t('mqtt-qos-2') || '恰好一次'}</option>
                        </select>
                    </div>
                    <div class="pure-control-group">
                        <label class="pure-checkbox">
                            <input type="checkbox" class="mqtt-retain-input" ${topicData.retain ? 'checked' : ''}> ${i18n.t('mqtt-publish-retain-label')}
                        </label>
                    </div>
                </div>
                <div class="mqtt-publish-topic-right">
                    <div class="pure-control-group" style="height: 100%;">
                        <label>${i18n.t('mqtt-publish-content-label')}</label>
                        <textarea class="pure-input-1 mqtt-content-input" rows="5" placeholder="${i18n.t('mqtt-publish-content-placeholder') || '输入要发布的消息内容'}">${topicData.content || ''}</textarea>
                    </div>
                </div>
            </div>
        `;
            
        container.appendChild(div);
    },
        
    /**
     * 添加新的MQTT发布主题配置组
     */
    addMqttPublishTopic() {
        const container = document.getElementById('mqtt-publish-topics');
        if (!container) return;
            
        const index = container.children.length;
        this._createMqttPublishTopicElement({ topic: '', qos: 0, retain: false, content: '' }, index);
    },
        
    /**
     * 删除MQTT发布主题配置组
     */
    deleteMqttPublishTopic(index) {
        const container = document.getElementById('mqtt-publish-topics');
        if (!container) return;
            
        const items = container.querySelectorAll('.mqtt-publish-topic-item');
        if (items[index]) {
            items[index].remove();
        }
            
        // 重新索引
        const remainingItems = container.querySelectorAll('.mqtt-publish-topic-item');
        remainingItems.forEach((item, idx) => {
            item.dataset.index = idx;
            const indexSpan = item.querySelector('.mqtt-publish-topic-index');
            if (indexSpan) indexSpan.textContent = idx + 1;
            const deleteBtn = item.querySelector('.mqtt-publish-topic-delete');
            if (deleteBtn) deleteBtn.setAttribute('onclick', `app.deleteMqttPublishTopic(${idx})`);
        });
            
        // 如果全部删除了，添加一个默认空组
        if (remainingItems.length === 0) {
            this._createMqttPublishTopicElement({ topic: '', qos: 0, retain: false, content: '' }, 0);
        }
    },
        
    /**
     * 收集所有MQTT发布主题配置
     */
    _collectMqttPublishTopics() {
        const container = document.getElementById('mqtt-publish-topics');
        if (!container) return [];
            
        const topics = [];
        const items = container.querySelectorAll('.mqtt-publish-topic-item');
            
        items.forEach(item => {
            const topicInput = item.querySelector('.mqtt-topic-input');
            const qosInput = item.querySelector('.mqtt-qos-input');
            const retainInput = item.querySelector('.mqtt-retain-input');
            const contentInput = item.querySelector('.mqtt-content-input');
                
            if (topicInput) {
                topics.push({
                    topic: topicInput.value || '',
                    qos: parseInt(qosInput?.value || '0'),
                    retain: retainInput?.checked || false,
                    content: contentInput?.value || ''
                });
            }
        });
            
        return topics;
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
            this._setValue('rtu-port', rtu.port || '/dev/ttyS0');
            this._setValue('rtu-baudrate', rtu.baudRate || 19200);
            this._setValue('rtu-databits', rtu.dataBits || 8);
            this._setValue('rtu-stopbits', rtu.stopBits || 1);
            this._setValue('rtu-parity', rtu.parity || 'none');
            this._setValue('rtu-timeout', rtu.timeout || 1000);
        }
        
        if (tabId === 'modbus-tcp' && config.modbusTcp) {
            const tcp = config.modbusTcp;
            this._setValue('tcp-ip', tcp.server || '192.168.1.100');
            this._setValue('tcp-mport', tcp.port || 502);
            this._setValue('tcp-slave-id', tcp.slaveId || 1);
            this._setValue('tcp-mtimeout', tcp.timeout || 5000);
        }
        
        if (tabId === 'mqtt' && config.mqtt) {
            const mqtt = config.mqtt;
            this._setValue('mqtt-broker', mqtt.server || 'iot.fastbee.cn');
            this._setValue('mqtt-port', mqtt.port || 1883);
            this._setValue('mqtt-client-id', mqtt.clientId || '');
            this._setValue('mqtt-username', mqtt.username || '');
            this._setValue('mqtt-password', mqtt.password || '');
            this._setValue('mqtt-alive', mqtt.keepAlive || 60);
            this._setValue('mqtt-subscribe', mqtt.subscribeTopic || '');
            // 新增字段
            this._setValue('mqtt-conn-timeout', mqtt.connectionTimeout ?? 30000);
            this._setCheckbox('mqtt-direct-connect', mqtt.directConnect ?? true);
            this._setCheckbox('mqtt-auto-reconnect', mqtt.autoReconnect ?? true);
            
            // 加载发布主题配置（支持多组）
            this._loadMqttPublishTopics(mqtt.publishTopics || []);
        }
        
        if (tabId === 'http' && config.http) {
            const http = config.http;
            this._setValue('http-url', http.url || 'https://api.example.com');
            this._setValue('http-port', http.port || 80);
            this._setValue('http-method', http.method || 'POST');
            this._setValue('http-timeout', http.timeout || 30);
            this._setValue('http-interval', http.interval || 60);
            this._setValue('http-retry', http.retry || 3);
        }
        
        if (tabId === 'coap' && config.coap) {
            const coap = config.coap;
            this._setValue('coap-server', coap.server || 'coap://example.com');
            this._setValue('coap-port', coap.port || 5683);
            this._setValue('coap-method', coap.method || 'POST');
            this._setValue('coap-path', coap.path || 'sensors/temperature');
        }
        
        if (tabId === 'tcp' && config.tcp) {
            const tcp = config.tcp;
            this._setValue('tcp-server', tcp.server || '192.168.1.200');
            this._setValue('tcp-port', tcp.port || 5000);
            this._setValue('tcp-timeout', tcp.timeout || 5000);
            this._setValue('tcp-keepalive', tcp.keepAlive || 60);
            this._setValue('tcp-retry', tcp.maxRetry || 5);
            this._setValue('tcp-reconnect', tcp.reconnectInterval || 10);
        }
    },
    
    /**
     * 保存协议配置
     */
    saveProtocolConfig(formId) {
        // 收集所有表单数据
        const data = {};
        
        // Modbus RTU
        data.modbusRtu_port = document.getElementById('rtu-port')?.value || '/dev/ttyS0';
        data.modbusRtu_baudRate = document.getElementById('rtu-baudrate')?.value || '19200';
        data.modbusRtu_dataBits = document.getElementById('rtu-databits')?.value || '8';
        data.modbusRtu_stopBits = document.getElementById('rtu-stopbits')?.value || '1';
        data.modbusRtu_parity = document.getElementById('rtu-parity')?.value || 'none';
        data.modbusRtu_timeout = document.getElementById('rtu-timeout')?.value || '1000';
        
        // Modbus TCP
        data.modbusTcp_server = document.getElementById('tcp-ip')?.value || '192.168.1.100';
        data.modbusTcp_port = document.getElementById('tcp-mport')?.value || '502';
        data.modbusTcp_slaveId = document.getElementById('tcp-slave-id')?.value || '1';
        data.modbusTcp_timeout = document.getElementById('tcp-mtimeout')?.value || '5000';
        
        // MQTT
        data.mqtt_server = document.getElementById('mqtt-broker')?.value || 'iot.fastbee.cn';
        data.mqtt_port = document.getElementById('mqtt-port')?.value || '1883';
        data.mqtt_clientId = document.getElementById('mqtt-client-id')?.value || '';
        data.mqtt_username = document.getElementById('mqtt-username')?.value || '';
        data.mqtt_password = document.getElementById('mqtt-password')?.value || '';
        data.mqtt_keepAlive = document.getElementById('mqtt-alive')?.value || '60';
        data.mqtt_subscribeTopic = document.getElementById('mqtt-subscribe')?.value || '';
        data.mqtt_connectionTimeout = document.getElementById('mqtt-conn-timeout')?.value || '30000';
        data.mqtt_directConnect = document.getElementById('mqtt-direct-connect')?.checked ?? true;
        data.mqtt_autoReconnect = document.getElementById('mqtt-auto-reconnect')?.checked ?? true;
        // 收集发布主题配置（多组）
        data.mqtt_publishTopics = this._collectMqttPublishTopics();
        
        // HTTP
        data.http_url = document.getElementById('http-url')?.value || 'https://api.example.com';
        data.http_port = document.getElementById('http-port')?.value || '80';
        data.http_method = document.getElementById('http-method')?.value || 'POST';
        data.http_timeout = document.getElementById('http-timeout')?.value || '30';
        data.http_interval = document.getElementById('http-interval')?.value || '60';
        data.http_retry = document.getElementById('http-retry')?.value || '3';
        
        // CoAP
        data.coap_server = document.getElementById('coap-server')?.value || 'coap://example.com';
        data.coap_port = document.getElementById('coap-port')?.value || '5683';
        data.coap_method = document.getElementById('coap-method')?.value || 'POST';
        data.coap_path = document.getElementById('coap-path')?.value || 'sensors/temperature';
        
        // TCP
        data.tcp_server = document.getElementById('tcp-server')?.value || '192.168.1.200';
        data.tcp_port = document.getElementById('tcp-port')?.value || '5000';
        data.tcp_timeout = document.getElementById('tcp-timeout')?.value || '5000';
        data.tcp_keepAlive = document.getElementById('tcp-keepalive')?.value || '60';
        data.tcp_maxRetry = document.getElementById('tcp-retry')?.value || '5';
        data.tcp_reconnectInterval = document.getElementById('tcp-reconnect')?.value || '10';
        
        const protocolName = this._getProtocolName(formId);
        
        apiPost('/api/protocol/config', data)
            .then(res => {
                if (res && res.success) {
                    // 清除缓存，下次重新加载
                    this._protocolConfig = null;
                    Notification.success(`${protocolName} ${i18n.t('protocol-save-ok-suffix')}`, i18n.t('protocol-config-title'));
                    
                    // 显示成功消息
                    const form = document.getElementById(formId);
                    const ok = form?.querySelector('.message-success');
                    if (ok) {
                        ok.style.display = 'block';
                        setTimeout(() => { ok.style.display = 'none'; }, 3000);
                    }
                } else {
                    Notification.error(res?.message || i18n.t('protocol-save-fail'), i18n.t('protocol-title'));
                }
            })
            .catch(err => {
                console.error('saveProtocolConfig error:', err);
                Notification.error(i18n.t('protocol-save-fail'), i18n.t('protocol-title'));
            });
    },
    
    // ============ GPIO配置 ============
    
    /**
     * 加载GPIO列表
     */
    loadGpioList() {
        const tbody = document.getElementById('gpio-table-body');
        if (!tbody) return;
        
        tbody.innerHTML = i18n.t('gpio-loading-html');
        
        apiGet('/api/gpio/config')
            .then(res => {
                if (!res || !res.success) {
                    tbody.innerHTML = i18n.t('gpio-fail-html');
                    return;
                }
                
                const pins = res.data?.pins || [];
                
                if (pins.length === 0) {
                    tbody.innerHTML = i18n.t('gpio-empty-html');
                    return;
                }
                
                let html = '';
                pins.forEach(pin => {
                    const modeNames = {
                        1: i18n.t('gpio-mode-1'), 2: i18n.t('gpio-mode-2'), 3: i18n.t('gpio-mode-3'),
                        4: i18n.t('gpio-mode-4'), 5: i18n.t('gpio-mode-5'), 6: i18n.t('gpio-mode-6'), 7: i18n.t('gpio-mode-7')
                    };
                    const modeName = modeNames[pin.mode] || i18n.t('gpio-status-unknown');
                    const stateText = pin.state === 1 ? i18n.t('gpio-state-high') : i18n.t('gpio-state-low');
                    const stateColor = pin.state === 1 ? '#52c41a' : '#999';
                    // 仅输出模式才支持切换: 2=数字输出, 6=模拟输出, 7=PWM
                    const canToggle = (pin.mode === 2 || pin.mode === 6 || pin.mode === 7);
                    const toggleBtn = canToggle
                        ? `<button class="pure-button pure-button-small" onclick="app.toggleGpio(${pin.pin})">${i18n.t('gpio-toggle')}</button>`
                        : '';
                    
                    html += `
                        <tr>
                            <td>${pin.pin}</td>
                            <td>${pin.name}</td>
                            <td>${modeName}</td>
                            <td style="color: ${stateColor};">${stateText}</td>
                            <td>
                                <button class="pure-button pure-button-small" onclick="app.editGpio(${pin.pin}, '${pin.name}', ${pin.mode}, ${pin.state || 0})">${i18n.t('gpio-edit')}</button>
                                ${toggleBtn}
                                <button class="pure-button pure-button-small" style="background: #ff4d4f; color: white;" onclick="app.deleteGpio(${pin.pin})">${i18n.t('gpio-delete')}</button>
                            </td>
                        </tr>
                    `;
                });
                
                tbody.innerHTML = html;
            })
            .catch(err => {
                console.error('Load GPIO list failed:', err);
                tbody.innerHTML = i18n.t('gpio-fail-html');
            });
    },
    
    /**
     * 打开GPIO模态框
     */
    openGpioModal(isEdit = false, pinData = null) {
        const modal = document.getElementById('gpio-modal');
        const title = document.getElementById('gpio-modal-title');
        const form = document.getElementById('gpio-form');
        
        if (!modal) return;
        
        form.reset();
        document.getElementById('gpio-error').style.display = 'none';
        
        if (isEdit && pinData) {
            title.textContent = i18n.t('gpio-edit-modal-title');
            document.getElementById('gpio-original-pin').value = pinData.pin;
            document.getElementById('gpio-pin-input').value = pinData.pin;
            document.getElementById('gpio-name-input').value = pinData.name;
            document.getElementById('gpio-mode-input').value = pinData.mode;
            document.getElementById('gpio-default-input').value = pinData.state || 0;
            document.getElementById('gpio-pin-input').disabled = true;
        } else {
            title.textContent = i18n.t('gpio-add-modal-title');
            document.getElementById('gpio-original-pin').value = '';
            document.getElementById('gpio-pin-input').disabled = false;
        }
        
        modal.style.display = 'flex';
    },
    
    /**
     * 关闭GPIO模态框
     */
    closeGpioModal() {
        const modal = document.getElementById('gpio-modal');
        if (modal) modal.style.display = 'none';
    },
    
    /**
     * 保存GPIO配置
     */
    saveGpioConfig() {
        const originalPin = document.getElementById('gpio-original-pin').value;
        const pin = document.getElementById('gpio-pin-input').value;
        const name = document.getElementById('gpio-name-input').value.trim();
        const mode = document.getElementById('gpio-mode-input').value;
        const defaultValue = document.getElementById('gpio-default-input').value;
        const errEl = document.getElementById('gpio-error');
        
        if (!pin || !name) {
            errEl.textContent = i18n.t('gpio-validate-pin-name');
            errEl.style.display = 'block';
            return;
        }
        
        const isEdit = originalPin !== '';
        
        // 安全起见：禁用按钮防止重复提交
        const saveBtn = document.getElementById('save-gpio-btn');
        const origText = saveBtn.textContent;
        saveBtn.disabled = true;
        saveBtn.textContent = i18n.t('gpio-saving-text');
        
        const data = { pin, name, mode, defaultValue };
        
        apiPost('/api/gpio/config', data)
            .then(res => {
                if (res && res.success) {
                    this.closeGpioModal();
                    this.loadGpioList();
                    Notification.success(isEdit ? i18n.t('gpio-update-ok') : i18n.t('gpio-add-ok'), i18n.t('gpio-config-title'));
                } else {
                    errEl.textContent = res?.error || i18n.t('gpio-save-fail');
                    errEl.style.display = 'block';
                }
            })
            .catch(err => {
                console.error('Save GPIO failed:', err);
                errEl.textContent = i18n.t('gpio-save-fail');
                errEl.style.display = 'block;';
            })
            .finally(() => {
                saveBtn.disabled = false;
                saveBtn.textContent = origText;
            });
    },
    
    /**
     * 编辑GPIO
     */
    editGpio(pin, name, mode, state) {
        this.openGpioModal(true, { pin, name, mode, state });
    },
    
    /**
     * 删除GPIO
     */
    deleteGpio(pin) {
        if (!confirm(`${i18n.t('gpio-confirm-delete')}${pin} ${i18n.t('gpio-confirm-suffix')}`)) return;
        
        apiPost('/api/gpio/delete', { pin: String(pin) })
            .then(res => {
                if (res && res.success) {
                    Notification.success(`GPIO ${pin}${i18n.t('gpio-deleted-prefix')}`, i18n.t('gpio-config-title'));
                    this.loadGpioList();
                } else {
                    Notification.error(res?.error || i18n.t('gpio-toggle-fail'), i18n.t('gpio-config-title'));
                }
            })
            .catch(err => {
                console.error('Delete GPIO failed:', err);
                Notification.error(i18n.t('gpio-delete-fail'), i18n.t('gpio-config-title'));
            });
    },
    
    /**
     * 切换GPIO状态
     */
    toggleGpio(pin) {
        apiPost('/api/gpio/write', { pin: String(pin), state: 'toggle' })
            .then(res => {
                if (res && res.success) {
                    Notification.success(i18n.t('gpio-toggle-ok'), i18n.t('gpio-config-title'));
                    this.loadGpioList();
                } else {
                    Notification.error(res?.error || i18n.t('gpio-toggle-fail'), i18n.t('gpio-config-title'));
                }
            })
            .catch(err => {
                console.error('Toggle GPIO failed:', err);
                Notification.error(i18n.t('gpio-toggle-fail'), i18n.t('gpio-config-title'));
            });
    },
    
    /**
     * 保存GPIO配置到文件 (gpio.json)
     */
    saveGpioToFile() {
        apiPost('/api/gpio/save', {})
            .then(res => {
                if (res && res.success) {
                    console.log('[GPIO] 配置已保存到 /config/gpio.json');
                }
            })
            .catch(err => {
                console.error('Save GPIO to file failed:', err);
            });
    },
    
    // ============ 外设接口管理（新版） ============
    
    /**
     * 加载外设列表
     */
    loadPeripherals(filterType = '') {
        const tbody = document.getElementById('peripheral-table-body');
        if (!tbody) return;
        
        tbody.innerHTML = '<tr><td colspan="6" style="text-align: center; color: #999;">' + i18n.t('peripheral-loading') + '</td></tr>';
        
        let url = '/api/peripherals';
        if (filterType) {
            url += '?category=' + filterType;
        }
        
        apiGet(url)
            .then(res => {
                if (!res || !res.success) {
                    tbody.innerHTML = '<tr><td colspan="6" style="text-align: center; color: #f56c6c;">' + i18n.t('peripheral-load-fail') + '</td></tr>';
                    return;
                }
                
                const peripherals = res.data || [];
                
                if (peripherals.length === 0) {
                    tbody.innerHTML = '<tr><td colspan="6" style="text-align: center; color: #999;">' + i18n.t('peripheral-empty') + '</td></tr>';
                    return;
                }
                
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
                                <button class="pure-button pure-button-small" onclick="app.editPeripheral('${periph.id}')">${i18n.t('peripheral-edit')}</button>
                                <button class="pure-button pure-button-small" onclick="app.togglePeripheral('${periph.id}')">${periph.enabled ? i18n.t('peripheral-disable') : i18n.t('peripheral-enable')}</button>
                                <button class="pure-button pure-button-small" style="background: #ff4d4f; color: white;" onclick="app.deletePeripheral('${periph.id}')">${i18n.t('peripheral-delete')}</button>
                            </td>
                        </tr>
                    `;
                });
                
                tbody.innerHTML = html;
            })
            .catch(err => {
                console.error('Load peripherals failed:', err);
                tbody.innerHTML = '<tr><td colspan="6" style="text-align: center; color: #f56c6c;">' + i18n.t('peripheral-load-fail') + '</td></tr>';
            });
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
        
        // 根据类型显示对应参数组
        if (type >= 11 && type <= 21) {
            // GPIO类型
            const gpioParams = document.getElementById('gpio-params');
            if (gpioParams) {
                gpioParams.style.display = 'block';
                // 显示/隐藏PWM相关字段
                const pwmOnly = gpioParams.querySelectorAll('.pwm-only');
                pwmOnly.forEach(el => {
                    el.style.display = (type === 17) ? 'block' : 'none'; // 17 = PWM输出
                });
            }
        } else if (type === 1) {
            // UART
            const uartParams = document.getElementById('uart-params');
            if (uartParams) uartParams.style.display = 'block';
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
                    document.getElementById('peripheral-enabled-input').value = data.enabled ? '1' : '0';
                    
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
                        if (data.params.inverted !== undefined) {
                            document.getElementById('gpio-inverted').value = data.params.inverted ? '1' : '0';
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
        const enabled = document.getElementById('peripheral-enabled-input').value;
        const pinsStr = document.getElementById('peripheral-pins-input').value.trim();
        const errEl = document.getElementById('peripheral-error');
        
        if (!name || !type || !pinsStr) {
            errEl.textContent = i18n.t('peripheral-validate-required');
            errEl.style.display = 'block';
            return;
        }
        
        const isEdit = originalId !== '';
        
        // 构建数据对象
        const data = {
            id: isEdit ? originalId : (id || undefined),  // 编辑时使用原始ID，新增时如果为空则后端自动生成
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
            data.inverted = document.getElementById('gpio-inverted')?.value || '0';
            if (typeNum === 17) { // PWM
                data.pwmFrequency = document.getElementById('gpio-pwm-freq')?.value || '1000';
                data.pwmResolution = document.getElementById('gpio-pwm-resolution')?.value || '8';
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
        }
        
        // 安全起见：禁用按钮防止重复提交
        const saveBtn = document.getElementById('save-peripheral-btn');
        const origText = saveBtn?.textContent;
        if (saveBtn) {
            saveBtn.disabled = true;
            saveBtn.textContent = i18n.t('peripheral-saving-text');
        }
        
        const url = isEdit ? '/api/peripherals/' : '/api/peripherals';
        const method = isEdit ? apiPut : apiPost;
        
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
                const statusEl = document.getElementById('provision-status');
                if (statusEl) {
                    if (d.active) {
                        statusEl.textContent = i18n.t('provision-active');
                        statusEl.className = 'badge badge-success';
                    } else {
                        statusEl.textContent = i18n.t('provision-inactive');
                        statusEl.className = 'badge badge-info';
                    }
                }
                
                this._setTextContent('provision-ap-name', d.apSSID || '--');
                this._setTextContent('provision-ap-ip', d.apIP || '192.168.4.1');
                this._setTextContent('provision-clients', d.clients || '0');
                
                // 更新按钮状态
                const startBtn = document.getElementById('provision-start-btn');
                const stopBtn = document.getElementById('provision-stop-btn');
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
        const startBtn = document.getElementById('provision-start-btn');
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
        const stopBtn = document.getElementById('provision-stop-btn');
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
                    if (remainingWrap) remainingWrap.style.display = 'flex';
                    if (remaining) remaining.textContent = (d.remainingTime || 0) + i18n.t('ble-remaining-unit');
                    if (startBtn) startBtn.style.display = 'none';
                    if (stopBtn) { stopBtn.style.display = 'inline-block'; stopBtn.disabled = false; }
                } else {
                    if (badge) { badge.className = 'status-badge status-offline'; badge.textContent = i18n.t('ble-inactive'); }
                    if (deviceName) deviceName.textContent = '--';
                    if (remainingWrap) remainingWrap.style.display = 'none';
                    if (startBtn) { startBtn.style.display = 'inline-block'; startBtn.disabled = false; }
                    if (stopBtn) stopBtn.style.display = 'none';
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
        apiGet('/api/ota/status')
            .then(res => {
                if (!res) return;
                
                const badge = document.getElementById('ota-status-badge');
                const progressWrap = document.getElementById('ota-progress-wrap');
                const progressBar = document.getElementById('ota-progress-bar');
                const progressText = document.getElementById('ota-progress-text');
                
                if (res.status === 'OTA ready') {
                    if (badge) { badge.className = 'status-badge status-online'; badge.textContent = i18n.t('ota-ready'); }
                    if (progressWrap) progressWrap.style.display = 'none';
                } else if (res.progress > 0 && res.progress < 100) {
                    if (badge) { badge.className = 'status-badge status-warning'; badge.textContent = i18n.t('ota-in-progress'); }
                    if (progressWrap) progressWrap.style.display = 'block';
                    if (progressBar) progressBar.style.width = res.progress + '%';
                    if (progressText) progressText.textContent = res.progress + '%';
                }
            })
            .catch(err => {
                console.error('加载OTA状态失败:', err);
            });
        
        // 同时加载系统信息以获取版本等
        apiGet('/api/system/info')
            .then(res => {
                if (!res || !res.success) return;
                const d = res.data || {};
                
                this._setValue('ota-current-version', d.firmwareVersion || '--');
                
                const flashSize = d.flashChipSize || 0;
                const freeSketch = d.freeSketchSpace || 0;
                
                const flashSizeEl = document.getElementById('ota-flash-size');
                const freeSpaceEl = document.getElementById('ota-free-space');
                
                if (flashSizeEl) flashSizeEl.textContent = flashSize > 0 ? (flashSize / 1024 / 1024).toFixed(2) + ' MB' : '--';
                if (freeSpaceEl) freeSpaceEl.textContent = freeSketch > 0 ? (freeSketch / 1024).toFixed(0) + ' KB' : '--';
            })
            .catch(err => {
                console.error('加载系统信息失败:', err);
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
        if (progressWrap) progressWrap.style.display = 'block';
        
        apiPost('/api/ota/url', { url })
            .then(res => {
                if (res && res.success) {
                    Notification.success(i18n.t('ota-start-ok'), i18n.t('ota-title'));
                    // 开始轮询状态
                    this._pollOtaProgress();
                } else {
                    Notification.error(res?.message || i18n.t('ota-start-fail'), i18n.t('ota-title'));
                    if (progressWrap) progressWrap.style.display = 'none';
                }
            })
            .catch(err => {
                Notification.error(i18n.t('ota-start-fail') + ': ' + (err.message || err), i18n.t('ota-title'));
                if (progressWrap) progressWrap.style.display = 'none';
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
        if (progressWrap) progressWrap.style.display = 'block';
        
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
            if (progressWrap) progressWrap.style.display = 'none';
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
    }
};
