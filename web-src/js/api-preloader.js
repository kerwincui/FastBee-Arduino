/**
 * API preloader
 *
 * Keep this opt-in and conservative on classic ESP32 boards:
 * - do not parallel-hit multiple config endpoints
 * - only warm endpoints that fit the batch response budget
 * - leave protocol config out because the payload is usually too large
 */
var ApiPreloader = (function() {
    'use strict';

    var CRITICAL_APIS = [
        '/api/system/info',
        '/api/network/config',
        '/api/device/config'
    ];

    function uniqueUrls(urls) {
        var seen = {};
        return (urls || []).filter(function(url) {
            if (!url || seen[url]) return false;
            seen[url] = true;
            return true;
        });
    }

    function shouldPreloadUrl(url) {
        if (typeof apiGetRequestProfile !== 'function') return true;
        var profile = apiGetRequestProfile(url);
        return !!profile && profile.tier !== 'heavy' && profile.cacheTtl > 0;
    }

    function pickGetter(url, options) {
        options = options || {};
        var profile = (typeof apiGetRequestProfile === 'function') ? apiGetRequestProfile(url) : null;
        var useFresh = options.noCache === true;
        var preferSilent = options.silent !== false;

        if (profile && profile.batchSafe) {
            if (useFresh && preferSilent && typeof apiBatchGetSilentFresh === 'function') return apiBatchGetSilentFresh;
            if (useFresh && typeof apiBatchGetFresh === 'function') return apiBatchGetFresh;
            if (preferSilent && typeof apiBatchGetSilent === 'function') return apiBatchGetSilent;
            if (typeof apiBatchGet === 'function') return apiBatchGet;
        }

        if (useFresh && preferSilent && typeof apiGetSilentFresh === 'function') return apiGetSilentFresh;
        if (useFresh && typeof apiGetFresh === 'function') return apiGetFresh;
        if (preferSilent && typeof apiGetSilent === 'function') return apiGetSilent;
        return apiGet;
    }

    function preloadUrls(urls, options) {
        return uniqueUrls(urls).filter(shouldPreloadUrl).reduce(function(chain, url) {
            return chain.then(function() {
                return pickGetter(url, options)(url).catch(function() {
                    // Ignore preload failures so page flow is never blocked.
                });
            });
        }, Promise.resolve());
    }

    function preloadCriticalData() {
        if (typeof apiGet !== 'function') {
            console.warn('[Preloader] apiGet not available, skip preload');
            return Promise.resolve();
        }
        return preloadUrls(CRITICAL_APIS, { silent: true });
    }

    function preloadPageData(pageKey, options) {
        options = options || {};
        if (typeof FastBeePageRequestContracts === 'undefined') {
            return Promise.resolve();
        }

        var contract = FastBeePageRequestContracts[pageKey];
        if (!contract) return Promise.resolve();

        var urls = [].concat(contract.firstScreen || []);
        if (options.includeDeferred === true) {
            urls = urls.concat(contract.deferred || []);
        }

        return preloadUrls(urls, { silent: true, noCache: options.noCache === true });
    }

    function getData(url, params) {
        if (typeof apiGet !== 'function') {
            return Promise.reject(new Error('apiGet not available'));
        }
        return apiGet(url, params);
    }

    function invalidate(url) {
        if (typeof apiInvalidateCache === 'function') {
            apiInvalidateCache(url);
        }
    }

    function clearAll() {
        if (typeof apiInvalidateCache === 'function') {
            apiInvalidateCache();
        }
    }

    function getCriticalAPIs() {
        return CRITICAL_APIS.slice();
    }

    return {
        preloadCriticalData: preloadCriticalData,
        getData: getData,
        invalidate: invalidate,
        clearAll: clearAll,
        getCriticalAPIs: getCriticalAPIs,
        preloadPageData: preloadPageData
    };
})();
