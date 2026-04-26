// 应用主入口和初始化
// 核心依赖已合并为 app-bundle.js（单文件），index.html 中有 bundle 失败时的降级回退

/**
 * 动态加载JS脚本，失败时自动重试一次（供模块加载等场景使用）
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
    // 降级 i18n（确保 i18n 未加载时有 fallback）
    if (typeof i18n === 'undefined') {
        window.i18n = { currentLang: 'zh-CN', t: function(k) { return k; }, updatePageText: function() {} };
    }

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

    // 如果英文语言需要额外加载英文翻译包（中文已由 bundle 包含）
    if (typeof i18n !== 'undefined' && i18n.currentLang === 'en') {
        _loadScript('./js/modules/i18n-en.js', function() {
            if (typeof i18n !== 'undefined' && i18n.updatePageText) {
                i18n.updatePageText();
            }
        });
    }
}

// 所有核心脚本已合并为 app-bundle.js，bundle 内按序执行无需额外动态加载
// 若 bundle 加载成功，AppState 必然已定义，直接启动
document.addEventListener('DOMContentLoaded', function() {
    if (typeof AppState !== 'undefined') {
        _bootApp();
    }
    // 若 AppState 未定义，说明 bundle 加载失败
    // index.html 中的内联回退脚本会处理逐个加载，加载完成后 main.js 会再次执行
});
