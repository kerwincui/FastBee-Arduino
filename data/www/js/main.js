// 应用主入口和初始化

// 页面加载完成后初始化
document.addEventListener('DOMContentLoaded', () => {
    // 初始化消息通知系统
    NotificationSystem.init();
    
    // 初始化多语言
    i18n.updatePageText();
    
    // 加载保存的登录信息
    const savedUsername = localStorage.getItem('username');
    const savedPassword = localStorage.getItem('password');
    const remember = localStorage.getItem('remember') === 'true';
    
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