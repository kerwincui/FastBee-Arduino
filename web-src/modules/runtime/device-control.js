/**
 * device-control.js - lightweight entry bundle.
 *
 * build-web-modules.js prepends device-control/core.js to this file.
 * The heavier render/layout/Modbus handlers are split into lazy bundles and
 * loaded by core.js when the device-control page is opened.
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
