// 应用状态管理
const AppState = {
    currentPage: 'dashboard',
    configTab: 'modbus',
    currentUser: { name: 'Admin', role: 'admin' },
    sidebarCollapsed: false,
    
    // 模拟数据绑定
    dashboard: {
        cpu: 42,
        memory: 68,
        storage: 35,
        temperature: 42,
        devices: [
            { id: 1, name: 'Main Controller', name_zh: '主控制器', status: 'online', lastUpdate: '2023-07-20 14:30:25' },
            { id: 2, name: 'Sensor Module', name_zh: '传感器模块', status: 'online', lastUpdate: '2023-07-20 14:28:10' },
            { id: 3, name: 'Communication Module', name_zh: '通信模块', status: 'warning', lastUpdate: '2023-07-20 13:45:33' },
            { id: 4, name: 'Storage Module', name_zh: '存储模块', status: 'offline', lastUpdate: '2023-07-20 12:15:20' }
        ]
    },
    
    users: [
        { id: 1, username: 'admin', role: 'admin', role_zh: '管理员', lastLogin: '2023-07-20 14:30:25', status: 'active' },
        { id: 2, username: 'user1', role: 'operator', role_zh: '操作员', lastLogin: '2023-07-20 10:15:33', status: 'active' },
        { id: 3, username: 'user2', role: 'viewer', role_zh: '查看者', lastLogin: '2023-07-19 16:45:22', status: 'inactive' },
        { id: 4, username: 'user3', role: 'operator', role_zh: '操作员', lastLogin: '2023-07-18 09:20:10', status: 'active' }
    ],
    
    // 初始化
    init() {
        this.setupEventListeners();
        this.renderDashboard();
        this.renderUsers();
        this.setupLanguage();
        this.setupConfigTabs();
        this.setupSidebarToggle();
    },
    
    // 设置侧边栏折叠按钮
    setupSidebarToggle() {
        const sidebarToggle = document.getElementById('sidebar-toggle');
        if (sidebarToggle) {
            sidebarToggle.addEventListener('click', () => {
                this.toggleSidebar();
            });
        }
        
        // 从本地存储读取侧边栏状态
        const savedSidebarState = localStorage.getItem('sidebarCollapsed');
        if (savedSidebarState === 'true') {
            this.collapseSidebar();
        }
    },
    
    // 切换侧边栏
    toggleSidebar() {
        if (this.sidebarCollapsed) {
            this.expandSidebar();
        } else {
            this.collapseSidebar();
        }
    },
    
    // 折叠侧边栏
    collapseSidebar() {
        const sidebar = document.getElementById('sidebar');
        if (sidebar) {
            sidebar.classList.add('collapsed');
            this.sidebarCollapsed = true;
            localStorage.setItem('sidebarCollapsed', 'true');
            
            // 更新按钮图标
            const sidebarToggle = document.getElementById('sidebar-toggle');
            if (sidebarToggle) {
                sidebarToggle.textContent = '☰';
            }
        }
    },
    
    // 展开侧边栏
    expandSidebar() {
        const sidebar = document.getElementById('sidebar');
        if (sidebar) {
            sidebar.classList.remove('collapsed');
            this.sidebarCollapsed = false;
            localStorage.setItem('sidebarCollapsed', 'false');
            
            // 更新按钮图标
            const sidebarToggle = document.getElementById('sidebar-toggle');
            if (sidebarToggle) {
                sidebarToggle.textContent = '✕';
            }
        }
    },
    
    // 设置语言
    setupLanguage() {
        const langSelect = document.getElementById('language-select');
        if (langSelect) {
            langSelect.value = i18n.currentLang;
            langSelect.addEventListener('change', (e) => {
                i18n.setLanguage(e.target.value);
                this.renderDashboard();
                this.renderUsers();
            });
        }
    },
    
    // 设置配置选项卡
    setupConfigTabs() {
        // 设备配置选项卡
        const configTabs = document.querySelectorAll('#config-page .config-tab');
        configTabs.forEach(tab => {
            tab.addEventListener('click', () => {
                const tabId = tab.getAttribute('data-tab');
                this.showConfigTab('config-page', tabId);
            });
        });
        
        // 网络配置选项卡
        const networkTabs = document.querySelectorAll('#network-page .config-tab');
        networkTabs.forEach(tab => {
            tab.addEventListener('click', () => {
                const tabId = tab.getAttribute('data-tab');
                this.showConfigTab('network-page', tabId);
            });
        });
        
        // 配置表单提交
        const forms = document.querySelectorAll('#config-page form, #network-page form');
        forms.forEach(form => {
            form.addEventListener('submit', (e) => {
                e.preventDefault();
                const formId = form.id;
                const protocol = formId.replace('-form', '').replace('modbus-', '');
                const protocolName = protocol === 'rtu' ? 'Modbus RTU' : 
                                    protocol === 'tcp' ? 'Modbus TCP' : 
                                    protocol === 'mqtt' ? 'MQTT' : 
                                    protocol === 'http' ? 'HTTP' : 
                                    protocol === 'coap' ? 'CoAP' : 'TCP';
                
                Notification.success(`${protocolName}配置保存成功！`);
                
                // 显示配置成功消息
                const successElement = form.querySelector('.message-success');
                if (successElement) {
                    successElement.style.display = 'block';
                    setTimeout(() => {
                        successElement.style.display = 'none';
                    }, 3000);
                }
            });
        });
    },
    
    // 事件监听
    setupEventListeners() {
        // 登录表单
        document.getElementById('login-form').addEventListener('submit', (e) => {
            e.preventDefault();
            this.handleLogin();
        });
        
        // 菜单点击
        document.querySelectorAll('.menu-item').forEach(item => {
            item.addEventListener('click', (e) => {
                e.preventDefault();
                const page = item.dataset.page;
                this.changePage(page);
            });
        });
        
        // 修改密码按钮
        document.getElementById('change-password-btn').addEventListener('click', () => this.showChangePasswordModal());
        
        // 退出登录按钮
        document.getElementById('logout-btn').addEventListener('click', () => this.logout());
        
        // 添加用户按钮
        document.getElementById('add-user-btn').addEventListener('click', () => this.showAddUserModal());
        
        // 模态窗关闭按钮
        const closeModal = (modalId) => {
            document.getElementById(modalId).style.display = 'none';
        };
        
        document.getElementById('close-password-modal').addEventListener('click', () => closeModal('change-password-modal'));
        document.getElementById('close-add-user-modal').addEventListener('click', () => closeModal('add-user-modal'));
        document.getElementById('cancel-password-btn').addEventListener('click', () => closeModal('change-password-modal'));
        document.getElementById('cancel-add-user-btn').addEventListener('click', () => closeModal('add-user-modal'));
        
        // 确认修改密码
        document.getElementById('confirm-password-btn').addEventListener('click', () => this.changePassword());
        
        // 确认添加用户
        document.getElementById('confirm-add-user-btn').addEventListener('click', () => this.addUser());
    },
    
    // 处理登录
    handleLogin() {
        const username = document.getElementById('username').value;
        const password = document.getElementById('password').value;
        const remember = document.getElementById('remember').checked;

        // 校验
        if (!username || !password) {
            Notification.warning('请输入用户名或密码', '登录失败');
            return;
        }

        // 显示加载状态
        const submitBtn = document.querySelector('#login-form button[type="submit"]');
        const originalText = submitBtn.innerHTML;
        submitBtn.innerHTML = '<i class="fas fa-spinner fa-spin"></i> 登录中...';
        submitBtn.disabled = true;

        // 登录请求
        axios.post('/api/auth/login',  toUrlEncoded({ username, password }))
        .then(response => {
            if(response.success){                
                // 保存登录状态
                if (remember) {
                    localStorage.setItem('remember', 'true');
                    localStorage.setItem('username', username);
                    localStorage.setItem('password', password);
                    localStorage.setItem('sessionId', response.sessionId);
                } else {
                    localStorage.removeItem('remember');
                    localStorage.removeItem('username');
                    localStorage.removeItem('password');
                    localStorage.removeItem('sessionId');
                }

                // 更新axios默认header
                if (response.sessionId) {
                    axios.defaults.headers.common['Authorization'] = `Bearer ${response.sessionId}`;
                    localStorage.setItem('auth_token', response.sessionId);
                }
                
                // 保存会话
                sessionStorage.setItem('userRole', 'ADMIN');

                // 切换到主应用
                document.getElementById('login-page').style.display = 'none';
                document.getElementById('app-container').style.display = 'block';

                Notification.success(response.msg, '登录成功');
            } else {
                Notification.error(response.msg, '登录失败');
            }
            
        }).catch(error => {
            Notification.error('登录发生错误' + error, '登录失败');
        }).finally(function() {
            // 恢复按钮状态
            submitBtn.innerHTML = originalText;
            submitBtn.disabled = false;
        });
    },
    
    // 切换页面
    changePage(page) {
        // 更新菜单状态
        document.querySelectorAll('.menu-item').forEach(item => {
            item.classList.remove('active');
            if (item.dataset.page === page) {
                item.classList.add('active');
            }
        });
        
        // 更新页面标题
        const titleKey = `page-title-${page}`;
        document.getElementById('page-title').textContent = i18n.t(titleKey);
        
        // 显示对应页面
        document.querySelectorAll('.page').forEach(pageEl => {
            pageEl.classList.remove('active');
        });
        
        const targetPage = document.getElementById(`${page}-page`);
        if (targetPage) {
            targetPage.classList.add('active');
        }
        
        this.currentPage = page;
    },
    
    // 显示配置选项卡
    showConfigTab(pageId, tabId) {
        const page = document.getElementById(pageId);
        if (!page) return;
        
        // 更新选项卡状态
        page.querySelectorAll('.config-tab').forEach(tab => {
            tab.classList.remove('active');
            if (tab.getAttribute('data-tab') === tabId) {
                tab.classList.add('active');
            }
        });
        
        // 显示对应内容
        page.querySelectorAll('.config-content').forEach(content => {
            content.classList.remove('active');
        });
        
        const targetContent = page.querySelector(`#${tabId}`);
        if (targetContent) {
            targetContent.classList.add('active');
        }
    },
    
    // 渲染仪表盘
    renderDashboard() {
        const tbody = document.getElementById('device-table-body');
        if (!tbody) return;
        
        // 更新统计数字
        document.getElementById('cpu-usage').textContent = `${this.dashboard.cpu}%`;
        document.getElementById('memory-usage').textContent = `${this.dashboard.memory}%`;
        document.getElementById('storage-usage').textContent = `${this.dashboard.storage}%`;
        document.getElementById('temperature').textContent = `${this.dashboard.temperature}°C`;
        
        // 计算在线设备
        const onlineCount = this.dashboard.devices.filter(d => d.status === 'online').length;
        document.getElementById('online-count').textContent = onlineCount;
        document.getElementById('total-count').textContent = this.dashboard.devices.length;
        
        // 渲染设备表格
        tbody.innerHTML = '';
        this.dashboard.devices.forEach(device => {
            const row = document.createElement('tr');
            
            // 设备名称（根据语言显示）
            const nameCell = document.createElement('td');
            nameCell.textContent = i18n.currentLang === 'zh-CN' ? device.name_zh : device.name;
            
            // 状态
            const statusCell = document.createElement('td');
            const statusBadge = document.createElement('span');
            statusBadge.className = 'badge ';
            
            let statusText = '';
            if (device.status === 'online') {
                statusBadge.classList.add('badge-success');
                statusText = i18n.t('status-online');
            } else if (device.status === 'warning') {
                statusBadge.classList.add('badge-warning');
                statusText = i18n.t('status-warning');
            } else {
                statusBadge.classList.add('badge-danger');
                statusText = i18n.t('status-offline');
            }
            
            statusBadge.textContent = statusText;
            statusCell.appendChild(statusBadge);
            
            // 最后更新
            const updateCell = document.createElement('td');
            updateCell.textContent = device.lastUpdate;
            
            // 操作
            const actionCell = document.createElement('td');
            const viewButton = document.createElement('button');
            viewButton.className = 'pure-button pure-button-small pure-button-primary';
            viewButton.textContent = i18n.t('view-details');
            viewButton.addEventListener('click', () => {
                Notification.info(
                    `${i18n.t('view-details')}: ${i18n.currentLang === 'zh-CN' ? device.name_zh : device.name}`,
                    '设备详情'
                );
            });
            actionCell.appendChild(viewButton);
            
            row.appendChild(nameCell);
            row.appendChild(statusCell);
            row.appendChild(updateCell);
            row.appendChild(actionCell);
            
            tbody.appendChild(row);
        });
    },
    
    // 渲染用户列表
    renderUsers() {
        const tbody = document.getElementById('users-table-body');
        if (!tbody) return;
        
        tbody.innerHTML = '';
        this.users.forEach(user => {
            const row = document.createElement('tr');
            
            // 用户名
            const usernameCell = document.createElement('td');
            usernameCell.textContent = user.username;
            
            // 角色
            const roleCell = document.createElement('td');
            roleCell.textContent = i18n.currentLang === 'zh-CN' ? user.role_zh : user.role;
            
            // 最后登录
            const loginCell = document.createElement('td');
            loginCell.textContent = user.lastLogin;
            
            // 状态
            const statusCell = document.createElement('td');
            const statusBadge = document.createElement('span');
            statusBadge.className = 'badge ';
            
            if (user.status === 'active') {
                statusBadge.classList.add('badge-success');
                statusBadge.textContent = i18n.t('user-status-active');
            } else {
                statusBadge.classList.add('badge-danger');
                statusBadge.textContent = i18n.t('user-status-inactive');
            }
            
            statusCell.appendChild(statusBadge);
            
            // 操作
            const actionCell = document.createElement('td');
            
            // 编辑按钮
            const editButton = document.createElement('button');
            editButton.className = 'pure-button pure-button-small pure-button-primary';
            editButton.textContent = i18n.t('edit-user');
            editButton.addEventListener('click', () => {
                Notification.primary(
                    `${i18n.t('edit-user')}: ${user.username}`,
                    '编辑用户'
                );
            });
            
            // 禁用/启用按钮
            const toggleButton = document.createElement('button');
            toggleButton.className = 'pure-button pure-button-small';
            
            if (user.status === 'active') {
                toggleButton.classList.add('pure-button-warning');
                toggleButton.textContent = i18n.t('disable-user');
                toggleButton.addEventListener('click', () => {
                    this.toggleUserStatus(user, 'disable');
                });
            } else {
                toggleButton.classList.add('pure-button-success');
                toggleButton.textContent = i18n.t('enable-user');
                toggleButton.addEventListener('click', () => {
                    this.toggleUserStatus(user, 'enable');
                });
            }
            
            // 删除按钮（不能删除admin）
            if (user.username !== 'admin') {
                const deleteButton = document.createElement('button');
                deleteButton.className = 'pure-button pure-button-small pure-button-error';
                deleteButton.textContent = i18n.t('delete-user');
                deleteButton.addEventListener('click', () => {
                    if (confirm(`确定要删除用户 ${user.username} 吗？`)) {
                        this.deleteUser(user.id);
                    }
                });
                actionCell.appendChild(deleteButton);
            }
            
            actionCell.appendChild(editButton);
            actionCell.appendChild(toggleButton);
            
            row.appendChild(usernameCell);
            row.appendChild(roleCell);
            row.appendChild(loginCell);
            row.appendChild(statusCell);
            row.appendChild(actionCell);
            
            tbody.appendChild(row);
        });
    },
    
    // 显示修改密码模态窗
    showChangePasswordModal() {
        document.getElementById('change-password-modal').style.display = 'flex';
        document.getElementById('current-password-input').value = '';
        document.getElementById('new-password-input').value = '';
        document.getElementById('confirm-password-input').value = '';
        document.getElementById('password-error').style.display = 'none';
    },
    
    // 修改密码
    changePassword() {
        const currentPassword = document.getElementById('current-password-input').value;
        const newPassword = document.getElementById('new-password-input').value;
        const confirmPassword = document.getElementById('confirm-password-input').value;
        const errorDiv = document.getElementById('password-error');
        
        // 验证
        if (!currentPassword || !newPassword || !confirmPassword) {
            errorDiv.textContent = '请填写所有字段！';
            errorDiv.style.display = 'block';
            
            Notification.error('请填写所有字段！', '修改密码失败');
            return;
        }
        
        if (newPassword !== confirmPassword) {
            errorDiv.textContent = i18n.t('password-error');
            errorDiv.style.display = 'block';
            
            Notification.error('新密码与确认密码不一致！', '修改密码失败');
            return;
        }
        
        if (newPassword.length < 6) {
            errorDiv.textContent = '新密码长度至少6位！';
            errorDiv.style.display = 'block';
            
            Notification.warning('新密码长度至少6位！', '密码要求');
            return;
        }
        
        // 模拟修改密码
        errorDiv.style.display = 'none';
        Notification.success(i18n.t('password-changed'), '修改密码成功');
        document.getElementById('change-password-modal').style.display = 'none';
    },
    
    // 显示添加用户模态窗
    showAddUserModal() {
        document.getElementById('add-user-modal').style.display = 'flex';
        document.getElementById('add-username-input').value = '';
        document.getElementById('add-password-input').value = '';
        document.getElementById('add-confirm-password-input').value = '';
        document.getElementById('add-role-select').value = 'operator';
        document.getElementById('add-user-error').style.display = 'none';
    },
    
    // 添加用户
    addUser() {
        const username = document.getElementById('add-username-input').value.trim();
        const password = document.getElementById('add-password-input').value;
        const confirmPassword = document.getElementById('add-confirm-password-input').value;
        const role = document.getElementById('add-role-select').value;
        const errorDiv = document.getElementById('add-user-error');
        
        // 验证
        if (!username) {
            errorDiv.textContent = '请输入用户名！';
            errorDiv.style.display = 'block';
            
            Notification.error('请输入用户名！', '添加用户失败');
            return;
        }
        
        // 检查用户名是否已存在
        const userExists = this.users.some(u => u.username === username);
        if (userExists) {
            errorDiv.textContent = '用户名已存在！';
            errorDiv.style.display = 'block';
            
            Notification.warning('用户名已存在！', '添加用户失败');
            return;
        }
        
        if (!password || !confirmPassword) {
            errorDiv.textContent = '请输入密码！';
            errorDiv.style.display = 'block';
            
            Notification.error('请输入密码！', '添加用户失败');
            return;
        }
        
        if (password !== confirmPassword) {
            errorDiv.textContent = '密码与确认密码不一致！';
            errorDiv.style.display = 'block';
            
            Notification.error('密码与确认密码不一致！', '添加用户失败');
            return;
        }
        
        if (password.length < 6) {
            errorDiv.textContent = '密码长度至少6位！';
            errorDiv.style.display = 'block';
            
            Notification.warning('密码长度至少6位！', '密码要求');
            return;
        }
        
        // 添加用户
        const roleText = role === 'admin' ? i18n.t('admin') : 
                        role === 'operator' ? i18n.t('operator') : i18n.t('viewer');
        const roleTextZh = role === 'admin' ? '管理员' : 
                            role === 'operator' ? '操作员' : '查看者';
        
        const newUser = {
            id: Date.now(),
            username: username,
            role: role,
            role_zh: roleTextZh,
            lastLogin: '从未登录',
            status: 'active'
        };
        
        this.users.push(newUser);
        this.renderUsers();
        
        errorDiv.style.display = 'none';
        Notification.success(i18n.t('user-added'), '添加用户成功');
        document.getElementById('add-user-modal').style.display = 'none';
    },
    
    // 切换用户状态
    toggleUserStatus(user, action) {
        const actionText = action === 'enable' ? i18n.t('enable-user') : i18n.t('disable-user');
        const confirmText = action === 'enable' ? '启用' : '禁用';
        
        if (confirm(`确定要${confirmText}用户${user.username}吗？`)) {
            user.status = action === 'enable' ? 'active' : 'inactive';
            this.renderUsers();
            
            const message = `${user.username} 已${confirmText}！`;
            if (action === 'enable') {
                Notification.success(message, '用户状态更新');
            } else {
                Notification.warning(message, '用户状态更新');
            }
        }
    },
    
    // 删除用户
    deleteUser(userId) {
        const user = this.users.find(u => u.id === userId);
        this.users = this.users.filter(u => u.id !== userId);
        this.renderUsers();
        
        Notification.error(`用户 ${user.username} 已删除！`, '删除用户');
    },

    getAuthHeader() {
        // 从cookie或localStorage获取认证信息
        const token = localStorage.getItem('auth_token');
        if (token) {
            return {
                'Authorization': `Bearer ${token}`,
                'Content-Type': 'application/json'
            };
        }
        return {
            'Content-Type': 'application/json'
        };
    },
    
    // 退出登录
    logout() {
        if (confirm(i18n.t('logout-confirm'))) {
            document.getElementById('app-container').style.display = 'none';
            document.getElementById('login-page').style.display = 'flex';
            document.getElementById('login-form').reset();

            // 登录请求
            axios.post('/api/auth/logout')
            .then(response => {
                if (response.data && response.data.success) {
                    // 清除本地存储
                    localStorage.removeItem('password');
                    localStorage.removeItem('auth_token');
                    delete axios.defaults.headers.common['Authorization'];
                    Notification.success('已成功退出登录', '退出登录');
                }
                return response.data;
            }).catch(error => {
                Notification.error('退出发生错误', '登录失败');
            })
        }
    }
};