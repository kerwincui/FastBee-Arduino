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

                    let html = '';

                    // 目录
                    dirs.forEach(dir => {
                        html += `<div class="file-tree-item" data-path="${path}${dir.name}/" data-type="dir">
                            <span class="file-tree-icon-dir">📁</span> ${dir.name}
                        </div>`;
                    });

                    // 文件
                    files.forEach(file => {
                        const size = file.size < 1024 ? `${file.size} B` :
                                    file.size < 1024 * 1024 ? `${(file.size / 1024).toFixed(1)} KB` :
                                    `${(file.size / 1024 / 1024).toFixed(2)} MB`;
                        html += `<div class="file-tree-item" data-path="${path}${file.name}" data-type="file">
                            <span class="file-tree-icon-file">📄</span> ${file.name} <span class="file-tree-size">(${size})</span>
                        </div>`;
                    });

                    if (dirs.length === 0 && files.length === 0) {
                        html = i18n.t('fs-empty-dir-html');
                    }

                    treeContainer.innerHTML = html;

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

            if (!editable) {
                statusDiv.textContent = i18n.t('fs-file-type-unsupported');
                return;
            }

            pathSpan.textContent = path;
            statusDiv.textContent = i18n.t('fs-file-loading');

            apiGet('/api/files/content', { path: path })
                .then(res => {
                    if (!res || !res.success) {
                        statusDiv.textContent = i18n.t('fs-file-load-fail-prefix') + (res.error || i18n.t('fs-file-unknown-error'));
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
                    statusDiv.textContent = i18n.t('fs-file-load-fail');
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
        }
    });

    // 自动绑定事件
    if (typeof AppState.setupFilesEvents === 'function') {
        AppState.setupFilesEvents();
    }
})();
