/**
 * 文件管理模块
 * 包含文件系统信息、文件树、文件打开（只读）
 */
(function() {
    AppState.registerModule('files', {

        // ============ 事件绑定 ============
        // 使用 onclick 属性赋值（而非 addEventListener）：onclick 同一时间仅能持有一个处理器，
        // 即使 setupFilesEvents 被多次调用或 DOM 节点被重新渲染，也能确保每个按钮仅响应一次。
        setupFilesEvents() {
            var self = this;

            // 刷新文件列表按钮
            const fsRefreshBtn = document.getElementById('fs-refresh-btn');
            if (fsRefreshBtn) fsRefreshBtn.onclick = function() {
                self.loadFileSystemInfo({ noCache: true });
                self.loadFileTree(self._currentDir || '/', { noCache: true });
            };

            // 返回上级按钮
            const fsUpBtn = document.getElementById('fs-up-btn');
            if (fsUpBtn) fsUpBtn.onclick = function() { self.navigateUp(); };

            // 导出配置按钮
            const exportBtn = document.getElementById('fs-export-config-btn');
            if (exportBtn) exportBtn.onclick = function() {
                self.exportConfigFile();
            };

            // 导入配置按钮
            const importBtn = document.getElementById('fs-import-config-btn');
            if (importBtn) importBtn.onclick = function() {
                self.importConfigFile();
            };
        },

        // ============ 文件管理 ============

        /**
         * 加载文件系统信息
         */
        loadFileSystemInfo(options) {
            var getter = (options && options.noCache === true && typeof apiGetFresh === 'function') ? apiGetFresh : apiGet;
            getter('/api/filesystem')
                .then(res => {
                    if (res && res.success) {
                        const d = res.data || {};
                        const infoSpan = document.getElementById('fs-info');
                        if (infoSpan) {
                            const total = d.totalBytes ? (d.totalBytes / 1024 / 1024).toFixed(2) + ' MB' : 'N/A';
                            const used = d.usedBytes ? (d.usedBytes / 1024 / 1024).toFixed(2) + ' MB' : 'N/A';
                            infoSpan.textContent = `总空间: ${total} | 已用: ${used}`;
                        }
                    }
                })
                .catch(() => {});
        },

        /**
         * 加载文件树
         */
        loadFileTree(path, options) {
            const treeContainer = document.getElementById('file-tree');
            if (!treeContainer) return;

            // 记录当前路径
            this._currentDir = path;

            // 更新路径显示
            const pathEl = document.getElementById('current-dir-path');
            if (pathEl) pathEl.textContent = path;

            treeContainer.innerHTML = '<div class="u-text-muted">加载中...</div>';

            var getter = (options && options.noCache === true && typeof apiGetFresh === 'function') ? apiGetFresh : apiGet;
            getter('/api/files', { path: path })
                .then(res => {
                    if (!res || !res.success) {
                        treeContainer.innerHTML = '<div class="u-text-danger">加载失败</div>';
                        return;
                    }

                    const data = res.data || {};
                    const dirs = data.dirs || [];
                    const files = data.files || [];

                    var fragment = document.createDocumentFragment();

                    // 目录
                    dirs.forEach(dir => {
                        var item = document.createElement('div');
                        item.className = 'file-tree-item';
                        item.dataset.path = path + dir.name + '/';
                        item.dataset.type = 'dir';
                        var icon = document.createElement('span');
                        icon.className = 'file-tree-icon-dir';
                        icon.textContent = '📁';
                        item.appendChild(icon);
                        item.appendChild(document.createTextNode(' ' + dir.name));
                        fragment.appendChild(item);
                    });

                    // 文件
                    files.forEach(file => {
                        const size = file.size < 1024 ? `${file.size} B` :
                                    file.size < 1024 * 1024 ? `${(file.size / 1024).toFixed(1)} KB` :
                                    `${(file.size / 1024 / 1024).toFixed(2)} MB`;
                        var item = document.createElement('div');
                        item.className = 'file-tree-item';
                        item.dataset.path = path + file.name;
                        item.dataset.type = 'file';
                        var icon = document.createElement('span');
                        icon.className = 'file-tree-icon-file';
                        icon.textContent = '📄';
                        item.appendChild(icon);
                        item.appendChild(document.createTextNode(' ' + file.name + ' '));
                        var sizeSpan = document.createElement('span');
                        sizeSpan.className = 'file-tree-size';
                        sizeSpan.textContent = '(' + size + ')';
                        item.appendChild(sizeSpan);
                        fragment.appendChild(item);
                    });

                    treeContainer.innerHTML = '';
                    if (dirs.length === 0 && files.length === 0) {
                        treeContainer.innerHTML = '<div class="u-empty-state u-text-muted">空目录</div>';
                    } else {
                        treeContainer.appendChild(fragment);
                    }

                    // 绑定点击事件
                    treeContainer.querySelectorAll('.file-tree-item').forEach(item => {
                        item.addEventListener('click', (e) => {
                            const path = e.currentTarget.dataset.path;
                            const type = e.currentTarget.dataset.type;

                            // 移除其他选中状态
                            treeContainer.querySelectorAll('.file-tree-item').forEach(i => {
                                i.classList.remove('is-active');
                            });
                            e.currentTarget.classList.add('is-active');

                            if (type === 'dir') {
                                this.loadFileTree(path);
                            } else {
                                this.openFile(path);
                            }
                        });
                    });
                })
                .catch(err => {
                    console.error('Load file tree failed:', err);
                    treeContainer.innerHTML = '<div class="u-text-danger">加载失败</div>';
                });
        },

        /**
         * 打开文件（只读）
         */
        openFile(path) {
            const editor = document.getElementById('file-editor');
            const pathSpan = document.getElementById('current-file-path');
            const statusDiv = document.getElementById('file-status');

            if (!editor || !pathSpan) return;

            // 检查是否可编辑
            const editable = path.endsWith('.json') || path.endsWith('.txt') || path.endsWith('.log') ||
                            path.endsWith('.html') || path.endsWith('.js') || path.endsWith('.css');

            // 显示/隐藏导出和导入配置按钮（仅对 /config/ 下的 JSON 文件显示）
            var isConfigJson = path.startsWith('/config/') && path.endsWith('.json');
            var exportBtn = document.getElementById('fs-export-config-btn');
            var importBtn = document.getElementById('fs-import-config-btn');
            if (exportBtn) exportBtn.classList.toggle('fb-hidden', !isConfigJson);
            if (importBtn) importBtn.classList.toggle('fb-hidden', !isConfigJson);

            // ★ 状态立即同步：一旦用户选中文件，_currentFilePath 即更新
            // （不等待内容加载成功），确保导出/导入始终作用于用户伸手选中的目标，
            // 而不会被上一个文件的遗留状态“偽装”。
            pathSpan.textContent = path;
            this._currentFilePath = path;

            // 先清空编辑器内容，避免用户误以为旧内容属于新选中文件
            editor.value = '';
            editor.readOnly = true;

            if (!editable) {
                statusDiv.textContent = '此文件类型不支持编辑';
                return;
            }

            statusDiv.textContent = '加载中...';

            apiGet('/api/files/content', { path: path })
                .then(res => {
                    // 异步回调期间用户可能已选中别的文件，丢弃过时响应
                    if (this._currentFilePath !== path) return;

                    if (!res || !res.success) {
                        // 优先使用服务器返回的 message/error 字段
                        var detail = (res && res.message) ? ' (' + res.message + ')'
                                   : (res && res.error) ? ' (' + res.error + ')'
                                   : '';
                        if (!detail) {
                            detail = res && typeof res === 'object' && Object.keys(res).length === 0
                                   ? ' (服务器响应为空，设备可能内存不足，请稍后重试)'
                                   : ' (未知错误)';
                        }
                        statusDiv.textContent = '加载失败: ' + detail;
                        return;
                    }

                    const data = res.data || {};
                    editor.value = data.content || '';
                    editor.disabled = false;
                    editor.readOnly = true;

                    const size = data.size < 1024 ? `${data.size} B` :
                                data.size < 1024 * 1024 ? `${(data.size / 1024).toFixed(1)} KB` :
                                `${(data.size / 1024 / 1024).toFixed(2)} MB`;
                    statusDiv.textContent = `大小: ${size} | 就绪`;
                })
                .catch(err => {
                    if (this._currentFilePath !== path) return;
                    console.error('Open file failed:', err);
                    var msg = '加载失败';
                    var detail = '';
                    if (err && err.data && (err.data.message || err.data.error)) {
                        detail = ' (' + (err.data.message || err.data.error) + ')';
                    } else if (err && err.status) {
                        detail = ' (HTTP ' + err.status + ')';
                    } else if (err && err.message) {
                        detail = ' (' + err.message + ')';
                    }
                    statusDiv.textContent = msg + detail;
                });
        },

        /**
         * 返回上级目录
         */
        navigateUp() {
            const currentPath = this._currentDir || '/';

            // 如果当前是根目录，不做任何操作
            if (currentPath === '/') return;

            // 移除末尾的斜杠
            let path = currentPath;
            if (path.endsWith('/')) path = path.slice(0, -1);

            // 找到最后一个斜杠的位置
            const lastSlashIndex = path.lastIndexOf('/');

            if (lastSlashIndex === 0) {
                this.loadFileTree('/');
            } else if (lastSlashIndex > 0) {
                const parentPath = path.substring(0, lastSlashIndex + 1);
                this.loadFileTree(parentPath);
            } else {
                this.loadFileTree('/');
            }
        },

        // ============ 配置导出/导入 ============

        /**
         * 导出当前配置文件（下载为本地 JSON 文件）
         *
         * 内存友好策略：优先走后端 raw=1 流式接口。
         * - raw=1：后端 chunked 流式返回原始字节，无 JSON 包装、设备内存占用 ~数百 B，
         *   即使 MemGuard CRITICAL 级也能下载，根本不依赖文件是否成功打开。
         * - 脱敏（清空 deviceId / clientId）在浏览器本地完成，不消耗设备内存。
         * - 如果 raw 也失败，冒着降级操作使用编辑器内容。
         */
        exportConfigFile() {
            var self = this;
            var filePath = this._currentFilePath;
            if (!filePath) {
                Notification.warning('请先选择一个配置文件');
                return;
            }

            // ★ 优先走 raw 流式下载：无需加载到编辑器，内存不足时仍可成功
            // 注意：项目用 Authorization: Bearer <token> 认证（token 存在 localStorage），
            // 原生 fetch 必须手动带上此 header，否则后端返回 401 Unauthorized。
            var rawUrl = '/api/files/content?path=' + encodeURIComponent(filePath) + '&raw=1';
            var headers = {};
            try {
                var token = localStorage.getItem('auth_token');
                if (token) headers['Authorization'] = 'Bearer ' + token;
            } catch (tokErr) {}
            fetch(rawUrl, { credentials: 'include', headers: headers, cache: 'no-store' })
                .then(function(resp) {
                    if (!resp.ok) {
                        throw new Error('HTTP ' + resp.status);
                    }
                    return resp.text();
                })
                .then(function(rawContent) {
                    self._doExport(filePath, rawContent);
                })
                .catch(function(err) {
                    console.warn('Raw export failed, falling back to editor content:', err);
                    // 降级元备方案：用已加载的编辑器内容（若有）
                    var editor = document.getElementById('file-editor');
                    if (editor && editor.value) {
                        self._doExport(filePath, editor.value);
                        return;
                    }
                    Notification.error(
                        '无法从设备获取配置内容' +
                        ' (' + (err && err.message ? err.message : 'unknown') + ')'
                    );
                });
        },

        /**
         * 实际执行导出动作（脱敏→格式化→下载）
         *
         * 关键实现要点：
         * - 延迟释放 object URL：浏览器的下载触发是异步的，若在 a.click() 后立即 revokeObjectURL，
         *   浏览器可能还没来得及读取 Blob 就被销毁导致下载静默失败。
         * - msSaveBlob 兑底：老 IE / 旧 Edge 不支持 a.download。
         * - try/catch 包裹：捕获 Blob / URL API 异常并提示用户（之前版本任何异常都会被后续的 success 通知掉包裹掉）。
         */
        _doExport(filePath, rawContent) {
            // 验证 JSON 格式
            var parsed;
            try {
                parsed = JSON.parse(rawContent);
            } catch (e) {
                Notification.error('文件内容不是有效的 JSON 格式');
                return;
            }

            // 导出时脱敏：清空设备相关的唯一标识，便于跨设备复用配置模板
            // - device.json：清空 deviceId
            // - protocol.json：清空 mqtt.clientId
            // 导入到目标设备时，后端会自动按 "FBE+MAC" / "S&deviceId&productId&userId" 规则回填
            try {
                if (filePath === '/config/device.json' && parsed && typeof parsed === 'object') {
                    if ('deviceId' in parsed) parsed.deviceId = '';
                }
                if (filePath === '/config/protocol.json' && parsed && typeof parsed === 'object') {
                    if (parsed.mqtt && typeof parsed.mqtt === 'object' && 'clientId' in parsed.mqtt) {
                        parsed.mqtt.clientId = '';
                    }
                }
            } catch (sanitizeErr) {
                console.warn('Sanitize export content failed:', sanitizeErr);
            }

            // 使用 2 空格缩进，保持可读性
            var exportText = JSON.stringify(parsed, null, 2);

            // 提取文件名
            var parts = filePath.split('/');
            var fileName = parts[parts.length - 1] || 'config.json';

            try {
                var blob = new Blob([exportText], { type: 'application/json;charset=utf-8' });

                // 旧 IE / 旧 Edge 兑底：不支持 a.download
                if (window.navigator && typeof window.navigator.msSaveBlob === 'function') {
                    window.navigator.msSaveBlob(blob, fileName);
                    Notification.success('配置导出成功');
                    return;
                }

                var url = URL.createObjectURL(blob);
                var a = document.createElement('a');
                a.href = url;
                a.download = fileName;
                a.rel = 'noopener';
                a.style.display = 'none';
                document.body.appendChild(a);

                // 触发下载
                a.click();

                // ★ 延迟清理：a.click() 在浏览器内部以异步方式触发下载，
                // 立即 revokeObjectURL 会让部分浏览器（尤其 Firefox / Safari）读不到 Blob
                // 内容而静默失败；延迟 ≥ 100ms 可避免这一种种子。
                setTimeout(function() {
                    try {
                        if (a.parentNode) a.parentNode.removeChild(a);
                        URL.revokeObjectURL(url);
                    } catch (cleanupErr) {
                        console.warn('Cleanup export anchor failed:', cleanupErr);
                    }
                }, 250);

                Notification.success('配置导出成功');
            } catch (downloadErr) {
                console.error('Export download failed:', downloadErr);
                Notification.error(
                    '配置导出失败' +
                    ' (' + (downloadErr && downloadErr.message ? downloadErr.message : 'unknown') + ')'
                );
            }
        },

        /**
         * 导入配置文件（上传 JSON 替换当前配置）
         */
        importConfigFile() {
            var self = this;
            var filePath = this._currentFilePath;
            if (!filePath) {
                Notification.warning('请先选择一个配置文件');
                return;
            }

            var input = document.createElement('input');
            input.type = 'file';
            input.accept = '.json';
            input.onchange = function(e) {
                var file = e.target.files[0];
                if (!file) return;

                // 文件大小检查（限制 64KB）
                if (file.size > 65536) {
                    Notification.error('文件过大，最大支持 64KB');
                    return;
                }

                var reader = new FileReader();
                reader.onload = function(evt) {
                    var content = evt.target.result;

                    // JSON 格式验证
                    var parsed;
                    try {
                        parsed = JSON.parse(content);
                        // 验证必须是对象类型
                        if (typeof parsed !== 'object' || parsed === null || Array.isArray(parsed)) {
                            Notification.error('配置文件必须是 JSON 对象格式');
                            return;
                        }
                    } catch (parseErr) {
                        Notification.error('文件内容不是有效的 JSON 格式');
                        return;
                    }

                    // 确认覆盖
                    if (!confirm('确定要用导入的文件覆盖当前配置吗？此操作不可撤销！')) {
                        return;
                    }

                    // 字段过滤容错：读取设备当前配置作为 schema，过滤多余字段
                    var doImport = function(finalContent) {
                        apiPost('/api/files/save', { path: filePath, content: finalContent })
                            .then(function(res) {
                                if (res && res.success) {
                                    Notification.success('配置导入成功');
                                    // 刷新编辑器显示（会读到已回填的身份字段）
                                    self.openFile(filePath);
                                } else {
                                    Notification.error(
                                        (res && res.message) || '配置导入失败'
                                    );
                                }
                            })
                            .catch(function(err) {
                                console.error('Import config failed:', err);
                                Notification.error('配置导入失败');
                            });
                    };

                    // 尝试获取当前配置进行字段过滤
                    if (typeof window.filterConfigFields === 'function') {
                        var rawUrl = '/api/files/content?path=' + encodeURIComponent(filePath) + '&raw=1';
                        var headers = {};
                        try {
                            var token = localStorage.getItem('auth_token');
                            if (token) headers['Authorization'] = 'Bearer ' + token;
                        } catch (tokErr) {}
                        fetch(rawUrl, { credentials: 'include', headers: headers, cache: 'no-store' })
                            .then(function(resp) {
                                if (!resp.ok) throw new Error('HTTP ' + resp.status);
                                return resp.text();
                            })
                            .then(function(currentText) {
                                var currentObj = JSON.parse(currentText);
                                var filtered = window.filterConfigFields(parsed, currentObj);
                                doImport(JSON.stringify(filtered));
                            })
                            .catch(function(filterErr) {
                                // 过滤失败不影响导入，继续使用原始内容
                                console.warn('[files] field filter skipped:', filterErr);
                                doImport(content);
                            });
                    } else {
                        doImport(content);
                    }
                };
                reader.readAsText(file);
            };
            input.click();
        }
    });

    // 自动绑定事件
    if (typeof AppState.setupFilesEvents === 'function') {
        AppState.setupFilesEvents();
    }
})();
