// ============================================================
// ModuleLoader: 模块注册与动态加载系统
// 管理模块的注册、按需加载、并发控制与重试
// ============================================================
var ModuleLoader = {
    _loadedModules: {},
    _moduleCallbacks: {},
    _loadedFiles: {},
    _moduleLoadQueue: [],
    _moduleLoadingCount: 0,
    _MAX_CONCURRENT_LOADS: 4,

    // 模块 → 打包文件映射（多个模块合并为一个文件）
    _bundleMap: {
        'users': 'admin-bundle',
        'roles': 'admin-bundle',
        'files': 'admin-bundle',
        'logs': 'admin-bundle',
        'rule-script': 'admin-bundle'
    },

    /**
     * 注册模块方法 - 将方法混入目标对象（通常是 AppState）
     * @param {string} name - 模块名
     * @param {Object} methods - 要混入的方法集合
     * @param {Object} target - 混入目标对象（默认 AppState）
     */
    registerModule(name, methods, target) {
        var self = this;
        var dest = target || (typeof AppState !== 'undefined' ? AppState : window);
        Object.keys(methods).forEach(key => {
            dest[key] = typeof methods[key] === 'function' ? methods[key].bind(dest) : methods[key];
        });
        self._loadedModules[name] = true;
        // 延迟执行回调，确保模块脚本中后续的 Object.assign 也完成
        if (self._moduleCallbacks[name]) {
            var cbs = self._moduleCallbacks[name];
            delete self._moduleCallbacks[name];
            setTimeout(function() {
                cbs.forEach(function(cb) { cb(); });
            }, 0);
        }
    },

    /**
     * 按需加载模块 JS 文件（串行加载，自动重试）
     * @param {string} name - 模块名
     * @param {Function} [callback] - 加载完成回调
     */
    loadModule(name, callback) {
        var self = this;
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
        this._processQueue();
    },

    /**
     * 有限并发处理模块加载队列
     */
    _processQueue() {
        var self = this;
        // 如果已达到最大并发数或队列为空，返回
        if (this._moduleLoadingCount >= this._MAX_CONCURRENT_LOADS || this._moduleLoadQueue.length === 0) return;

        // 找到第一个未在加载中的模块
        var item = null;
        for (var i = 0; i < this._moduleLoadQueue.length; i++) {
            if (!this._moduleLoadQueue[i].loading) {
                item = this._moduleLoadQueue[i];
                break;
            }
        }
        if (!item) return;

        // 如果模块已加载（可能在排队期间被另一个回调加载了）
        if (this._loadedModules[item.name]) {
            this._moduleLoadQueue = this._moduleLoadQueue.filter(q => q.name !== item.name);
            this._processQueue();
            return;
        }

        // 解析实际文件名（bundle 映射）
        var fileName = self._bundleMap[item.name] || item.name;

        // 如果该文件（或 bundle）已在加载中或已加载完成，跳过重复请求
        // 模块会在 bundle 加载完成后通过 registerModule 自动注册
        if (self._loadedFiles[fileName]) {
            item.loading = true;
            self._moduleLoadQueue = self._moduleLoadQueue.filter(q => q.name !== item.name);
            self._processQueue();
            return;
        }

        item.loading = true;
        this._moduleLoadingCount++;
        self._loadedFiles[fileName] = true;

        // 移除可能存在的旧 script 标签（失败后清理）
        var oldScript = document.querySelector('script[data-module="' + fileName + '"]');
        if (oldScript) oldScript.remove();

        var script = document.createElement('script');
        script.src = '/js/modules/' + fileName + '.js';
        script.dataset.module = fileName;

        script.onload = function() {
            self._moduleLoadingCount--;
            // 从队列中移除当前模块以及同 bundle 中已注册的模块
            self._moduleLoadQueue = self._moduleLoadQueue.filter(function(q) {
                return !self._loadedModules[q.name];
            });
            // 立即尝试加载下一个（减少延迟）
            setTimeout(function() { self._processQueue(); }, 10);
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
                setTimeout(function() { self._processQueue(); }, 1000);
            } else {
                console.error('[Module] Giving up loading: ' + fileName);
                self._moduleLoadQueue = self._moduleLoadQueue.filter(q => q.name !== item.name);
                // 继续加载队列中的下一个
                setTimeout(function() { self._processQueue(); }, 200);
            }
        };

        document.head.appendChild(script);

        // 尝试并发加载下一个
        this._processQueue();
    },

    /**
     * 检查模块是否已加载
     * @param {string} name - 模块名
     * @returns {boolean}
     */
    isLoaded(name) {
        return !!this._loadedModules[name];
    }
};
