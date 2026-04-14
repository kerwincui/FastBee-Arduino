/**
 * fetch-api.js — 原生 fetch 封装（替代 axios.min.js 54KB）
 * 提供与原 axios 配置等价的功能：
 *   - 自动注入 Authorization: Bearer <token>
 *   - 统一返回 response.data（JSON body）
 *   - 统一错误处理（HTTP 状态码 → Notification 提示）
 *   - 超时控制（默认 15 秒）
 *   - Content-Type: application/x-www-form-urlencoded（POST/PUT）
 *
 * RequestGovernor — ESP32 资源保护层：
 *   - 并发限制（最多 2 个在途请求）
 *   - FIFO 队列 + 优先级（HIGH=用户操作 / LOW=后台轮询）
 *   - 503/网络错误 → 全局冷却退避（1s→10s 指数递增）
 *   - 请求去重（相同 URL+方法 合并 Promise）
 *   - 页面切换时取消排队中的旧请求
 *   - GET 响应缓存（可配置 TTL）
 *
 * 全局暴露：apiGet / apiPost / apiPut / apiDelete / apiAbortPageRequests / apiInvalidateCache
 * 兼容：state.js 和其他模块中原有的 apiGet/apiPost/apiPut/apiDelete 调用
 */
(function () {
    'use strict';

    // ── 配置 ─────────────────────────────────────────────────────────────────
    const BASE_URL = (location.hostname === 'localhost' || location.hostname === '127.0.0.1')
        ? 'http://fastbee.local'
        : (location.origin || 'http://fastbee.local');

    const DEFAULT_TIMEOUT = 15000; // ms，ESP32 并发连接有限，需要更长超时
    const RESTART_TIMEOUT = 15000; // ms，重启操作需要更长超时

    // ── 工具：序列化为 URL 编码 ───────────────────────────────────────────────
    function toUrlEncoded(data) {
        if (!data) return '';
        return Object.keys(data)
            .map(k => encodeURIComponent(k) + '=' + encodeURIComponent(data[k] == null ? '' : data[k]))
            .join('&');
    }

    // ── 工具：带超时的 fetch ──────────────────────────────────────────────────
    function fetchWithTimeout(url, options, timeoutMs) {
        const controller = new AbortController();
        const timer = setTimeout(() => controller.abort(), timeoutMs);
        return fetch(url, Object.assign({ signal: controller.signal }, options))
            .finally(() => clearTimeout(timer));
    }

    // ── 工具：构造完整 URL ────────────────────────────────────────────────────
    function buildUrl(path, params) {
        const url = new URL(path, BASE_URL);
        if (params) {
            Object.keys(params).forEach(k => {
                if (params[k] != null) url.searchParams.append(k, params[k]);
            });
        }
        return url.toString();
    }

    // ── 核心请求函数 ──────────────────────────────────────────────────────────
    // options.silent: 为 true 时跳过全局错误处理（用于后台轮询等不应影响全局状态的请求）
    function request(method, path, options, timeoutMs) {
        options = options || {};
        var silent = options.silent || false;
        const token = localStorage.getItem('auth_token');
        const headers = Object.assign({}, options.headers);

        if (token) {
            headers['Authorization'] = 'Bearer ' + token;
        }

        const fetchOptions = {
            method: method,
            headers: headers,
            credentials: 'include',
            cache: 'no-store',
        };

        if (options.body) {
            fetchOptions.body = options.body;
        }

        const url = buildUrl(path, options.params);
        const timeout = timeoutMs || DEFAULT_TIMEOUT;

        return fetchWithTimeout(url, fetchOptions, timeout)
            .then(function (response) {
                return response.json().catch(function () { return {}; })
                    .then(function (data) {
                        if (response.ok) {
                            return data;
                        }
                        // HTTP 错误：统一处理
                        return Promise.reject({ status: response.status, data: data, response: response });
                    });
            })
            .catch(function (err) {
                if (silent) {
                    // 静默模式：不触发全局错误处理，直接传递错误给调用方
                    return Promise.reject(err);
                }
                if (err && err._pageAborted) {
                    // 页面切换导致的取消，静默忽略
                    return Promise.reject(err);
                }
                if (err && err.status !== undefined) {
                    // HTTP 层面的错误（4xx / 5xx）
                    _handleHttpError(err.status, err.data);
                } else if (err && err.name === 'AbortError') {
                    // 超时 - 对于重启等特殊操作，由调用方处理提示
                    err._handled = false;
                } else if (err && err.message && err.message.includes('fetch')) {
                    // fetch 错误可能是连接被服务器关闭（正常情况）
                    err._handled = false;
                } else {
                    // 其他网络错误
                    if (typeof Notification !== 'undefined') {
                        Notification.error('无法连接到设备，请检查网络连接', '网络错误');
                    }
                }
                return Promise.reject(err);
            });
    }

    // ── HTTP 错误处理（对应原 axios 响应拦截器）──────────────────────────────
    function _handleHttpError(status, data) {
        if (typeof Notification === 'undefined') return;
        const errMsg = (data && data.error) ? data.error : '请求失败';

        switch (status) {
            case 400:
                Notification.warning(errMsg || '请求参数错误', '参数错误');
                break;
            case 401:
                localStorage.removeItem('auth_token');
                localStorage.removeItem('sessionId');
                // 注意：不再删除 password 和 remember，以支持页面刷新后自动重新登录
                if (!document.getElementById('login-page') ||
                    document.getElementById('login-page').style.display === 'none') {
                    // 先关闭所有弹窗、下拉菜单等浮层，确保页面状态干净
                    if (typeof AppState !== 'undefined' && AppState.closeAllOverlays) {
                        AppState.closeAllOverlays();
                    }
                    Notification.warning('登录已过期，请重新登录', '会话超时');
                    setTimeout(function () {
                        // 如果在此期间已通过自动登录获取新 token，则不跳转
                        if (localStorage.getItem('auth_token')) return;
                        var lp = document.getElementById('login-page');
                        var ac = document.getElementById('app-container');
                        // 跳转到登录页面
                        if (lp && ac) {
                            ac.style.display = 'none';
                            lp.style.display = 'flex';
                        }
                    }, 1500);
                }
                break;
            case 403:
                Notification.warning('权限不足，无法执行此操作', '权限拒绝');
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

    // ── RequestGovernor — ESP32 资源保护调度器 ────────────────────────────────
    var Governor = {
        _queue: [],              // 待发请求队列
        _inflight: 0,            // 当前在途请求数
        MAX_CONCURRENT: 2,       // 最大并发 HTTP 请求数（SSE/MQTT 不占此计数）
        _cooldownUntil: 0,       // 全局冷却截止时间戳
        _backoffMs: 1000,        // 当前退避时间（成功后重置为 1000）
        _cooldownTimer: null,    // 冷却恢复定时器
        _dedupMap: {},           // 去重映射 { dedupKey: Promise }
        _pageGen: 0,             // 页面代次计数器
        _cache: {},              // GET 响应缓存 { path: { data, ts } }
        _cacheTTL: {             // 可缓存的 URL 路径及其 TTL(ms)
            '/api/protocol/config': 60000,
            '/api/system/info': 30000,
            '/api/device/config': 120000
        },

        /**
         * 将请求加入调度队列
         * @param {string} method - HTTP 方法
         * @param {string} path - API 路径
         * @param {Object} options - 请求选项 { params, headers, body, silent }
         * @param {number} [timeoutMs] - 超时时间
         * @returns {Promise}
         */
        enqueue: function (method, path, options, timeoutMs) {
            var self = this;
            var fullUrl = buildUrl(path, options ? options.params : undefined);
            var body = options ? (options.body || '') : '';
            var dedupKey = method + ':' + fullUrl + (body ? '#' + body : '');
            var isSilent = !!(options && options.silent);

            // 非 GET 请求自动失效同路径缓存
            if (method !== 'GET') {
                delete this._cache[path];
            }

            // GET 请求检查缓存
            if (method === 'GET' && this._cacheTTL[path]) {
                var cached = this._cache[path];
                if (cached && (Date.now() - cached.ts < this._cacheTTL[path])) {
                    return Promise.resolve(JSON.parse(JSON.stringify(cached.data)));
                }
            }

            // 请求去重 — 相同请求在途/排队时，复用同一个 Promise
            if (this._dedupMap[dedupKey]) {
                return this._dedupMap[dedupKey];
            }

            // 创建 Promise 和队列条目
            var resolve, reject;
            var promise = new Promise(function (res, rej) { resolve = res; reject = rej; });

            this._queue.push({
                method: method, path: path, options: options, timeoutMs: timeoutMs,
                resolve: resolve, reject: reject,
                priority: isSilent ? 0 : 1,  // 0=LOW(后台), 1=HIGH(用户操作)
                dedupKey: dedupKey,
                pageGen: this._pageGen
            });

            // 注册去重，完成后自动清理
            this._dedupMap[dedupKey] = promise;
            promise.then(
                function () { delete self._dedupMap[dedupKey]; },
                function () { delete self._dedupMap[dedupKey]; }
            );

            this._drain();
            return promise;
        },

        /**
         * 排水：从队列中取出请求执行，受并发数和冷却期约束
         */
        _drain: function () {
            var self = this;

            // 清理已过期的页面代次请求
            for (var i = this._queue.length - 1; i >= 0; i--) {
                if (this._queue[i].pageGen < this._pageGen) {
                    var expired = this._queue.splice(i, 1)[0];
                    delete this._dedupMap[expired.dedupKey];
                    var err = new Error('Request cancelled by page navigation');
                    err._pageAborted = true;
                    expired.reject(err);
                }
            }

            // 冷却期中：延迟排水
            var now = Date.now();
            if (now < this._cooldownUntil && this._queue.length > 0) {
                if (!this._cooldownTimer) {
                    var delay = this._cooldownUntil - now;
                    this._cooldownTimer = setTimeout(function () {
                        self._cooldownTimer = null;
                        self._drain();
                    }, delay);
                }
                return;
            }

            // 在并发容量内处理队列
            while (this._inflight < this.MAX_CONCURRENT && this._queue.length > 0) {
                // 优先执行 HIGH 优先级，同级别 FIFO
                var bestIdx = 0;
                for (var j = 1; j < this._queue.length; j++) {
                    if (this._queue[j].priority > this._queue[bestIdx].priority) {
                        bestIdx = j;
                    }
                }

                var entry = this._queue.splice(bestIdx, 1)[0];
                this._inflight++;
                this._execute(entry);
            }
        },

        /**
         * 执行单个请求
         */
        _execute: function (entry) {
            var self = this;
            request(entry.method, entry.path, entry.options, entry.timeoutMs)
                .then(function (data) {
                    self._backoffMs = 1000; // 成功后重置退避
                    // 缓存 GET 响应
                    if (entry.method === 'GET' && self._cacheTTL[entry.path]) {
                        self._cache[entry.path] = { data: data, ts: Date.now() };
                    }
                    entry.resolve(data);
                })
                .catch(function (err) {
                    if (self._isCooldownError(err)) {
                        self._cooldownUntil = Date.now() + self._backoffMs;
                        console.warn('[Governor] Cooldown ' + self._backoffMs + 'ms after overload');
                        self._backoffMs = Math.min(self._backoffMs * 2, 10000);
                    }
                    entry.reject(err);
                })
                .finally(function () {
                    self._inflight--;
                    self._drain();
                });
        },

        /**
         * 判断错误是否应触发全局冷却
         */
        _isCooldownError: function (err) {
            if (!err) return false;
            // HTTP 503 (服务不可用) 或 429 (请求过多)
            if (err.status === 503 || err.status === 429) return true;
            // TypeError: Failed to fetch — 连接被拒、重置、DNS 失败等
            if (err instanceof TypeError) return true;
            return false;
        },

        /**
         * 页面切换时取消所有排队中的旧页面请求
         */
        abortPageRequests: function () {
            this._pageGen++;
            // 主动清理队列中的过期请求
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

        /**
         * 失效指定路径的缓存
         * @param {string} [urlPattern] - URL 路径模式，不传则清空全部缓存
         */
        invalidateCache: function (urlPattern) {
            if (!urlPattern) { this._cache = {}; return; }
            for (var key in this._cache) {
                if (this._cache.hasOwnProperty(key) && key.indexOf(urlPattern) !== -1) {
                    delete this._cache[key];
                }
            }
        }
    };

    // ── 全局 API 函数（通过 Governor 调度）────────────────────────────────────

    window.apiGet = function (url, params) {
        return Governor.enqueue('GET', url, { params: params || {} });
    };

    window.apiGetSilent = function (url, params) {
        return Governor.enqueue('GET', url, { params: params || {}, silent: true });
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

    window.apiPostJson = function (url, data) {
        return Governor.enqueue('POST', url, {
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify(data || {})
        });
    };

    window.apiDelete = function (url, params) {
        return Governor.enqueue('DELETE', url, { params: params || {} });
    };

    // ── 特殊请求（绕过 Governor，直接执行）───────────────────────────────────
    // 这些请求要么是系统关键操作，要么有超长超时，不应受调度器约束

    window.apiRestart = function (data) {
        return request('POST', '/api/system/restart', {
            headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
            body: toUrlEncoded(data || {})
        }, RESTART_TIMEOUT);
    };

    window.apiFactoryReset = function () {
        return request('POST', '/api/system/factory-reset', {}, RESTART_TIMEOUT);
    };

    const MQTT_TEST_TIMEOUT = 30000;
    window.apiMqttTest = function (data) {
        return request('POST', '/api/mqtt/test', {
            headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
            body: toUrlEncoded(data || {}),
            silent: true
        }, MQTT_TEST_TIMEOUT);
    };

    // ── Governor 控制接口 ────────────────────────────────────────────────────

    window.apiAbortPageRequests = function () { Governor.abortPageRequests(); };
    window.apiInvalidateCache = function (urlPattern) { Governor.invalidateCache(urlPattern); };

})();
