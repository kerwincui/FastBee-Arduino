// 应用主入口和初始化
// 核心依赖已合并为 app-bundle.js（单文件），index.html 中有 bundle 失败时的降级回退

/**
 * 动态加载JS脚本，失败时自动重试一次（供模块加载等场景使用）
 * @param {string} src 脚本路径
 * @param {function} onSuccess 成功回调
 * @param {function} [onFail] 最终失败回调（不提供则跳过继续）
 */
function _loadScript(src, onSuccess, onFail) {
    var resolveAssetUrl = (typeof window !== 'undefined' && typeof window.__fastbeeResolveAssetUrl === 'function')
        ? window.__fastbeeResolveAssetUrl
        : function(path) { return path; };
    var script = document.createElement('script');
    script.src = resolveAssetUrl(src);
    script.onload = onSuccess;
    script.onerror = function() {
        // 首次失败，1.5秒后重试
        setTimeout(function() {
            var retry = document.createElement('script');
            retry.src = resolveAssetUrl(src);
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

function _ensureMainStylesheet(onReady) {
    var resolveAssetUrl = (typeof window !== 'undefined' && typeof window.__fastbeeResolveAssetUrl === 'function')
        ? window.__fastbeeResolveAssetUrl
        : function(path) { return path; };
    var done = false;
    function finish() {
        if (done) return;
        done = true;
        onReady();
    }

    var existing = document.getElementById('fb-main-stylesheet');
    if (existing) {
        if (existing.dataset.ready === '1' || existing.sheet) {
            finish();
            return;
        }
        existing.addEventListener('load', function markReady() {
            existing.dataset.ready = '1';
            finish();
        }, { once: true });
        existing.addEventListener('error', finish, { once: true });
        setTimeout(finish, 2500);
        return;
    }

    var link = document.createElement('link');
    link.id = 'fb-main-stylesheet';
    link.rel = 'stylesheet';
    link.href = resolveAssetUrl('./css/main.css');
    link.onload = function() {
        link.dataset.ready = '1';
        finish();
    };
    link.onerror = finish;
    document.head.appendChild(link);
    setTimeout(finish, 2500);
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

    // 初始化应用
    AppState.init();

    // 暴露全局 app 引用，供表格内 onclick 使用
    window.app = AppState;

    window.__fastbeeBootReady = true;
    if (typeof document !== 'undefined' && typeof document.dispatchEvent === 'function') {
        try {
            document.dispatchEvent(new Event('fastbee:boot-ready'));
        } catch (_) {}
    }
}

// 所有核心脚本已合并为 app-bundle.js，bundle 内按序执行无需额外动态加载
// 若 bundle 加载成功，AppState 必然已定义，直接启动
function _bootAppWhenReady() {
    window.__fastbeeBootReady = false;
    if (typeof AppState !== 'undefined') {
        _ensureMainStylesheet(_bootApp);
    }
    // 若 AppState 未定义，说明 bundle 加载失败
    // index.html 中的内联回退脚本会处理逐个加载，加载完成后 main.js 会再次执行
}

if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', _bootAppWhenReady, { once: true });
} else {
    _bootAppWhenReady();
}
