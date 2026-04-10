// 应用主入口和初始化

// 动态加载 i18n.js（从 index.html 移除以减少初始并发请求）
function _bootApp() {
    // 初始化消息通知系统
    Notification.init();
    
    // 初始化多语言
    i18n.updatePageText();
    
    // 加载保存的登录信息
    var savedUsername = localStorage.getItem('username');
    var savedPassword = localStorage.getItem('password');
    var remember = localStorage.getItem('remember') === 'true';
    
    if (savedUsername && savedPassword && remember) {
        document.getElementById('username').value = savedUsername;
        document.getElementById('password').value = savedPassword;
        document.getElementById('remember').checked = true;
    }
    
    // 设置语言选择器
    var langSelect = document.getElementById('language-select');
    if (langSelect) {
        langSelect.value = i18n.currentLang;
    }
    
    // 初始化应用
    AppState.init();
    
    // 暴露全局 app 引用，供表格内 onclick 使用
    window.app = AppState;
}

document.addEventListener('DOMContentLoaded', function() {
    // 动态加载 i18n.js，避免初始页面加载时并发连接过多
    var script = document.createElement('script');
    script.src = './js/modules/i18n.js';
    script.onload = function() { _bootApp(); };
    script.onerror = function() {
        // i18n 加载失败时重试一次
        setTimeout(function() {
            var retry = document.createElement('script');
            retry.src = './js/modules/i18n.js';
            retry.onload = function() { _bootApp(); };
            retry.onerror = function() {
                console.error('Failed to load i18n.js after retry');
                // 即使没有 i18n 也尝试启动（使用降级翻译）
                if (typeof i18n === 'undefined') {
                    window.i18n = { currentLang: 'zh-CN', t: function(k) { return k; }, updatePageText: function() {} };
                }
                _bootApp();
            };
            document.head.appendChild(retry);
        }, 1500);
    };
    document.head.appendChild(script);
});