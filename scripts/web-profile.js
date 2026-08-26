/**
 * @file web-profile.js
 * @brief Web UI profile 配置判定模块
 * 
 * FastBee Arduino 统一架构 (Unified Architecture v2.0):
 * **单一固件、单一镜像**,不再有任何版本区分!
 * 所有功能模块通过 C++ 编译宏 FASTBEE_ENABLE_* 控制。
 * 
 * ========== DEPRECATED UNIFIED ARCHITECTURE v2.0 ==========
 * 以下废弃函数仅用于保持与旧构建链的兼容性
 * 建议新代码直接使用 isFullWebProfile()
 * ===========================================================
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
 * 是否 compact profile（已废弃）
 * @deprecated Unified Architecture v2.0 不区分 profile 版本
 *     Please use isFullWebProfile() instead
 * @returns {false} Always returns false
 * @see isFullWebProfile
 */
function isCompactWebProfile() {
    return false;
}

/**
 * 是否 lite profile（已废弃）
 * @deprecated Unified Architecture v2.0 不区分 profile 版本
 *     Please use isFullWebProfile() instead
 * @returns {false} Always returns false
 * @see isFullWebProfile
 */
function isLiteWebProfile() {
    return false;
}

/**
 * 是否 standard profile（已废弃）
 * @deprecated Unified Architecture v2.0 不区分 profile 版本
 *     Please use isFullWebProfile() instead
 * @returns {false} Always returns false
 * @see isFullWebProfile
 */
function isStandardWebProfile() {
    return false;
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
    isCompactWebProfile,      // DEPRECATED - Use isFullWebProfile()
    isLiteWebProfile,         // DEPRECATED - Use isFullWebProfile()
    isStandardWebProfile,     // DEPRECATED - Use isFullWebProfile()
    isFullWebProfile,
    hasDeviceControl
};
