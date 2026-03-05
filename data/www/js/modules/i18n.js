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
            
            // 应用通用
            'app-title': '设备管理系统',
            'page-title-dashboard': '设备监控仪表盘',
            'page-title-protocol': '通信协议',
            'page-title-network': '网络设置',
            'page-title-data': '文件管理',
            'page-title-system': '系统监控',
            'page-title-users': '用户管理',
            'page-title-logs': '设备日志',
            'page-title-gpio': 'GPIO配置',
            
            // 菜单
            'menu-dashboard': '设备监控',
            'menu-protocol': '通信协议',
            'menu-network': '网络设置',
            'menu-data': '文件管理',
            'menu-system': '系统监控',
            'menu-users': '用户管理',
            'menu-logs': '设备日志',
            'menu-gpio': 'GPIO配置',
            
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
            
            // App common
            'app-title': 'Device Manage',
            'page-title-dashboard': 'Device Dashboard',
            'page-title-protocol': 'Protocol Configuration',
            'page-title-network': 'Network Settings',
            'page-title-data': 'Data Management',
            'page-title-system': 'System Monitoring',
            'page-title-users': 'User Management',
            'page-title-logs': 'Device Logs',
            'page-title-gpio': 'GPIO Config',
            
            // Menu
            'menu-dashboard': 'Dashboard',
            'menu-protocol': 'Protocol',
            'menu-network': 'Network',
            'menu-data': 'Data',
            'menu-system': 'System',
            'menu-users': 'Users',
            'menu-logs': 'Logs',
            'menu-gpio': 'Gpio',
            
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
            Notification.success(
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