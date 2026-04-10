// 应用主入口和初始化
// 所有核心依赖由此文件链式动态加载，避免 ESP32 并发连接过多导致传输失败

/**
 * 动态加载JS脚本，失败时自动重试一次
 * @param {string} src 脚本路径
 * @param {function} onSuccess 成功回调
 * @param {function} [onFail] 最终失败回调（不提供则跳过继续）
 */
function _loadScript(src, onSuccess, onFail) {
    var script = document.createElement('script');
    script.src = src;
    script.onload = onSuccess;
    script.onerror = function() {
        // 首次失败，1.5秒后重试
        setTimeout(function() {
            var retry = document.createElement('script');
            retry.src = src;
            retry.onload = onSuccess;
            retry.onerror = function() {
                console.error('[FastBee] Failed to load ' + src + ' after retry');
                if (onFail) onFail();
            };
            document.head.appendChild(retry);
        }, 1500);
    };
    document.head.appendChild(script);
}

function _bootApp() {
    // 初始化消息通知系统
    if (typeof Notification !== 'undefined' && Notification.init) {
        Notification.init();
    }

    // 初始化多语言
    if (typeof i18n !== 'undefined') {
        i18n.updatePageText();
    }

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
    if (langSelect && typeof i18n !== 'undefined') {
        langSelect.value = i18n.currentLang;
    }

    // 初始化应用
    AppState.init();

    // 暴露全局 app 引用，供表格内 onclick 使用
    window.app = AppState;
}

document.addEventListener('DOMContentLoaded', function() {
    // 链式加载核心依赖（每次只发一个请求，ESP32 友好）
    // 加载顺序: fetch-api → notification → state → i18n → 启动应用
    _loadScript('./js/modules/fetch-api.js', function() {
        _loadScript('./js/modules/notification.js', function() {
            _loadScript('./js/state.js', function() {
                _loadScript('./js/modules/i18n.js', function() {
                    _bootApp();
                }, function() {
                    // i18n 加载失败，使用降级翻译
                    if (typeof i18n === 'undefined') {
                        window.i18n = { currentLang: 'zh-CN', t: function(k) { return k; }, updatePageText: function() {} };
                    }
                    _bootApp();
                });
            });
        });
    });
});
