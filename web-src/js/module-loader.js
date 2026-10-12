// ============================================================
// ModuleLoader: module registration and on-demand loading
// Keep runtime module delivery conservative for ESP32.
// ============================================================
var ModuleLoader = {
    _loadedModules: {},
    _moduleCallbacks: {},
    _loadedFiles: {},
    _sequenceLoading: {},
    _moduleLoadQueue: [],
    _moduleLoadingCount: 0,
    _MAX_CONCURRENT_LOADS: 1,

    _sequenceMap: {
        'protocol': [
            'protocol'
        ],
        'protocol-full-config': [
            'protocol-full-config'
        ],
        'protocol-modbus-rtu': [
            'protocol-full-config',
            'protocol-modbus-rtu'
        ],
        'protocol-modbus-control': [
            'protocol-modbus-control'
        ],
        'device-control': [
            'device-control'
        ],
        'device-control-runtime': [
            'device-control-view',
            'device-control-modbus'
        ],
        'periph-exec': [
            'periph-exec'
        ],
        'screen-panel': [
            'screen-panel-model',
            'screen-panel'
        ],
        'periph-exec-editor': [
            'periph-exec',
            'periph-exec-modbus',
            'periph-exec-form',
            'periph-exec-templates'
        ]
    },

    _resolveAssetUrl(path) {
        if (typeof window !== 'undefined' && typeof window.__fastbeeResolveAssetUrl === 'function') {
            return window.__fastbeeResolveAssetUrl(path);
        }
        return path;
    },

    _completeModuleLoad(name) {
        var self = this;
        self._loadedModules[name] = true;
        if (self._moduleCallbacks[name]) {
            var callbacks = self._moduleCallbacks[name];
            delete self._moduleCallbacks[name];
            setTimeout(function() {
                callbacks.forEach(function(cb) { cb(); });
            }, 0);
        }
    },

    registerModule(name, methods, target) {
        var self = this;
        var dest = target || (typeof AppState !== 'undefined' ? AppState : window);
        Object.keys(methods).forEach(function(key) {
            dest[key] = typeof methods[key] === 'function' ? methods[key].bind(dest) : methods[key];
        });
        self._completeModuleLoad(name);
    },

    _loadScriptFile(fileName, onSuccess, onFailure) {
        var self = this;

        if (self._loadedFiles[fileName]) {
            if (typeof onSuccess === 'function') onSuccess();
            return;
        }

        if (self._moduleLoadingCount >= self._MAX_CONCURRENT_LOADS) {
            setTimeout(function() {
                self._loadScriptFile(fileName, onSuccess, onFailure);
            }, 60);
            return;
        }

        self._moduleLoadingCount++;
        self._loadedFiles[fileName] = true;

        var oldScript = document.querySelector('script[data-module="' + fileName + '"]');
        if (oldScript) oldScript.remove();

        function startRequest() {
            return new Promise(function(resolve, reject) {
                var script = document.createElement('script');
                script.src = self._resolveAssetUrl('/js/modules/' + fileName + '.js');
                script.dataset.module = fileName;
                script.onload = resolve;
                script.onerror = function() {
                    script.remove();
                    reject(new Error('module script load failed: ' + fileName));
                };
                document.head.appendChild(script);
            });
        }

        var queuedLoad = (typeof window !== 'undefined' &&
            typeof window.__fastbeeQueueStaticAsset === 'function')
            ? window.__fastbeeQueueStaticAsset(startRequest)
            : startRequest();

        queuedLoad.then(function() {
            self._moduleLoadingCount--;
            if (typeof onSuccess === 'function') onSuccess();
            setTimeout(function() {
                self._processQueue();
            }, 10);
        }, function() {
            self._moduleLoadingCount--;
            delete self._loadedFiles[fileName];
            if (typeof onFailure === 'function') onFailure();
            setTimeout(function() {
                self._processQueue();
            }, 60);
        });
    },

    _startSequenceLoad(name) {
        var self = this;
        var files = self._sequenceMap[name];
        if (!files || self._sequenceLoading[name]) return;

        self._sequenceLoading[name] = true;

        function failSequence() {
            console.error('[Module] Giving up loading sequence:', name);
            delete self._sequenceLoading[name];
            delete self._moduleCallbacks[name];
            // 通知用户加载失败，提供刷新建议
            if (typeof Notification !== 'undefined') {
                Notification.warning('模块加载失败，请刷新页面重试', '模块加载');
            }
        }

        function loadNext(index, retries) {
            if (self._loadedModules[name]) {
                delete self._sequenceLoading[name];
                return;
            }
            if (index >= files.length) {
                self._completeModuleLoad(name);
                delete self._sequenceLoading[name];
                return;
            }

            var fileName = files[index];
            self._loadScriptFile(
                fileName,
                function() {
                    loadNext(index + 1, 0);
                },
                function() {
                    // 指数退避重试: 800ms → 1.6s → 3.2s（服务端限流为立即 503 +
                    // Retry-After，快速退避把 503 风暴消化在客户端；旧 2s 起步
                    // 退避把偶发失败放大成感知卡顿）
                    var retryDelay = Math.min(800 * Math.pow(2, retries), 8000);
                    console.warn('[Module] Failed to load sequence file: ' + fileName + ' (attempt ' + (retries + 1) + '/4, retry in ' + retryDelay + 'ms)');
                    if (retries < 3) {
                        setTimeout(function() {
                            loadNext(index, retries + 1);
                        }, retryDelay);
                        return;
                    }
                    failSequence();
                }
            );
        }

        loadNext(0, 0);
    },

    loadModule(name, callback) {
        if (this._loadedModules[name]) {
            if (callback) callback();
            return;
        }

        if (callback) {
            if (!this._moduleCallbacks[name]) this._moduleCallbacks[name] = [];
            this._moduleCallbacks[name].push(callback);
        }

        if (this._sequenceMap[name]) {
            this._startSequenceLoad(name);
            return;
        }

        if (this._moduleLoadQueue.some(function(item) { return item.name === name; })) return;
        this._moduleLoadQueue.push({ name: name, retries: 0 });
        this._processQueue();
    },

    _processQueue() {
        var self = this;
        if (this._moduleLoadingCount >= this._MAX_CONCURRENT_LOADS || this._moduleLoadQueue.length === 0) return;

        var item = null;
        for (var i = 0; i < this._moduleLoadQueue.length; i++) {
            if (!this._moduleLoadQueue[i].loading) {
                item = this._moduleLoadQueue[i];
                break;
            }
        }
        if (!item) return;

        if (this._loadedModules[item.name]) {
            this._moduleLoadQueue = this._moduleLoadQueue.filter(function(q) { return q.name !== item.name; });
            this._processQueue();
            return;
        }

        var fileName = item.name;
        if (self._loadedFiles[fileName]) {
            item.loading = true;
            self._moduleLoadQueue = self._moduleLoadQueue.filter(function(q) { return q.name !== item.name; });
            self._processQueue();
            return;
        }

        item.loading = true;
        self._loadScriptFile(
            fileName,
            function() {
                self._moduleLoadQueue = self._moduleLoadQueue.filter(function(q) {
                    return !self._loadedModules[q.name];
                });
                setTimeout(function() { self._processQueue(); }, 10);
            },
            function() {
                // 指数退避重试: 800ms → 1.6s → 3.2s（同 sequence 加载，配合服务端立即 503）
                var retryDelay = Math.min(800 * Math.pow(2, item.retries), 8000);
                console.warn('[Module] Failed to load: ' + fileName + ' (attempt ' + (item.retries + 1) + '/4, retry in ' + retryDelay + 'ms)');
                item.loading = false;

                if (item.retries < 3) {
                    item.retries++;
                    setTimeout(function() { self._processQueue(); }, retryDelay);
                } else {
                    console.error('[Module] Giving up loading: ' + fileName);
                    if (typeof Notification !== 'undefined') {
                        Notification.warning('模块 ' + fileName + ' 加载失败，请刷新页面重试', '模块加载');
                    }
                    self._moduleLoadQueue = self._moduleLoadQueue.filter(function(q) { return q.name !== item.name; });
                    setTimeout(function() { self._processQueue(); }, 200);
                }
            }
        );

        this._processQueue();
    },

    isLoaded(name) {
        return !!this._loadedModules[name];
    }
};
