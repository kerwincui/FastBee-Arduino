// ============================================================
// PageLoader: 动态页面 HTML 片段加载与缓存
// 供 AppState 调用，管理页面 HTML 的按需加载
// ============================================================
var PageLoader = {
    _loadedPages: {},
    _loadingPages: {},
    // 静态资源串行队列：低内存设备（无 PSRAM 的经典 ESP32，最大空闲块仅 ~10KB）
    // 无法同时支撑多个并发的 HTTP 静态响应，页面/模态框/分片排队串行执行，
    // 避免响应截断（ERR_CONTENT_LENGTH_MISMATCH）与连接被断开
    _opChain: null,
    _enqueueOp(op) {
        if (typeof window !== 'undefined' &&
            typeof window.__fastbeeQueueStaticAsset === 'function') {
            return window.__fastbeeQueueStaticAsset(op);
        }
        var prev = this._opChain || Promise.resolve();
        var run = prev.catch(function () {}).then(op);
        this._opChain = run.catch(function () {});
        return run;
    },
    _resolveAssetUrl(path) {
        if (typeof window !== 'undefined' && typeof window.__fastbeeResolveAssetUrl === 'function') {
            return window.__fastbeeResolveAssetUrl(path);
        }
        return path;
    },

    // A timed-out/retried static response can leave the previous page root in
    // the DOM before the loader state is updated. Keep one active root per
    // page so repeated navigation cannot create duplicate element IDs.
    _dedupePageRoot(moduleName) {
        var rootId = moduleName + '-page';
        var roots = Array.prototype.slice.call(document.querySelectorAll('#' + rootId));
        if (roots.length <= 1) return;
        var keep = roots.find(function(root) { return root.classList.contains('active'); }) || roots[0];
        roots.forEach(function(root) {
            if (root !== keep) root.remove();
        });
    },

    /**
     * 带重试的 fetch（设备繁忙/堆压力下偶发连接超时，退避重试避免页面加载失败）
     * @param {string} url - 请求地址
     * @param {number} [retries=3] - 最大重试次数
     */
    async _fetchWithRetry(url, retries) {
        retries = (retries == null) ? 3 : retries;
        var lastErr;
        for (var i = 0; i <= retries; i++) {
            try {
                var resp = await fetch(url);
                if (!resp.ok) throw new Error('HTTP ' + resp.status);
                return resp;
            } catch (e) {
                lastErr = e;
                if (i < retries) {
                    await new Promise(function(resolve) { setTimeout(resolve, 800 * (i + 1)); });
                }
            }
        }
        throw lastErr;
    },

    _renderUnavailablePage(moduleName, error) {
        var container = document.getElementById('content-area');
        if (!container || !moduleName) return false;

        var rootId = moduleName + '-page';
        var root = document.getElementById(rootId);
        if (!root) {
            root = document.createElement('div');
            root.id = rootId;
            root.className = 'page page-layout-flat';
            container.appendChild(root);
        }
        root.innerHTML = '';

        var message = document.createElement('div');
        message.className = 'message message-warning';
        message.setAttribute('data-page-fallback', 'memory-pressure');
        message.textContent = '设备当前内存紧张，页面暂时不可用，请稍后刷新重试';
        root.appendChild(message);

        var statusText = String(error && error.message || '');
        var isPressure = /HTTP\s+503/i.test(statusText);
        if (typeof Notification !== 'undefined' && Notification.warning) {
            Notification.warning(
                isPressure
                    ? '设备当前内存紧张，页面已进入保护模式，请稍后刷新重试'
                    : '页面加载失败，请稍后刷新重试',
                isPressure ? '内存保护' : '页面加载'
            );
        }
        return true;
    },

    /**
     * 加载页面 HTML 片段到 content-area
     * @param {string} pageId - 页面元素 ID，如 'dashboard-page'
     * @param {Object} pageMapping - pageId → moduleName 映射表
     */
    async loadPage(pageId, pageMapping) {
        var moduleName = pageMapping[pageId];
        if (!moduleName) return;
        if (this._loadedPages[moduleName]) {
            this._dedupePageRoot(moduleName);
            return;
        }
        if (this._loadingPages[moduleName]) return this._loadingPages[moduleName];

        this._loadingPages[moduleName] = this._enqueueOp(async () => {
            var resp = await this._fetchWithRetry(this._resolveAssetUrl('/pages/' + moduleName + '.html'));
            var html = await resp.text();
            var container = document.getElementById('content-area');
            if (!container) throw new Error('Page container missing: #content-area');

            var wrapper = document.createElement('div');
            wrapper.innerHTML = html;
            var rootId = moduleName + '-page';
            var incomingRoot = wrapper.querySelector('#' + rootId);
            if (!incomingRoot) {
                throw new Error('Page response missing expected root: #' + rootId);
            }

            // A previous failed attempt may have left an active fallback with
            // the same ID. Remove it before inserting the verified response so
            // deduplication cannot preserve the fallback and discard real UI.
            var wasActive = false;
            Array.prototype.slice.call(document.querySelectorAll('#' + rootId)).forEach(function(root) {
                wasActive = wasActive || root.classList.contains('active');
                root.remove();
            });
            while (wrapper.firstChild) {
                container.appendChild(wrapper.firstChild);
            }
            if (wasActive) incomingRoot.classList.add('active');
            this._dedupePageRoot(moduleName);
            this._loadedPages[moduleName] = true;
            // 若当前语言非中文，翻译新加载的页面内容
            if (typeof i18n !== 'undefined' && i18n.currentLang !== 'zh-CN' && container) {
                i18n.updatePageText(container);
            }
        });

        try {
            await this._loadingPages[moduleName];
        } catch(e) {
            console.error('[PageLoader] Failed to load:', moduleName, e);
            // A no-PSRAM device may intentionally reject heavyweight page HTML
            // with 503. Keep navigation usable and explain the controlled
            // degradation instead of leaving a blank page root.
            delete this._loadedPages[moduleName];
            this._renderUnavailablePage(moduleName, e);
        } finally {
            delete this._loadingPages[moduleName];
        }
    },

    preloadPages(pageIds, pageMapping, options) {
        options = options || {};
        var delayMs = Number(options.delayMs || 180);
        var self = this;
        return (pageIds || []).reduce(function(chain, pageId) {
            return chain.then(function() {
                return self.loadPage(pageId, pageMapping).then(function() {
                    return new Promise(function(resolve) { setTimeout(resolve, delayMs); });
                });
            });
        }, Promise.resolve());
    },

    /**
     * 加载模态框 HTML 片段到 body
     */
    async loadModals() {
        if (this._loadedPages['modals']) return;
        try {
            await this._enqueueOp(async () => {
                var resp = await this._fetchWithRetry(this._resolveAssetUrl('/pages/modals.html'));
                if (!resp.ok) return;
                var html = await resp.text();
                var wrapper = document.createElement('div');
                wrapper.innerHTML = html;
                while (wrapper.firstChild) {
                    document.body.appendChild(wrapper.firstChild);
                }
                this._loadedPages['modals'] = true;
                // 若当前语言非中文，翻译新加载的模态框内容
                if (typeof i18n !== 'undefined' && i18n.currentLang !== 'zh-CN') {
                    i18n.updatePageText(document.body);
                }
            });
        } catch(e) {
            console.error('[PageLoader] Failed to load modals:', e);
        }
    },

    /**
     * 加载 HTML 分片到指定容器（按需懒加载）
     * @param {string} containerId - 目标容器元素 ID
     * @param {string} fragmentName - 分片文件名（不含路径和扩展名）
     * @param {Function} [onLoaded] - 加载完成后的回调（可选）
     */
    async loadFragment(containerId, fragmentName, onLoaded) {
        var cacheKey = 'fragment:' + fragmentName;
        if (this._loadedPages[cacheKey]) {
            if (typeof onLoaded === 'function') onLoaded();
            return;
        }
        try {
            await this._enqueueOp(async () => {
                var resp = await this._fetchWithRetry(this._resolveAssetUrl('/pages/fragments/' + fragmentName + '.html'));
                var html = await resp.text();
                var container = document.getElementById(containerId);
                if (container) {
                    container.innerHTML = html;
                }
                this._loadedPages[cacheKey] = true;
                // 若当前语言非中文，翻译新加载的分片内容
                if (typeof i18n !== 'undefined' && i18n.currentLang !== 'zh-CN' && container) {
                    i18n.updatePageText(container);
                }
            });
            if (typeof onLoaded === 'function') onLoaded();
        } catch(e) {
            console.error('[PageLoader] Failed to load fragment:', fragmentName, e);
            // Fallback：在容器中显示错误提示
            var container = document.getElementById(containerId);
            if (container) {
                container.innerHTML = '<div class="message message-error">加载失败，请刷新重试</div>';
            }
        }
    },

    /**
     * 检查页面是否已加载
     * @param {string} moduleName - 模块名
     * @returns {boolean}
     */
    isLoaded(moduleName) {
        return !!this._loadedPages[moduleName];
    }
};
