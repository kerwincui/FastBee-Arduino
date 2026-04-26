/**
 * API 数据预加载器
 * 在应用初始化时并行预加载关键 API 数据，利用 Governor 缓存机制
 * 减少后续页面切换时的等待时间
 *
 * 依赖: request-governor.js (apiGet, apiInvalidateCache)
 */
var ApiPreloader = (function() {
    'use strict';

    // 关键 API 列表 — 这些是页面切换时最常请求的端点
    // 与 Governor._cacheTTL 中已配置缓存的端点对齐
    var CRITICAL_APIS = [
        '/api/system/info',      // dashboard + device-config 共用
        '/api/network/config',   // network 模块
        '/api/protocol/config',  // protocol + periph-exec 共用
        '/api/device/config'     // device-config 模块
    ];

    /**
     * 预加载关键数据（并行触发 apiGet 预热 Governor 缓存）
     * 失败静默处理，不影响正常功能
     */
    function preloadCriticalData() {
        if (typeof apiGet !== 'function') {
            console.warn('[Preloader] apiGet not available, skip preload');
            return Promise.resolve();
        }

        var promises = CRITICAL_APIS.map(function(url) {
            return apiGet(url).catch(function() {
                // 预加载失败静默忽略
            });
        });

        return Promise.allSettled
            ? Promise.allSettled(promises)
            : Promise.all(promises.map(function(p) {
                return p.then(function(v) { return { status: 'fulfilled', value: v }; },
                              function(e) { return { status: 'rejected', reason: e }; });
            })).then(function() {});
    }

    /**
     * 从缓存获取数据（委托给 Governor 的 apiGet，自动命中缓存）
     * @param {string} url API 路径
     * @param {object} [params] 查询参数
     * @returns {Promise} 数据 Promise
     */
    function getData(url, params) {
        if (typeof apiGet !== 'function') {
            return Promise.reject(new Error('apiGet not available'));
        }
        return apiGet(url, params);
    }

    /**
     * 使特定 URL 的缓存失效
     * @param {string} url API 路径（或路径片段，支持模糊匹配）
     */
    function invalidate(url) {
        if (typeof apiInvalidateCache === 'function') {
            apiInvalidateCache(url);
        }
    }

    /**
     * 清除所有缓存
     */
    function clearAll() {
        if (typeof apiInvalidateCache === 'function') {
            apiInvalidateCache();
        }
    }

    /**
     * 获取关键 API 列表（只读）
     */
    function getCriticalAPIs() {
        return CRITICAL_APIS.slice();
    }

    return {
        preloadCriticalData: preloadCriticalData,
        getData: getData,
        invalidate: invalidate,
        clearAll: clearAll,
        getCriticalAPIs: getCriticalAPIs
    };
})();
