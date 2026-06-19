// ============================================================
// Request Governor: fetch-api + request scheduling
// ============================================================
(function () {
    'use strict';

    const BASE_URL = (location.hostname === 'localhost' || location.hostname === '127.0.0.1')
        ? 'http://fastbee.local'
        : (location.origin || 'http://fastbee.local');
    const DEFAULT_TIMEOUT = 15000;
    const RESTART_TIMEOUT = 15000;
    const MQTT_TEST_TIMEOUT = 45000;
    const CONTROL_PRIORITY = 3;
    const PRESSURE_INTERVAL_PROFILES = Object.freeze({
        modbusControl: [8000, 10000, 15000, 25000],
        mqttDeferred: [2000, 3000, 5000, 8000]
    });
    const GUARD_LEVEL_RANK = Object.freeze({
        NORMAL: 0,
        WARN: 1,
        SEVERE: 2,
        CRITICAL: 3
    });

    function toUrlEncoded(data) {
        if (!data) return '';
        return Object.keys(data).map(function (key) {
            var value = data[key] == null ? '' : data[key];
            return encodeURIComponent(key) + '=' + encodeURIComponent(value);
        }).join('&');
    }

    function fetchWithTimeout(url, options, timeoutMs) {
        const controller = new AbortController();
        const timer = setTimeout(function () { controller.abort(); }, timeoutMs);
        return fetch(url, Object.assign({ signal: controller.signal }, options))
            .finally(function () { clearTimeout(timer); });
    }

    function buildUrl(path, params) {
        const url = new URL(path, BASE_URL);
        if (params) {
            Object.keys(params).forEach(function (key) {
                if (params[key] != null) url.searchParams.append(key, params[key]);
            });
        }
        return url.toString();
    }

    function parseRetryAfterMs(headerValue) {
        if (!headerValue) return 0;

        var seconds = parseInt(headerValue, 10);
        if (!isNaN(seconds) && seconds > 0) return seconds * 1000;

        var retryAt = Date.parse(headerValue);
        if (!isNaN(retryAt)) return Math.max(0, retryAt - Date.now());

        return 0;
    }

    function normalizePath(path, params) {
        try {
            return new URL(buildUrl(path, params), BASE_URL).pathname;
        } catch (err) {
            var raw = String(path || '');
            var queryIndex = raw.indexOf('?');
            return queryIndex >= 0 ? raw.substring(0, queryIndex) : raw;
        }
    }

    function getProtocolCompactProfile(path, params) {
        try {
            var url = new URL(buildUrl(path, params), BASE_URL);
            if (url.pathname !== '/api/protocol/config' || url.searchParams.get('compact') !== '1') {
                return null;
            }
            var section = url.searchParams.get('section') || '';
            if (section === 'mqtt') {
                return { tier: 'cheap', cost: 1, cacheTtl: 60000, priority: 2, timeoutMs: 8000 };
            }
            return { tier: 'normal', cost: 1, cacheTtl: 60000, priority: 2, timeoutMs: 10000 };
        } catch (err) {
            return null;
        }
    }

    function normalizeGuardLevel(level) {
        var normalized = String(level || 'NORMAL').toUpperCase();
        return Object.prototype.hasOwnProperty.call(GUARD_LEVEL_RANK, normalized)
            ? normalized
            : 'NORMAL';
    }

    function resolvePressureInterval(profileName, rank, fallbackMs) {
        var profile = PRESSURE_INTERVAL_PROFILES[profileName];
        if (!profile) return Number(fallbackMs || 0);

        var safeRank = Math.max(0, Math.min(profile.length - 1, Number(rank || 0)));
        return Number(profile[safeRank] || fallbackMs || 0);
    }

    const DEFAULT_REQUEST_PROFILE = Object.freeze({
        tier: 'normal',
        cost: 1,
        batchSafe: false,
        cacheTtl: 0,
        priority: 1,
        timeoutMs: DEFAULT_TIMEOUT
    });

    const REQUEST_PROFILE_MAP = Object.freeze({
        '/api/auth/session': { tier: 'cheap', cost: 1, priority: 3, timeoutMs: 8000 },
        '/api/batch': { tier: 'heavy', cost: 2, priority: 2, timeoutMs: 15000 },
        '/api/system/status': { tier: 'cheap', cost: 1, batchSafe: true, cacheTtl: 5000, priority: 3, timeoutMs: 8000 },
        '/api/system/info': { tier: 'cheap', cost: 1, batchSafe: true, cacheTtl: 30000, priority: 3, timeoutMs: 10000 },
        '/api/health': { tier: 'cheap', cost: 1, batchSafe: true, cacheTtl: 5000, priority: 2, timeoutMs: 8000 },
        '/api/system/health': { tier: 'cheap', cost: 1, batchSafe: true, cacheTtl: 5000, priority: 2, timeoutMs: 8000 },
        '/api/network/status': { tier: 'cheap', cost: 1, batchSafe: true, cacheTtl: 5000, priority: 3, timeoutMs: 8000 },
        '/api/network/config': { tier: 'cheap', cost: 1, batchSafe: true, cacheTtl: 60000, priority: 2, timeoutMs: 10000 },
        '/api/device/config': { tier: 'cheap', cost: 1, batchSafe: true, cacheTtl: 120000, priority: 2, timeoutMs: 10000 },
        '/api/device/info': { tier: 'cheap', cost: 1, cacheTtl: 60000, priority: 2, timeoutMs: 10000 },
        '/api/device/time': { tier: 'cheap', cost: 1, batchSafe: true, cacheTtl: 10000, priority: 2, timeoutMs: 8000 },
        '/api/config': { tier: 'cheap', cost: 1, cacheTtl: 60000, timeoutMs: 10000 },
        '/api/config/transfer/list': { tier: 'normal', cost: 1, cacheTtl: 5000, timeoutMs: 10000 },
        '/api/config/transfer/import': { tier: 'heavy', cost: 3, priority: 2, timeoutMs: 30000 },
        '/api/config/transfer/import-chunk': { tier: 'heavy', cost: 2, priority: 2, timeoutMs: 30000 },
        '/api/modbus/status': { tier: 'normal', cost: 2, cacheTtl: 10000, timeoutMs: 12000 },
        '/api/protocol/config': { tier: 'heavy', cost: 3, cacheTtl: 60000, timeoutMs: 20000 },
        '/api/protocol/mqtt/config': { tier: 'normal', cost: 2, priority: 2, timeoutMs: 20000 },
        '/api/protocol/modbus-rtu/config': { tier: 'heavy', cost: 3, priority: 2, timeoutMs: 30000 },
        '/api/periph-exec': { tier: 'normal', cost: 2, cacheTtl: 15000, timeoutMs: 15000 },
        '/api/periph-exec/controls': { tier: 'heavy', cost: 2, cacheTtl: 60000, timeoutMs: 20000 },
        '/api/periph-exec/events/static': { tier: 'normal', cost: 2, cacheTtl: 120000, timeoutMs: 12000 },
        '/api/periph-exec/events/dynamic': { tier: 'heavy', cost: 3, cacheTtl: 15000, timeoutMs: 15000 },
        '/api/periph-exec/events/categories': { tier: 'cheap', cost: 1, cacheTtl: 120000, timeoutMs: 10000 },
        '/api/periph-exec/trigger-types': { tier: 'cheap', cost: 1, cacheTtl: 120000, timeoutMs: 10000 },
        '/api/modbus/coil/control': { tier: 'normal', cost: 1, timeoutMs: 12000 },
        '/api/modbus/coil/batch': { tier: 'heavy', cost: 3, timeoutMs: 15000 },
        '/api/modbus/coil/delay': { tier: 'normal', cost: 2, timeoutMs: 15000 },
        '/api/modbus/register/write': { tier: 'normal', cost: 1, timeoutMs: 12000 },
        '/api/modbus/register/batch-write': { tier: 'heavy', cost: 3, timeoutMs: 15000 },
        '/api/modbus/motor/control': { tier: 'normal', cost: 1, timeoutMs: 15000 },
        '/api/modbus/device/address': { tier: 'heavy', cost: 2, timeoutMs: 15000 },
        '/api/modbus/device/baudrate': { tier: 'heavy', cost: 2, timeoutMs: 15000 }
    });

    const REQUEST_PROFILE_PREFIXES = Object.freeze([
        { prefix: '/api/auth/', profile: { tier: 'cheap', cost: 1, priority: 3, timeoutMs: 8000 } },
        { prefix: '/api/periph-exec', profile: { tier: 'normal', cost: 2, timeoutMs: 15000 } },
        { prefix: '/api/peripherals/status', profile: { tier: 'normal', cost: 2, cacheTtl: 5000, timeoutMs: 12000 } },
        { prefix: '/api/peripherals', profile: { tier: 'normal', cost: 2, timeoutMs: 15000 } },
        { prefix: '/api/modbus/register/read', profile: { tier: 'normal', cost: 2, timeoutMs: 12000 } },
        { prefix: '/api/modbus/coil/status', profile: { tier: 'cheap', cost: 1, timeoutMs: 10000 } },
        { prefix: '/api/modbus/device/inputs', profile: { tier: 'normal', cost: 2, timeoutMs: 12000 } }
    ]);

    const PAGE_REQUEST_CONTRACTS = Object.freeze({
        dashboard: {
            firstScreen: ['/api/system/status', '/api/system/info', '/api/network/status'],
            deferred: []
        },
        device: {
            firstScreen: ['/api/device/config', '/api/system/info'],
            deferred: ['/api/device/time', '/api/network/status']
        },
        protocol: {
            firstScreen: ['/api/protocol/config?compact=1&section=mqtt'],
            deferred: ['/api/modbus/status?compact=1', '/api/peripherals?compact=1&enabledOnly=1&pageSize=100&category=communication']
        },
        periphExec: {
            firstScreen: ['/api/peripherals?compact=1&enabledOnly=1&pageSize=100', '/api/periph-exec?pageSize=100'],
            deferred: ['/api/protocol/config?compact=1&section=periph-exec', '/api/periph-exec/events/static', '/api/periph-exec/events/categories']
        }
    });

    function cloneProfile(profile) {
        return Object.assign({}, DEFAULT_REQUEST_PROFILE, profile || {});
    }

    function resolveRequestProfile(path, params, overrideProfile) {
        var normalized = normalizePath(path, params);
        var profile = getProtocolCompactProfile(path, params) || REQUEST_PROFILE_MAP[normalized];

        if (!profile) {
            for (var i = 0; i < REQUEST_PROFILE_PREFIXES.length; i++) {
                if (normalized.indexOf(REQUEST_PROFILE_PREFIXES[i].prefix) === 0) {
                    profile = REQUEST_PROFILE_PREFIXES[i].profile;
                    break;
                }
            }
        }

        return cloneProfile(Object.assign({}, profile || {}, overrideProfile || {}));
    }

    function request(method, path, options, timeoutMs) {
        options = options || {};
        var silent = !!options.silent;
        const token = localStorage.getItem('auth_token');
        const headers = Object.assign({}, options.headers);
        if (token) headers.Authorization = 'Bearer ' + token;

        const fetchOptions = {
            method: method,
            headers: headers,
            credentials: 'include',
            cache: 'no-store'
        };

        if (options.body) fetchOptions.body = options.body;

        const url = buildUrl(path, options.params);
        const timeout = timeoutMs || DEFAULT_TIMEOUT;

        return fetchWithTimeout(url, fetchOptions, timeout)
            .then(function (response) {
                return response.json().catch(function () { return {}; }).then(function (data) {
                    if (response.ok) return data;
                    return Promise.reject({
                        status: response.status,
                        data: data,
                        response: response,
                        retryAfterMs: parseRetryAfterMs(response.headers ? response.headers.get('Retry-After') : '')
                    });
                });
            })
            .catch(function (err) {
                if (silent) return Promise.reject(err);
                if (err && err._pageAborted) return Promise.reject(err);

                if (err && err.status !== undefined) {
                    _handleHttpError(err.status, err.data, token);
                    err._handled = true;
                } else if (err && err.name === 'AbortError') {
                    err._handled = false;
                } else if (err && err.message && err.message.indexOf('fetch') !== -1) {
                    err._handled = false;
                } else if (typeof Notification !== 'undefined') {
                    Notification.error('无法连接到设备，请检查网络连接', '网络错误');
                }

                return Promise.reject(err);
            });
    }

    function _handleHttpError(status, data, requestToken) {
        if (typeof Notification === 'undefined') return;

        const errMsg = (data && data.error) ? data.error : '请求失败';
        switch (status) {
            case 400:
                Notification.warning(errMsg || '请求参数错误', '参数错误');
                break;
            case 401:
                if (requestToken && localStorage.getItem('auth_token') &&
                    localStorage.getItem('auth_token') !== requestToken) {
                    return;
                }
                localStorage.removeItem('auth_token');
                localStorage.removeItem('sessionId');
                if (!document.getElementById('login-page') || document.getElementById('login-page').style.display === 'none') {
                    if (typeof AppState !== 'undefined' && AppState.closeAllOverlays) AppState.closeAllOverlays();
                    Notification.warning('登录已过期，请重新登录', '会话超时');
                    setTimeout(function () {
                        if (localStorage.getItem('auth_token')) return;
                        var loginPage = document.getElementById('login-page');
                        var appContainer = document.getElementById('app-container');
                        if (loginPage && appContainer) {
                            appContainer.classList.add('fb-hidden');
                            loginPage.style.display = 'flex';
                        }
                    }, 1500);
                }
                break;
            case 403:
                Notification.warning('请求被拒绝', '访问受限');
                break;
            case 404:
                Notification.warning('请求的资源不存在', '未找到');
                break;
            case 409:
                Notification.warning(errMsg || '资源冲突（可能已存在）', '冲突');
                break;
            case 429:
                Notification.warning('操作过于频繁，请稍后再试', '请求限流');
                break;
            case 500:
                Notification.error('服务器内部错误，请检查设备状态', '服务器错误');
                break;
            case 503:
                Notification.error('服务暂时不可用', '服务不可用');
                break;
            default:
                Notification.warning(errMsg, '请求失败(' + status + ')');
        }
    }

    function describeApiError(err, fallback) {
        if (err && err._pageAborted) return '';
        if (err && err._handled) return '';
        if (err && err.name === 'AbortError') return '\u8bf7\u6c42\u8d85\u65f6\uff0c\u8bbe\u5907\u53ef\u80fd\u6b63\u5fd9\uff0c\u8bf7\u7a0d\u540e\u91cd\u8bd5';
        if (err && err.status === 503) return '\u8bbe\u5907\u8d44\u6e90\u7d27\u5f20\uff0c\u8bf7\u7a0d\u540e\u91cd\u8bd5';
        if (err && err.status === 504) return '\u8bbe\u5907\u54cd\u5e94\u8d85\u65f6\uff0c\u8bf7\u7a0d\u540e\u91cd\u8bd5';
        if (err instanceof TypeError || (err && err.message && /fetch/i.test(err.message))) {
            return '\u8bbe\u5907\u8fde\u63a5\u5931\u8d25\uff0c\u8bf7\u68c0\u67e5\u7f51\u7edc\u540e\u91cd\u8bd5';
        }
        return fallback || '\u8bf7\u6c42\u5931\u8d25';
    }

    var Governor = {
        _queue: [],
        _inflight: 0,
        _inflightWeight: 0,
        _heavyInflight: 0,
        _cooldownUntil: 0,
        _backoffMs: 1000,
        _cooldownTimer: null,
        _dedupMap: {},
        _pageGen: 0,
        _seq: 0,
        _cache: {},
        _batchQueue: [],
        _batchTimer: null,
        _pressureState: {
            level: 'NORMAL',
            updatedAt: 0,
            recoveryUntil: 0,
            source: 'init'
        },

        MAX_CONCURRENT: 2,
        MAX_INFLIGHT_WEIGHT: 2,
        MAX_HEAVY_INFLIGHT: 1,
        BATCH_WINDOW_MS: 25,

        enqueue: function (method, path, options, timeoutMs) {
            var self = this;
            options = options || {};

            var params = options.params || {};
            var fullUrl = buildUrl(path, params);
            var normalizedPath = normalizePath(path, params);
            var profile = resolveRequestProfile(path, params, options.profile);
            var body = options.body || '';
            var isSilent = !!options.silent;
            var skipCache = options.noCache === true;
            var dedupKey = method + ':' + fullUrl + (body ? '#' + body : '') + (skipCache ? '#fresh' : '');
            var defaultPriority = isSilent ? 0 : profile.priority;
            if (!isSilent && method !== 'GET' && defaultPriority < 2) defaultPriority = 2;
            var priority = typeof options.priority === 'number' ? options.priority : defaultPriority;

            if (method !== 'GET') this._invalidatePathCache(normalizedPath);

            if (method === 'GET' && profile.cacheTtl > 0 && !skipCache) {
                var cached = this._cache[fullUrl];
                if (cached && (Date.now() - cached.ts < profile.cacheTtl)) {
                    return Promise.resolve(JSON.parse(JSON.stringify(cached.data)));
                }
            }

            if (this._dedupMap[dedupKey]) return this._dedupMap[dedupKey];

            var resolve;
            var reject;
            var promise = new Promise(function (res, rej) {
                resolve = res;
                reject = rej;
            });

            this._queue.push({
                method: method,
                path: path,
                options: options,
                timeoutMs: timeoutMs || profile.timeoutMs,
                resolve: resolve,
                reject: reject,
                priority: priority,
                dedupKey: dedupKey,
                pageGen: this._pageGen,
                fullUrl: fullUrl,
                normalizedPath: normalizedPath,
                profile: profile,
                cost: profile.cost || 1,
                seq: (++this._seq)
            });

            this._dedupMap[dedupKey] = promise;
            promise.then(
                function () { delete self._dedupMap[dedupKey]; },
                function () { delete self._dedupMap[dedupKey]; }
            );

            this._drain();
            return promise;
        },

        enqueuePriority: function (method, path, options, timeoutMs) {
            var nextOptions = Object.assign({}, options || {}, { priority: 2 });
            return this.enqueue(method, path, nextOptions, timeoutMs);
        },

        _invalidatePathCache: function (normalizedPath) {
            if (!normalizedPath) return;
            var patterns = [];
            var currentPath = normalizedPath;

            while (currentPath && patterns.indexOf(currentPath) === -1) {
                patterns.push(currentPath);

                var trimmedPath = currentPath.replace(/\/+$/, '');
                var segments = trimmedPath.split('/').filter(function (segment) {
                    return segment && segment.length > 0;
                });
                if (segments.length <= 2) break;

                segments.pop();
                currentPath = '/' + segments.join('/');
            }

            for (var key in this._cache) {
                if (!this._cache.hasOwnProperty(key)) continue;
                for (var i = 0; i < patterns.length; i++) {
                    if (key.indexOf(patterns[i]) !== -1) {
                        delete this._cache[key];
                        break;
                    }
                }
            }
        },

        _canStartEntry: function (entry) {
            var limits = this._getEffectiveLimits();
            if (limits.blockSilent && entry.priority <= 0) return false;
            if (this._inflight >= limits.maxConcurrent) return false;
            if (entry.profile && entry.profile.tier === 'heavy' && this._heavyInflight >= limits.maxHeavy) return false;
            // Prevent starvation: always allow at least one request to start
            // when nothing is in-flight, regardless of its cost.
            // This avoids deadlock when entry.cost > maxWeight (e.g. batch cost=2, limit=2).
            if (this._inflightWeight === 0) return true;
            if ((this._inflightWeight + entry.cost) > limits.maxWeight) return false;
            return true;
        },

        _pickNextQueueIndex: function () {
            var bestIdx = -1;
            for (var i = 0; i < this._queue.length; i++) {
                var candidate = this._queue[i];
                if (!this._canStartEntry(candidate)) continue;

                if (bestIdx === -1) {
                    bestIdx = i;
                    continue;
                }

                var current = this._queue[bestIdx];
                if (candidate.priority > current.priority
                    || (candidate.priority === current.priority && candidate.cost < current.cost)
                    || (candidate.priority === current.priority && candidate.cost === current.cost && candidate.seq < current.seq)) {
                    bestIdx = i;
                }
            }
            return bestIdx;
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
                if (this._queue[k].priority >= 2) {
                    hasPriority = true;
                    break;
                }
            }

            if (!hasPriority && now < this._cooldownUntil && this._queue.length > 0) {
                if (!this._cooldownTimer) {
                    var delay = this._cooldownUntil - now;
                    this._cooldownTimer = setTimeout(function () {
                        self._cooldownTimer = null;
                        self._drain();
                    }, delay);
                }
                return;
            }

            while (this._queue.length > 0) {
                var bestIdx = this._pickNextQueueIndex();
                if (bestIdx < 0) break;

                var entry = this._queue.splice(bestIdx, 1)[0];
                this._inflight++;
                this._inflightWeight += entry.cost;
                if (entry.profile && entry.profile.tier === 'heavy') this._heavyInflight++;
                this._execute(entry);
            }
        },

        _execute: function (entry) {
            var self = this;
            request(entry.method, entry.path, entry.options, entry.timeoutMs)
                .then(function (data) {
                    if (entry.pageGen < self._pageGen && !(entry.options && entry.options.staleOk === true)) {
                        var staleErr = new Error('Request result ignored after page navigation');
                        staleErr._pageAborted = true;
                        entry.reject(staleErr);
                        return;
                    }
                    self._backoffMs = 1000;
                    if (entry.method === 'GET' && entry.profile && entry.profile.cacheTtl > 0) {
                        self._cache[entry.fullUrl] = { data: data, ts: Date.now() };
                    }
                    entry.resolve(data);
                })
                .catch(function (err) {
                    var isAuthEndpoint = entry.normalizedPath.indexOf('/api/auth/') === 0;
                    var cooldownMs = self._getCooldownDelayMs(err);
                    if (!isAuthEndpoint && cooldownMs > 0) {
                        self._cooldownUntil = Date.now() + cooldownMs;
                        console.warn('[Governor] Cooldown ' + cooldownMs + 'ms after overload');
                        self._backoffMs = Math.min(Math.max(cooldownMs, self._backoffMs) * 2, 10000);
                    }
                    entry.reject(err);
                })
                .finally(function () {
                    self._inflight--;
                    self._inflightWeight = Math.max(0, self._inflightWeight - (entry.cost || 1));
                    if (entry.profile && entry.profile.tier === 'heavy') {
                        self._heavyInflight = Math.max(0, self._heavyInflight - 1);
                    }
                    self._drain();
                });
        },

        _isCooldownError: function (err) {
            if (!err) return false;
            if (err.status === 503 || err.status === 429) return true;
            if (err instanceof TypeError) return true;
            return false;
        },

        _getCooldownDelayMs: function (err) {
            if (!this._isCooldownError(err)) return 0;
            if (err && err.retryAfterMs > 0) return Math.min(err.retryAfterMs, 15000);
            return this._backoffMs;
        },

        reportPressureState: function (runtimeData, source) {
            var memory = runtimeData && runtimeData.memory ? runtimeData.memory : {};
            var level = normalizeGuardLevel(memory.guardLevel);
            var now = Date.now();
            var recoveryMs = 0;

            if (level === 'CRITICAL') {
                recoveryMs = 20000;
            } else if (level === 'SEVERE') {
                recoveryMs = 12000;
            }

            this._pressureState.level = level;
            this._pressureState.updatedAt = now;
            this._pressureState.source = source || 'runtime';

            if (recoveryMs > 0) {
                this._pressureState.recoveryUntil = now + recoveryMs;
                this._cooldownUntil = Math.max(
                    this._cooldownUntil,
                    now + Math.min(recoveryMs, level === 'CRITICAL' ? 10000 : 6000)
                );
            } else if (now >= (this._pressureState.recoveryUntil || 0)) {
                this._pressureState.recoveryUntil = 0;
            }

            this._drain();
        },

        _getActivePressureRank: function () {
            var rank = GUARD_LEVEL_RANK[normalizeGuardLevel(this._pressureState.level)];
            if (rank < GUARD_LEVEL_RANK.SEVERE && Date.now() < (this._pressureState.recoveryUntil || 0)) {
                return GUARD_LEVEL_RANK.SEVERE;
            }
            return rank;
        },

        _getEffectiveLimits: function () {
            var rank = this._getActivePressureRank();

            if (rank >= GUARD_LEVEL_RANK.CRITICAL) {
                return {
                    maxConcurrent: 1,
                    maxWeight: 1,
                    maxHeavy: 0,
                    blockSilent: true
                };
            }

            if (rank >= GUARD_LEVEL_RANK.SEVERE) {
                return {
                    maxConcurrent: 1,
                    maxWeight: 2,
                    maxHeavy: 1,
                    blockSilent: true
                };
            }

            return {
                maxConcurrent: this.MAX_CONCURRENT,
                maxWeight: this.MAX_INFLIGHT_WEIGHT,
                maxHeavy: this.MAX_HEAVY_INFLIGHT,
                blockSilent: false
            };
        },

        getPressureInterval: function (profileName, fallbackMs) {
            return resolvePressureInterval(profileName, this._getActivePressureRank(), fallbackMs);
        },

        abortPageRequests: function () {
            this._pageGen++;
            for (var b = this._batchQueue.length - 1; b >= 0; b--) {
                if (this._batchQueue[b].pageGen < this._pageGen) {
                    var expiredBatch = this._batchQueue.splice(b, 1)[0];
                    var batchErr = new Error('Batch request cancelled by page navigation');
                    batchErr._pageAborted = true;
                    expiredBatch.reject(batchErr);
                }
            }
            if (this._batchQueue.length === 0 && this._batchTimer) {
                clearTimeout(this._batchTimer);
                this._batchTimer = null;
            }
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

        invalidateCache: function (urlPattern) {
            if (!urlPattern) {
                this._cache = {};
                return;
            }

            for (var key in this._cache) {
                if (this._cache.hasOwnProperty(key) && key.indexOf(urlPattern) !== -1) {
                    delete this._cache[key];
                }
            }
        },

        getState: function () {
            return {
                inflight: this._inflight,
                inflightWeight: this._inflightWeight,
                heavyInflight: this._heavyInflight,
                queueLength: this._queue.length,
                cooldownUntil: this._cooldownUntil,
                pageGeneration: this._pageGen,
                pressure: {
                    level: normalizeGuardLevel(this._pressureState.level),
                    activeRank: this._getActivePressureRank(),
                    updatedAt: this._pressureState.updatedAt || 0,
                    recoveryUntil: this._pressureState.recoveryUntil || 0,
                    source: this._pressureState.source || 'runtime'
                }
            };
        },

        batchGet: function (url, params, options) {
            var self = this;
            options = options || {};
            var profile = resolveRequestProfile(url, params);
            var fullUrl = buildUrl(url, params || {});
            var isSilent = !!options.silent;
            var skipCache = options.noCache === true;
            var priority = typeof options.priority === 'number' ? options.priority : (isSilent ? 0 : profile.priority);

            if (profile.batchSafe !== true || profile.tier === 'heavy') {
                return this.enqueue('GET', url, {
                    params: params || {},
                    profile: profile,
                    silent: isSilent,
                    priority: priority,
                    noCache: skipCache
                });
            }

            if (profile.cacheTtl > 0 && !skipCache) {
                var cached = this._cache[fullUrl];
                if (cached && (Date.now() - cached.ts < profile.cacheTtl)) {
                    return Promise.resolve(JSON.parse(JSON.stringify(cached.data)));
                }
            }

            return new Promise(function (resolve, reject) {
                self._batchQueue.push({
                    url: url,
                    params: params || {},
                    resolve: resolve,
                    reject: reject,
                    fullUrl: fullUrl,
                    profile: profile,
                    silent: isSilent,
                    priority: priority,
                    noCache: skipCache,
                    pageGen: self._pageGen
                });

                if (!self._batchTimer) {
                    self._batchTimer = setTimeout(function () {
                        self._flushBatch();
                    }, self.BATCH_WINDOW_MS);
                }
            });
        },

        batchGetMany: function (requests, options) {
            var self = this;
            options = options || {};
            requests = Array.isArray(requests) ? requests : [];
            if (requests.length === 0) return Promise.resolve([]);

            var pageGen = this._pageGen;
            var skipCache = options.noCache === true;
            var cachedResults = new Array(requests.length);
            var pendingRequests = [];
            var batchPriority = typeof options.priority === 'number' ? options.priority : 0;

            requests.forEach(function (item, index) {
                item = item || {};
                var params = item.params || {};
                var profile = resolveRequestProfile(item.url, params);
                var fullUrl = buildUrl(item.url, params);
                batchPriority = Math.max(batchPriority, options.silent ? 0 : (profile.priority || 0));
                if (!skipCache && profile.cacheTtl > 0) {
                    var cached = self._cache[fullUrl];
                    if (cached && (Date.now() - cached.ts < profile.cacheTtl)) {
                        cachedResults[index] = JSON.parse(JSON.stringify(cached.data));
                        return;
                    }
                }
                pendingRequests.push({
                    index: index,
                    url: item.url,
                    params: params,
                    profile: profile,
                    fullUrl: fullUrl
                });
            });

            if (pendingRequests.length === 0) return Promise.resolve(cachedResults);
            if (pendingRequests.length === 1) {
                var single = pendingRequests[0];
                return this.batchGet(single.url, single.params, options).then(function (res) {
                    cachedResults[single.index] = res;
                    return cachedResults;
                });
            }

            var payload = pendingRequests.map(function (item) {
                var req = { url: item.url };
                if (item.params && Object.keys(item.params).length > 0) req.params = item.params;
                return req;
            });

            return this.enqueue('POST', '/api/batch', {
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ requests: payload }),
                priority: batchPriority,
                silent: !!options.silent,
                staleOk: options.staleOk === true
            }).then(function (resp) {
                if (pageGen < self._pageGen && options.staleOk !== true) {
                    var staleErr = new Error('Batch result ignored after page navigation');
                    staleErr._pageAborted = true;
                    throw staleErr;
                }
                var results = resp.results || resp;
                if (!Array.isArray(results)) throw new Error('Invalid batch response');
                if (results.length < pendingRequests.length) throw new Error('Incomplete batch response');
                pendingRequests.forEach(function (pending, index) {
                    var item = results[index];
                    if (pending.profile && pending.profile.cacheTtl > 0 && !skipCache && item && item.success !== false && !item.error) {
                        self._cache[pending.fullUrl] = { data: item, ts: Date.now() };
                    }
                    cachedResults[pending.index] = item;
                });
                return cachedResults;
            });
        },

        _flushBatch: function () {
            var queue = this._batchQueue;
            this._batchQueue = [];
            this._batchTimer = null;

            var self = this;
            if (queue.length === 0) return;
            queue = queue.filter(function (item) {
                if (item.pageGen >= self._pageGen) return true;
                var err = new Error('Batch request cancelled by page navigation');
                err._pageAborted = true;
                item.reject(err);
                return false;
            });
            if (queue.length === 0) return;

            if (queue.length === 1) {
                self.enqueue('GET', queue[0].url, {
                    params: queue[0].params,
                    profile: queue[0].profile,
                    silent: queue[0].silent,
                    priority: queue[0].priority,
                    noCache: queue[0].noCache
                })
                    .then(queue[0].resolve)
                    .catch(queue[0].reject);
                return;
            }

            var batchPriority = 0;
            var requests = queue.map(function (item) {
                batchPriority = Math.max(batchPriority, item.priority || 0);
                var req = { url: item.url };
                if (item.params && Object.keys(item.params).length > 0) req.params = item.params;
                return req;
            });

            self.enqueue('POST', '/api/batch', {
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ requests: requests }),
                priority: batchPriority
            }).then(function (resp) {
                var results = resp.results || resp;
                if (!Array.isArray(results)) {
                    queue.forEach(function (item) {
                        item.reject(new Error('Invalid batch response'));
                    });
                    return;
                }

                results.forEach(function (item, index) {
                    if (index >= queue.length) return;
                    if (queue[index].pageGen < self._pageGen) {
                        var staleErr = new Error('Batch result ignored after page navigation');
                        staleErr._pageAborted = true;
                        queue[index].reject(staleErr);
                        return;
                    }

                    if (item.success === false || item.error) {
                        queue[index].reject(item.error || new Error('Batch sub-request failed'));
                        return;
                    }

                    if (queue[index].profile && queue[index].profile.cacheTtl > 0) {
                        self._cache[queue[index].fullUrl] = { data: item, ts: Date.now() };
                    }
                    queue[index].resolve(item);
                });

                for (var i = results.length; i < queue.length; i++) {
                    queue[i].reject(new Error('Batch response missing result'));
                }
            }).catch(function (err) {
                queue.forEach(function (item) { item.reject(err); });
            });
        }
    };

    window.apiDescribeError = describeApiError;

    window.apiNotifyError = function (err, fallback, title) {
        var message = describeApiError(err, fallback);
        if (!message || typeof Notification === 'undefined') return;
        Notification.error(message, title || '\u8bf7\u6c42\u5931\u8d25');
    };

    window.apiGet = function (url, params) {
        return Governor.enqueue('GET', url, { params: params || {} });
    };

    window.apiGetSilent = function (url, params) {
        return Governor.enqueue('GET', url, { params: params || {}, silent: true });
    };

    window.apiGetFresh = function (url, params) {
        return Governor.enqueue('GET', url, { params: params || {}, noCache: true });
    };

    window.apiGetSilentFresh = function (url, params) {
        return Governor.enqueue('GET', url, { params: params || {}, silent: true, noCache: true });
    };

    window.apiPost = function (url, data, timeoutMs) {
        return Governor.enqueue('POST', url, {
            headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
            body: toUrlEncoded(data || {})
        }, timeoutMs);
    };

    window.apiPostSilent = function (url, data) {
        return Governor.enqueue('POST', url, {
            headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
            body: toUrlEncoded(data || {}),
            silent: true
        });
    };

    window.apiPut = function (url, data) {
        return Governor.enqueue('PUT', url, {
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify(data || {})
        });
    };

    window.apiPutForm = function (url, data) {
        return Governor.enqueue('PUT', url, {
            headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
            body: toUrlEncoded(data || {})
        });
    };

    window.apiPostJson = function (url, data) {
        return Governor.enqueue('POST', url, {
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify(data || {})
        });
    };

    window.apiDelete = function (url, params) {
        return Governor.enqueue('DELETE', url, { params: params || {} });
    };

    window.apiPostPriority = function (url, data, timeoutMs) {
        return Governor.enqueue('POST', url, {
            headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
            body: toUrlEncoded(data || {}),
            priority: CONTROL_PRIORITY
        }, timeoutMs);
    };

    window.apiPostSilentPriority = function (url, data, timeoutMs) {
        return Governor.enqueue('POST', url, {
            headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
            body: toUrlEncoded(data || {}),
            silent: true,
            priority: CONTROL_PRIORITY
        }, timeoutMs);
    };

    window.apiRestart = function (data) {
        return request('POST', '/api/system/restart', {
            headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
            body: toUrlEncoded(data || {})
        }, RESTART_TIMEOUT);
    };

    window.apiFactoryReset = function () {
        return request('POST', '/api/system/factory-reset', {}, RESTART_TIMEOUT);
    };

    window.apiMqttTest = function (data) {
        return Governor.enqueue('POST', '/api/mqtt/test', {
            headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
            body: toUrlEncoded(data || {}),
            silent: true
        }, MQTT_TEST_TIMEOUT);
    };

    window.apiBatchGet = function (url, params) {
        return Governor.batchGet(url, params);
    };

    window.apiBatchGetSilent = function (url, params) {
        return Governor.batchGet(url, params, { silent: true });
    };

    window.apiBatchGetFresh = function (url, params) {
        return Governor.batchGet(url, params, { noCache: true });
    };

    window.apiBatchGetMany = function (requests, options) {
        return Governor.batchGetMany(requests, options || {});
    };

    window.apiBatchGetManyFresh = function (requests, options) {
        return Governor.batchGetMany(requests, Object.assign({}, options || {}, { noCache: true }));
    };

    window.apiBatchGetSilentFresh = function (url, params) {
        return Governor.batchGet(url, params, { silent: true, noCache: true });
    };

    window.apiAbortPageRequests = function () {
        Governor.abortPageRequests();
    };

    window.apiInvalidateCache = function (urlPattern) {
        Governor.invalidateCache(urlPattern);
    };

    window.apiGetRequestProfile = function (url, params) {
        return resolveRequestProfile(url, params);
    };

    window.apiGetGovernorState = function () {
        return Governor.getState();
    };

    window.apiReportRuntimePressure = function (runtimeData, source) {
        Governor.reportPressureState(runtimeData, source);
    };

    window.apiGetPressureState = function () {
        return Governor.getState().pressure;
    };

    window.apiGetPressureAwareInterval = function (profileName, fallbackMs) {
        return Governor.getPressureInterval(profileName, fallbackMs);
    };

    window.FastBeePageRequestContracts = PAGE_REQUEST_CONTRACTS;
})();
