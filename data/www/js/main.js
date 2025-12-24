// axios全局配置
axios.defaults.baseURL = 'http://fastbee.local';
axios.defaults.timeout = 3000; // 5秒超时

// 如果需要，可以设置每次请求默认携带的参数或认证信息

 // 1. 配置请求拦截器[citation:1]
axios.interceptors.request.use(
    function (config) {
        // 发送请求前可修改配置，如添加统一请求头[citation:2]
        console.log('请求即将发送:', config.url);
        config.headers['X-Requested-With'] = 'XMLHttpRequest';
        // 示例：添加认证令牌（需自行实现获取逻辑）
        // const token = localStorage.getItem('auth_token');
        // if (token) config.headers.Authorization = `Bearer ${token}`;
        const cookie = localStorage.getItem('Cookie');
        if (cookie) config.headers.Authorization = `bear 123456`;
        config.headers.Cookie = "fastbee_session=authenticated"
        config.withCredentials = true
        return config; // 必须返回配置对象[citation:2]
    },
    function (error) {
        // 处理请求错误（如配置无效）
        return Promise.reject(error);
    }
);

// 2. 配置响应拦截器[citation:1]
axios.interceptors.response.use(
    function (response) {
        // 对2xx范围内的响应状态码进行处理[citation:5]
        console.log('收到响应:', response.status);
        // 可以统一处理响应数据格式，例如直接返回data部分[citation:2]
        return response.data;
    },
    function (error) {
        // 处理2xx范围外的响应错误[citation:5]
        if (error.response) {
            // 服务器有响应但状态码错误
            console.error('请求失败，状态码:', error.response.status);
            // 可根据状态码进行统一处理，如401跳转登录[citation:4]
        } else if (error.request) {
            // 请求已发出但无响应（网络问题）
            console.error('网络错误，无服务器响应');
        } else {
            // 请求配置出错
            console.error('请求配置错误:', error.message);
        }
        return Promise.reject(error); // 将错误继续抛出，以便在具体请求的catch中处理[citation:2]
    }
);

// 消息通知系统
const NotificationSystem = {
    container: null,
    notifications: [],
    
    init() {
        this.container = document.getElementById('notification-container');
        if (!this.container) {
            this.container = document.createElement('div');
            this.container.id = 'notification-container';
            this.container.className = 'notification-container';
            document.body.appendChild(this.container);
        }
    },
    
    /**
     * 显示通知
     * @param {Object} options 配置选项
     * @param {string} options.type 类型: primary, success, warning, error, info
     * @param {string} options.title 标题
     * @param {string} options.message 消息内容
     * @param {number} options.duration 持续时间(毫秒)，默认3000
     * @param {boolean} options.autoClose 是否自动关闭，默认true
     */
    show(options) {
        const id = 'notification-' + Date.now();
        const duration = options.duration || 3000;
        
        // 创建通知元素
        const notification = document.createElement('div');
        notification.id = id;
        notification.className = `notification notification-${options.type || 'info'}`;
        
        // 图标映射
        const icons = {
            primary: '🔷',
            success: '✅',
            warning: '⚠️',
            error: '❌',
            info: 'ℹ️'
        };
        
        notification.innerHTML = `
            <div class="notification-header">
                <div class="notification-title">
                    <i>${icons[options.type] || icons.info}</i>
                    <span>${options.title || this.getDefaultTitle(options.type)}</span>
                </div>
                <button class="notification-close" onclick="NotificationSystem.close('${id}')">×</button>
            </div>
            <div class="notification-body">
                ${options.message || ''}
            </div>
            <div class="notification-progress">
                <div class="notification-progress-bar" style="animation-duration: ${duration}ms;"></div>
            </div>
        `;
        
        this.container.appendChild(notification);
        
        // 添加到通知列表
        const notificationObj = {
            id,
            element: notification,
            timeout: null
        };
        
        this.notifications.push(notificationObj);
        
        // 设置自动关闭
        if (options.autoClose !== false) {
            notificationObj.timeout = setTimeout(() => {
                this.close(id);
            }, duration);
        }
        
        return id;
    },
    
    /**
     * 关闭通知
     * @param {string} id 通知ID
     */
    close(id) {
        const notification = document.getElementById(id);
        if (notification) {
            notification.classList.add('hiding');
            
            // 清除对应的定时器
            const notificationObj = this.notifications.find(n => n.id === id);
            if (notificationObj && notificationObj.timeout) {
                clearTimeout(notificationObj.timeout);
            }
            
            // 动画结束后移除元素
            setTimeout(() => {
                if (notification.parentNode) {
                    notification.parentNode.removeChild(notification);
                }
                
                // 从列表中移除
                this.notifications = this.notifications.filter(n => n.id !== id);
            }, 300);
        }
    },
    
    /**
     * 关闭所有通知
     */
    closeAll() {
        this.notifications.forEach(notification => {
            this.close(notification.id);
        });
    },
    
    /**
     * 获取默认标题
     * @param {string} type 通知类型
     * @returns {string} 默认标题
     */
    getDefaultTitle(type) {
        const titles = {
            primary: '提示',
            success: '成功',
            warning: '警告',
            error: '错误',
            info: '信息'
        };
        return titles[type] || '通知';
    },
    
    /**
     * 快速显示主要通知
     * @param {string} message 消息内容
     * @param {string} title 标题
     */
    primary(message, title) {
        return this.show({
            type: 'primary',
            title: title || '提示',
            message: message
        });
    },
    
    /**
     * 快速显示成功通知
     * @param {string} message 消息内容
     * @param {string} title 标题
     */
    success(message, title) {
        return this.show({
            type: 'success',
            title: title || '成功',
            message: message
        });
    },
    
    /**
     * 快速显示警告通知
     * @param {string} message 消息内容
     * @param {string} title 标题
     */
    warning(message, title) {
        return this.show({
            type: 'warning',
            title: title || '警告',
            message: message
        });
    },
    
    /**
     * 快速显示错误通知
     * @param {string} message 消息内容
     * @param {string} title 标题
     */
    error(message, title) {
        return this.show({
            type: 'error',
            title: title || '错误',
            message: message
        });
    },
    
    /**
     * 快速显示信息通知
     * @param {string} message 消息内容
     * @param {string} title 标题
     */
    info(message, title) {
        return this.show({
            type: 'info',
            title: title || '信息',
            message: message
        });
    }
};

// 多语言支持
const i18n = {
    currentLang: localStorage.getItem('language') || 'zh-CN',
    
    translations: {
        'zh-CN': {
            // 登录页面
            'login-title': '设备配置管理',
            'username-label': '用户名',
            'password-label': '密码',
            'remember-label': '记住密码',
            'login-button': '登录系统',
            'login-error': '用户名或密码错误！',
            
            // 应用通用
            'app-title': '设备管理系统',
            'page-title-dashboard': '设备监控仪表盘',
            'page-title-config': '设备配置',
            'page-title-network': '网络设置',
            'page-title-data': '数据管理',
            'page-title-system': '系统监控',
            'page-title-users': '用户管理',
            'page-title-logs': '设备日志',
            
            // 菜单
            'menu-dashboard': '设备监控',
            'menu-config': '设备配置',
            'menu-network': '网络设置',
            'menu-data': '数据管理',
            'menu-system': '系统监控',
            'menu-users': '用户管理',
            'menu-logs': '设备日志',
            
            // 头部按钮
            'user-name': '管理员',
            'change-password-btn': '修改密码',
            'logout-btn': '退出登录',
            
            // 仪表盘
            'cpu-label': 'CPU使用率',
            'memory-label': '内存使用率',
            'storage-label': '存储使用率',
            'temp-label': '设备温度',
            'device-status-title': '设备状态',
            'online-text': '在线',
            'device-name-col': '设备名称',
            'device-status-col': '状态',
            'last-update-col': '最后更新',
            'device-actions-col': '操作',
            'status-online': '运行正常',
            'status-warning': '警告',
            'status-offline': '离线',
            'view-details': '查看详情',
            
            // 配置页面
            'config-title': '通信协议配置',
            
            // 网络配置
            'network-title': '网络设置',
            
            // 页脚
            'sidebar-copyright': 'FastBee物联网平台',
            'sidebar-version': 'v1.0.0',
            'footer-copyright': '© 2026 FastBee物联网平台 | 官网: www.fastbee.cn',
            
            // 用户管理
            'users-title': '用户管理',
            'add-user': '添加用户',
            'user-username-col': '用户名',
            'user-role-col': '角色',
            'user-lastlogin-col': '最后登录',
            'user-status-col': '状态',
            'user-actions-col': '操作',
            'user-status-active': '启用',
            'user-status-inactive': '禁用',
            'edit-user': '编辑',
            'disable-user': '禁用',
            'enable-user': '启用',
            'delete-user': '删除',
            
            // 修改密码
            'change-password-title': '修改密码',
            'current-password-label': '当前密码',
            'new-password-label': '新密码',
            'confirm-password-label': '确认新密码',
            'cancel': '取消',
            'confirm-change': '确认修改',
            'password-changed': '密码修改成功！',
            'password-error': '密码错误或新密码不匹配！',
            
            // 添加用户
            'add-user-title': '添加用户',
            'add-username-label': '用户名',
            'add-password-label': '密码',
            'add-confirm-password-label': '确认密码',
            'add-role-label': '角色',
            'add-user-confirm': '添加',
            'user-added': '用户添加成功！',
            
            // 系统消息
            'logout': '退出登录',
            'change-password': '修改密码',
            'logout-confirm': '确定要退出登录吗？',
            'user': '用户',
            'admin': '管理员',
            'operator': '操作员',
            'viewer': '查看者'
        },
        
        'en': {
            // Login page
            'login-title': 'Device Manage',
            'username-label': 'Username',
            'password-label': 'Password',
            'remember-label': 'Remember password',
            'login-button': 'Login',
            'login-error': 'Invalid username or password!',
            
            // App common
            'app-title': 'Device Manage',
            'page-title-dashboard': 'Device Dashboard',
            'page-title-config': 'Device Configuration',
            'page-title-network': 'Network Settings',
            'page-title-data': 'Data Management',
            'page-title-system': 'System Monitoring',
            'page-title-users': 'User Management',
            'page-title-logs': 'Device Logs',
            
            // Menu
            'menu-dashboard': 'Dashboard',
            'menu-config': 'Configuration',
            'menu-network': 'Network',
            'menu-data': 'Data',
            'menu-system': 'System',
            'menu-users': 'Users',
            'menu-logs': 'Logs',
            
            // Header buttons
            'user-name': 'Administrator',
            'change-password-btn': 'Change Password',
            'logout-btn': 'Logout',
            
            // Dashboard
            'cpu-label': 'CPU Usage',
            'memory-label': 'Memory Usage',
            'storage-label': 'Storage Usage',
            'temp-label': 'Device Temperature',
            'device-status-title': 'Device Status',
            'online-text': 'Online',
            'device-name-col': 'Device Name',
            'device-status-col': 'Status',
            'last-update-col': 'Last Update',
            'device-actions-col': 'Actions',
            'status-online': 'Online',
            'status-warning': 'Warning',
            'status-offline': 'Offline',
            'view-details': 'View Details',
            
            // Configuration page
            'config-title': 'Protocol Configuration',
            
            // Network Configuration
            'network-title': 'Network Settings',
            
            // Footer
            'sidebar-copyright': 'FastBee IoT Platform',
            'sidebar-version': 'v1.0.0',
            'footer-copyright': '© 2026 FastBee IoT Platform | site: www.fastbee.cn',
            
            // User management
            'users-title': 'User Management',
            'add-user': 'Add User',
            'user-username-col': 'Username',
            'user-role-col': 'Role',
            'user-lastlogin-col': 'Last Login',
            'user-status-col': 'Status',
            'user-actions-col': 'Actions',
            'user-status-active': 'Active',
            'user-status-inactive': 'Inactive',
            'edit-user': 'Edit',
            'disable-user': 'Disable',
            'enable-user': 'Enable',
            'delete-user': 'Delete',
            
            // Change password
            'change-password-title': 'Change Password',
            'current-password-label': 'Current Password',
            'new-password-label': 'New Password',
            'confirm-password-label': 'Confirm New Password',
            'cancel': 'Cancel',
            'confirm-change': 'Confirm Change',
            'password-changed': 'Password changed successfully!',
            'password-error': 'Password error or new passwords do not match!',
            
            // Add user
            'add-user-title': 'Add User',
            'add-username-label': 'Username',
            'add-password-label': 'Password',
            'add-confirm-password-label': 'Confirm Password',
            'add-role-label': 'Role',
            'add-user-confirm': 'Add',
            'user-added': 'User added successfully!',
            
            // System messages
            'logout': 'Logout',
            'change-password': 'Change Password',
            'logout-confirm': 'Are you sure you want to logout?',
            'user': 'User',
            'admin': 'Administrator',
            'operator': 'Operator',
            'viewer': 'Viewer'
        }
    },
    
    // 翻译函数
    t(key) {
        return this.translations[this.currentLang][key] || key;
    },
    
    // 切换语言
    setLanguage(lang) {
        if (this.translations[lang]) {
            this.currentLang = lang;
            localStorage.setItem('language', lang);
            this.updatePageText();
            
            // 显示语言切换成功的消息
            NotificationSystem.success(
                lang === 'zh-CN' ? '语言已切换为中文' : 'Language switched to English',
                lang === 'zh-CN' ? '成功' : 'Success'
            );
            
            return true;
        }
        return false;
    },
    
    // 更新页面文本
    updatePageText() {
        // 更新所有有ID的元素
        const elements = document.querySelectorAll('[id]');
        elements.forEach(element => {
            const key = element.id;
            if (this.translations[this.currentLang][key]) {
                if (element.tagName === 'INPUT' && element.type !== 'button' && element.type !== 'submit') {
                    element.placeholder = this.t(key);
                } else {
                    element.textContent = this.t(key);
                }
            }
        });
    }
};

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
                
                NotificationSystem.success(`${protocolName}配置保存成功！`);
                
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
        const errorDiv = document.getElementById('login-error');

        // 校验
        if (!username || !password) {
            showNotification('请输入用户名和密码', 'error');
            return;
        }

        // 显示加载状态
        const submitBtn = e.target.querySelector('button[type="submit"]');
        const originalText = submitBtn.innerHTML;
        submitBtn.innerHTML = '<i class="fas fa-spinner fa-spin"></i> 登录中...';
        submitBtn.disabled = true;
        
        // 简单验证
        if (username === 'admin' && password === 'admin') {
            // 保存登录信息
            if (remember) {
                localStorage.setItem('login_username', username);
                localStorage.setItem('login_password', password);
                localStorage.setItem('login_remember', 'true');
            }
            
            // 切换到主应用
            document.getElementById('login-page').style.display = 'none';
            document.getElementById('app-container').style.display = 'block';
            
            errorDiv.style.display = 'none';
            
            // 显示登录成功消息
            NotificationSystem.success('登录成功！欢迎使用设备管理系统', '登录成功');
            
        } else {
            errorDiv.textContent = i18n.t('login-error');
            errorDiv.style.display = 'block';
            
            // 显示登录失败消息
            NotificationSystem.error('用户名或密码错误，请重试', '登录失败');
        }
    },

    // 登录功能
    document.getElementById('login-form').addEventListener('submit', async function(e) {
        e.preventDefault();

        

        
        
        
        // 登录请求
        axios.post('/login', new URLSearchParams({
            username: username,
            password: password
        })
        ).then(response => {
            console.log('登录请求', response);
            if(response.status==1){
                // 认证成功
                systemState.isAuthenticated = true;
                systemState.currentUser = {
                    username: 'admin',
                    role: 'ADMIN',
                    email: 'admin@example.com'
                };
                
                // 保存登录状态
                if (rememberMe) {
                    localStorage.setItem('rememberMe', 'true');
                    localStorage.setItem('username', username);
                    localStorage.setItem('password', password);
                } else {
                    localStorage.removeItem('rememberMe');
                    localStorage.removeItem('username');
                    localStorage.removeItem('password');
                }
                
                // 保存会话
                sessionStorage.setItem('userRole', 'ADMIN');

                document.getElementById('login-page').style.display = 'none';
                document.getElementById('app').style.display = 'flex';

                showNotification(response.msg,"error");
            }else{
                showNotification(response.msg , "error");
            }
            
        }).catch(error=>{
            showNotification("登录发生错误" , "error");
        }).finally(function(){
            // 恢复按钮状态
            submitBtn.innerHTML = originalText;
            submitBtn.disabled = false;
        });

    });

    // 检查自动登录
    function checkAutoLogin() {
        const rememberMe = localStorage.getItem('rememberMe');
        const savedUsername = localStorage.getItem('username');
        const savedPassowrd = localStorage.getItem('password');
        
        if (rememberMe === 'true' && savedUsername) {
            document.getElementById('username').value = savedUsername;
            document.getElementById('password').value = savedPassowrd;
            document.getElementById('remember-me').checked = true;
        }
    }
    
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
                NotificationSystem.info(
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
                NotificationSystem.primary(
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
    
    // 显示消息（现在使用NotificationSystem）
    showMessage(text, type = 'info', title) {
        switch (type) {
            case 'success':
                NotificationSystem.success(text, title);
                break;
            case 'warning':
                NotificationSystem.warning(text, title);
                break;
            case 'error':
                NotificationSystem.error(text, title);
                break;
            case 'primary':
                NotificationSystem.primary(text, title);
                break;
            default:
                NotificationSystem.info(text, title);
        }
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
            
            NotificationSystem.error('请填写所有字段！', '修改密码失败');
            return;
        }
        
        if (newPassword !== confirmPassword) {
            errorDiv.textContent = i18n.t('password-error');
            errorDiv.style.display = 'block';
            
            NotificationSystem.error('新密码与确认密码不一致！', '修改密码失败');
            return;
        }
        
        if (newPassword.length < 6) {
            errorDiv.textContent = '新密码长度至少6位！';
            errorDiv.style.display = 'block';
            
            NotificationSystem.warning('新密码长度至少6位！', '密码要求');
            return;
        }
        
        // 模拟修改密码
        errorDiv.style.display = 'none';
        this.showMessage(i18n.t('password-changed'), 'success', '修改密码成功');
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
            
            NotificationSystem.error('请输入用户名！', '添加用户失败');
            return;
        }
        
        // 检查用户名是否已存在
        const userExists = this.users.some(u => u.username === username);
        if (userExists) {
            errorDiv.textContent = '用户名已存在！';
            errorDiv.style.display = 'block';
            
            NotificationSystem.warning('用户名已存在！', '添加用户失败');
            return;
        }
        
        if (!password || !confirmPassword) {
            errorDiv.textContent = '请输入密码！';
            errorDiv.style.display = 'block';
            
            NotificationSystem.error('请输入密码！', '添加用户失败');
            return;
        }
        
        if (password !== confirmPassword) {
            errorDiv.textContent = '密码与确认密码不一致！';
            errorDiv.style.display = 'block';
            
            NotificationSystem.error('密码与确认密码不一致！', '添加用户失败');
            return;
        }
        
        if (password.length < 6) {
            errorDiv.textContent = '密码长度至少6位！';
            errorDiv.style.display = 'block';
            
            NotificationSystem.warning('密码长度至少6位！', '密码要求');
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
        this.showMessage(i18n.t('user-added'), 'success', '添加用户成功');
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
                NotificationSystem.success(message, '用户状态更新');
            } else {
                NotificationSystem.warning(message, '用户状态更新');
            }
        }
    },
    
    // 删除用户
    deleteUser(userId) {
        const user = this.users.find(u => u.id === userId);
        this.users = this.users.filter(u => u.id !== userId);
        this.renderUsers();
        
        NotificationSystem.error(`用户 ${user.username} 已删除！`, '删除用户');
    },
    
    // 退出登录
    logout() {
        if (confirm(i18n.t('logout-confirm'))) {
            document.getElementById('app-container').style.display = 'none';
            document.getElementById('login-page').style.display = 'flex';
            document.getElementById('login-form').reset();
            document.getElementById('login-error').style.display = 'none';
            
            NotificationSystem.info('已成功退出登录', '退出登录');
        }
    }
};

// WiFi扫描功能
window.openWifiScanner = function() {
    const wifiList = document.getElementById('wifi-list');
    wifiList.innerHTML = `
        <div class="wifi-item">
            <div class="wifi-ssid">MyWiFi</div>
            <div class="wifi-signal">
                <i class="fas fa-wifi"></i>
                <span>强</span>
            </div>
        </div>
        <div class="wifi-item">
            <div class="wifi-ssid">GuestWiFi</div>
            <div class="wifi-signal">
                <i class="fas fa-wifi"></i>
                <span>中</span>
            </div>
        </div>
        <div class="wifi-item">
            <div class="wifi-ssid">OfficeWiFi</div>
            <div class="wifi-signal">
                <i class="fas fa-wifi"></i>
                <span>弱</span>
            </div>
        </div>
    `;
    
    // 点击WiFi项自动填充SSID
    document.querySelectorAll('.wifi-item').forEach(item => {
        item.addEventListener('click', function() {
            const ssid = this.querySelector('.wifi-ssid').textContent;
            document.getElementById('wifi-ssid').value = ssid;
            
            NotificationSystem.primary(`已选择WiFi网络: ${ssid}`, '网络选择');
        });
    });
    
    NotificationSystem.success('已扫描到3个WiFi网络', '网络扫描完成');
};

// 静态IP切换功能
window.toggleStaticIP = function() {
    const useStaticIP = document.getElementById('use-static-ip').value;
    const staticIpSection = document.getElementById('static-ip-section');
    if (useStaticIP === 'enabled') {
        staticIpSection.style.display = 'block';
        NotificationSystem.info('已启用静态IP配置', '网络设置');
    } else {
        staticIpSection.style.display = 'none';
    }
};

// 页面加载完成后初始化
document.addEventListener('DOMContentLoaded', () => {
    // 初始化消息通知系统
    NotificationSystem.init();
    
    // 初始化多语言
    i18n.updatePageText();
    
    // 加载保存的登录信息
    const savedUsername = localStorage.getItem('login_username');
    const savedPassword = localStorage.getItem('login_password');
    const remember = localStorage.getItem('login_remember') === 'true';
    
    if (savedUsername && savedPassword && remember) {
        document.getElementById('username').value = savedUsername;
        document.getElementById('password').value = savedPassword;
        document.getElementById('remember').checked = true;
    }
    
    // 设置语言选择器
    const langSelect = document.getElementById('language-select');
    if (langSelect) {
        langSelect.value = i18n.currentLang;
    }
    
    // 初始化应用
    AppState.init();
    
    // 显示欢迎消息
    setTimeout(() => {
        NotificationSystem.primary('欢迎使用设备管理系统', '系统提示');
    }, 1000);
    
    // 模拟实时数据更新
    setInterval(() => {
        // 更新仪表盘数据
        AppState.dashboard.cpu = Math.min(100, Math.max(10, AppState.dashboard.cpu + (Math.random() * 6 - 3)));
        AppState.dashboard.cpu = Math.round(AppState.dashboard.cpu);
        
        AppState.dashboard.memory = Math.min(100, Math.max(20, AppState.dashboard.memory + (Math.random() * 4 - 2)));
        AppState.dashboard.memory = Math.round(AppState.dashboard.memory);
        
        AppState.dashboard.temperature = Math.min(60, Math.max(30, AppState.dashboard.temperature + (Math.random() * 2 - 1)));
        AppState.dashboard.temperature = Math.round(AppState.dashboard.temperature);
        
        // 随机更新一个设备状态
        if (Math.random() > 0.7 && AppState.dashboard.devices.length > 0) {
            const randomIndex = Math.floor(Math.random() * AppState.dashboard.devices.length);
            const device = AppState.dashboard.devices[randomIndex];
            const oldStatus = device.status;
            const statuses = ['online', 'warning', 'offline'];
            const newStatus = statuses[Math.floor(Math.random() * statuses.length)];
            
            if (oldStatus !== newStatus) {
                device.status = newStatus;
                device.lastUpdate = new Date().toLocaleString();
                
                // 显示状态变更通知
                const deviceName = i18n.currentLang === 'zh-CN' ? device.name_zh : device.name;
                const statusText = newStatus === 'online' ? '在线' : 
                                    newStatus === 'warning' ? '警告' : '离线';
                
                if (newStatus === 'online') {
                    NotificationSystem.success(`${deviceName} 状态恢复正常`, '设备状态更新');
                } else if (newStatus === 'warning') {
                    NotificationSystem.warning(`${deviceName} 出现警告`, '设备状态更新');
                } else {
                    NotificationSystem.error(`${deviceName} 已离线`, '设备状态更新');
                }
            }
        }
        
        // 更新显示
        AppState.renderDashboard();
    }, 5000);
});