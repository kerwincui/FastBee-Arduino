/**
 * fetch-api.js — 原生 fetch 封装（替代 axios.min.js 54KB）
 * 提供与原 axios 配置等价的功能：
 *   - 自动注入 Authorization: Bearer <token>
 *   - 统一返回 response.data（JSON body）
 *   - 统一错误处理（HTTP 状态码 → Notification 提示）
 *   - 超时控制（默认 8 秒）
 *   - Content-Type: application/x-www-form-urlencoded（POST/PUT）
 *
 * 全局暴露：apiGet / apiPost / apiPut / apiDelete
 * 兼容：state.js 和其他模块中原有的 apiGet/apiPost/apiPut/apiDelete 调用
 */
(function () {
    'use strict';

    // ── 配置 ─────────────────────────────────────────────────────────────────
    const BASE_URL = (location.hostname === 'localhost' || location.hostname === '127.0.0.1')
        ? 'http://fastbee.local'
        : (location.origin || 'http://fastbee.local');

    const DEFAULT_TIMEOUT = 8000; // ms，与原 axios 超时一致
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
    function request(method, path, options, timeoutMs) {
        options = options || {};
        const token = localStorage.getItem('auth_token');
        const headers = Object.assign({}, options.headers);

        if (token) {
            headers['Authorization'] = 'Bearer ' + token;
        }

        const fetchOptions = {
            method: method,
            headers: headers,
            credentials: 'include',
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
                localStorage.removeItem('password');
                if (!document.getElementById('login-page') ||
                    document.getElementById('login-page').style.display === 'none') {
                    Notification.warning('登录已过期，请重新登录', '会话超时');
                    setTimeout(function () {
                        var lp = document.getElementById('login-page');
                        var ac = document.getElementById('app-container');
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

    // ── 全局 API 函数（与原 axios 版本接口完全兼容）──────────────────────────

    /**
     * GET 请求
     * @param {string} url
     * @param {Object} [params] - 查询参数
     * @returns {Promise<any>}
     */
    window.apiGet = function (url, params) {
        return request('GET', url, { params: params || {} });
    };

    /**
     * POST 请求（application/x-www-form-urlencoded）
     * @param {string} url
     * @param {Object} [data] - 表单数据
     * @returns {Promise<any>}
     */
    window.apiPost = function (url, data) {
        return request('POST', url, {
            headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
            body: toUrlEncoded(data || {})
        });
    };

    /**
     * PUT 请求（application/json）
     * @param {string} url
     * @param {Object} [data] - JSON 数据
     * @returns {Promise<any>}
     */
    window.apiPut = function (url, data) {
        return request('PUT', url, {
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify(data || {})
        });
    };

    /**
     * DELETE 请求
     * @param {string} url
     * @returns {Promise<any>}
     */
    window.apiDelete = function (url) {
        return request('DELETE', url, {});
    };

    /**
     * 系统重启请求（使用更长超时）
     * @param {Object} [data] - 表单数据（包含 delay 参数）
     * @returns {Promise<any>}
     */
    window.apiRestart = function (data) {
        return request('POST', '/api/system/restart', {
            headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
            body: toUrlEncoded(data || {})
        }, RESTART_TIMEOUT);
    };

    /**
     * 恢复出厂设置请求（使用更长超时）
     * @returns {Promise<any>}
     */
    window.apiFactoryReset = function () {
        return request('POST', '/api/system/factory-reset', {}, RESTART_TIMEOUT);
    };

})();
