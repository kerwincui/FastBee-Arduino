// ============================================================
// FastBee Service Worker - Offline Cache & Resource Prefetch
// ============================================================

var CACHE_NAME = 'fastbee-v1';

var PRECACHE_URLS = [
    '/',
    '/index.html',
    '/css/main.css',
    '/css/pure-min.css',
    '/js/main.js',
    '/js/state.js',
    '/js/modules/i18n-engine.js',
    '/assets/favicon.ico',
    '/assets/logo.png'
];

// Install: precache core resources
self.addEventListener('install', function(event) {
    event.waitUntil(
        caches.open(CACHE_NAME).then(function(cache) {
            return cache.addAll(PRECACHE_URLS);
        }).then(function() {
            return self.skipWaiting();
        })
    );
});

// Activate: clean old caches
self.addEventListener('activate', function(event) {
    event.waitUntil(
        caches.keys().then(function(cacheNames) {
            return Promise.all(
                cacheNames.filter(function(name) {
                    return name !== CACHE_NAME;
                }).map(function(name) {
                    return caches.delete(name);
                })
            );
        }).then(function() {
            return self.clients.claim();
        })
    );
});

// Fetch strategy: network-first for HTML, cache-first for static assets
self.addEventListener('fetch', function(event) {
    var url = new URL(event.request.url);
    
    // API requests: network only (no caching)
    if (url.pathname.startsWith('/api/')) {
        return;
    }
    
    // HTML pages: network first, fallback to cache
    if (event.request.headers.get('accept') && 
        event.request.headers.get('accept').includes('text/html')) {
        event.respondWith(
            fetch(event.request).then(function(response) {
                var clone = response.clone();
                caches.open(CACHE_NAME).then(function(cache) {
                    cache.put(event.request, clone);
                });
                return response;
            }).catch(function() {
                return caches.match(event.request);
            })
        );
        return;
    }
    
    // JS/CSS/images: cache first, fallback to network
    event.respondWith(
        caches.match(event.request).then(function(cached) {
            if (cached) return cached;
            return fetch(event.request).then(function(response) {
                if (response.ok) {
                    var clone = response.clone();
                    caches.open(CACHE_NAME).then(function(cache) {
                        cache.put(event.request, clone);
                    });
                }
                return response;
            });
        })
    );
});
