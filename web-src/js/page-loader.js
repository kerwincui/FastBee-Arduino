// ============================================================
// PageLoader: 动态页面 HTML 片段加载与缓存
// 供 AppState 调用，管理页面 HTML 的按需加载
// ============================================================
var PageLoader = {
    _loadedPages: {},
    _loadingPages: {},
    _resolveAssetUrl(path) {
        if (typeof window !== 'undefined' && typeof window.__fastbeeResolveAssetUrl === 'function') {
            return window.__fastbeeResolveAssetUrl(path);
        }
        return path;
    },

    /**
     * 加载页面 HTML 片段到 content-area
     * @param {string} pageId - 页面元素 ID，如 'dashboard-page'
     * @param {Object} pageMapping - pageId → moduleName 映射表
     */
    async loadPage(pageId, pageMapping) {
        var moduleName = pageMapping[pageId];
        if (!moduleName || this._loadedPages[moduleName]) return;
        if (this._loadingPages[moduleName]) return this._loadingPages[moduleName];

        this._loadingPages[moduleName] = (async () => {
            var resp = await fetch(this._resolveAssetUrl('/pages/' + moduleName + '.html'));
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
        })();

        try {
            await this._loadingPages[moduleName];
        } catch(e) {
            console.error('[PageLoader] Failed to load:', moduleName, e);
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
            var resp = await fetch(this._resolveAssetUrl('/pages/modals.html'));
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
            var resp = await fetch(this._resolveAssetUrl('/pages/fragments/' + fragmentName + '.html'));
            if (!resp.ok) throw new Error('HTTP ' + resp.status);
            var html = await resp.text();
            var container = document.getElementById(containerId);
            if (container) {
                container.innerHTML = html;
                // 应用 i18n 翻译到新加载的内容
                if (typeof i18n !== 'undefined' && i18n.updatePageText) {
                    i18n.updatePageText(container);
                }
            }
            this._loadedPages[cacheKey] = true;
            if (typeof onLoaded === 'function') onLoaded();
        } catch(e) {
            console.error('[PageLoader] Failed to load fragment:', fragmentName, e);
            // Fallback：在容器中显示错误提示
            var container = document.getElementById(containerId);
            if (container) {
                container.innerHTML = '<div class="message message-error" data-i18n="fragment-load-error">加载失败，请刷新重试</div>';
                if (typeof i18n !== 'undefined' && i18n.updatePageText) {
                    i18n.updatePageText(container);
                }
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
