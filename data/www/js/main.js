// 应用主入口和初始化
// 核心依赖（state.js, i18n-engine.js, i18n-zh-CN.js）已由 index.html 中的
// <script defer> 标签按序加载，浏览器并行下载、按序执行，无需动态加载链

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

    // 如果英文语言需要额外加载英文翻译包（中文已由 defer 脚本加载）
    if (typeof i18n !== 'undefined' && i18n.currentLang === 'en') {
        _loadScript('./js/modules/i18n-en.js', function() {
            if (typeof i18n !== 'undefined' && i18n.updatePageText) {
                i18n.updatePageText();
            }
        });
    }
}

// defer 脚本正常时：state.js → i18n-engine.js → i18n-zh-CN.js → main.js 按序执行
// 但 mDNS 解析失败时 defer 脚本会静默跳过，需检测并回退到动态重试加载
document.addEventListener('DOMContentLoaded', function() {
    if (typeof AppState !== 'undefined') {
        // defer 脚本全部成功，直接启动
        _bootApp();
    } else {
        // 核心依赖未加载（网络间歇失败），回退到动态加载 + 自动重试
        console.warn('[FastBee] Core scripts not loaded, retrying dynamically...');
        _loadScript('./js/state.js', function() {
            // state.js 成功，继续加载 i18n（容错：失败也启动）
            _loadScript('./js/modules/i18n-engine.js', function() {
                _loadScript('./js/modules/i18n-zh-CN.js', function() {
                    _bootApp();
                }, function() { _bootApp(); });
            }, function() { _bootApp(); });
        });
    }
});
