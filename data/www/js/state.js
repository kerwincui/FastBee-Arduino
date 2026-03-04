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
        ['#config-page', '#network-page'].forEach(pageSelector => {
            const page = document.querySelector(pageSelector);
            if (!page) return;
            page.querySelectorAll('.config-tab').forEach(tab => {
                tab.addEventListener('click', () => {
                    this.showConfigTab(pageSelector.replace('#', ''), tab.getAttribute('data-tab'));
                });
            });
        });

        // 配置表单提交（协议配置，暂模拟，等待后端协议API）
        document.querySelectorAll('#config-page form, #network-page form').forEach(form => {
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
    }
};
