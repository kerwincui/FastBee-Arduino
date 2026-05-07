/**
 * device-control.js — 入口（合并模式）
 *
 * 注意：
 *   构建脚本 build-web-modules.js 会把 web-src/modules/runtime/device-control/*.js
 *   按依赖顺序 prepend 到本文件之前，输出为单文件 data/www/js/modules/device-control.js。
 *   因此这里**不再**做运行时 loadScript 动态加载——子模块代码已经先于本 IIFE 执行完毕。
 *   这样把 6 个 HTTP 请求合并为 1 个，避免 ESP32 AsyncTCP 连接频繁建立/拆除引发的内存碎片。
 */
(function() {
    if (typeof AppState === 'undefined') return;
    if (typeof AppState.registerModule === 'function') {
        AppState.registerModule('device-control', {});
    }
    if (typeof AppState.setupDeviceControlEvents === 'function') {
        AppState.setupDeviceControlEvents();
    }
    document.dispatchEvent(new CustomEvent('device-control-modules-loaded'));
})();
