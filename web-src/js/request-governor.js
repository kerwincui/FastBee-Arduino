// ============================================================
// Request Governor: fetch-api + RequestGovernor
// ESP32 资源保护调度器，请求去重、缓存、并发控制
// ============================================================
(function () {
    'use strict';
    const BASE_URL = (location.hostname === 'localhost' || location.hostname === '127.0.0.1')
        ? 'http://fastbee.local'
        : (location.origin || 'http://fastbee.local');
    const DEFAULT_TIMEOUT = 15000;
    const RESTART_TIMEOUT = 15000;
    function toUrlEncoded(data) {
        if (!data) return '';
        return Object.keys(data).map(k => encodeURIComponent(k) + '=' + encodeURIComponent(data[k] == null ? '' : data[k])).join('&');
    }
    function fetchWithTimeout(url, options, timeoutMs) {
        const controller = new AbortController();
        const timer = setTimeout(() => controller.abort(), timeoutMs);
        return fetch(url, Object.assign({ signal: controller.signal }, options)).finally(() => clearTimeout(timer));
    }
    function buildUrl(path, params) {
        const url = new URL(path, BASE_URL);
        if (params) { Object.keys(params).forEach(k => { if (params[k] != null) url.searchParams.append(k, params[k]); }); }
        return url.toString();
    }
    function request(method, path, options, timeoutMs) {
        options = options || {};
        var silent = options.silent || false;
        const token = localStorage.getItem('auth_token');
        const headers = Object.assign({}, options.headers);
        if (token) { headers['Authorization'] = 'Bearer ' + token; }
        const fetchOptions = { method: method, headers: headers, credentials: 'include', cache: 'no-store' };
        if (options.body) { fetchOptions.body = options.body; }
        const url = buildUrl(path, options.params);
        const timeout = timeoutMs || DEFAULT_TIMEOUT;
        return fetchWithTimeout(url, fetchOptions, timeout)
            .then(function (response) {
                return response.json().catch(function () { return {}; }).then(function (data) {
                    if (response.ok) return data;
                    return Promise.reject({ status: response.status, data: data, response: response });
                });
            })
            .catch(function (err) {
                if (silent) return Promise.reject(err);
                if (err && err._pageAborted) return Promise.reject(err);
                if (err && err.status !== undefined) { _handleHttpError(err.status, err.data); }
                else if (err && err.name === 'AbortError') { err._handled = false; }
                else if (err && err.message && err.message.includes('fetch')) { err._handled = false; }
                else { if (typeof Notification !== 'undefined') { Notification.error('无法连接到设备，请检查网络连接', '网络错误'); } }
                return Promise.reject(err);
            });
    }
    function _handleHttpError(status, data) {
        if (typeof Notification === 'undefined') return;
        const errMsg = (data && data.error) ? data.error : '请求失败';
        switch (status) {
            case 400: Notification.warning(errMsg || '请求参数错误', '参数错误'); break;
            case 401:
                localStorage.removeItem('auth_token'); localStorage.removeItem('sessionId');
                if (!document.getElementById('login-page') || document.getElementById('login-page').style.display === 'none') {
                    if (typeof AppState !== 'undefined' && AppState.closeAllOverlays) { AppState.closeAllOverlays(); }
                    Notification.warning('登录已过期，请重新登录', '会话超时');
                    setTimeout(function () {
                        if (localStorage.getItem('auth_token')) return;
                        var lp = document.getElementById('login-page'); var ac = document.getElementById('app-container');
                        if (lp && ac) { ac.classList.add('fb-hidden'); lp.style.display = 'flex'; }
                    }, 1500);
                } break;
            case 403: Notification.warning('权限不足，无法执行此操作', '权限拒绝'); break;
            case 404: Notification.warning('请求的资源不存在', '未找到'); break;
            case 409: Notification.warning(errMsg || '资源冲突（可能已存在）', '冲突'); break;
            case 429: Notification.warning('操作过于频繁，请稍后再试', '请求限流'); break;
            case 500: Notification.error('服务器内部错误，请检查设备状态', '服务器错误'); break;
            case 503: Notification.error('服务暂时不可用', '服务不可用'); break;
            default: Notification.warning(errMsg, '请求失败(' + status + ')');
        }
    }
    // ── RequestGovernor — ESP32 资源保护调度器 ──
    var Governor = {
        _queue: [], _inflight: 0, MAX_CONCURRENT: 2,
        _cooldownUntil: 0, _backoffMs: 1000, _cooldownTimer: null,
        _dedupMap: {}, _pageGen: 0,
        _cache: {},
        _cacheTTL: {
            '/api/protocol/config': 60000, '/api/system/info': 30000, '/api/device/config': 120000,
            '/api/network/config': 60000, '/api/device/info': 60000, '/api/config': 60000,
            '/api/peripherals/types': 120000, '/api/periph-exec/controls': 60000,
            '/api/periph-exec/events/static': 120000, '/api/periph-exec/events/categories': 120000,
            '/api/periph-exec/trigger-types': 120000, '/api/permissions': 120000
        },
        enqueue: function (method, path, options, timeoutMs) {
            var self = this;
            var fullUrl = buildUrl(path, options ? options.params : undefined);
            var body = options ? (options.body || '') : '';
            var dedupKey = method + ':' + fullUrl + (body ? '#' + body : '');
            var isSilent = !!(options && options.silent);
            if (method !== 'GET') { delete this._cache[path]; }
            if (method === 'GET' && this._cacheTTL[path]) {
                var cached = this._cache[path];
                if (cached && (Date.now() - cached.ts < this._cacheTTL[path])) {
                    return Promise.resolve(JSON.parse(JSON.stringify(cached.data)));
                }
            }
            if (this._dedupMap[dedupKey]) { return this._dedupMap[dedupKey]; }
            var resolve, reject;
            var promise = new Promise(function (res, rej) { resolve = res; reject = rej; });
            this._queue.push({
                method: method, path: path, options: options, timeoutMs: timeoutMs,
                resolve: resolve, reject: reject,
                priority: isSilent ? 0 : 1, dedupKey: dedupKey, pageGen: this._pageGen
            });
            this._dedupMap[dedupKey] = promise;
            promise.then(function () { delete self._dedupMap[dedupKey]; }, function () { delete self._dedupMap[dedupKey]; });
            this._drain();
            return promise;
        },
        _drain: function () {
            var self = this;
            for (var i = this._queue.length - 1; i >= 0; i--) {
                if (this._queue[i].pageGen < this._pageGen) {
                    var expired = this._queue.splice(i, 1)[0];
                    delete this._dedupMap[expired.dedupKey];
                    var err = new Error('Request cancelled by page navigation');
                    err._pageAborted = true;
                    expired.reject(err);
                }
            }
            var now = Date.now();
            var hasPriority = false;
            for (var k = 0; k < this._queue.length; k++) {
                if (this._queue[k].priority >= 2) { hasPriority = true; break; }
            }
            if (!hasPriority && now < this._cooldownUntil && this._queue.length > 0) {
                if (!this._cooldownTimer) {
                    var delay = this._cooldownUntil - now;
                    this._cooldownTimer = setTimeout(function () { self._cooldownTimer = null; self._drain(); }, delay);
                }
                return;
            }
            while (this._inflight < this.MAX_CONCURRENT && this._queue.length > 0) {
                var bestIdx = 0;
                for (var j = 1; j < this._queue.length; j++) {
                    if (this._queue[j].priority > this._queue[bestIdx].priority) { bestIdx = j; }
                }
                var entry = this._queue.splice(bestIdx, 1)[0];
                this._inflight++;
                this._execute(entry);
            }
        },
        _execute: function (entry) {
            var self = this;
            request(entry.method, entry.path, entry.options, entry.timeoutMs)
                .then(function (data) {
                    self._backoffMs = 1000;
                    if (entry.method === 'GET' && self._cacheTTL[entry.path]) {
                        self._cache[entry.path] = { data: data, ts: Date.now() };
                    }
                    entry.resolve(data);
                })
                .catch(function (err) {
                    // 503 on auth endpoints should NOT trigger global cooldown (avoid blocking other requests)
                    var isAuthEndpoint = entry.path.indexOf('/api/auth/') === 0;
                    if (!isAuthEndpoint && self._isCooldownError(err)) {
                        self._cooldownUntil = Date.now() + self._backoffMs;
                        console.warn('[Governor] Cooldown ' + self._backoffMs + 'ms after overload');
                        self._backoffMs = Math.min(self._backoffMs * 2, 10000);
                    }
                    entry.reject(err);
                })
                .finally(function () { self._inflight--; self._drain(); });
        },
        _isCooldownError: function (err) {
            if (!err) return false;
            if (err.status === 503 || err.status === 429) return true;
            if (err instanceof TypeError) return true;
            return false;
        },
        abortPageRequests: function () {
            this._pageGen++;
            for (var i = this._queue.length - 1; i >= 0; i--) {
                if (this._queue[i].pageGen < this._pageGen) {
                    var expired = this._queue.splice(i, 1)[0];
                    delete this._dedupMap[expired.dedupKey];
                    var err = new Error('Request cancelled by page navigation');
                    err._pageAborted = true;
                    expired.reject(err);
                }
            }
        },
        enqueuePriority: function (method, path, options, timeoutMs) {
            var self = this;
            var body = options ? (options.body || '') : '';
            var fullUrl = buildUrl(path, options ? options.params : undefined);
            var dedupKey = method + ':' + fullUrl + (body ? '#' + body : '');
            if (method !== 'GET') { delete this._cache[path]; }
            if (this._dedupMap[dedupKey]) { return this._dedupMap[dedupKey]; }
            var resolve, reject;
            var promise = new Promise(function (res, rej) { resolve = res; reject = rej; });
            this._queue.unshift({
                method: method, path: path, options: options, timeoutMs: timeoutMs,
                resolve: resolve, reject: reject,
                priority: 2, dedupKey: dedupKey, pageGen: this._pageGen
            });
            this._dedupMap[dedupKey] = promise;
            promise.then(function () { delete self._dedupMap[dedupKey]; }, function () { delete self._dedupMap[dedupKey]; });
            this._drain();
            return promise;
        },
        invalidateCache: function (urlPattern) {
            if (!urlPattern) { this._cache = {}; return; }
            for (var key in this._cache) {
                if (this._cache.hasOwnProperty(key) && key.indexOf(urlPattern) !== -1) { delete this._cache[key]; }
            }
        },
        // ── 批量请求合并（50ms 窗口）──
        _batchQueue: [],
        _batchTimer: null,
        BATCH_WINDOW_MS: 50,
        batchGet: function (url, params) {
            var self = this;
            // 先检查缓存
            if (this._cacheTTL[url]) {
                var cached = this._cache[url];
                if (cached && (Date.now() - cached.ts < this._cacheTTL[url])) {
                    return Promise.resolve(JSON.parse(JSON.stringify(cached.data)));
                }
            }
            return new Promise(function (resolve, reject) {
                self._batchQueue.push({ url: url, params: params || {}, resolve: resolve, reject: reject });
                if (!self._batchTimer) {
                    self._batchTimer = setTimeout(function () { self._flushBatch(); }, self.BATCH_WINDOW_MS);
                }
            });
        },
        _flushBatch: function () {
            var queue = this._batchQueue;
            this._batchQueue = [];
            this._batchTimer = null;
            var self = this;
            if (queue.length === 0) return;
            if (queue.length === 1) {
                // 单个请求直接走普通通道
                self.enqueue('GET', queue[0].url, { params: queue[0].params })
                    .then(queue[0].resolve).catch(queue[0].reject);
                return;
            }
            // 多个请求合并为 /api/batch
            var requests = queue.map(function (q) {
                var r = { url: q.url };
                if (q.params && Object.keys(q.params).length > 0) r.params = q.params;
                return r;
            });
            self.enqueue('POST', '/api/batch', {
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ requests: requests })
            }).then(function (resp) {
                var results = resp.results || resp;
                if (!Array.isArray(results)) {
                    queue.forEach(function (q) { q.reject(new Error('Invalid batch response')); });
                    return;
                }
                results.forEach(function (item, i) {
                    if (i >= queue.length) return;
                    if (item.success === false || item.error) {
                        queue[i].reject(item.error || 'Batch sub-request failed');
                    } else {
                        // 缓存子响应
                        var url = queue[i].url;
                        if (self._cacheTTL[url]) {
                            self._cache[url] = { data: item.data || item, ts: Date.now() };
                        }
                        queue[i].resolve(item);
                    }
                });
            }).catch(function (err) {
                queue.forEach(function (q) { q.reject(err); });
            });
        }
    };
    // ── 全局 API 函数（通过 Governor 调度）──
    window.apiGet = function (url, params) { return Governor.enqueue('GET', url, { params: params || {} }); };
    window.apiGetSilent = function (url, params) { return Governor.enqueue('GET', url, { params: params || {}, silent: true }); };
    window.apiPost = function (url, data, timeoutMs) { return Governor.enqueue('POST', url, { headers: { 'Content-Type': 'application/x-www-form-urlencoded' }, body: toUrlEncoded(data || {}) }, timeoutMs); };
    window.apiPostSilent = function (url, data) { return Governor.enqueue('POST', url, { headers: { 'Content-Type': 'application/x-www-form-urlencoded' }, body: toUrlEncoded(data || {}), silent: true }); };
    window.apiPut = function (url, data) { return Governor.enqueue('PUT', url, { headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(data || {}) }); };
    window.apiPutForm = function (url, data) { return Governor.enqueue('PUT', url, { headers: { 'Content-Type': 'application/x-www-form-urlencoded' }, body: toUrlEncoded(data || {}) }); };
    window.apiPostJson = function (url, data) { return Governor.enqueue('POST', url, { headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(data || {}) }); };
    window.apiDelete = function (url, params) { return Governor.enqueue('DELETE', url, { params: params || {} }); };
    window.apiPostPriority = function (url, data, timeoutMs) { return Governor.enqueuePriority('POST', url, { headers: { 'Content-Type': 'application/x-www-form-urlencoded' }, body: toUrlEncoded(data || {}) }, timeoutMs); };
    // ── 特殊请求（绕过 Governor）──
    window.apiRestart = function (data) { return request('POST', '/api/system/restart', { headers: { 'Content-Type': 'application/x-www-form-urlencoded' }, body: toUrlEncoded(data || {}) }, RESTART_TIMEOUT); };
    window.apiFactoryReset = function () { return request('POST', '/api/system/factory-reset', {}, RESTART_TIMEOUT); };
    const MQTT_TEST_TIMEOUT = 30000;
    window.apiMqttTest = function (data) { return request('POST', '/api/mqtt/test', { headers: { 'Content-Type': 'application/x-www-form-urlencoded' }, body: toUrlEncoded(data || {}), silent: true }, MQTT_TEST_TIMEOUT); };
    // ── 批量请求 API ──
    window.apiBatchGet = function (url, params) { return Governor.batchGet(url, params); };
    // ── Governor 控制接口 ──
    window.apiAbortPageRequests = function () { Governor.abortPageRequests(); };
    window.apiInvalidateCache = function (urlPattern) { Governor.invalidateCache(urlPattern); };
})();
