// 应用状态管理
const AppState = {
    currentPage: 'dashboard',
    configTab: 'modbus',
    currentUser: { name: '', role: '' },
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
        this.sidebarCollapsed ? this.expandSidebar() : this.collapseSidebar();
    },

    collapseSidebar() {
        const sidebar = document.getElementById('sidebar');
        if (sidebar) {
            sidebar.classList.add('collapsed');
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
        ['#config-page', '#network-page', '#device-page'].forEach(pageSelector => {
            const page = document.querySelector(pageSelector);
            if (!page) return;
            page.querySelectorAll('.config-tab').forEach(tab => {
                tab.addEventListener('click', () => {
                    this.showConfigTab(pageSelector.replace('#', ''), tab.getAttribute('data-tab'));
                });
            });
        });

        // 协议配置表单提交
        document.querySelectorAll('#config-page form').forEach(form => {
            form.addEventListener('submit', e => {
                e.preventDefault();
                this.saveProtocolConfig(form.id);
            });
        });
        
        // 网络配置表单提交（已有单独处理）
        document.querySelectorAll('#network-page form').forEach(form => {
            form.addEventListener('submit', e => {
                e.preventDefault();
                const protocolName = this._getProtocolName(form.id);
                Notification.success(`${protocolName}配置保存成功！`);
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
        if (pageId === 'config-page') {
            this.loadProtocolConfig(tabId);
        }
        
        // 切换到网络状态tab时自动刷新
        if (pageId === 'network-page' && tabId === 'netstatus') {
            this.loadNetworkStatus();
        }
        // 切换到NTP时间tab时自动加载时间
        if (pageId === 'device-page' && tabId === 'dev-ntp') {
            this.loadDeviceTime();
        }
        // 切换到系统操作tab时自动加载硬件信息
        if (pageId === 'device-page' && tabId === 'dev-system') {
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
        ['close-add-user-modal', 'cancel-add-user-btn'].forEach(id => {
            const el = document.getElementById(id);
            if (el) el.addEventListener('click', () => closeId('add-user-modal'));
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
        
        // WiFi表单提交
        const wifiForm = document.getElementById('wifi-form');
        if (wifiForm) {
            wifiForm.addEventListener('submit', (e) => {
                e.preventDefault();
                this.saveNetworkConfig();
            });
        }
        
        // 静态IP切换
        const useStaticIp = document.getElementById('use-static-ip');
        if (useStaticIp) {
            useStaticIp.addEventListener('change', (e) => {
                const section = document.getElementById('static-ip-section');
                if (section) {
                    section.style.display = e.target.value === '1' ? 'block' : 'none';
                }
            });
        }
        
        // ============ GPIO配置事件绑定 ============
        
        // 新增GPIO按钮
        const addGpioBtn = document.getElementById('add-gpio-btn');
        if (addGpioBtn) addGpioBtn.addEventListener('click', () => this.openGpioModal());
        
        // 关闭GPIO模态框
        const closeGpioModal = document.getElementById('close-gpio-modal');
        if (closeGpioModal) closeGpioModal.addEventListener('click', () => this.closeGpioModal());
        
        // 取消GPIO按钮
        const cancelGpioBtn = document.getElementById('cancel-gpio-btn');
        if (cancelGpioBtn) cancelGpioBtn.addEventListener('click', () => this.closeGpioModal());
        
        // 保存GPIO按钮
        const saveGpioBtn = document.getElementById('save-gpio-btn');
        if (saveGpioBtn) saveGpioBtn.addEventListener('click', () => this.saveGpioConfig());
        
        // ============ 网络状态事件绑定 ============
        
        // 刷新网络状态按钮
        const netStatusRefreshBtn = document.getElementById('net-status-refresh-btn');
        if (netStatusRefreshBtn) netStatusRefreshBtn.addEventListener('click', () => this.loadNetworkStatus());
        
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
        
        // 时间刷新按钮
        const devTimeRefreshBtn = document.getElementById('dev-time-refresh-btn');
        if (devTimeRefreshBtn) devTimeRefreshBtn.addEventListener('click', () => this.loadDeviceTime());
        
        // 重启按钮
        const devRestartBtn = document.getElementById('dev-restart-btn');
        if (devRestartBtn) devRestartBtn.addEventListener('click', () => this.restartDevice());

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
            Notification.warning('请输入用户名和密码', '登录失败');
            return;
        }

        const submitBtn = document.querySelector('#login-form button[type="submit"]');
        const originalText = submitBtn ? submitBtn.innerHTML : '';
        if (submitBtn) { submitBtn.innerHTML = '<i class="fas fa-spinner fa-spin"></i> 登录中...'; submitBtn.disabled = true; }

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

                    this._showAppPage();
                    this.renderDashboard();
                    this.loadSystemMonitor();  // 加载监控仪表盘数据
                    this.loadUsers();
                    Notification.success('登录成功', '欢迎');
                } else {
                    Notification.error((res && res.error) || '用户名或密码错误', '登录失败');
                }
            })
            .catch(() => { /* 错误已由拦截器处理 */ })
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
        if (page === 'gpio') this.loadGpioList();
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
        
        // 切换到协议配置页面时自动加载第一个tab的配置
        if (page === 'config') {
            this.loadProtocolConfig('modbus-rtu');
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
                        ${d.networkConnected ? '网络已连接 IP: ' + (d.ipAddress || '') : '网络未连接'}
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
            tbody.innerHTML = '<tr><td colspan="5" style="text-align:center;color:#888">暂无用户数据</td></tr>';
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
                badge.textContent = '已锁定';
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
                unlockBtn.textContent = '解锁';
                unlockBtn.addEventListener('click', () => this.unlockUser(user.username));
                actionCell.appendChild(unlockBtn);
            }

            // 删除按钮（不能删除 admin）
            if (user.username !== 'admin') {
                const delBtn = document.createElement('button');
                delBtn.className = 'pure-button pure-button-small pure-button-error';
                delBtn.textContent = i18n.t('delete-user');
                delBtn.addEventListener('click', () => {
                    if (confirm(`确定要删除用户 ${user.username} 吗？`)) {
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
                tdType.innerHTML = '<span style="background: #fff1f0; color: #f5222d; padding: 2px 8px; border-radius: 4px; font-size: 12px;">超级管理员</span>';
            } else if (role.isBuiltin) {
                tdType.innerHTML = '<span style="background: #f6ffed; color: #52c41a; padding: 2px 8px; border-radius: 4px; font-size: 12px;">内置角色</span>';
            } else {
                tdType.innerHTML = '<span style="background: #f0f5ff; color: #2f54eb; padding: 2px 8px; border-radius: 4px; font-size: 12px;">自定义</span>';
            }
            row.appendChild(tdType);
            
            // 操作
            const tdAction = document.createElement('td');
            
            // 查看权限按钮
            const viewBtn = document.createElement('button');
            viewBtn.className = 'pure-button pure-button-small';
            viewBtn.textContent = '查看权限';
            viewBtn.style.marginRight = '5px';
            viewBtn.addEventListener('click', () => this.showRolePermissions(role));
            tdAction.appendChild(viewBtn);
            
            // 仅 admin 角色不可编辑/删除
            if (role.id !== 'admin') {
                // 编辑按钮
                const editBtn = document.createElement('button');
                editBtn.className = 'pure-button pure-button-small pure-button-primary';
                editBtn.textContent = '编辑';
                editBtn.style.marginRight = '5px';
                editBtn.addEventListener('click', () => this.showEditRoleModal(role.id));
                tdAction.appendChild(editBtn);
                
                // 删除按钮
                const delBtn = document.createElement('button');
                delBtn.className = 'pure-button pure-button-small pure-button-error';
                delBtn.textContent = '删除';
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
                        <h3 style="margin: 0;">${role.name} - 权限详情</h3>
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
        if (title) title.textContent = '新增角色';
        
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
                    Notification.error('角色不存在', '错误');
                    return;
                }
                
                // 设置标题
                const title = document.getElementById('role-modal-title');
                if (title) title.textContent = '编辑角色';
                
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
            Notification.error('弹窗元素不存在', '错误');
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
            Notification.error(msg, isEditMode ? '编辑角色失败' : '新增角色失败');
        };
        
        if (!id) return showErr('请输入角色ID！');
        if (!name) return showErr('请输入角色名称！');
        
        // 获取选中的权限
        const permCheckboxes = document.querySelectorAll('input[name="role-perm"]:checked');
        const permissions = Array.from(permCheckboxes).map(cb => cb.value).join(',');
        
        if (errDiv) errDiv.style.display = 'none';
        
        const btn = document.getElementById('confirm-role-btn');
        if (btn) { btn.disabled = true; btn.textContent = '保存中...'; }
        
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
                    throw new Error(res.error || '更新角色失败');
                });
        } else {
            // 新增模式
            apiCall = apiPost('/api/roles', { id, name, description })
                .then(res => {
                    if (res && res.success) {
                        return apiPost('/api/roles/permissions', { id, permissions });
                    }
                    throw new Error(res.error || '创建角色失败');
                });
        }
        
        apiCall
            .then(res => {
                if (res && res.success) {
                    Notification.success(`角色 ${name} ${isEditMode ? '修改' : '创建'}成功`, '角色管理');
                    if (modal) modal.style.display = 'none';
                    this.loadRoles();
                } else {
                    showErr((res && res.error) || '操作失败');
                }
            })
            .catch(err => {
                showErr(err.message || '操作失败');
            })
            .finally(() => {
                if (btn) { btn.disabled = false; btn.textContent = '保存'; }
            });
    },
    
    deleteRole(roleId, roleName) {
        if (!confirm(`确定要删除角色 "${roleName || roleId}" 吗？`)) return;
        
        apiPost('/api/roles/delete', { id: roleId })
            .then(res => {
                if (res && res.success) {
                    Notification.success(`角色 ${roleName || roleId} 已删除`, '删除成功');
                    this.loadRoles();
                } else {
                    Notification.error((res && res.error) || '删除失败', '操作失败');
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
            Notification.error(msg, '修改密码失败');
        };

        if (!oldPwd || !newPwd || !confirmPwd) return showErr('请填写所有字段！');
        if (newPwd !== confirmPwd) return showErr(i18n.t('password-error') || '新密码与确认密码不一致！');
        if (newPwd.length < 6) return showErr('新密码长度至少6位！');

        if (errDiv) errDiv.style.display = 'none';

        const btn = document.getElementById('confirm-password-btn');
        if (btn) { btn.disabled = true; btn.textContent = '提交中...'; }

        apiPost('/api/auth/change-password', { oldPassword: oldPwd, newPassword: newPwd })
            .then(res => {
                if (res && res.success) {
                    Notification.success('密码修改成功，请重新登录', '修改成功');
                    const modal = document.getElementById('change-password-modal');
                    if (modal) modal.style.display = 'none';
                    // 修改密码后后端会踢出所有会话，需重新登录
                    setTimeout(() => {
                        localStorage.removeItem('auth_token');
                        this._showLoginPage();
                    }, 1500);
                } else {
                    showErr((res && res.error) || '密码修改失败，请检查原密码是否正确');
                }
            })
            .catch(() => {})
            .finally(() => { if (btn) { btn.disabled = false; btn.textContent = '确认修改'; } });
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
        if (title) title.textContent = '添加用户';
        
        // 修改按钮文本
        const confirmBtn = document.getElementById('confirm-add-user-btn');
        if (confirmBtn) confirmBtn.textContent = '确认添加';
        
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
            Notification.error(msg, isEditMode ? '编辑用户失败' : '添加用户失败');
        };

        if (!username) return showErr('请输入用户名！');
        
        // 添加模式时验证
        if (!isEditMode) {
            if (username.length < 3 || username.length > 32) return showErr('用户名长度3-32位！');
            if (!password || !confirmPwd) return showErr('请输入密码！');
        }
        
        // 密码验证（编辑模式密码可选，但如果填写了就要验证）
        if (password || confirmPwd) {
            if (password !== confirmPwd) return showErr('密码与确认密码不一致！');
            if (password.length < 6) return showErr('密码长度至少6位！');
        }

        if (errDiv) errDiv.style.display = 'none';

        const btn = document.getElementById('confirm-add-user-btn');
        if (btn) { btn.disabled = true; btn.textContent = isEditMode ? '保存中...' : '添加中...'; }

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
                    Notification.success(`用户 ${username} ${isEditMode ? '修改' : '添加'}成功`, isEditMode ? '编辑用户' : '添加用户');
                    if (modal) modal.style.display = 'none';
                    this.loadUsers(); // 刷新列表
                } else {
                    showErr((res && res.error) || (isEditMode ? '修改用户失败' : '添加用户失败，用户名可能已存在'));
                }
            })
            .catch(() => {})
            .finally(() => { if (btn) { btn.disabled = false; btn.textContent = '确认添加'; } });
    },

    // ============ 编辑用户（复用添加用户弹窗）============
    showEditUserModal(user) {
        // 复用添加用户的 modal
        const modal = document.getElementById('add-user-modal');
        if (!modal) {
            Notification.info(`编辑用户功能：${user.username}（角色：${user.role}）`, '编辑用户');
            return;
        }
        
        // 修改标题
        const title = document.getElementById('add-user-title');
        if (title) title.textContent = '编辑用户';
        
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
        if (confirmBtn) confirmBtn.textContent = '保存修改';
        
        // 显示弹窗
        modal.style.display = 'flex';
        
        // 清除错误提示
        const errDiv = document.getElementById('add-user-error');
        if (errDiv) errDiv.style.display = 'none';
    },

    // ============ 切换用户状态（启用/禁用）============
    toggleUserStatus(username, enable) {
        const action = enable ? '启用' : '禁用';
        if (!confirm(`确定要${action}用户 ${username} 吗？`)) return;

        apiPost('/api/users/update', { username, enabled: enable ? 'true' : 'false' })
            .then(res => {
                if (res && res.success) {
                    Notification.success(`用户 ${username} 已${action}`, '状态更新');
                    this.loadUsers();
                } else {
                    Notification.error((res && res.error) || `${action}失败`, '操作失败');
                }
            })
            .catch(() => {});
    },

    // ============ 解锁用户 ============
    unlockUser(username) {
        if (!confirm(`确定要解锁账户 ${username} 吗？`)) return;
        apiPost('/api/users/unlock-account', { username })
            .then(res => {
                if (res && res.success) {
                    Notification.success(`账户 ${username} 已解锁`, '解锁成功');
                    this.loadUsers();
                } else {
                    Notification.error((res && res.error) || '解锁失败', '操作失败');
                }
            })
            .catch(() => {});
    },

    // ============ 删除用户 ============
    deleteUser(username) {
        if (!confirm(`确定要删除用户 ${username} 吗？`)) return;
        
        apiPost('/api/users/delete', { username })
            .then(res => {
                if (res && res.success) {
                    Notification.success(`用户 ${username} 已删除`, '删除成功');
                    this.loadUsers();
                } else {
                    Notification.error((res && res.error) || '删除失败', '操作失败');
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
                Notification.success('已成功退出登录', '退出登录');
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
                    container.innerHTML = '<div style="color: #f56c6c; text-align: center; padding: 20px;">加载日志失败</div>';
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
                    let infoText = `${lineCount} 行 | ${sizeStr}`;
                    if (truncated) infoText += ' (部分显示)';
                    infoSpan.textContent = infoText;
                }
                
                // 格式化并显示日志内容
                if (!content || content.trim() === '') {
                    container.innerHTML = '<div style="color: #909399; text-align: center; padding: 20px;">日志文件为空</div>';
                } else {
                    container.innerHTML = this._formatLogContent(content);
                    // 滚动到底部
                    container.scrollTop = container.scrollHeight;
                }
            })
            .catch(err => {
                console.error('Load logs failed:', err);
                container.innerHTML = '<div style="color: #f56c6c; text-align: center; padding: 20px;">加载日志失败</div>';
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
        if (!confirm('确定要清空所有设备日志吗？此操作不可恢复。')) return;
        
        const btn = document.getElementById('clear-logs-btn');
        if (btn) {
            btn.disabled = true;
            btn.innerHTML = '<i class="fas fa-spinner fa-spin"></i> 清空中...';
        }
        
        apiPost('/api/logs/clear', {})
            .then(res => {
                if (res && res.success) {
                    Notification.success('日志已清空', '操作成功');
                    this.loadLogs();  // 重新加载
                } else {
                    Notification.error((res && res.error) || '清空日志失败', '操作失败');
                }
            })
            .catch(err => {
                console.error('Clear logs failed:', err);
                Notification.error('清空日志失败', '操作失败');
            })
            .finally(() => {
                if (btn) {
                    btn.disabled = false;
                    btn.innerHTML = '<i class="fas fa-trash"></i> 清空日志';
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
                        infoSpan.textContent = `总空间: ${total} | 已用: ${used}`;
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
        
        treeContainer.innerHTML = '<div style="color: #999;">加载中...</div>';
        
        apiGet('/api/fs/list', { path: path })
            .then(res => {
                if (!res || !res.success) {
                    treeContainer.innerHTML = '<div style="color: #f56c6c;">加载失败</div>';
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
                    html = '<div style="color: #999; padding: 20px;">空目录</div>';
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
                treeContainer.innerHTML = '<div style="color: #f56c6c;">加载失败</div>';
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
            statusDiv.textContent = '此文件类型不支持编辑';
            return;
        }
        
        pathSpan.textContent = path;
        statusDiv.textContent = '加载中...';
        
        apiGet('/api/fs/read', { path: path })
            .then(res => {
                if (!res || !res.success) {
                    statusDiv.textContent = '加载失败: ' + (res.error || '未知错误');
                    return;
                }
                
                const data = res.data || {};
                editor.value = data.content || '';
                editor.disabled = false;
                saveBtn.disabled = false;
                closeBtn.disabled = false;
                
                const size = data.size < 1024 ? `${data.size} B` : 
                            data.size < 1024 * 1024 ? `${(data.size / 1024).toFixed(1)} KB` :
                            `${(data.size / 1024 / 1024).toFixed(2)} MB`;
                statusDiv.textContent = `大小: ${size} | 就绪`;
                
                this._currentFilePath = path;
                this._currentFileModified = false;
            })
            .catch(err => {
                console.error('Open file failed:', err);
                statusDiv.textContent = '加载失败';
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
        
        const editor = document.getElementById('file-editor');
        const statusDiv = document.getElementById('file-status');
        const saveBtn = document.getElementById('fs-save-btn');
        
        if (!editor || !statusDiv) return;
        
        const content = editor.value;
        statusDiv.textContent = '保存中...';
        saveBtn.disabled = true;
        
        apiPost('/api/fs/save', { path: this._currentFilePath, content: content })
            .then(res => {
                if (res && res.success) {
                    statusDiv.textContent = '保存成功';
                    this._currentFileModified = false;
                    Notification.success('文件保存成功', '文件管理');
                } else {
                    statusDiv.textContent = '保存失败: ' + (res.error || '未知错误');
                    Notification.error(res.error || '保存失败', '文件管理');
                }
            })
            .catch(err => {
                console.error('Save file failed:', err);
                statusDiv.textContent = '保存失败';
                Notification.error('保存失败', '文件管理');
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
            if (!confirm('文件已修改，是否保存？')) {
                return;
            }
            this.saveCurrentFile();
        }
        
        if (editor) {
            editor.value = '';
            editor.disabled = true;
        }
        if (pathSpan) pathSpan.textContent = '请选择文件';
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
                    `<span style="color: #52c41a;">●</span> 已连接 (${network.ssid || 'N/A'})` : 
                    '<span style="color: #f5222d;">●</span> 未连接';
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
                    Notification.error('加载网络配置失败', '网络设置');
                    return;
                }
                
                const data = res.data || {};
                const device = data.device || {};
                const network = data.network || {};
                const sta = data.sta || {};
                const ap = data.ap || {};
                
                // 设备名称
                this._setValue('device-name', device.name || '');
                
                // 网络模式
                this._setValue('wifi-mode', network.mode !== undefined ? network.mode.toString() : '2');
                
                // STA 配置
                this._setValue('wifi-ssid', sta.ssid || '');
                this._setValue('wifi-password', sta.password || '');
                this._setValue('wifi-dhcp', network.ipConfigType !== undefined ? network.ipConfigType.toString() : '0');
                this._setValue('use-static-ip', network.ipConfigType === 1 ? '1' : '0');
                
                // 静态IP配置
                this._setValue('static-ip', sta.staticIP || '');
                this._setValue('gateway', sta.gateway || '');
                this._setValue('subnet', sta.subnet || '');
                this._setValue('dns1', sta.dns1 || '');
                
                // 显示/隐藏静态IP区域
                const staticSection = document.getElementById('static-ip-section');
                if (staticSection) {
                    staticSection.style.display = network.ipConfigType === 1 ? 'block' : 'none';
                }
                
                // mDNS
                this._setValue('enable-mdns', network.enableMDNS ? '1' : '0');
                
                // AP 配置
                this._setValue('ap-ssid', ap.ssid || '');
                this._setValue('ap-password', ap.password || '');
                this._setValue('ap-channel', ap.channel !== undefined ? ap.channel.toString() : '1');
                this._setValue('ap-hidden', ap.hidden ? '1' : '0');
            })
            .catch(err => {
                console.error('Load network config failed:', err);
                Notification.error('加载网络配置失败', '网络设置');
            });
    },
    
    /**
     * 保存网络配置
     */
    saveNetworkConfig() {
        const config = {
            deviceName: document.getElementById('device-name')?.value || '',
            mode: document.getElementById('wifi-mode')?.value || '2',
            staSSID: document.getElementById('wifi-ssid')?.value || '',
            staPassword: document.getElementById('wifi-password')?.value || '',
            ipConfigType: document.getElementById('use-static-ip')?.value || '0',
            staticIP: document.getElementById('static-ip')?.value || '',
            gateway: document.getElementById('gateway')?.value || '',
            subnet: document.getElementById('subnet')?.value || '',
            dns1: document.getElementById('dns1')?.value || '',
            apSSID: document.getElementById('ap-ssid')?.value || '',
            apPassword: document.getElementById('ap-password')?.value || '',
            apChannel: document.getElementById('ap-channel')?.value || '1',
            apHidden: document.getElementById('ap-hidden')?.value || '0'
        };
        
        // 显示加载状态
        const submitBtn = document.querySelector('#wifi-form button[type="submit"]');
        const originalText = submitBtn?.innerHTML;
        if (submitBtn) {
            submitBtn.disabled = true;
            submitBtn.innerHTML = '<i class="fas fa-spinner fa-spin"></i> 保存中...';
        }
        
        apiPut('/api/network/config', config)
            .then(res => {
                if (res && res.success) {
                    this._showMessage('wifi-success', true);
                    this._showMessage('wifi-error', false);
                    Notification.success('网络配置保存成功', '网络设置');
                } else {
                    this._showMessage('wifi-success', false);
                    this._showMessage('wifi-error', true);
                    Notification.error(res?.error || '保存失败', '网络设置');
                }
            })
            .catch(err => {
                console.error('Save network config failed:', err);
                this._showMessage('wifi-success', false);
                this._showMessage('wifi-error', true);
                Notification.error('保存失败', '网络设置');
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
        const refreshBtn = document.getElementById('net-status-refresh-btn');
        if (refreshBtn) {
            refreshBtn.disabled = true;
            refreshBtn.innerHTML = '<i class="fas fa-spinner fa-spin"></i> 刷新中...';
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
                    Notification.error('获取网络状态失败', '网络状态');
                    return;
                }
                const d = res.data || {};

                // 状态徽章
                const statusMap = {
                    connected:    '<span class="badge badge-success">已连接</span>',
                    disconnected: '<span class="badge badge-danger">未连接</span>',
                    connecting:   '<span class="badge badge-warning">连接中...</span>',
                    ap_mode:      '<span class="badge badge-primary">AP模式</span>',
                    failed:       '<span class="badge badge-danger">连接失败</span>',
                };
                setHtml('ns-status', statusMap[d.status] || `<span class="badge badge-info">${d.status || '--'}</span>`);

                // STA 信息
                setText('ns-ssid', d.ssid);
                setText('ns-ip', d.ipAddress);
                setText('ns-gateway', d.gateway);
                setText('ns-subnet', d.subnet);
                setText('ns-dns', d.dnsServer);
                const rssi = d.rssi;
                if (rssi && rssi !== 0) {
                    const pct = d.signalStrength || 0;
                    const color = pct >= 70 ? '#52c41a' : pct >= 40 ? '#faad14' : '#f5222d';
                    setHtml('ns-rssi', `<span style="color:${color};">${rssi} dBm (${pct}%)</span>`);
                } else {
                    setText('ns-rssi', '--');
                }
                setText('ns-mac', d.macAddress);

                // AP 信息
                setText('ns-ap-ssid', d.apSSID);
                setText('ns-ap-ip', d.apIPAddress);
                setText('ns-ap-clients', d.apClientCount !== undefined ? d.apClientCount + ' 台' : '--');

                // 连接统计
                const modeLabel = { STA: '仅客户端 (STA)', AP: '仅热点 (AP)', 'AP+STA': '客户端+热点 (AP+STA)' };
                setText('ns-mode', modeLabel[d.mode] || d.mode || '--');
                setText('ns-mdns', d.enableMDNS ? (d.customDomain ? d.customDomain + '.local' : '已启用') : '禁用');
                setText('ns-reconnect', d.reconnectAttempts !== undefined ? d.reconnectAttempts + ' 次' : '--');
                setHtml('ns-internet', d.internetAvailable
                    ? '<span class="badge badge-success">可访问</span>'
                    : '<span class="badge badge-danger">不可访问</span>');
                setHtml('ns-conflict', d.conflictDetected
                    ? '<span class="badge badge-danger">已检测到冲突</span>'
                    : '<span class="badge badge-success">无冲突</span>');
            })
            .catch(err => {
                console.error('Load network status failed:', err);
                Notification.error('获取网络状态失败', '网络状态');
            })
            .finally(() => {
                if (refreshBtn) {
                    refreshBtn.disabled = false;
                    refreshBtn.innerHTML = '<i class="fas fa-sync-alt"></i> 刷新';
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
                this._setValue('dev-name',          d.deviceName   || '');
                this._setValue('dev-location',      d.location     || '');
                const desc = document.getElementById('dev-description');
                if (desc) desc.value = d.description || '';
                this._setValue('dev-ntp-enable',    d.enableNTP ? '1' : '0');
                this._setValue('dev-ntp-server1',   d.ntpServer1   || 'pool.ntp.org');
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
        const config = {
            deviceName:   document.getElementById('dev-name')?.value || '',
            location:     document.getElementById('dev-location')?.value || '',
            description:  document.getElementById('dev-description')?.value || '',
            ntpServer1:   document.getElementById('dev-ntp-server1')?.value || 'pool.ntp.org',
            ntpServer2:   document.getElementById('dev-ntp-server2')?.value || 'time.nist.gov',
            timezone:     document.getElementById('dev-timezone')?.value || 'CST-8',
            enableNTP:    document.getElementById('dev-ntp-enable')?.value || '1',
            syncInterval: document.getElementById('dev-sync-interval')?.value || '3600',
        };
        apiPut('/api/device/config', config)
            .then(res => {
                if (res && res.success) {
                    this._showMessage('dev-basic-success', true);
                    Notification.success('设备信息保存成功', '设备配置');
                } else {
                    Notification.error(res?.error || '保存失败', '设备配置');
                }
            })
            .catch(() => Notification.error('保存失败', '设备配置'));
    },

    saveDeviceNTP() {
        const config = {
            ntpServer1:   document.getElementById('dev-ntp-server1')?.value || 'pool.ntp.org',
            ntpServer2:   document.getElementById('dev-ntp-server2')?.value || 'time.nist.gov',
            timezone:     document.getElementById('dev-timezone')?.value || 'CST-8',
            enableNTP:    document.getElementById('dev-ntp-enable')?.value || '1',
            syncInterval: document.getElementById('dev-sync-interval')?.value || '3600',
            deviceName:   document.getElementById('dev-name')?.value || '',
            location:     document.getElementById('dev-location')?.value || '',
            description:  document.getElementById('dev-description')?.value || '',
        };
        apiPut('/api/device/config', config)
            .then(res => {
                if (res && res.success) {
                    this._showMessage('dev-ntp-success', true);
                    Notification.success('NTP配置保存成功', '设备配置');
                    this.loadDeviceTime();
                } else {
                    Notification.error(res?.error || '保存失败', 'NTP配置');
                }
            })
            .catch(() => Notification.error('保存失败', 'NTP配置'));
    },

    loadDeviceTime() {
        const btn = document.getElementById('dev-time-refresh-btn');
        if (btn) { btn.disabled = true; btn.innerHTML = '<i class="fas fa-spinner fa-spin"></i> 刷新中...'; }
        apiGet('/api/device/time')
            .then(res => {
                if (!res || !res.success) return;
                const d = res.data || {};
                const setEl   = (id, val)  => { const el = document.getElementById(id); if (el) el.textContent = val || '--'; };
                const setHtml = (id, html) => { const el = document.getElementById(id); if (el) el.innerHTML = html; };
                setEl('dev-time-datetime', d.datetime);
                setHtml('dev-time-synced', d.synced
                    ? '<span class="badge badge-success">已同步</span>'
                    : '<span class="badge badge-warning">未同步 (等待连网)</span>');
                if (d.uptime !== undefined) {
                    const ms = d.uptime;
                    const h  = Math.floor(ms / 3600000);
                    const m  = Math.floor((ms % 3600000) / 60000);
                    const s  = Math.floor((ms % 60000) / 1000);
                    setEl('dev-time-uptime', `${h}时 ${m}分 ${s}秒`);
                }
            })
            .catch(err => console.error('Load device time failed:', err))
            .finally(() => {
                if (btn) { btn.disabled = false; btn.innerHTML = '<i class="fas fa-sync-alt"></i> 刷新'; }
            });
    },

    restartDevice() {
        const delay = document.getElementById('dev-restart-delay')?.value || '3';
        const btn   = document.getElementById('dev-restart-btn');
        if (!confirm(`确定要重启设备？将在 ${delay} 秒后重启。`)) return;
        if (btn) { btn.disabled = true; btn.innerHTML = '<i class="fas fa-spinner fa-spin"></i> 处理中...'; }
        apiPost('/api/system/restart', { delay })
            .then(res => {
                if (res && (res.success || res.message)) {
                    Notification.warning(`设备将在 ${delay} 秒后重启，请稍后刷新页面。`, '设备重启');
                } else {
                    if (btn) { btn.disabled = false; btn.innerHTML = '<i class="fas fa-power-off"></i> 立即重启'; }
                    Notification.error('重启指令发送失败', '设备重启');
                }
            })
            .catch(() => {
                if (btn) { btn.disabled = false; btn.innerHTML = '<i class="fas fa-power-off"></i> 立即重启'; }
                Notification.error('重启指令发送失败', '设备重启');
            });
    },

    /**
     * 扫描 WiFi 网络
     */
    scanWifiNetworks() {
        const scanBtn = document.getElementById('wifi-scan-btn');
        const resultsDiv = document.getElementById('wifi-scan-results');
        
        if (!scanBtn || !resultsDiv) return;
        
        // 显示加载状态
        scanBtn.disabled = true;
        scanBtn.innerHTML = '<i class="fas fa-spinner fa-spin"></i> 扫描中...';
        resultsDiv.style.display = 'block';
        resultsDiv.innerHTML = '<div style="padding: 20px; text-align: center; color: #999;"><i class="fas fa-spinner fa-spin"></i> 正在扫描...</div>';
        
        apiGet('/api/network/scan')
            .then(res => {
                if (!res || !res.success) {
                    resultsDiv.innerHTML = '<div style="padding: 20px; text-align: center; color: #f56c6c;">扫描失败</div>';
                    return;
                }
                
                const networks = res.data || [];
                
                if (networks.length === 0) {
                    resultsDiv.innerHTML = '<div style="padding: 20px; text-align: center; color: #999;">未找到WiFi网络</div>';
                    return;
                }
                
                // 按信号强度排序
                networks.sort((a, b) => b.rssi - a.rssi);
                
                let html = '';
                networks.forEach(net => {
                    const signalClass = net.rssi > -50 ? 'strong' : net.rssi > -70 ? 'medium' : 'weak';
                    const signalIcon = net.rssi > -50 ? 'fas fa-signal' : net.rssi > -70 ? 'fas fa-signal' : 'fas fa-signal';
                    const signalColor = net.rssi > -50 ? '#52c41a' : net.rssi > -70 ? '#faad14' : '#f5222d';
                    const encryptIcon = net.encryption > 0 ? '<i class="fas fa-lock" style="color: #52c41a;"></i>' : '<i class="fas fa-lock-open" style="color: #999;"></i>';
                    
                    html += `
                        <div class="wifi-item" style="padding: 10px; border-bottom: 1px solid #eee; cursor: pointer; display: flex; justify-content: space-between; align-items: center;" data-ssid="${net.ssid}">
                            <div style="flex: 1;">
                                <div style="font-weight: bold; color: #333;">${net.ssid}</div>
                                <div style="font-size: 12px; color: #999;">
                                    ${encryptIcon} ${net.encryption > 0 ? '加密' : '开放'} | 信道: ${net.channel}
                                </div>
                            </div>
                            <div style="text-align: right;">
                                <div style="color: ${signalColor};">
                                    <i class="${signalIcon}"></i> ${net.rssi} dBm
                                </div>
                            </div>
                        </div>
                    `;
                });
                
                resultsDiv.innerHTML = html;
                
                // 绑定点击事件
                resultsDiv.querySelectorAll('.wifi-item').forEach(item => {
                    item.addEventListener('click', (e) => {
                        const ssid = e.currentTarget.dataset.ssid;
                        document.getElementById('wifi-ssid').value = ssid;
                        resultsDiv.style.display = 'none';
                        Notification.success(`已选择: ${ssid}`, 'WiFi扫描');
                    });
                });
            })
            .catch(err => {
                console.error('WiFi scan failed:', err);
                resultsDiv.innerHTML = '<div style="padding: 20px; text-align: center; color: #f56c6c;">扫描失败</div>';
            })
            .finally(() => {
                scanBtn.disabled = false;
                scanBtn.innerHTML = '<i class="fas fa-search"></i> 扫描网络';
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
            this._setValue('mqtt-publish', mqtt.publishTopic || '');
            this._setValue('mqtt-subscribe', mqtt.subscribeTopic || '');
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
        data.mqtt_publishTopic = document.getElementById('mqtt-publish')?.value || '';
        data.mqtt_subscribeTopic = document.getElementById('mqtt-subscribe')?.value || '';
        
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
                    Notification.success(`${protocolName}配置保存成功！`, '通信协议');
                    
                    // 显示成功消息
                    const form = document.getElementById(formId);
                    const ok = form?.querySelector('.message-success');
                    if (ok) {
                        ok.style.display = 'block';
                        setTimeout(() => { ok.style.display = 'none'; }, 3000);
                    }
                } else {
                    Notification.error(res?.message || '保存失败', '通信协议');
                }
            })
            .catch(err => {
                console.error('保存协议配置失败:', err);
                Notification.error('保存协议配置失败', '通信协议');
            });
    },
    
    // ============ GPIO配置 ============
    
    /**
     * 加载GPIO列表
     */
    loadGpioList() {
        const tbody = document.getElementById('gpio-table-body');
        if (!tbody) return;
        
        tbody.innerHTML = '<tr><td colspan="5" style="text-align: center; color: #999;">加载中...</td></tr>';
        
        apiGet('/api/gpio/config')
            .then(res => {
                if (!res || !res.success) {
                    tbody.innerHTML = '<tr><td colspan="5" style="text-align: center; color: #f56c6c;">加载失败</td></tr>';
                    return;
                }
                
                const pins = res.data?.pins || [];
                
                if (pins.length === 0) {
                    tbody.innerHTML = '<tr><td colspan="5" style="text-align: center; color: #999;">暂无GPIO配置</td></tr>';
                    return;
                }
                
                let html = '';
                pins.forEach(pin => {
                    const modeNames = {
                        1: '数字输入', 2: '数字输出', 3: '数字输入(上拉)',
                        4: '数字输入(下拉)', 5: '模拟输入', 6: '模拟输出', 7: 'PWM输出'
                    };
                    const modeName = modeNames[pin.mode] || '未知';
                    const stateText = pin.state === 1 ? '高电平' : '低电平';
                    const stateColor = pin.state === 1 ? '#52c41a' : '#999';
                    // 仅输出模式才支持切换: 2=数字输出, 6=模拟输出, 7=PWM
                    const canToggle = (pin.mode === 2 || pin.mode === 6 || pin.mode === 7);
                    const toggleBtn = canToggle
                        ? `<button class="pure-button pure-button-small" onclick="app.toggleGpio(${pin.pin})">切换</button>`
                        : '';
                    
                    html += `
                        <tr>
                            <td>${pin.pin}</td>
                            <td>${pin.name}</td>
                            <td>${modeName}</td>
                            <td style="color: ${stateColor};">${stateText}</td>
                            <td>
                                <button class="pure-button pure-button-small" onclick="app.editGpio(${pin.pin}, '${pin.name}', ${pin.mode}, ${pin.state || 0})">编辑</button>
                                ${toggleBtn}
                                <button class="pure-button pure-button-small" style="background: #ff4d4f; color: white;" onclick="app.deleteGpio(${pin.pin})">删除</button>
                            </td>
                        </tr>
                    `;
                });
                
                tbody.innerHTML = html;
            })
            .catch(err => {
                console.error('Load GPIO list failed:', err);
                tbody.innerHTML = '<tr><td colspan="5" style="text-align: center; color: #f56c6c;">加载失败</td></tr>';
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
            title.textContent = '编辑GPIO';
            document.getElementById('gpio-original-pin').value = pinData.pin;
            document.getElementById('gpio-pin-input').value = pinData.pin;
            document.getElementById('gpio-name-input').value = pinData.name;
            document.getElementById('gpio-mode-input').value = pinData.mode;
            document.getElementById('gpio-default-input').value = pinData.state || 0;
            document.getElementById('gpio-pin-input').disabled = true;
        } else {
            title.textContent = '新增GPIO';
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
            errEl.textContent = '请填写引脚号和名称';
            errEl.style.display = 'block';
            return;
        }
        
        const isEdit = originalPin !== '';
        
        // 安全起见：禁用按钮防止重复提交
        const saveBtn = document.getElementById('save-gpio-btn');
        const origText = saveBtn.textContent;
        saveBtn.disabled = true;
        saveBtn.textContent = '保存中...';
        
        const data = { pin, name, mode, defaultValue };
        
        apiPost('/api/gpio/config', data)
            .then(res => {
                if (res && res.success) {
                    this.closeGpioModal();
                    this.loadGpioList();
                    Notification.success(isEdit ? 'GPIO更新成功' : 'GPIO添加成功', 'GPIO配置');
                } else {
                    errEl.textContent = res?.error || '保存失败';
                    errEl.style.display = 'block';
                }
            })
            .catch(err => {
                console.error('Save GPIO failed:', err);
                errEl.textContent = '保存失败，请检查连接';
                errEl.style.display = 'block';
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
        if (!confirm(`确定要删除 GPIO ${pin} 吗？`)) return;
        
        apiPost('/api/gpio/delete', { pin: String(pin) })
            .then(res => {
                if (res && res.success) {
                    Notification.success(`GPIO ${pin} 已删除`, 'GPIO配置');
                    this.loadGpioList();
                } else {
                    Notification.error(res?.error || '删除失败', 'GPIO配置');
                }
            })
            .catch(err => {
                console.error('Delete GPIO failed:', err);
                Notification.error('删除失败，请检查连接', 'GPIO配置');
            });
    },
    
    /**
     * 切换GPIO状态
     */
    toggleGpio(pin) {
        apiPost('/api/gpio/write', { pin: String(pin), state: 'toggle' })
            .then(res => {
                if (res && res.success) {
                    Notification.success('状态已切换', 'GPIO配置');
                    this.loadGpioList();
                } else {
                    Notification.error(res?.error || '切换失败', 'GPIO配置');
                }
            })
            .catch(err => {
                console.error('Toggle GPIO failed:', err);
                Notification.error('切换失败', 'GPIO配置');
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
                        statusEl.textContent = '配网中';
                        statusEl.className = 'badge badge-success';
                    } else {
                        statusEl.textContent = '未启动';
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
                    Notification.success('AP配网配置保存成功', 'AP配网');
                } else {
                    Notification.error(res?.message || '保存失败', 'AP配网');
                }
            })
            .catch(err => {
                Notification.error('保存失败: ' + (err.message || err), 'AP配网');
            });
    },

    /**
     * 启动AP配网
     */
    startProvision() {
        const startBtn = document.getElementById('provision-start-btn');
        if (startBtn) {
            startBtn.disabled = true;
            startBtn.innerHTML = '<i class="fas fa-spinner fa-spin"></i> 启动中...';
        }

        apiPost('/api/provision/start', {})
            .then(res => {
                if (res && res.success) {
                    Notification.success(`配网热点已启动: ${res.data?.apSSID || ''}`, 'AP配网');
                    this.loadProvisionStatus();
                } else {
                    Notification.error(res?.message || '启动失败', 'AP配网');
                }
            })
            .catch(err => {
                Notification.error('启动失败: ' + (err.message || err), 'AP配网');
            })
            .finally(() => {
                if (startBtn) {
                    startBtn.innerHTML = '<i class="fas fa-play"></i> 启动配网';
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
            stopBtn.innerHTML = '<i class="fas fa-spinner fa-spin"></i> 停止中...';
        }

        apiPost('/api/provision/stop', {})
            .then(res => {
                if (res && res.success) {
                    Notification.success('配网热点已停止', 'AP配网');
                    this.loadProvisionStatus();
                } else {
                    Notification.error(res?.message || '停止失败', 'AP配网');
                }
            })
            .catch(err => {
                Notification.error('停止失败: ' + (err.message || err), 'AP配网');
            })
            .finally(() => {
                if (stopBtn) {
                    stopBtn.innerHTML = '<i class="fas fa-stop"></i> 停止配网';
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
                    if (badge) { badge.className = 'status-badge status-online'; badge.textContent = '配网中'; }
                    if (deviceName) deviceName.textContent = d.deviceName || '--';
                    if (remainingWrap) remainingWrap.style.display = 'flex';
                    if (remaining) remaining.textContent = (d.remainingTime || 0) + '秒';
                    if (startBtn) startBtn.style.display = 'none';
                    if (stopBtn) { stopBtn.style.display = 'inline-block'; stopBtn.disabled = false; }
                } else {
                    if (badge) { badge.className = 'status-badge status-offline'; badge.textContent = '未启动'; }
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
                    Notification.success('蓝牙配网配置保存成功', '蓝牙配网');
                } else {
                    Notification.error(res?.message || '保存失败', '蓝牙配网');
                }
            })
            .catch(err => {
                Notification.error('保存失败: ' + (err.message || err), '蓝牙配网');
            });
    },

    /**
     * 启动蓝牙配网
     */
    startBLEProvision() {
        const startBtn = document.getElementById('ble-provision-start-btn');
        if (startBtn) {
            startBtn.disabled = true;
            startBtn.innerHTML = '<i class="fas fa-spinner fa-spin"></i> 启动中...';
        }

        apiPost('/api/ble/provision/start', {})
            .then(res => {
                if (res && res.success) {
                    Notification.success(`蓝牙配网已启动: ${res.data?.deviceName || ''}`, '蓝牙配网');
                    this.loadBLEProvisionStatus();
                } else {
                    Notification.error(res?.message || '启动失败', '蓝牙配网');
                }
            })
            .catch(err => {
                Notification.error('启动失败: ' + (err.message || err), '蓝牙配网');
            })
            .finally(() => {
                if (startBtn) {
                    startBtn.innerHTML = '<i class="fas fa-play"></i> 启动蓝牙配网';
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
            stopBtn.innerHTML = '<i class="fas fa-spinner fa-spin"></i> 停止中...';
        }

        apiPost('/api/ble/provision/stop', {})
            .then(res => {
                if (res && res.success) {
                    Notification.success('蓝牙配网已停止', '蓝牙配网');
                    this.loadBLEProvisionStatus();
                } else {
                    Notification.error(res?.message || '停止失败', '蓝牙配网');
                }
            })
            .catch(err => {
                Notification.error('停止失败: ' + (err.message || err), '蓝牙配网');
            })
            .finally(() => {
                if (stopBtn) {
                    stopBtn.innerHTML = '<i class="fas fa-stop"></i> 停止蓝牙配网';
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
                    if (badge) { badge.className = 'status-badge status-online'; badge.textContent = '就绪'; }
                    if (progressWrap) progressWrap.style.display = 'none';
                } else if (res.progress > 0 && res.progress < 100) {
                    if (badge) { badge.className = 'status-badge status-warning'; badge.textContent = '升级中'; }
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
            Notification.error('请输入固件下载地址', 'OTA升级');
            return;
        }
        
        if (!url.startsWith('http://') && !url.startsWith('https://')) {
            Notification.error('URL必须以http://或https://开头', 'OTA升级');
            return;
        }
        
        const btn = document.getElementById('ota-url-btn');
        if (btn) {
            btn.disabled = true;
            btn.innerHTML = '<i class="fas fa-spinner fa-spin"></i> 下载中...';
        }
        
        // 显示进度条
        const progressWrap = document.getElementById('ota-progress-wrap');
        if (progressWrap) progressWrap.style.display = 'block';
        
        apiPost('/api/ota/url', { url })
            .then(res => {
                if (res && res.success) {
                    Notification.success('开始从URL下载固件并升级', 'OTA升级');
                    // 开始轮询状态
                    this._pollOtaProgress();
                } else {
                    Notification.error(res?.message || '启动失败', 'OTA升级');
                    if (progressWrap) progressWrap.style.display = 'none';
                }
            })
            .catch(err => {
                Notification.error('启动失败: ' + (err.message || err), 'OTA升级');
                if (progressWrap) progressWrap.style.display = 'none';
            })
            .finally(() => {
                if (btn) {
                    btn.disabled = false;
                    btn.innerHTML = '<i class="fas fa-download"></i> 开始在线升级';
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
            Notification.error('请选择固件文件', 'OTA升级');
            return;
        }
        
        if (!file.name.endsWith('.bin')) {
            Notification.error('仅支持.bin格式的固件文件', 'OTA升级');
            return;
        }
        
        const btn = document.getElementById('ota-upload-btn');
        if (btn) {
            btn.disabled = true;
            btn.innerHTML = '<i class="fas fa-spinner fa-spin"></i> 上传中...';
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
                        Notification.success('固件上传成功，设备即将重启', 'OTA升级');
                        // 设备会重启，等待后刷新页面
                        setTimeout(() => {
                            window.location.reload();
                        }, 5000);
                    } else {
                        Notification.error(res.message || '上传失败', 'OTA升级');
                    }
                } catch (e) {
                    Notification.success('固件上传完成', 'OTA升级');
                }
            } else {
                Notification.error('上传失败: HTTP ' + xhr.status, 'OTA升级');
            }
            
            if (btn) {
                btn.disabled = false;
                btn.innerHTML = '<i class="fas fa-upload"></i> 上传并升级';
            }
        });
        
        xhr.addEventListener('error', () => {
            Notification.error('上传失败: 网络错误', 'OTA升级');
            if (btn) {
                btn.disabled = false;
                btn.innerHTML = '<i class="fas fa-upload"></i> 上传并升级';
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
                        if (badge) { badge.className = 'status-badge status-warning'; badge.textContent = '升级中'; }
                        setTimeout(poll, 1000);
                    } else if (progress >= 100) {
                        if (badge) { badge.className = 'status-badge status-online'; badge.textContent = '完成'; }
                        Notification.success('固件升级完成，设备即将重启', 'OTA升级');
                    }
                })
                .catch(err => {
                    console.error('获取OTA进度失败:', err);
                });
        };
        
        poll();
    }
};
