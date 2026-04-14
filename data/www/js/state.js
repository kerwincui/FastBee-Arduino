// ============================================================
// Core Bundle: fetch-api + notification + state
// 合并核心依赖，减少 ESP32 HTTP 请求数
// ============================================================

// ── fetch-api + RequestGovernor ──────────────────────────────
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
                        if (lp && ac) { ac.style.display = 'none'; lp.style.display = 'flex'; }
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
        _cacheTTL: { '/api/protocol/config': 60000, '/api/system/info': 30000, '/api/device/config': 120000 },
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
            if (now < this._cooldownUntil && this._queue.length > 0) {
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
                    if (self._isCooldownError(err)) {
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
        invalidateCache: function (urlPattern) {
            if (!urlPattern) { this._cache = {}; return; }
            for (var key in this._cache) {
                if (this._cache.hasOwnProperty(key) && key.indexOf(urlPattern) !== -1) { delete this._cache[key]; }
            }
        }
    };
    // ── 全局 API 函数（通过 Governor 调度）──
    window.apiGet = function (url, params) { return Governor.enqueue('GET', url, { params: params || {} }); };
    window.apiGetSilent = function (url, params) { return Governor.enqueue('GET', url, { params: params || {}, silent: true }); };
    window.apiPost = function (url, data, timeoutMs) { return Governor.enqueue('POST', url, { headers: { 'Content-Type': 'application/x-www-form-urlencoded' }, body: toUrlEncoded(data || {}) }, timeoutMs); };
    window.apiPostSilent = function (url, data) { return Governor.enqueue('POST', url, { headers: { 'Content-Type': 'application/x-www-form-urlencoded' }, body: toUrlEncoded(data || {}), silent: true }); };
    window.apiPut = function (url, data) { return Governor.enqueue('PUT', url, { headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(data || {}) }); };
    window.apiPostJson = function (url, data) { return Governor.enqueue('POST', url, { headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(data || {}) }); };
    window.apiDelete = function (url, params) { return Governor.enqueue('DELETE', url, { params: params || {} }); };
    // ── 特殊请求（绕过 Governor）──
    window.apiRestart = function (data) { return request('POST', '/api/system/restart', { headers: { 'Content-Type': 'application/x-www-form-urlencoded' }, body: toUrlEncoded(data || {}) }, RESTART_TIMEOUT); };
    window.apiFactoryReset = function () { return request('POST', '/api/system/factory-reset', {}, RESTART_TIMEOUT); };
    const MQTT_TEST_TIMEOUT = 30000;
    window.apiMqttTest = function (data) { return request('POST', '/api/mqtt/test', { headers: { 'Content-Type': 'application/x-www-form-urlencoded' }, body: toUrlEncoded(data || {}), silent: true }, MQTT_TEST_TIMEOUT); };
    // ── Governor 控制接口 ──
    window.apiAbortPageRequests = function () { Governor.abortPageRequests(); };
    window.apiInvalidateCache = function (urlPattern) { Governor.invalidateCache(urlPattern); };
})();

// ── notification ────────────────────────────────────────────
const Notification = {
    container: null, notifications: [],
    init() {
        this.container = document.getElementById('notification-container');
        if (!this.container) { this.container = document.createElement('div'); this.container.id = 'notification-container'; this.container.className = 'notification-container'; document.body.appendChild(this.container); }
    },
    show(options) {
        const id = 'notification-' + Date.now(); const duration = options.duration || 3000;
        const notification = document.createElement('div'); notification.id = id;
        notification.className = 'notification notification-' + (options.type || 'info');
        const icons = { primary: '✅', success: '✅', warning: '⚠️', error: '❌', info: 'ℹ️' };

        const header = document.createElement('div');
        header.className = 'notification-header';

        const title = document.createElement('div');
        title.className = 'notification-title';
        const icon = document.createElement('i');
        icon.textContent = icons[options.type] || icons.info;
        const titleText = document.createElement('span');
        titleText.textContent = options.title || this.getDefaultTitle(options.type);
        title.appendChild(icon);
        title.appendChild(titleText);

        const closeBtn = document.createElement('button');
        closeBtn.type = 'button';
        closeBtn.className = 'notification-close';
        closeBtn.textContent = '×';
        closeBtn.addEventListener('click', () => this.close(id));

        header.appendChild(title);
        header.appendChild(closeBtn);

        const body = document.createElement('div');
        body.className = 'notification-body';
        body.innerHTML = options.message || '';

        const progress = document.createElement('div');
        progress.className = 'notification-progress';
        const progressBar = document.createElement('div');
        progressBar.className = 'notification-progress-bar';
        progressBar.style.setProperty('--notification-duration', duration + 'ms');
        progress.appendChild(progressBar);

        notification.appendChild(header);
        notification.appendChild(body);
        notification.appendChild(progress);

        this.container.appendChild(notification);
        const notificationObj = { id: id, element: notification, timeout: null }; this.notifications.push(notificationObj);
        if (options.autoClose !== false) { notificationObj.timeout = setTimeout(() => { this.close(id); }, duration); }
        return id;
    },
    close(id) {
        const notification = document.getElementById(id);
        if (notification) { notification.classList.add('hiding');
            const notificationObj = this.notifications.find(n => n.id === id);
            if (notificationObj && notificationObj.timeout) { clearTimeout(notificationObj.timeout); }
            setTimeout(() => { if (notification.parentNode) { notification.parentNode.removeChild(notification); } this.notifications = this.notifications.filter(n => n.id !== id); }, 300);
        }
    },
    closeAll() { this.notifications.forEach(n => { this.close(n.id); }); },
    getDefaultTitle(type) { const t = { primary: '提示', success: '成功', warning: '警告', error: '错误', info: '信息' }; return t[type] || '通知'; },
    primary(message, title) { return this.show({ type: 'primary', title: title || '提示', message: message }); },
    success(message, title) { return this.show({ type: 'success', title: title || '成功', message: message }); },
    warning(message, title) { return this.show({ type: 'warning', title: title || '警告', message: message }); },
    error(message, title) { return this.show({ type: 'error', title: title || '错误', message: message }); },
    info(message, title) { return this.show({ type: 'info', title: title || '信息', message: message }); }
};

// ── state (AppState) ────────────────────────────────────────
// 全局工具函数（从 utils.js 迁移，确保始终可用）
function escapeHtml(str) {
    if (str == null) return '';
    return String(str).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;').replace(/'/g, '&#39;');
}

// 应用状态管理
const AppState = {
    currentPage: 'dashboard',
    configTab: 'modbus',
    currentUser: { name: '', role: '', canManageFs: false },
    sidebarCollapsed: false,
    _logAutoRefreshTimer: null,  // 日志自动刷新定时器

    // 已加载的模块记录
    _loadedModules: {},
    _moduleCallbacks: {},

    // SSE 连接管理
    _sseSource: null,
    _sseHandlers: {},  // { eventType: [handler1, handler2, ...] }
    _sseReconnectTimer: null,
    _sseReconnectDelay: 1000,

    // 页面模块动态加载器
    _loadedPages: {},
    _pageMapping: {
        'dashboard-page': 'dashboard',
        'protocol-page': 'protocol',
        'network-page': 'network',
        'device-page': 'device',
        'data-page': 'admin',
        'users-page': 'admin',
        'roles-page': 'admin',
        'logs-page': 'admin',
        'peripheral-page': 'peripheral',
        'periph-exec-page': 'peripheral',
        'device-control-page': 'peripheral',
        'rule-script-page': 'rule-script'
    },
    // 模块 → 打包文件映射（多个模块合并为一个文件）
    _bundleMap: { 'users': 'admin-bundle', 'roles': 'admin-bundle', 'files': 'admin-bundle', 'logs': 'admin-bundle', 'rule-script': 'admin-bundle' },
    _loadedFiles: {},
    // 模块加载队列（有限并发加载，最多4个并发）
    _moduleLoadQueue: [],
    _moduleLoadingCount: 0,
    _MAX_CONCURRENT_LOADS: 4,

    // ============ 初始化 ============
    init() {
        this.setupTheme();  // 主题初始化
        this.setupUserDropdown(); // 用户下拉菜单
        this.setupSidebarToggle();
        this.setupLanguage();
        this.setupConfigTabs();
        this.setupEventListeners();
        this.setupGlobalEventDelegation(); // 全局事件委托
        this.refreshPage();
    },

    // ============ 动态页面加载器 ============
    async loadPage(pageId) {
        var moduleName = this._pageMapping[pageId];
        if (!moduleName || this._loadedPages[moduleName]) return;

        try {
            var resp = await fetch('/pages/' + moduleName + '.html');
            if (!resp.ok) throw new Error('HTTP ' + resp.status);
            var html = await resp.text();
            var container = document.getElementById('content-area');
            if (container) {
                var wrapper = document.createElement('div');
                wrapper.innerHTML = html;
                // 将所有子元素移入 content-area
                while (wrapper.firstChild) {
                    container.appendChild(wrapper.firstChild);
                }
            }
            this._loadedPages[moduleName] = true;
        } catch(e) {
            console.error('[PageLoader] Failed to load:', moduleName, e);
        }
    },

    async _loadModals() {
        if (this._loadedPages['modals']) return;
        try {
            var resp = await fetch('/pages/modals.html');
            if (!resp.ok) return;
            var html = await resp.text();
            var wrapper = document.createElement('div');
            wrapper.innerHTML = html;
            while (wrapper.firstChild) {
                document.body.appendChild(wrapper.firstChild);
            }
            this._loadedPages['modals'] = true;
        } catch(e) {
            console.error('[PageLoader] Failed to load modals:', e);
        }
    },

    // ============ 全局事件委托 ============
    setupGlobalEventDelegation() {
        const self = this;
        // 全局事件委托 - 替代内联 onclick
        document.addEventListener('click', (e) => {
            // 处理 config-tab 点击（动态加载页面的 tab 切换）
            var tab = e.target.closest('.config-tab');
            if (tab) {
                var pageEl = tab.closest('[id$="-page"]');
                if (pageEl) {
                    var tabId = tab.getAttribute('data-tab');
                    self.showConfigTab(pageEl.id, tabId);
                }
                return;
            }
    
            const el = e.target.closest('[data-action]');
            if (!el) return;
    
            const action = el.dataset.action;
            const args = el.dataset.args ? el.dataset.args.split(',') : [];
    
            // 优先在 AppState 上查找方法，如果不存在则在 app 对象上查找
            if (typeof this[action] === 'function') {
                e.preventDefault();
                this[action](...args);
            } else if (typeof window.app !== 'undefined' && typeof window.app[action] === 'function') {
                e.preventDefault();
                window.app[action](...args);
            } else {
                // 方法尚未注册（模块可能还在加载中），延迟重试一次
                e.preventDefault();
                setTimeout(function() {
                    if (typeof self[action] === 'function') {
                        self[action](...args);
                    } else if (typeof window.app !== 'undefined' && typeof window.app[action] === 'function') {
                        window.app[action](...args);
                    }
                }, 800);
            }
        });

        // 全局 change 事件委托 - 替代内联 onchange
        document.addEventListener('change', (e) => {
            const el = e.target.closest('[data-change-action]');
            if (!el) return;
    
            const action = el.dataset.changeAction;
            const value = el.value;
    
            // 优先在 AppState 上查找方法，如果不存在则在 app 对象上查找
            if (typeof this[action] === 'function') {
                this[action](value);
            } else if (typeof window.app !== 'undefined' && typeof window.app[action] === 'function') {
                window.app[action](value);
            }
        });
    
        // 全局 submit 事件委托 - 处理协议页面表单提交
        document.addEventListener('submit', (e) => {
            const form = e.target.closest('form');
            if (!form) return;
    
            // 仅处理协议页面的表单
            const protocolForms = ['mqtt-form', 'modbus-rtu-form', 'modbus-tcp-form', 'http-form', 'coap-form', 'tcp-form'];
            if (protocolForms.includes(form.id)) {
                e.preventDefault();
                if (typeof self.saveProtocolConfig === 'function') {
                    self.saveProtocolConfig(form.id);
                }
            }
        });
    
        // 全局 input/change 事件委托 - 清除 MQTT 测试结果提示
        ['input', 'change'].forEach(function(eventType) {
            document.addEventListener(eventType, function(e) {
                const targetId = e.target.id;
                if (targetId === 'mqtt-client-id' || targetId === 'mqtt-auth-type') {
                    var resultEl = document.getElementById('mqtt-test-result');
                    if (resultEl) resultEl.textContent = '';
                }
            });
        });
    },

    // 触发 OTA 文件选择
    triggerOtaFileSelect() {
        const fileInput = document.getElementById('ota-file');
        if (fileInput) fileInput.click();
    },

    getEl(ref) {
        if (!ref) return null;
        return typeof ref === 'string' ? document.getElementById(ref) : ref;
    },

    showElement(ref, displayValue) {
        const el = this.getEl(ref);
        if (!el) return null;
        el.classList.remove('is-hidden');
        if (displayValue) {
            el.style.display = displayValue;
        } else {
            el.style.removeProperty('display');
        }
        return el;
    },

    hideElement(ref) {
        const el = this.getEl(ref);
        if (!el) return null;
        el.classList.add('is-hidden');
        el.style.display = 'none';
        return el;
    },

    showModal(ref) {
        const el = this.showElement(ref, 'flex');
        if (el) {
            const self = this;
            // 点击遮罩层关闭
            el._modalOverlayHandler = function(e) {
                if (e.target === el) {
                    self.hideModal(ref);
                }
            };
            el.addEventListener('click', el._modalOverlayHandler);
                
            // 绑定关闭按钮
            const closeBtn = el.querySelector('.modal-close, .modal-close-btn');
            if (closeBtn) {
                closeBtn._modalCloseHandler = function() {
                    self.hideModal(ref);
                };
                closeBtn.addEventListener('click', closeBtn._modalCloseHandler);
            }
        }
        return el;
    },
    
    hideModal(ref) {
        const el = this.getEl(ref);
        if (!el) return null;
        // 清理遮罩点击事件
        if (el._modalOverlayHandler) {
            el.removeEventListener('click', el._modalOverlayHandler);
            el._modalOverlayHandler = null;
        }
        // 清理关闭按钮事件
        const closeBtn = el.querySelector('.modal-close, .modal-close-btn');
        if (closeBtn && closeBtn._modalCloseHandler) {
            closeBtn.removeEventListener('click', closeBtn._modalCloseHandler);
            closeBtn._modalCloseHandler = null;
        }
        el.classList.add('is-hidden');
        el.style.display = 'none';
        return el;
    },

    showInlineError(ref, message) {
        const el = this.getEl(ref);
        if (!el) return null;
        el.textContent = message || '';
        return this.showElement(el, 'block');
    },

    clearInlineError(ref) {
        const el = this.getEl(ref);
        if (!el) return null;
        el.textContent = '';
        return this.hideElement(el);
    },

    renderEmptyTableRow(tbodyRef, colspan, text, className) {
        const tbody = this.getEl(tbodyRef);
        if (!tbody) return null;
        tbody.innerHTML = '';
        const row = document.createElement('tr');
        const cell = document.createElement('td');
        cell.colSpan = colspan;
        cell.className = className || 'u-empty-cell';
        cell.textContent = text || '';
        row.appendChild(cell);
        tbody.appendChild(row);
        return row;
    },
    
    toggleVisible(ref, show) {
        const el = this.getEl(ref);
        if (!el) return null;
        if (show) {
            return this.showElement(el);
        } else {
            return this.hideElement(el);
        }
    },
    
    setLoading(ref, text) {
        const el = this.getEl(ref);
        if (!el) return null;
        if (!el.hasAttribute('data-original-text')) {
            el.setAttribute('data-original-text', el.textContent);
        }
        el.disabled = true;
        el.textContent = text || (typeof i18n !== 'undefined' ? i18n.t('saving') : '保存中...');
        el.classList.add('is-loading');
        return el;
    },
    
    restoreButton(ref, text) {
        const el = this.getEl(ref);
        if (!el) return null;
        el.disabled = false;
        el.textContent = text || el.getAttribute('data-original-text') || '';
        el.classList.remove('is-loading');
        el.removeAttribute('data-original-text');
        return el;
    },
    
    renderBadge(type, text) {
        const span = document.createElement('span');
        span.className = 'badge badge-' + (type || 'info');
        span.textContent = text || '';
        return span;
    },
    
    renderPagination(containerRef, options) {
        const container = this.getEl(containerRef);
        if (!container) return null;

        const total = Math.max(0, Number(options?.total) || 0);
        const pageSize = Math.max(1, Number(options?.pageSize) || 10);
        const totalPages = Math.max(1, Math.ceil(total / pageSize));
        const currentPage = Math.min(Math.max(1, Number(options?.page) || 1), totalPages);
        const maxVisiblePages = Math.max(3, Number(options?.maxVisiblePages) || 5);
        const summaryText = options?.summaryText || '';
        const onPageChange = typeof options?.onPageChange === 'function' ? options.onPageChange : null;

        container.innerHTML = '';

        const wrap = document.createElement('div');
        wrap.className = 'pagination u-pagination';

        const appendButton = (label, targetPage, disabled, active) => {
            const button = document.createElement('button');
            button.type = 'button';
            button.className = `btn btn-sm${active ? ' btn-primary' : ''}`;
            button.textContent = label;
            button.disabled = !!disabled;
            if (!button.disabled && onPageChange) {
                button.addEventListener('click', () => onPageChange(targetPage));
            }
            wrap.appendChild(button);
        };

        const appendEllipsis = () => {
            const ellipsis = document.createElement('span');
            ellipsis.className = 'u-pagination-ellipsis';
            ellipsis.textContent = '...';
            wrap.appendChild(ellipsis);
        };

        const appendSummary = () => {
            if (!summaryText) return;
            const summary = document.createElement('span');
            summary.className = 'u-pagination-summary';
            summary.textContent = summaryText;
            wrap.appendChild(summary);
        };

        if (totalPages <= 1) {
            appendSummary();
            container.appendChild(wrap);
            return wrap;
        }

        appendButton('«', currentPage - 1, currentPage <= 1, false);

        let startPage = Math.max(1, currentPage - Math.floor(maxVisiblePages / 2));
        let endPage = Math.min(totalPages, startPage + maxVisiblePages - 1);
        if (endPage - startPage + 1 < maxVisiblePages) {
            startPage = Math.max(1, endPage - maxVisiblePages + 1);
        }

        if (startPage > 1) {
            appendButton('1', 1, false, false);
            if (startPage > 2) appendEllipsis();
        }

        for (let page = startPage; page <= endPage; page++) {
            appendButton(String(page), page, page === currentPage, page === currentPage);
        }

        if (endPage < totalPages) {
            if (endPage < totalPages - 1) appendEllipsis();
            appendButton(String(totalPages), totalPages, false, false);
        }

        appendButton('»', currentPage + 1, currentPage >= totalPages, false);
        appendSummary();

        container.appendChild(wrap);
        return wrap;
    },

    setExclusiveActive(containerRef, selector, activeEl, className) {
        const container = this.getEl(containerRef);
        if (!container) return;
        const activeClass = className || 'is-active';
        container.querySelectorAll(selector).forEach(el => {
            el.classList.toggle(activeClass, el === activeEl);
        });
    },

    // 注册模块方法 - 将方法混入 AppState
    registerModule(name, methods) {
        const self = this;
        Object.keys(methods).forEach(key => {
            self[key] = typeof methods[key] === 'function' ? methods[key].bind(self) : methods[key];
        });
        self._loadedModules[name] = true;
        // 延迟执行回调，确保模块脚本中后续的 Object.assign 也完成
        if (self._moduleCallbacks[name]) {
            const cbs = self._moduleCallbacks[name];
            delete self._moduleCallbacks[name];
            setTimeout(function() {
                cbs.forEach(function(cb) { cb(); });
            }, 0);
        }
    },

    // 按需加载模块JS文件（串行加载，自动重试）
    _loadModule(name, callback) {
        const self = this;
        if (this._loadedModules[name]) {
            if (callback) callback();
            return;
        }
        // 注册回调
        if (callback) {
            if (!this._moduleCallbacks[name]) this._moduleCallbacks[name] = [];
            this._moduleCallbacks[name].push(callback);
        }
        // 如果已在队列中，不重复入队
        if (this._moduleLoadQueue.some(function(item) { return item.name === name; })) return;
        // 入队
        this._moduleLoadQueue.push({ name: name, retries: 0 });
        this._processModuleQueue();
    },

    // 有限并发处理模块加载队列（最多2个并发）
    _processModuleQueue() {
        const self = this;
        // 如果已达到最大并发数或队列为空，返回
        if (this._moduleLoadingCount >= this._MAX_CONCURRENT_LOADS || this._moduleLoadQueue.length === 0) return;

        // 找到第一个未在加载中的模块
        let item = null;
        for (let i = 0; i < this._moduleLoadQueue.length; i++) {
            if (!this._moduleLoadQueue[i].loading) {
                item = this._moduleLoadQueue[i];
                break;
            }
        }
        if (!item) return;

        // 如果模块已加载（可能在排队期间被另一个回调加载了）
        if (this._loadedModules[item.name]) {
            this._moduleLoadQueue = this._moduleLoadQueue.filter(q => q.name !== item.name);
            this._processModuleQueue();
            return;
        }

        // 解析实际文件名（bundle 映射）
        const fileName = self._bundleMap[item.name] || item.name;

        // 如果该文件（或 bundle）已在加载中或已加载完成，跳过重复请求
        // 模块会在 bundle 加载完成后通过 registerModule 自动注册
        if (self._loadedFiles[fileName]) {
            item.loading = true;
            self._moduleLoadQueue = self._moduleLoadQueue.filter(q => q.name !== item.name);
            self._processModuleQueue();
            return;
        }

        item.loading = true;
        this._moduleLoadingCount++;
        self._loadedFiles[fileName] = true;

        // 移除可能存在的旧 script 标签（失败后清理）
        const oldScript = document.querySelector('script[data-module="' + fileName + '"]');
        if (oldScript) oldScript.remove();

        const script = document.createElement('script');
        script.src = '/js/modules/' + fileName + '.js';
        script.dataset.module = fileName;

        script.onload = function() {
            self._moduleLoadingCount--;
            // 从队列中移除当前模块以及同 bundle 中已注册的模块
            self._moduleLoadQueue = self._moduleLoadQueue.filter(function(q) {
                return !self._loadedModules[q.name];
            });
            // 立即尝试加载下一个（减少延迟）
            setTimeout(function() { self._processModuleQueue(); }, 10);
        };

        script.onerror = function() {
            console.warn('[Module] Failed to load: ' + fileName + ' (attempt ' + (item.retries + 1) + '/3)');
            script.remove();
            self._moduleLoadingCount--;
            item.loading = false;
            delete self._loadedFiles[fileName]; // 允许重试

            if (item.retries < 2) {
                item.retries++;
                // 延迟重试，给 ESP32 喘息时间
                setTimeout(function() { self._processModuleQueue(); }, 1000);
            } else {
                console.error('[Module] Giving up loading: ' + fileName);
                self._moduleLoadQueue = self._moduleLoadQueue.filter(q => q.name !== item.name);
                // 继续加载队列中的下一个
                setTimeout(function() { self._processModuleQueue(); }, 200);
            }
        };

        document.head.appendChild(script);

        // 尝试并发加载下一个
        this._processModuleQueue();
    },

    // ============ 用户下拉菜单 ============
    setupUserDropdown() {
        const dropdownBtn = document.getElementById('user-dropdown-btn');
        const dropdownMenu = document.getElementById('user-dropdown-menu');
        
        if (!dropdownBtn || !dropdownMenu) return;
        
        // 切换下拉菜单显示
        dropdownBtn.addEventListener('click', (e) => {
            e.stopPropagation();
            const dropdown = dropdownBtn.closest('.user-dropdown');
            dropdown.classList.toggle('open');
        });
        
        // 点击外部关闭下拉菜单
        document.addEventListener('click', (e) => {
            if (!dropdownBtn.contains(e.target) && !dropdownMenu.contains(e.target)) {
                const dropdown = dropdownBtn.closest('.user-dropdown');
                if (dropdown) dropdown.classList.remove('open');
            }
        });
        
        // 点击菜单项后关闭下拉菜单
        dropdownMenu.querySelectorAll('.dropdown-item').forEach(item => {
            item.addEventListener('click', () => {
                const dropdown = dropdownBtn.closest('.user-dropdown');
                if (dropdown) dropdown.classList.remove('open');
            });
        });
    },

    // ============ 主题管理 ============
    setupTheme() {
        // 检测系统主题偏好
        const savedTheme = localStorage.getItem('theme');
        const systemPrefersDark = window.matchMedia('(prefers-color-scheme: dark)').matches;
        
        // 优先级: 用户手动设置 > 系统偏好
        let theme;
        if (savedTheme) {
            theme = savedTheme;
        } else {
            theme = systemPrefersDark ? 'dark' : 'light';
            // 标记为自动模式（未手动设置）
            localStorage.setItem('theme-auto', 'true');
        }
        
        this.setTheme(theme, false);
        this.updateThemeToggleIcon(theme);
        
        // 绑定主题切换按钮 (下拉菜单中的)
        const themeToggleItem = document.getElementById('theme-toggle-item');
        if (themeToggleItem) {
            themeToggleItem.addEventListener('click', () => this.toggleTheme());
        }
        
        // 监听系统主题变化（仅在自动模式下）
        window.matchMedia('(prefers-color-scheme: dark)').addEventListener('change', (e) => {
            const isAutoMode = localStorage.getItem('theme-auto') === 'true';
            if (isAutoMode) {
                const newTheme = e.matches ? 'dark' : 'light';
                this.setTheme(newTheme, false);
                this.updateThemeToggleIcon(newTheme);
            }
        });
    },
    
    setTheme(theme, isManual = true) {
        document.documentElement.setAttribute('data-theme', theme);
        localStorage.setItem('theme', theme);
        
        // 如果是手动设置，清除自动模式标记
        if (isManual) {
            localStorage.removeItem('theme-auto');
        }
        
        this.updateThemeToggleIcon(theme);
    },
    
    toggleTheme() {
        const current = document.documentElement.getAttribute('data-theme');
        const newTheme = current === 'dark' ? 'light' : 'dark';
        this.setTheme(newTheme, true);
    },
    
    updateThemeToggleIcon(theme) {
        const themeToggleItem = document.getElementById('theme-toggle-item');
        if (themeToggleItem) {
            const iconSpan = themeToggleItem.querySelector('.item-icon');
            const textSpan = themeToggleItem.querySelector('span:not(.item-icon)');
            
            if (iconSpan) {
                iconSpan.textContent = theme === 'dark' ? '☀' : '🌙';
            }
            if (textSpan) {
                // 获取当前语言的翻译
                const key = theme === 'dark' ? 'theme-light' : 'theme-dark';
                const translated = window.i18n ? window.i18n.t(key) : (theme === 'dark' ? '浅色模式' : '深色模式');
                textSpan.textContent = translated;
            }
        }
    },

    // ============ 会话验证 ============
    refreshPage() {
        const token = localStorage.getItem('auth_token');

        // 在 API 调用前保存"记住密码"凭据到局部变量
        // （防止 401 处理器清除 localStorage 中的凭据）
        const savedRemember = localStorage.getItem('remember');
        const savedUsername = localStorage.getItem('username');
        const savedPassword = localStorage.getItem('password');

        if (!token) {
            // 没有 token，尝试使用保存的凭据自动登录
            this._tryAutoLogin(savedRemember, savedUsername, savedPassword);
            return;
        }
        apiGet('/api/auth/session')
            .then(res => {
                if (res && res.success && res.data && res.data.sessionValid) {
                    this.currentUser.name = res.data.username || 'Admin';
                    this.currentUser.role = res.data.role || 'VIEWER';
                    this.currentUser.canManageFs = res.data.canManageFs === true;
                    this._showAppPage();
                    this._loadModule('dashboard', () => {
                        this.renderDashboard();
                        this.loadSystemMonitor();
                    });
                } else {
                    // 会话无效，尝试使用保存的凭据重新登录
                    this._tryAutoLogin(savedRemember, savedUsername, savedPassword);
                }
            })
            .catch(() => {
                // 会话验证失败（如 401），尝试自动重新登录
                this._tryAutoLogin(savedRemember, savedUsername, savedPassword);
            });
    },

    // 尝试使用保存的凭据自动登录
    _tryAutoLogin(remember, username, password) {
        if (remember === 'true' && username && password) {
            apiPost('/api/auth/login', { username, password })
                .then(res => {
                    if (res && res.success) {
                        const sid = res.sessionId;
                        localStorage.setItem('auth_token', sid);
                        localStorage.setItem('sessionId', sid);
                        // 恢复"记住密码"凭据（可能已被 401 处理器清除）
                        localStorage.setItem('remember', 'true');
                        localStorage.setItem('username', username);
                        localStorage.setItem('password', password);

                        this.currentUser.name = res.username || username;
                        sessionStorage.setItem('currentUsername', this.currentUser.name);

                        // 获取角色和权限信息
                        apiGet('/api/auth/session').then(sr => {
                            if (sr && sr.success && sr.data) {
                                this.currentUser.role = sr.data.role || 'VIEWER';
                                this.currentUser.canManageFs = sr.data.canManageFs === true;
                            }
                        }).catch(() => {});

                        this._showAppPage();
                        this._loadModule('dashboard', () => {
                            this.renderDashboard();
                            this.loadSystemMonitor();
                        });
                    } else {
                        // 自动登录失败（如密码已更改），清除无效凭据并显示登录页
                        localStorage.removeItem('password');
                        localStorage.removeItem('remember');
                        this._showLoginPage();
                    }
                })
                .catch(() => {
                    this._showLoginPage();
                });
        } else {
            this._showLoginPage();
        }
    },

    _showLoginPage() {
        document.getElementById('login-page').style.display = 'flex';
        document.getElementById('app-container').style.display = 'none';

        // 预填充已保存的用户名和"记住密码"状态
        const savedUsername = localStorage.getItem('username');
        const savedRemember = localStorage.getItem('remember');
        const usernameInput = document.getElementById('username');
        const rememberCheckbox = document.getElementById('remember');
        if (usernameInput && savedUsername) usernameInput.value = savedUsername;
        if (rememberCheckbox && savedRemember === 'true') rememberCheckbox.checked = true;
    },

    async _showAppPage() {
        document.getElementById('login-page').style.display = 'none';
        document.getElementById('app-container').style.display = 'block';
        // 登录成功后将URL从 /login 等路径重定向到根路径 /
        if (location.pathname !== '/' || location.hash) {
            history.replaceState(null, '', '/');
        }
        // 预加载模态框和仪表盘页面
        await this._loadModals();
        await this.loadPage('dashboard-page');
        // 移除骨架屏
        var skeleton = document.getElementById('skeleton-screen');
        if (skeleton) skeleton.remove();
    },

    // ============ 通用折叠面板 ============
    toggleSection(bodyId) {
        const body = document.getElementById(bodyId);
        const icon = document.getElementById(bodyId + '-icon');
        if (!body) return;
        if (body.classList.contains('fb-hidden')) {
            body.classList.remove('fb-hidden');
            body.style.display = '';
            if (icon) icon.innerHTML = '&#9660;';
        } else {
            body.classList.add('fb-hidden');
            body.style.display = '';
            if (icon) icon.innerHTML = '&#9654;';
        }
    },

    // ============ 侧边栏 ============
    setupSidebarToggle() {
        const btn = document.getElementById('sidebar-toggle');
        if (btn) btn.addEventListener('click', () => this.toggleSidebar());
        if (localStorage.getItem('sidebarCollapsed') === 'true') this.collapseSidebar();
    },

    toggleSidebar() {
        // 移动端使用 expanded 类，桌面端使用 collapsed 类
        const sidebar = document.getElementById('sidebar');
        if (!sidebar) return;
        
        // 检测是否为移动端
        const isMobile = window.innerWidth <= 768;
        
        if (isMobile) {
            // 移动端：切换 expanded 类
            if (sidebar.classList.contains('expanded')) {
                sidebar.classList.remove('expanded');
                this.sidebarCollapsed = true;
                const btn = document.getElementById('sidebar-toggle');
                if (btn) btn.textContent = '☰';
            } else {
                sidebar.classList.add('expanded');
                this.sidebarCollapsed = false;
                const btn = document.getElementById('sidebar-toggle');
                if (btn) btn.textContent = '✕';
            }
        } else {
            // 桌面端：使用原有逻辑
            this.sidebarCollapsed ? this.expandSidebar() : this.collapseSidebar();
        }
    },

    collapseSidebar() {
        const sidebar = document.getElementById('sidebar');
        if (sidebar) {
            sidebar.classList.add('collapsed');
            sidebar.classList.remove('expanded');
            this.sidebarCollapsed = true;
            localStorage.setItem('sidebarCollapsed', 'true');
            const btn = document.getElementById('sidebar-toggle');
            if (btn) btn.textContent = '☰';
        }
    },

    expandSidebar() {
        const sidebar = document.getElementById('sidebar');
        if (sidebar) {
            sidebar.classList.remove('collapsed');
            this.sidebarCollapsed = false;
            localStorage.setItem('sidebarCollapsed', 'false');
            const btn = document.getElementById('sidebar-toggle');
            if (btn) btn.textContent = '✕';
        }
    },

    // ============ 语言 ============
    setupLanguage() {
        // 初始化时立即应用 i18n 到登录页（在登录前就需要正确显示）
        i18n.updatePageText();

        // 登录页语言切换
        const loginLangSelect = document.getElementById('login-language-select');
        if (loginLangSelect) {
            loginLangSelect.value = i18n.currentLang;
            loginLangSelect.addEventListener('change', e => {
                i18n.setLanguage(e.target.value);
                // 同步主应用的语言选择器
                const mainSelect = document.getElementById('language-select');
                if (mainSelect) mainSelect.value = e.target.value;
            });
        }

        // 主应用语言切换
        const langSelect = document.getElementById('language-select');
        if (langSelect) {
            langSelect.value = i18n.currentLang;
            langSelect.addEventListener('change', e => {
                i18n.setLanguage(e.target.value);
                // 同步登录页的语言选择器
                if (loginLangSelect) loginLangSelect.value = e.target.value;
                // 仅刷新当前页的动态 i18n 内容，避免额外预加载其它模块
                this._refreshCurrentPageLocalizedContent();
            });
        }
    },

    _getPageModuleMap() {
        return {
            dashboard: 'dashboard',
            network: 'network',
            device: 'device-config',
            users: 'users',
            roles: 'roles',
            peripheral: 'peripherals',
            'periph-exec': 'periph-exec',
            protocol: 'protocol',
            'rule-script': 'rule-script',
            data: 'files',
            logs: 'logs',
            'device-control': 'device-control'
        };
    },

    _getPageLoaders() {
        return {
            dashboard: () => { this.renderDashboard(); this.loadSystemMonitor(); },
            network: () => { this.loadNetworkConfig(); },
            device: () => { this.loadDeviceConfig(); },
            users: () => { this.loadUsers(); },
            roles: () => { this.loadRoles(); },
            peripheral: () => { this.loadPeripherals(); },
            'periph-exec': () => { this.loadPeriphExecPage(); },
            protocol: () => { this.loadProtocolConfig('mqtt'); if (this._startMqttStatusPolling) this._startMqttStatusPolling(); },
            'rule-script': () => { this.loadRuleScriptPage(); },
            data: () => { this.loadFileTree(this._currentDir || '/'); this.loadFileSystemInfo(); },
            logs: () => {
                if (!this._currentLogFile) {
                    this._currentLogFile = 'system.log';
                }
                const currentSpan = document.getElementById('current-log-file');
                if (currentSpan) currentSpan.textContent = i18n.t('log-current-file-prefix') + this._currentLogFile;
                this.loadLogFileList();
                this.loadLogs();
                const autoRefresh = document.getElementById('log-auto-refresh');
                if (autoRefresh && autoRefresh.checked) {
                    this.startLogAutoRefresh();
                }
            },
            'device-control': () => {
                if (typeof this.loadDeviceControlPage === 'function') {
                    this.loadDeviceControlPage();
                } else {
                    console.warn('[changePage] loadDeviceControlPage not available, module may not be loaded');
                    var content = document.getElementById('dc-content');
                    if (content) {
                        content.innerHTML = '<div class="dc-empty">鈴?' + i18n.t('loading') + '</div>';
                        var self = this;
                        setTimeout(function() {
                            if (typeof self.loadDeviceControlPage === 'function') {
                                self.loadDeviceControlPage();
                            } else {
                                content.innerHTML = '<div class="dc-empty u-text-danger">鉂?妯″潡鍔犺浇澶辫触锛岃鍒锋柊椤甸潰閲嶈瘯</div>';
                            }
                        }, 1000);
                    } else {
                        console.error('[changePage] dc-content element not found!');
                    }
                }
            }
        };
    },

    _refreshCurrentPageLocalizedContent() {
        const loader = this._getPageLoaders()[this.currentPage];
        if (typeof loader === 'function') {
            loader();
        }
    },

    // ============ 配置选项卡 ============
    // 注意：tab 切换已通过 setupGlobalEventDelegation() 中的事件委托处理
    // 此方法保留用于其他静态页面的初始化（目前为空）
    setupConfigTabs() {
        // Tab 切换已移至全局事件委托 (setupGlobalEventDelegation)
        // 协议表单提交、MQTT 按钮等已在各自的模块 JS 中处理
    },
    

    _getProtocolName(formId) {
        const map = { 'modbus-rtu': 'Modbus RTU', 'modbus-tcp': 'Modbus TCP', mqtt: 'MQTT', http: 'HTTP', coap: 'CoAP' };
        for (const key of Object.keys(map)) {
            if (formId.includes(key)) return map[key];
        }
        return 'TCP';
    },

    showConfigTab(pageId, tabId) {
        const page = document.getElementById(pageId);
        if (!page) return;
        page.querySelectorAll('.config-tab').forEach(t => {
            t.classList.toggle('active', t.getAttribute('data-tab') === tabId);
        });
        page.querySelectorAll('.config-content').forEach(c => c.classList.remove('active'));
        const target = page.querySelector(`#${tabId}`);
        if (target) target.classList.add('active');
        
        // 切换到协议配置页面时自动加载配置
        if (pageId === 'protocol-page') {
            if (typeof this.loadProtocolConfig === 'function') this.loadProtocolConfig(tabId);
        }
        
        // 切换到设备监控页面时自动加载网络状态
        if (pageId === 'dashboard-page') {
            if (typeof this.loadNetworkStatus === 'function') this.loadNetworkStatus();
        }
        // 切换到NTP时间tab时自动加载时间
        if (pageId === 'device-page' && tabId === 'dev-ntp') {
            if (typeof this.loadDeviceTime === 'function') this.loadDeviceTime();
        }
        // 切换到基本信息tab时自动加载硬件信息
        if (pageId === 'device-page' && tabId === 'dev-basic') {
            if (typeof this._loadDeviceHardwareInfo === 'function') this._loadDeviceHardwareInfo();
        }
        // 切换到热点配置tab时自动加载配网状态
        if (pageId === 'network-page' && tabId === 'ap-config') {
            if (typeof this.loadProvisionStatus === 'function') this.loadProvisionStatus();
        }
        // 切换到蓝牙配网tab时自动加载蓝牙配网状态和配置
        if (pageId === 'device-page' && tabId === 'dev-ble') {
            if (typeof this.loadBLEProvisionStatus === 'function') this.loadBLEProvisionStatus();
            if (typeof this.loadBLEProvisionConfig === 'function') this.loadBLEProvisionConfig();
        }
        // 切换到OTA升级tab时自动加载OTA状态
        if (pageId === 'device-page' && tabId === 'dev-ota') {
            if (typeof this.loadOtaStatus === 'function') this.loadOtaStatus();
        }
    },

    // ============ 事件绑定（仅通用事件） ============
    setupEventListeners() {
        // 登录表单
        const loginForm = document.getElementById('login-form');
        if (loginForm) loginForm.addEventListener('submit', e => { e.preventDefault(); this.handleLogin(); });

        // 菜单
        document.querySelectorAll('.menu-item').forEach(item => {
            item.addEventListener('click', e => { e.preventDefault(); this.changePage(item.dataset.page); });
        });

        // 修改密码
        const changePwdBtn = document.getElementById('change-password-btn');
        if (changePwdBtn) changePwdBtn.addEventListener('click', () => this.showChangePasswordModal());

        // 退出登录
        const logoutBtn = document.getElementById('logout-btn');
        if (logoutBtn) logoutBtn.addEventListener('click', () => this.logout());

        // 模态窗关闭
        const closeId = (id) => { this.hideModal(id); };
        ['close-password-modal', 'cancel-password-btn'].forEach(id => {
            const el = document.getElementById(id);
            if (el) el.addEventListener('click', () => closeId('change-password-modal'));
        });

        // 确认修改密码
        const confirmPwd = document.getElementById('confirm-password-btn');
        if (confirmPwd) confirmPwd.addEventListener('click', () => this.changePassword());

        // 移动端侧边栏：点击菜单项后自动收起
        document.querySelectorAll('.menu-item').forEach(item => {
            item.addEventListener('click', () => {
                const sidebar = document.getElementById('sidebar');
                if (sidebar && window.innerWidth <= 768 && sidebar.classList.contains('expanded')) {
                    sidebar.classList.remove('expanded');
                    this.sidebarCollapsed = true;
                    const btn = document.getElementById('sidebar-toggle');
                    if (btn) btn.textContent = '☰';
                }
            });
        });
    },

    // ============ 登录 ============
    handleLogin() {
        const username = (document.getElementById('username') || {}).value;
        const password = (document.getElementById('password') || {}).value;
        const remember = (document.getElementById('remember') || {}).checked;

        if (!username || !password) {
            Notification.warning(i18n.t('login-empty-warning'), i18n.t('login-fail-title'));
            return;
        }

        const submitBtn = document.querySelector('#login-form button[type="submit"]');
        const originalText = submitBtn ? submitBtn.innerHTML : '';
        if (submitBtn) { submitBtn.innerHTML = i18n.t('login-logging-in-html'); submitBtn.disabled = true; }

        apiPost('/api/auth/login', { username, password })
            .then(res => {
                if (res && res.success) {
                    const sid = res.sessionId;

                    // 保存 token 到 localStorage，fetch-api.js 会自动注入 Authorization 头
                    localStorage.setItem('auth_token', sid);

                    // 记住密码
                    if (remember) {
                        localStorage.setItem('remember', 'true');
                        localStorage.setItem('username', username);
                        localStorage.setItem('password', password);  // 保存密码用于自动填充
                        localStorage.setItem('sessionId', sid);
                    } else {
                        ['remember', 'username', 'password', 'sessionId'].forEach(k => localStorage.removeItem(k));
                    }

                    this.currentUser.name = res.username || username;
                    sessionStorage.setItem('currentUsername', this.currentUser.name);

                    // 获取角色和权限信息
                    apiGet('/api/auth/session').then(sr => {
                        if (sr && sr.success && sr.data) {
                            this.currentUser.role = sr.data.role || 'VIEWER';
                            this.currentUser.canManageFs = sr.data.canManageFs === true;
                        }
                    }).catch(() => {});

                    this._showAppPage();
                    this._loadModule('dashboard', () => {
                        this.renderDashboard();
                        this.loadSystemMonitor();
                    });
                    Notification.success(i18n.t('login-success-msg'), i18n.t('login-welcome-title'));
                } else {
                    Notification.error((res && res.error) || i18n.t('login-fail-title'), i18n.t('login-fail-title'));
                }
            })
            .catch((err) => {
                // 登录失败，显示错误信息
                const errorMsg = (err && err.data && err.data.error) || i18n.t('login-fail-msg');
                Notification.error(errorMsg, i18n.t('login-fail-title'));
            })
            .finally(() => {
                if (submitBtn) { submitBtn.innerHTML = originalText; submitBtn.disabled = false; }
            });
    },

    // ============ 页面切换 ============
    async changePage(page) {
        // 取消旧页面排队中的请求，释放 ESP32 资源
        if (typeof window.apiAbortPageRequests === 'function') {
            window.apiAbortPageRequests();
        }

        const pageAlias = { monitor: 'dashboard' };
        const normalizedPage = pageAlias[page] || page;

        // 先确保页面HTML已加载
        await this.loadPage(normalizedPage + '-page');

        document.querySelectorAll('.page').forEach(p => p.classList.remove('active'));
        const target = document.getElementById(normalizedPage + '-page');
        if (!target) {
            console.warn('[changePage] target page not found:', page, '=>', normalizedPage);
            return;
        }
        target.classList.add('active');

        document.querySelectorAll('.menu-item').forEach(item => {
            item.classList.toggle('active', item.dataset.page === normalizedPage);
        });

        const titleKey = `page-title-${normalizedPage}`;
        const titleEl = document.getElementById('page-title');
        if (titleEl) titleEl.textContent = i18n.t(titleKey);

        this.currentPage = normalizedPage;

        // 模块名到页面的映射
        const pageModuleMap = this._getPageModuleMap();
        const pageLoaders = this._getPageLoaders();

        if (normalizedPage !== 'logs' && this._logAutoRefreshTimer) {
            clearInterval(this._logAutoRefreshTimer);
            this._logAutoRefreshTimer = null;
        }

        if (normalizedPage !== 'device-control' && typeof this._dcStopAllAutoRefresh === 'function') {
            this._dcStopAllAutoRefresh();
        }

        // 停止协议页面的所有轮询（如果离开协议页面）
        if (normalizedPage !== 'protocol') {
            // 停止 MQTT 状态轮询
            if (typeof this._stopMqttStatusPolling === 'function') {
                this._stopMqttStatusPolling();
            }
            // 停止 Modbus 主站状态轮询
            if (typeof this._stopMasterStatusRefresh === 'function') {
                this._stopMasterStatusRefresh();
            }
            // 停止 Modbus 线圈自动刷新
            if (this._coilAutoRefreshTimer) {
                clearInterval(this._coilAutoRefreshTimer);
                this._coilAutoRefreshTimer = null;
            }
            // 停止 PID 自动刷新
            if (typeof this._stopPidAutoRefresh === 'function') {
                this._stopPidAutoRefresh();
            }
        }

        const moduleName = pageModuleMap[normalizedPage];
        const loader = pageLoaders[normalizedPage];
        
        if (moduleName && loader) {
            this._loadModule(moduleName, loader);
        } else if (loader) {
            loader();
        } else {
            console.warn('[changePage] no page loader mapped for:', normalizedPage);
        }
    },

    // ============ 修改密码 ============
    showChangePasswordModal() {
        this.showModal('change-password-modal');
        ['current-password-input', 'new-password-input', 'confirm-password-input'].forEach(id => {
            const el = document.getElementById(id); if (el) el.value = '';
        });
        this.clearInlineError('password-error');
    },

    changePassword() {
        const oldPwd = (document.getElementById('current-password-input') || {}).value || '';
        const newPwd = (document.getElementById('new-password-input') || {}).value || '';
        const confirmPwd = (document.getElementById('confirm-password-input') || {}).value || '';
        const showErr = (msg) => {
            this.showInlineError('password-error', msg);
            Notification.error(msg, i18n.t('change-pwd-fail'));
        };

        if (!oldPwd || !newPwd || !confirmPwd) return showErr(i18n.t('validate-all-fields'));
        if (newPwd !== confirmPwd) return showErr(i18n.t('password-error') || i18n.t('validate-new-pwd-mismatch'));
        if (newPwd.length < 6) return showErr(i18n.t('validate-new-pwd-len'));

        this.clearInlineError('password-error');

        const btn = document.getElementById('confirm-password-btn');
        if (btn) { btn.disabled = true; btn.textContent = i18n.t('change-pwd-submitting'); }

        apiPost('/api/auth/change-password', { oldPassword: oldPwd, newPassword: newPwd })
            .then(res => {
                if (res && res.success) {
                    Notification.success(i18n.t('change-pwd-success-msg'), i18n.t('change-pwd-success-title'));
                    this.hideModal('change-password-modal');
                    // 修改密码后后端会踢出所有会话，需重新登录
                    setTimeout(() => {
                        localStorage.removeItem('auth_token');
                        this._showLoginPage();
                    }, 1500);
                } else {
                    showErr((res && res.error) || i18n.t('change-pwd-fail-msg'));
                }
            })
            .catch(() => {})
            .finally(() => { if (btn) { btn.disabled = false; btn.textContent = i18n.t('confirm-change-btn'); } });
    },

    // ============ 退出登录 ============
    logout() {
        if (!confirm(i18n.t('logout-confirm') || '确定要退出登录吗？')) return;

        this._showLoginPage();
        document.getElementById('login-form') && document.getElementById('login-form').reset();

        apiPost('/api/auth/logout', {})
            .then(() => {})
            .catch(() => {})
            .finally(() => {
                localStorage.removeItem('auth_token');
                localStorage.removeItem('sessionId');
                localStorage.removeItem('password');
                sessionStorage.removeItem('savedPassword');
                sessionStorage.removeItem('currentUsername');
                Notification.success(i18n.t('logout-success'), i18n.t('logout-title'));
            });
    },

    // ============ 关闭所有浮层（token 过期时调用）============
    closeAllOverlays() {
        // 1. 关闭所有 .modal 弹窗并清理事件
        document.querySelectorAll('.modal').forEach(function (m) {
            // 清理遮罩点击事件
            if (m._modalOverlayHandler) {
                m.removeEventListener('click', m._modalOverlayHandler);
                m._modalOverlayHandler = null;
            }
            // 清理关闭按钮事件
            const closeBtn = m.querySelector('.modal-close, .modal-close-btn');
            if (closeBtn && closeBtn._modalCloseHandler) {
                closeBtn.removeEventListener('click', closeBtn._modalCloseHandler);
                closeBtn._modalCloseHandler = null;
            }
            m.style.display = 'none';
        });

        // 2. 关闭动态创建的全屏浮层（如角色权限详情弹窗）
        document.querySelectorAll('div[style*="position: fixed"][style*="z-index"]').forEach(function (el) {
            // 排除 Notification 容器等非弹窗元素
            if (el.querySelector('.modal-content, [onclick*="remove"]')) {
                el.remove();
            }
        });

        // 3. 关闭用户下拉菜单
        document.querySelectorAll('.user-dropdown.open').forEach(function (d) {
            d.classList.remove('open');
        });

        // 4. 停止日志自动刷新定时器
        if (typeof this.stopLogAutoRefresh === 'function') this.stopLogAutoRefresh();

        // 5. 停止 MQTT 状态轮询（如有）
        if (typeof this._stopMqttStatusPolling === 'function') {
            this._stopMqttStatusPolling();
        }
    },

    // ============ 工具方法 ============
    getAuthHeader() {
        const token = localStorage.getItem('auth_token');
        return token ? { 'Authorization': `Bearer ${token}` } : {};
    },

    /**
     * 设置文本内容
     */
    _setText(id, text) {
        const el = document.getElementById(id);
        if (el) el.textContent = text;
    },

    /**
     * 设置 HTML 内容
     */
    _setHtml(id, html) {
        const el = document.getElementById(id);
        if (el) el.innerHTML = html;
    },

    /**
     * 设置进度条宽度
     */
    _setBar(id, percent) {
        const el = document.getElementById(id);
        if (el) el.style.width = Math.min(percent, 100) + '%';
    },

    /**
     * 格式化字节数
     */
    _formatBytes(bytes) {
        if (bytes === 0) return '0 B';
        const k = 1024;
        const sizes = ['B', 'KB', 'MB', 'GB'];
        const i = Math.floor(Math.log(bytes) / Math.log(k));
        return parseFloat((bytes / Math.pow(k, i)).toFixed(2)) + ' ' + sizes[i];
    },

    /**
     * 设置表单元素的值（支持 input, select, textarea）
     * @param {string} id - 元素 ID
     * @param {string|number} value - 要设置的值
     */
    _setValue(id, value) {
        const el = document.getElementById(id);
        if (el) el.value = value;
    },

    /**
     * 设置复选框的选中状态
     * @param {string} id - 复选框元素 ID
     * @param {boolean} checked - 是否选中
     */
    _setCheckbox(id, checked) {
        const el = document.getElementById(id);
        if (el) el.checked = !!checked;
    },

    /**
     * 设置复选框的选中状态（_setCheckbox 的别名）
     * @param {string} id - 复选框元素 ID
     * @param {boolean} checked - 是否选中
     */
    _setChecked(id, checked) {
        this._setCheckbox(id, checked);
    },

    /**
     * 显示/隐藏消息提示元素
     * @param {string} id - 元素 ID
     * @param {boolean} show - true 显示，false 隐藏
     */
    _showMessage(id, show) {
        const el = document.getElementById(id);
        if (!el) return;
        el.classList.toggle('is-hidden', !show);
        el.style.display = show ? '' : 'none';
    },

    /**
     * 设置元素的文本内容（与 _setText 功能相同，提供兼容性别名）
     * @param {string} id - 元素 ID
     * @param {string} text - 文本内容
     */
    _setTextContent(id, text) {
        this._setText(id, text);
    },

    // ============ SSE 连接管理 ============
    connectSSE: function() {
        if (this._sseSource) return;  // 已连接

        try {
            this._sseSource = new EventSource('/api/events');
            this._sseReconnectDelay = 1000;  // 重置重连延迟

            this._sseSource.onopen = function() {
                console.log('[SSE] 连接已建立');
            };

            this._sseSource.onerror = function() {
                console.warn('[SSE] 连接错误，将自动重连');
                this.disconnectSSE();
                // 检查是否还有活跃的事件处理器，无监听者则不重连
                var hasHandlers = false;
                var handlers = this._sseHandlers || {};
                for (var key in handlers) {
                    if (handlers.hasOwnProperty(key) && handlers[key] && handlers[key].length > 0) {
                        hasHandlers = true;
                        break;
                    }
                }
                if (!hasHandlers) {
                    console.log('[SSE] 无活跃监听者，停止重连');
                    return;
                }
                // 指数退避重连
                this._sseReconnectTimer = setTimeout(function() {
                    this.connectSSE();
                }.bind(this), this._sseReconnectDelay);
                this._sseReconnectDelay = Math.min(this._sseReconnectDelay * 2, 30000);
            }.bind(this);

            // 绑定已注册的事件处理器
            this._rebindSSEHandlers();
        } catch (e) {
            console.error('[SSE] 创建 EventSource 失败:', e);
        }
    },

    disconnectSSE: function() {
        if (this._sseSource) {
            this._sseSource.close();
            this._sseSource = null;
        }
        if (this._sseReconnectTimer) {
            clearTimeout(this._sseReconnectTimer);
            this._sseReconnectTimer = null;
        }
    },

    onSSEEvent: function(eventType, handler) {
        if (!this._sseHandlers[eventType]) {
            this._sseHandlers[eventType] = [];
        }
        this._sseHandlers[eventType].push(handler);
        // 如果已连接，立即绑定
        if (this._sseSource) {
            this._sseSource.addEventListener(eventType, handler);
        }
    },

    offSSEEvent: function(eventType, handler) {
        if (this._sseHandlers[eventType]) {
            var idx = this._sseHandlers[eventType].indexOf(handler);
            if (idx !== -1) this._sseHandlers[eventType].splice(idx, 1);
        }
        if (this._sseSource) {
            this._sseSource.removeEventListener(eventType, handler);
        }
    },

    _rebindSSEHandlers: function() {
        var self = this;
        Object.keys(this._sseHandlers).forEach(function(eventType) {
            self._sseHandlers[eventType].forEach(function(handler) {
                self._sseSource.addEventListener(eventType, handler);
            });
        });
    }
};
