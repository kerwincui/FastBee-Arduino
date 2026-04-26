/**
 * protocol.js — 入口文件
 * 按顺序加载协议配置子模块
 */
(function() {
    var basePath = '/js/modules/protocol';
    var scripts = [
        'common.js',
        'mqtt-config.js',
        'modbus-config.js',
        'protocol-config.js',
        'modbus-control.js',
        'modbus-relay-motor.js'
    ];

    function loadScript(src) {
        return new Promise(function(resolve, reject) {
            var s = document.createElement('script');
            s.src = basePath + '/' + src;
            s.onload = resolve;
            s.onerror = function() {
                console.error('[protocol] Failed to load: ' + src);
                reject(new Error('Failed to load ' + src));
            };
            document.head.appendChild(s);
        });
    }

    // 顺序加载确保依赖正确
    (async function() {
        for (var i = 0; i < scripts.length; i++) {
            await loadScript(scripts[i]);
        }
        // 所有子模块加载完毕后，统一触发 ModuleLoader 回调
        if (AppState && typeof AppState.registerModule === 'function') {
            AppState.registerModule('protocol', {});
        }
        // 自动绑定事件
        if (typeof AppState.setupProtocolEvents === 'function') {
            AppState.setupProtocolEvents();
        }
        // 通知加载完成
        document.dispatchEvent(new CustomEvent('protocol-modules-loaded'));
    })();
})();
