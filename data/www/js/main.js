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
}

document.addEventListener('DOMContentLoaded', function() {
    // 链式加载核心依赖：state.js 必须同步加载，i18n 改为引擎+数据分离加载
    _loadScript('./js/state.js', function() {
        // state.js 加载完成，立即启动应用（不等 i18n）
        _bootApp();
        
        // 异步加载 i18n 引擎（后台执行，不阻塞应用启动）
        setTimeout(function() {
            _loadScript('./js/modules/i18n-engine.js', function() {
                // i18n 引擎加载成功，并发加载翻译数据
                var currentLang = (typeof i18n !== 'undefined' && i18n.currentLang) ? i18n.currentLang : 'zh-CN';
                var translationsLoaded = 0;
                var totalTranslations = (currentLang === 'en') ? 2 : 1;
                
                function onTranslationLoaded() {
                    translationsLoaded++;
                    if (translationsLoaded >= totalTranslations) {
                        // 所有翻译数据加载完成，更新页面文本
                        if (typeof i18n !== 'undefined' && i18n.updatePageText) {
                            i18n.updatePageText();
                            // 更新语言选择器
                            var langSelect = document.getElementById('language-select');
                            if (langSelect) {
                                langSelect.value = i18n.currentLang;
                            }
                        }
                    }
                }
                
                // 始终加载中文翻译（默认语言）
                _loadScript('./js/modules/i18n-zh-CN.js', onTranslationLoaded, function() {
                    console.warn('[FastBee] i18n-zh-CN.js load failed');
                    onTranslationLoaded();
                });
                
                // 如果当前语言是英文，也加载英文翻译
                if (currentLang === 'en') {
                    _loadScript('./js/modules/i18n-en.js', onTranslationLoaded, function() {
                        console.warn('[FastBee] i18n-en.js load failed');
                        onTranslationLoaded();
                    });
                }
            }, function() {
                // i18n 引擎加载失败，使用降级翻译（已在 _bootApp 中处理）
                console.warn('[FastBee] i18n-engine.js load failed, using fallback');
            });
        }, 0);
    });
});
