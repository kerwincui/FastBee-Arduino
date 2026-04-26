/**
 * device-control.js — 入口文件
 * 按顺序加载子模块脚本，确保依赖关系正确
 */
(function() {
    var basePath = '/js/modules/device-control';
    var scripts = [
        'core.js',
        'render.js',
        'modbus-ops.js',
        'modbus-ctrl.js',
        'layout.js'
    ];

    function loadScript(src) {
        return new Promise(function(resolve, reject) {
            var s = document.createElement('script');
            s.src = basePath + '/' + src;
            s.onload = resolve;
            s.onerror = function() { reject(new Error('Failed to load ' + src)); };
            document.head.appendChild(s);
        });
    }

    (async function() {
        for (var i = 0; i < scripts.length; i++) {
            await loadScript(scripts[i]);
        }
        // 所有子模块加载完毕后，统一触发 ModuleLoader 回调
        if (AppState && typeof AppState.registerModule === 'function') {
            AppState.registerModule('device-control', {});
        }
        if (typeof AppState.setupDeviceControlEvents === 'function') {
            AppState.setupDeviceControlEvents();
        }
        document.dispatchEvent(new CustomEvent('device-control-modules-loaded'));
    })();
})();
