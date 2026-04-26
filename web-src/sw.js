// ============================================================
// FastBee Service Worker - Development Version
// Production: auto-generated from sw.js.template by generate-sw-manifest.js
// ============================================================
var CACHE_VERSION = 'v-dev';
var CACHE_NAME = 'fastbee-' + CACHE_VERSION;
var MAX_IMG_ENTRIES = 30;

var PRECACHE_URLS = [
    '/',
    '/index.html',
    '/css/main.css',
    '/js/main.js',
    '/js/state.js',
    '/js/modules/i18n-engine.js',
    '/js/modules/i18n-zh-CN.js',
    '/assets/favicon.ico',
    '/assets/logo.png'
];

function sequentialCache(cache, urls) {
    var i = 0;
    function next() {
        if (i >= urls.length) return Promise.resolve();
        var url = urls[i++];
        return fetch(url).then(function(r) {
            if (r.ok) return cache.put(url, r);
        }).catch(function() {}).then(function() {
            return new Promise(function(r) { setTimeout(r, 100); });
        }).then(next);
    }
    return next();
}

function trimCache(cache) {
    return cache.keys().then(function(k) {
        if (k.length <= MAX_IMG_ENTRIES) return;
        return Promise.all(k.slice(0, k.length - MAX_IMG_ENTRIES).map(function(r) {
            return cache.delete(r);
        }));
    });
}

function offline() {
    return caches.match('/404.html.gz').then(function(r) {
        return r || new Response('Offline', { status: 503 });
    });
}

// 缓存过期检测 - 基于缓存响应的Date头或自定义时间戳
function isStaleCache(response, maxAgeMs) {
    if (!response) return true;
    var dateHeader = response.headers.get('date') || response.headers.get('sw-cached-at');
    if (!dateHeader) return true;
    var cacheTime = new Date(dateHeader).getTime();
    return (Date.now() - cacheTime) > (maxAgeMs || 86400000); // 默认24小时
}

function staleWhileRevalidate(event, trim) {
    return caches.match(event.request).then(function(cached) {
        var fetchPromise = fetch(event.request).then(function(resp) {
            if (resp.ok) {
                var clone = resp.clone();
                caches.open(CACHE_NAME).then(function(c) {
                    c.put(event.request, clone);
                    if (trim) trimCache(c);
                });
            }
            return resp;
        });
        // 有缓存且未过期：立即返回缓存，后台更新
        // 无缓存或已过期：等待网络
        if (cached && !isStaleCache(cached, 86400000)) {
            fetchPromise.catch(function(){}); // 后台静默更新
            return cached;
        }
        return fetchPromise.catch(function() { return cached || offline(); });
    });
}

self.addEventListener('install', function() { self.skipWaiting(); });

self.addEventListener('activate', function(e) {
    e.waitUntil(
        self.clients.claim().then(function() {
            return caches.keys().then(function(names) {
                return Promise.all(
                    names.filter(function(n) {
                        return n.startsWith('fastbee-') && n !== CACHE_NAME;
                    }).map(function(n) { return caches.delete(n); })
                );
            });
        }).then(function() {
            setTimeout(function() {
                caches.open(CACHE_NAME).then(function(c) {
                    sequentialCache(c, PRECACHE_URLS);
                });
            }, 5000);
        })
    );
});

self.addEventListener('fetch', function(e) {
    var url = new URL(e.request.url);
    if (url.origin !== self.location.origin) return;

    // SSE: pass through, never cache
    if (url.pathname === '/api/events') return;
    var accept = e.request.headers.get('Accept') || '';
    if (accept.indexOf('text/event-stream') !== -1) return;

    // API: Network First
    if (url.pathname.startsWith('/api/')) {
        e.respondWith(
            fetch(e.request).then(function(resp) {
                if (resp.ok) {
                    var clone = resp.clone();
                    caches.open(CACHE_NAME).then(function(c) { c.put(e.request, clone); });
                }
                return resp;
            }).catch(function() {
                return caches.match(e.request).then(function(c) { return c || offline(); });
            })
        );
        return;
    }

    // Static: Stale-While-Revalidate
    var ext = url.pathname.split('.').pop().split('?')[0].toLowerCase();
    var isStatic = ext === 'css' || ext === 'js' || ext === 'html';
    var isMedia = ext === 'png' || ext === 'jpg' || ext === 'jpeg' ||
        ext === 'gif' || ext === 'ico' || ext === 'svg' ||
        ext === 'woff' || ext === 'woff2' || ext === 'ttf';

    if (isStatic || isMedia) {
        e.respondWith(staleWhileRevalidate(e, isMedia));
        return;
    }

    // Navigation: Stale-While-Revalidate
    if (e.request.mode === 'navigate' || accept.indexOf('text/html') !== -1) {
        e.respondWith(staleWhileRevalidate(e, false));
        return;
    }

    // Default: Stale-While-Revalidate
    e.respondWith(staleWhileRevalidate(e, false));
});
