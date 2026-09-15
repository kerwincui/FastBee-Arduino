// ============================================================
// PageLoader: 动态页面 HTML 片段加载与缓存
// 供 AppState 调用，管理页面 HTML 的按需加载
// ============================================================
var PAGE_FETCH_TIMEOUT_MS = 8000;
var PageLoader = {
    _loadedPages: {},
    _loadingPages: {},
    _pageTemplates: {},
    _fragmentTemplates: {},
    _modalTemplates: {},
    _modalHosts: {},
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
    _dedupePageRoot(pageId) {
        var roots = Array.prototype.slice.call(document.querySelectorAll('#' + pageId));
        if (roots.length <= 1) return;
        var keep = roots.find(function(root) { return root.classList.contains('active'); }) || roots[0];
        roots.forEach(function(root) {
            if (root !== keep) root.remove();
        });
    },

    _cachePageTemplates(moduleName, html) {
        var wrapper = document.createElement('div');
        wrapper.innerHTML = html;
        var roots = Array.prototype.slice.call(wrapper.querySelectorAll('.page[id]'));
        if (roots.length === 0) {
            throw new Error('Page response contains no page root: ' + moduleName);
        }
        roots.forEach((root) => {
            this._pageTemplates[root.id] = root.outerHTML;
        });
        this._loadedPages[moduleName] = true;
    },

    _mountPage(pageId) {
        var existing = document.getElementById(pageId);
        if (existing && existing.querySelector('[data-page-fallback]')) {
            existing.remove();
            existing = null;
        }
        if (existing) {
            this._dedupePageRoot(pageId);
            return existing;
        }
        var html = this._pageTemplates[pageId];
        if (!html) return null;
        var container = document.getElementById('content-area');
        if (!container) throw new Error('Page container missing: #content-area');
        var wrapper = document.createElement('div');
        wrapper.innerHTML = html;
        var root = wrapper.querySelector('#' + pageId);
        if (!root) return null;
        container.appendChild(root);
        if (typeof i18n !== 'undefined' && i18n.currentLang !== 'zh-CN') {
            i18n.updatePageText(root);
        }
        return root;
    },

    mountPage(pageId) {
        return this._mountPage(pageId);
    },

    unmountPages() {
        var container = document.getElementById('content-area');
        if (!container) return 0;
        var roots = Array.prototype.slice.call(container.querySelectorAll(':scope > .page'));
        roots.forEach(function(root) { root.remove(); });
        return roots.length;
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
            var controller = (typeof AbortController !== 'undefined') ? new AbortController() : null;
            var timedOut = false;
            var timer = controller ? setTimeout(function() {
                timedOut = true;
                controller.abort();
            }, PAGE_FETCH_TIMEOUT_MS) : null;
            try {
                var resp = await fetch(url, controller ? { signal: controller.signal } : undefined);
                if (!resp.ok) throw new Error('HTTP ' + resp.status);
                return resp;
            } catch (e) {
                lastErr = timedOut
                    ? new Error('Static asset request timed out after ' + PAGE_FETCH_TIMEOUT_MS + 'ms')
                    : e;
            } finally {
                if (timer) clearTimeout(timer);
            }
            if (i < retries) {
                await new Promise(function(resolve) { setTimeout(resolve, 800 * (i + 1)); });
            }
        }
        throw lastErr;
    },

    _renderUnavailablePage(pageId, error) {
        var container = document.getElementById('content-area');
        if (!container || !pageId) return false;

        var rootId = /-page$/.test(pageId) ? pageId : pageId + '-page';
        var root = document.getElementById(rootId);
        if (!root) {
            root = document.createElement('div');
            root.id = rootId;
            root.className = 'page page-layout-flat';
            container.appendChild(root);
        }
        root.innerHTML = '';

        var statusText = String(error && error.message || '');
        var isFeatureDisabled = !!(error && error.code === 'FEATURE_DISABLED');
        var isPressure = /HTTP\s+503/i.test(statusText);
        var message = document.createElement('div');
        message.className = 'message message-warning';
        message.setAttribute('data-page-fallback', isFeatureDisabled ? 'feature-disabled' : 'memory-pressure');
        message.textContent = isFeatureDisabled
            ? '该功能当前未启用，请在功能配置中启用后重试'
            : '设备当前内存紧张，页面暂时不可用，请稍后刷新重试';
        root.appendChild(message);

        if (!isFeatureDisabled && typeof Notification !== 'undefined' && Notification.warning) {
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
    async loadPage(pageId, pageMapping, options) {
        options = options || {};
        var moduleName = pageMapping[pageId];
        if (!moduleName) return;
        if (this._loadedPages[moduleName]) {
            return options.mount === false ? null : this._mountPage(pageId);
        }
        if (this._loadingPages[moduleName]) {
            try {
                await this._loadingPages[moduleName];
                return options.mount === false ? null : this._mountPage(pageId);
            } catch (e) {
                if (options.mount !== false) this._renderUnavailablePage(pageId, e);
                return null;
            }
        }

        this._loadingPages[moduleName] = this._enqueueOp(async () => {
            var resp = await this._fetchWithRetry(this._resolveAssetUrl('/pages/' + moduleName + '.html'));
            var html = await resp.text();
            this._cachePageTemplates(moduleName, html);
            if (!this._pageTemplates[pageId]) {
                throw new Error('Page response missing expected root: #' + pageId);
            }
        });

        try {
            await this._loadingPages[moduleName];
            return options.mount === false ? null : this._mountPage(pageId);
        } catch(e) {
            console.error('[PageLoader] Failed to load:', moduleName, e);
            // A no-PSRAM device may intentionally reject heavyweight page HTML
            // with 503. Keep navigation usable and explain the controlled
            // degradation instead of leaving a blank page root.
            delete this._loadedPages[moduleName];
            if (options.mount !== false) this._renderUnavailablePage(pageId, e);
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
                return self.loadPage(pageId, pageMapping, { mount: false }).then(function() {
                    return new Promise(function(resolve) { setTimeout(resolve, delayMs); });
                });
            });
        }, Promise.resolve());
    },

    _cacheModalTemplates(html, hostId, requiredIds) {
        var wrapper = document.createElement('div');
        wrapper.innerHTML = html;
        var modals = Array.prototype.slice.call(wrapper.querySelectorAll('.modal[id]'));
        var required = Array.isArray(requiredIds) ? requiredIds : [];
        var available = {};
        modals.forEach(function(modal) { available[modal.id] = true; });
        var missing = required.filter(function(id) { return !available[id]; });
        if (required.length > 0 && (modals.length === 0 || missing.length > 0)) {
            throw new Error('Modal response missing required templates: ' +
                (missing.length > 0 ? missing.join(', ') : 'no modal roots'));
        }
        modals.forEach((modal) => {
            this._modalTemplates[modal.id] = modal.outerHTML;
            this._modalHosts[modal.id] = hostId || '';
            modal.remove();
        });
        return wrapper.innerHTML;
    },

    mountModal(modalId) {
        var existing = document.getElementById(modalId);
        if (existing) return existing;
        var html = this._modalTemplates[modalId];
        if (!html) return null;
        var wrapper = document.createElement('div');
        wrapper.innerHTML = html;
        var modal = wrapper.querySelector('#' + modalId);
        if (!modal) return null;
        var hostId = this._modalHosts[modalId];
        var host = hostId ? document.getElementById(hostId) : null;
        (host || document.body).appendChild(modal);
        if (typeof i18n !== 'undefined' && i18n.currentLang !== 'zh-CN') {
            i18n.updatePageText(modal);
        }
        return modal;
    },

    unmountModal(modalId) {
        var modal = typeof modalId === 'string' ? document.getElementById(modalId) : modalId;
        if (!modal || !modal.classList || !modal.classList.contains('modal')) return null;
        modal.remove();
        return modal;
    },

    /**
     * 加载模态框 HTML 并缓存为独立模板。打开时才挂载到 DOM。
     */
    loadModals() {
        if (this._loadedPages['modals']) return Promise.resolve();
        if (this._loadingPages['modals']) return this._loadingPages['modals'];
        var self = this;
        var operation = this._enqueueOp(async () => {
            var resp = await this._fetchWithRetry(this._resolveAssetUrl('/pages/modals.html'));
            var html = await resp.text();
            this._cacheModalTemplates(html, '', [
                'change-password-modal',
                'peripheral-modal',
                'config-transfer-modal'
            ]);
            this._loadedPages['modals'] = true;
        });
        var shared = operation.catch(function(error) {
            delete self._loadedPages['modals'];
            console.error('[PageLoader] Failed to load modals:', error);
            throw error;
        }).finally(function() {
            if (self._loadingPages['modals'] === shared) {
                delete self._loadingPages['modals'];
            }
        });
        this._loadingPages['modals'] = shared;
        return shared;
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
            var cachedContainer = document.getElementById(containerId);
            if (cachedContainer && this._fragmentTemplates[fragmentName] != null) {
                cachedContainer.innerHTML = this._fragmentTemplates[fragmentName];
                if (typeof i18n !== 'undefined' && i18n.currentLang !== 'zh-CN') {
                    i18n.updatePageText(cachedContainer);
                }
            }
            if (typeof onLoaded === 'function') onLoaded();
            return;
        }
        try {
            await this._enqueueOp(async () => {
                var resp = await this._fetchWithRetry(this._resolveAssetUrl('/pages/fragments/' + fragmentName + '.html'));
                var html = await resp.text();
                this._fragmentTemplates[fragmentName] = this._cacheModalTemplates(html, containerId);
                var container = document.getElementById(containerId);
                if (container) {
                    container.innerHTML = this._fragmentTemplates[fragmentName];
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
