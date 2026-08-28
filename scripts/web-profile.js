/**
 * @file web-profile.js
 * @brief Web UI profile 配置判定模块
 * 
 * FastBee Arduino 统一架构 (Unified Architecture v2.0):
 * **单一固件、单一镜像**,不再有任何版本区分!
 * 所有功能模块通过 C++ 编译宏 FASTBEE_ENABLE_* 控制。
 */

const WEB_PROFILE = 'full';

/**
 * 获取当前 Web profile
 * @returns {'full'} Always returns 'full'
 */
function getWebProfile() {
    return WEB_PROFILE;
}



/**
 * 是否 full profile
 */
function isFullWebProfile() {
    return WEB_PROFILE === 'full';
}

/**
 * 是否启用设备控制大屏功能
 * 
 * 统一版本：始终由编译宏 FASTBEE_ENABLE_DEVICE_CONTROL 控制
 */
function hasDeviceControl() {
    const enableDC = process.env.FASTBEE_ENABLE_DEVICE_CONTROL === '1';
    return enableDC;
}

module.exports = {
    getWebProfile,
    isFullWebProfile,
    hasDeviceControl
};
