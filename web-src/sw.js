// ============================================================
// FastBee Service Worker - Offline Cache & Resource Prefetch
// ============================================================

var CACHE_NAME = 'fastbee-v3';

var PRECACHE_URLS = [
    '/',
    '/index.html',
    '/css/main.css',
    '/css/pure-min.css',
    '/js/main.js',
    '/js/state.js',
    '/js/modules/i18n-engine.js',
    '/js/modules/i18n-zh-CN.js',
    '/assets/favicon.ico',
    '/assets/logo.png',
    // JS modules
    '/js/modules/protocol.js',
    '/js/modules/periph-exec.js',
    '/js/modules/device-control.js',
    '/js/modules/peripherals.js',
    '/js/modules/dashboard.js',
    '/js/modules/device-config.js',
    '/js/modules/network.js',
    '/js/modules/admin-bundle.js',
    // Page HTML
    '/pages/dashboard.html',
    '/pages/protocol.html',
    '/pages/device.html',
    '/pages/peripheral.html',
    '/pages/network.html',
    '/pages/admin.html',
    '/pages/rule-script.html',
    '/pages/modals.html'
];

// Sequential precache: one file at a time to avoid overwhelming ESP32
function sequentialCache(cache, urls) {
    return urls.reduce(function(chain, url) {
        return chain.then(function() {
            return fetch(url).then(function(resp) {
                if (resp.ok) return cache.put(url, resp);
            }).catch(function() { /* skip failed */ });
        }).then(function() {
            return new Promise(function(r) { setTimeout(r, 150); });
        });
    }, Promise.resolve());
}

// Install: skip waiting immediately, defer precache to avoid blocking first render
self.addEventListener('install', function(event) {
    self.skipWaiting();
});

// After activation, start background precache with delay
// (avoid competing with initial page load for ESP32 connections)
function startBackgroundPrecache() {
    setTimeout(function() {
        caches.open(CACHE_NAME).then(function(cache) {
            sequentialCache(cache, PRECACHE_URLS).then(function() {
                // Precache done, now safe to clean old caches
                caches.keys().then(function(names) {
                    names.filter(function(n) { return n !== CACHE_NAME; })
                         .forEach(function(n) { caches.delete(n); });
                });
            });
        });
    }, 5000);
}

// Activate: claim clients first, then precache, finally clean old caches
// (keep old cache alive until new cache is populated to avoid "empty cache" gap)
self.addEventListener('activate', function(event) {
    event.waitUntil(
        self.clients.claim()
            .then(function() { startBackgroundPrecache(); })
    );
});

// Fetch strategy
self.addEventListener('fetch', function(event) {
    var url = new URL(event.request.url);

    // API: pass through, no caching
    if (url.pathname.startsWith('/api/')) return;

    // HTML pages: stale-while-revalidate
    if (event.request.headers.get('accept') &&
        event.request.headers.get('accept').includes('text/html')) {
        event.respondWith(
            caches.match(event.request).then(function(cached) {
                var fetchPromise = fetch(event.request).then(function(resp) {
                    if (resp.ok) {
                        var clone = resp.clone();
                        caches.open(CACHE_NAME).then(function(c) { c.put(event.request, clone); });
                    }
                    return resp;
                }).catch(function() { return cached; });
                return cached || fetchPromise;
            })
        );
        return;
    }

    // JS/CSS/images: cache-first (try current cache, then old caches, then network)
    event.respondWith(
        caches.match(event.request).then(function(cached) {
            if (cached) return cached;
            return fetch(event.request).then(function(resp) {
                if (resp.ok) {
                    var clone = resp.clone();
                    caches.open(CACHE_NAME).then(function(c) { c.put(event.request, clone); });
                }
                return resp;
            });
        })
    );
});
