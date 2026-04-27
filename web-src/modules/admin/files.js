/**
 * 文件管理模块
 * 包含文件系统信息、文件树、文件打开（只读）
 */
(function() {
    AppState.registerModule('files', {

        // ============ 事件绑定 ============
        setupFilesEvents() {
            // 刷新文件列表按钮
            const fsRefreshBtn = document.getElementById('fs-refresh-btn');
            if (fsRefreshBtn) fsRefreshBtn.addEventListener('click', () => this.loadFileTree(this._currentDir || '/'));

            // 返回上级按钮
            const fsUpBtn = document.getElementById('fs-up-btn');
            if (fsUpBtn) fsUpBtn.addEventListener('click', () => this.navigateUp());

            // 导出配置按钮
            const exportBtn = document.getElementById('fs-export-config-btn');
            if (exportBtn) exportBtn.addEventListener('click', () => this.exportConfigFile());

            // 导入配置按钮
            const importBtn = document.getElementById('fs-import-config-btn');
            if (importBtn) importBtn.addEventListener('click', () => this.importConfigFile());
        },

        // ============ 文件管理 ============

        /**
         * 加载文件系统信息
         */
        loadFileSystemInfo() {
            apiGet('/api/filesystem')
                .then(res => {
                    if (res && res.success) {
                        const d = res.data || {};
                        const infoSpan = document.getElementById('fs-info');
                        if (infoSpan) {
                            const total = d.totalBytes ? (d.totalBytes / 1024 / 1024).toFixed(2) + ' MB' : 'N/A';
                            const used = d.usedBytes ? (d.usedBytes / 1024 / 1024).toFixed(2) + ' MB' : 'N/A';
                            infoSpan.textContent = `${i18n.t('fs-space-prefix')}${total}${i18n.t('fs-space-used-prefix')}${used}`;
                        }
                    }
                })
                .catch(() => {});
        },

        /**
         * 加载文件树
         */
        loadFileTree(path) {
            const treeContainer = document.getElementById('file-tree');
            if (!treeContainer) return;

            // 记录当前路径
            this._currentDir = path;

            // 更新路径显示
            const pathEl = document.getElementById('current-dir-path');
            if (pathEl) pathEl.textContent = path;

            treeContainer.innerHTML = i18n.t('fs-loading-text');

            apiGet('/api/files', { path: path })
                .then(res => {
                    if (!res || !res.success) {
                        treeContainer.innerHTML = i18n.t('fs-load-fail-text');
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
                        treeContainer.innerHTML = i18n.t('fs-empty-dir-html');
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
                    treeContainer.innerHTML = i18n.t('fs-load-fail-text');
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

            if (!editable) {
                statusDiv.textContent = i18n.t('fs-file-type-unsupported');
                return;
            }

            pathSpan.textContent = path;
            statusDiv.textContent = i18n.t('fs-file-loading');

            apiGet('/api/files/content', { path: path })
                .then(res => {
                    if (!res || !res.success) {
                        var detail = (res && res.message) ? ' (' + res.message + ')'
                                   : (res && res.error) ? ' (' + res.error + ')'
                                   : '';
                        statusDiv.textContent = i18n.t('fs-file-load-fail-prefix') + (detail || i18n.t('fs-file-unknown-error'));
                        return;
                    }

                    const data = res.data || {};
                    editor.value = data.content || '';
                    editor.disabled = false;
                    editor.readOnly = true;

                    const size = data.size < 1024 ? `${data.size} B` :
                                data.size < 1024 * 1024 ? `${(data.size / 1024).toFixed(1)} KB` :
                                `${(data.size / 1024 / 1024).toFixed(2)} MB`;
                    statusDiv.textContent = `${i18n.t('fs-file-ready-prefix')}${size}${i18n.t('fs-file-ready-suffix')}`;

                    this._currentFilePath = path;
                })
                .catch(err => {
                    console.error('Open file failed:', err);
                    var msg = i18n.t('fs-file-load-fail');
                    if (err && err.status) msg += ' (HTTP ' + err.status + ')';
                    else if (err && err.message) msg += ' (' + err.message + ')';
                    statusDiv.textContent = msg;
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
         */
        exportConfigFile() {
            var filePath = this._currentFilePath;
            if (!filePath) {
                Notification.warning(i18n.t('fs-no-file-selected') || '请先选择一个配置文件');
                return;
            }

            var editor = document.getElementById('file-editor');
            if (!editor || !editor.value) {
                Notification.warning(i18n.t('fs-editor-empty') || '文件内容为空');
                return;
            }

            // 验证 JSON 格式
            try {
                JSON.parse(editor.value);
            } catch (e) {
                Notification.error(i18n.t('fs-invalid-json') || '文件内容不是有效的 JSON 格式');
                return;
            }

            // 提取文件名
            var parts = filePath.split('/');
            var fileName = parts[parts.length - 1] || 'config.json';

            var blob = new Blob([editor.value], { type: 'application/json' });
            var url = URL.createObjectURL(blob);
            var a = document.createElement('a');
            a.href = url;
            a.download = fileName;
            document.body.appendChild(a);
            a.click();
            document.body.removeChild(a);
            URL.revokeObjectURL(url);
            Notification.success(i18n.t('fs-export-ok') || '配置导出成功');
        },

        /**
         * 导入配置文件（上传 JSON 替换当前配置）
         */
        importConfigFile() {
            var self = this;
            var filePath = this._currentFilePath;
            if (!filePath) {
                Notification.warning(i18n.t('fs-no-file-selected') || '请先选择一个配置文件');
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
                    Notification.error(i18n.t('fs-import-too-large') || '文件过大，最大支持 64KB');
                    return;
                }

                var reader = new FileReader();
                reader.onload = function(evt) {
                    var content = evt.target.result;

                    // JSON 格式验证
                    try {
                        var parsed = JSON.parse(content);
                        // 验证必须是对象类型
                        if (typeof parsed !== 'object' || parsed === null || Array.isArray(parsed)) {
                            Notification.error(i18n.t('fs-import-not-object') || '配置文件必须是 JSON 对象格式');
                            return;
                        }
                    } catch (parseErr) {
                        Notification.error(i18n.t('fs-import-invalid-json') || '文件内容不是有效的 JSON 格式');
                        return;
                    }

                    // 确认覆盖
                    if (!confirm(i18n.t('fs-import-confirm') || '确定要用导入的文件覆盖当前配置吗？此操作不可撤销！')) {
                        return;
                    }

                    // 上传到设备
                    apiPost('/api/files/content', { path: filePath, content: content })
                        .then(function(res) {
                            if (res && res.success) {
                                Notification.success(i18n.t('fs-import-ok') || '配置导入成功');
                                // 刷新编辑器显示
                                self.openFile(filePath);
                            } else {
                                Notification.error(
                                    (res && res.message) || i18n.t('fs-import-fail') || '配置导入失败'
                                );
                            }
                        })
                        .catch(function(err) {
                            console.error('Import config failed:', err);
                            Notification.error(i18n.t('fs-import-fail') || '配置导入失败');
                        });
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
