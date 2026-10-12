/**
 * 用户管理模块
 * 包含用户列表、添加/编辑/删除用户、启用/禁用/解锁
 */
(function() {
    // This template follows its lazy page module and mounts only on demand.
    if (typeof PageLoader !== 'undefined') {
        PageLoader._cacheModalTemplates("<div id=\"add-user-modal\" class=\"modal\"><div class=\"modal-content\"><div class=\"modal-header\"><h2 class=\"modal-title\" id=\"add-user-title\">添加用户</h2><button id=\"close-add-user-modal\" class=\"modal-close-btn\">×</button></div><div class=\"modal-body\"><form id=\"add-user-form\" class=\"fb-form\"><fieldset><div class=\"fb-form-group\"><label for=\"add-username-input\" id=\"add-username-label\">用户名</label><input type=\"text\" id=\"add-username-input\" class=\"\" placeholder=\"请输入用户名\" required></div><div class=\"fb-form-group\"><label for=\"add-password-input\" id=\"add-password-label\">密码</label><input type=\"password\" id=\"add-password-input\" class=\"\" placeholder=\"请输入密码\" required></div><div class=\"fb-form-group\"><label for=\"add-confirm-password-input\" id=\"add-confirm-password-label\">确认密码</label><input type=\"password\" id=\"add-confirm-password-input\" class=\"\" placeholder=\"请再次输入密码\" required></div><div class=\"fb-form-group\"><label for=\"add-description-input\" id=\"add-description-label\">描述</label><input type=\"text\" id=\"add-description-input\" class=\"\" placeholder=\"可选，最多64字符\" maxlength=\"64\"></div><div id=\"add-user-error\" class=\"message message-error is-hidden\"></div></fieldset></form></div><div class=\"modal-footer\"><button class=\"fb-btn\" id=\"cancel-add-user-btn\" type=\"button\">取消</button><button class=\"fb-btn fb-btn-primary\" id=\"confirm-add-user-btn\" type=\"button\">添加</button></div></div></div>", '', ['add-user-modal']);
    }

    AppState.registerModule('users', {

        _usersEventsBound: false,

        _updateUsersSummary(users, errorText) {
            const summary = document.getElementById('users-list-summary');
            if (!summary) return;
            summary.classList.toggle('is-warning', !!errorText);
            if (errorText) {
                summary.textContent = errorText;
                return;
            }
            const list = Array.isArray(users) ? users : [];
            const available = list.filter(user => user && user.enabled !== false && !user.isLocked).length;
            summary.textContent = '共 ' + list.length + ' 个用户 · ' + available + ' 个可用';
        },

        _configureUserPasswordFields(isEditMode) {
            const password = document.getElementById('add-password-input');
            const confirmPassword = document.getElementById('add-confirm-password-input');
            [password, confirmPassword].forEach((input) => {
                if (!input) return;
                input.required = !isEditMode;
                input.minLength = this._userPasswordPolicy?.minLength || 6;
                input.maxLength = this._userPasswordPolicy?.maxLength || 64;
                input.title = '密码长度 ' + input.minLength + '–' + input.maxLength + ' 位' + (this._userPasswordPolicy?.requireStrong ? '，需包含大小写字母和数字' : '');
            });
            if (password) password.placeholder = isEditMode ? '留空则不修改密码' : '请输入密码';
            if (confirmPassword) {
                confirmPassword.placeholder = isEditMode ? '留空则不修改密码' : '请再次输入密码';
            }
        },

        _notifyUserActionError(err, message, title) {
            if (typeof window.isPageAbortError === 'function' && window.isPageAbortError(err)) return;
            if (typeof window.apiNotifyError === 'function') {
                window.apiNotifyError(err, message, title);
            } else {
                Notification.error(message, title);
            }
        },

        // ============ 事件绑定 ============
        setupUsersEvents() {
            if (this._usersEventsBound) return;
            // 添加用户按钮
            const addUserBtn = document.getElementById('add-user-btn');
            if (addUserBtn) {
                addUserBtn.addEventListener('click', () => this.showAddUserModal());
            }
            const usersRefreshBtn = document.getElementById('users-refresh-btn');
            if (usersRefreshBtn) usersRefreshBtn.addEventListener('click', () => this._refreshUsersList());

            if (typeof this._registerModalBinder === 'function') {
                this._registerModalBinder('users', () => this._bindUserModalEvents());
            }
            this._usersEventsBound = true;
        },

        _bindUserModalEvents() {
            if (this._userModalBound || !document.getElementById('add-user-modal')) return;
            // 关闭用户添加/编辑 modal 时重置状态
            const closeUserModal = () => {
                this._userEditorSeq = (this._userEditorSeq || 0) + 1;
                const modal = document.getElementById('add-user-modal');
                if (modal) {
                    AppState.hideModal(modal);
                    modal.dataset.editMode = 'add';
                    modal.dataset.editUsername = '';
                    modal.dataset.editEnabled = '';
                }
                // 重置用户名输入框状态
                const usernameInput = document.getElementById('add-username-input');
                if (usernameInput) usernameInput.disabled = false;
                this._configureUserPasswordFields(false);
            };
            ['close-add-user-modal', 'cancel-add-user-btn'].forEach(id => {
                const el = document.getElementById(id);
                if (el) el.addEventListener('click', closeUserModal);
            });

            // 确认添加用户
            const confirmAdd = document.getElementById('confirm-add-user-btn');
            if (confirmAdd) confirmAdd.addEventListener('click', () => this.addUser());
            this._userModalBound = true;
        },

        // ============ 用户列表（从 API 加载）============

        _refreshUsersList() {
            var btn = document.getElementById('users-refresh-btn');
            if (btn && btn.disabled) return;
            if (btn) { btn.disabled = true; btn.textContent = '加载中...'; }
            if (typeof window.apiInvalidateCache === 'function') {
                window.apiInvalidateCache('/api/users');
            }
            return this.loadUsers({ noCache: true }).finally(function() {
                if (btn && btn.isConnected) { btn.disabled = false; btn.textContent = '刷新'; }
            });
        },

        loadUsers(options) {
            var getter = (options && options.noCache === true && typeof apiGetFresh === 'function') ? apiGetFresh : apiGet;
            const summary = document.getElementById('users-list-summary');
            if (summary && !document.querySelector('#users-table-body tr')) summary.textContent = '加载中...';
            return getter('/api/users', { page: 1, limit: 100 })
                .then(res => {
                    if (!res || !res.success) throw new Error(res && (res.error || res.message) || 'load failed');
                    const users = (res.data && res.data.users) ? res.data.users : [];
                    this._userPasswordPolicy = res.data?.passwordPolicy || null;
                    this._renderUsers(users);
                    const modal = document.getElementById('add-user-modal');
                    if (modal) this._configureUserPasswordFields(modal.dataset.editMode === 'edit');
                })
                .catch((err) => {
                    if (typeof window.isPageAbortError === 'function' && window.isPageAbortError(err)) return;
                    this._updateUsersSummary(null, '用户列表加载失败');
                });
        },

        _renderUsers(users) {
            const tbody = document.getElementById('users-table-body');
            if (!tbody) return;

            this._updateUsersSummary(users);
            tbody.innerHTML = '';
            if (!users || users.length === 0) {
                this.renderEmptyTableRow(tbody, 5, '暂无用户数据');
                return;
            }

            users.forEach(user => {
                const row = document.createElement('tr');

                const td = (content) => {
                    const cell = document.createElement('td');
                    if (typeof content === 'string' || typeof content === 'number') {
                        cell.textContent = content;
                    } else {
                        cell.appendChild(content);
                    }
                    return cell;
                };

                const lastLogin = user.lastLogin ? new Date(user.lastLogin * 1000).toLocaleString() : '—';

                // 状态标志
                const badge = document.createElement('span');
                badge.className = 'badge';
                if (user.isLocked) {
                    badge.classList.add('badge-warning');
                    badge.textContent = '已锁定';
                } else if (!user.enabled) {
                    badge.classList.add('badge-danger');
                    badge.textContent = '禁用';
                } else {
                    badge.classList.add('badge-success');
                    badge.textContent = '启用';
                }

                // 操作按钮区域
                const actionCell = document.createElement('td');
                actionCell.className = 'u-toolbar-sm';

                const editBtn = document.createElement('button');
                editBtn.className = 'fb-btn fb-btn-sm fb-btn-primary fb-btn-action-edit';
                editBtn.textContent = '修改密码';
                editBtn.addEventListener('click', () => this.showEditUserModal(user));
                actionCell.appendChild(editBtn);

                // 超级管理员(admin)不显示禁用/启用按钮，必须始终保持可用
                if (user.username !== 'admin') {
                    const toggleBtn = document.createElement('button');
                    if (user.enabled && !user.isLocked) {
                        toggleBtn.className = 'fb-btn fb-btn-sm fb-btn-warning';
                        toggleBtn.textContent = '禁用';
                        toggleBtn.addEventListener('click', () => this.toggleUserStatus(user.username, false));
                    } else {
                        toggleBtn.className = 'fb-btn fb-btn-sm fb-btn-success';
                        toggleBtn.textContent = '启用';
                        toggleBtn.addEventListener('click', () => this.toggleUserStatus(user.username, true));
                    }
                    actionCell.appendChild(toggleBtn);
                }

                // 解锁按钮
                if (user.isLocked) {
                    const unlockBtn = document.createElement('button');
                    unlockBtn.className = 'fb-btn fb-btn-sm fb-btn-success';
                    unlockBtn.textContent = '解锁';
                    unlockBtn.addEventListener('click', () => this.unlockUser(user.username));
                    actionCell.appendChild(unlockBtn);
                }

                // 删除按钮（不能删除 admin）
                if (user.username !== 'admin') {
                    const delBtn = document.createElement('button');
                    delBtn.className = 'fb-btn fb-btn-sm fb-btn-danger';
                    delBtn.textContent = '删除';
                    delBtn.addEventListener('click', () => this.deleteUser(user.username));
                    actionCell.appendChild(delBtn);
                }

                row.appendChild(td(user.username));
                row.appendChild(td(lastLogin));
                row.appendChild(td(badge));
                row.appendChild(td(user.description || ''));
                row.appendChild(actionCell);
                tbody.appendChild(row);
            });
        },

        // ============ 添加用户 ============
        async showAddUserModal() {
            const seq = this._userEditorSeq = (this._userEditorSeq || 0) + 1;
            const modal = await this._ensureModalForAction('add-user-modal');
            if (seq !== this._userEditorSeq || this.currentPage !== 'users') return;
            if (modal) {
                // 先重置为添加模式，再显示弹窗
                modal.dataset.editMode = 'add';
                modal.dataset.editUsername = '';
                modal.dataset.editEnabled = '';
                AppState.showModal(modal);
            }

            // 修改标题
            const title = document.getElementById('add-user-title');
            if (title) title.textContent = '添加用户';

            // 修改按钮文本
            const confirmBtn = document.getElementById('confirm-add-user-btn');
            if (confirmBtn) confirmBtn.textContent = '确认添加';

            // 清空输入框
            ['add-username-input', 'add-password-input', 'add-confirm-password-input', 'add-description-input'].forEach(id => {
                const el = document.getElementById(id); if (el) el.value = '';
            });

            // 启用用户名输入
            const usernameInput = document.getElementById('add-username-input');
            if (usernameInput) usernameInput.disabled = false;
            this._configureUserPasswordFields(false);

            AppState.clearInlineError('add-user-error');
        },

        addUser() {
            if (this._userSavePending) return;
            const modal = document.getElementById('add-user-modal');
            if (!modal || !modal.isConnected) return;
            const seq = this._userEditorSeq;
            const active = () => seq === this._userEditorSeq && this.currentPage === 'users' && document.getElementById('add-user-modal') === modal;
            const isEditMode = modal && modal.dataset.editMode === 'edit';
            const editUsername = modal ? modal.dataset.editUsername : '';


            const username = isEditMode ? editUsername : ((document.getElementById('add-username-input') || {}).value || '').trim();
            const password = (document.getElementById('add-password-input') || {}).value || '';
            const confirmPwd = (document.getElementById('add-confirm-password-input') || {}).value || '';
            const description = ((document.getElementById('add-description-input') || {}).value || '').trim();
            const errDiv = document.getElementById('add-user-error');

            const showErr = (msg) => {
                AppState.showInlineError('add-user-error', msg);
                Notification.error(msg, isEditMode ? '编辑用户失败' : '添加用户失败');
            };

            if (!username) return showErr('请输入用户名！');

            // 用户名长度验证
            if (username.length < 3 || username.length > 32) return showErr('用户名长度3-32位！');

            // 新增时密码必填；编辑时两个密码框都留空表示保留原密码。
            if (!isEditMode && (!password || !confirmPwd)) return showErr('请输入密码！');
            if (password || confirmPwd) {
                if (password !== confirmPwd) return showErr('密码与确认密码不一致！');
                const policy = this._userPasswordPolicy || { minLength: 6, maxLength: 64 };
                if (password.length < policy.minLength || password.length > policy.maxLength) return showErr('密码长度应为 ' + policy.minLength + '–' + policy.maxLength + ' 位');
                if (policy.requireStrong && !(/[a-z]/.test(password) && /[A-Z]/.test(password) && /[0-9]/.test(password))) return showErr('密码需包含大小写字母和数字');
            }
            
            AppState.clearInlineError('add-user-error');

            const btn = document.getElementById('confirm-add-user-btn');
            this._userSavePending = true;
            if (btn) { btn.disabled = true; btn.textContent = isEditMode ? '保存中...' : '添加中...'; }

            // 根据模式选择 API
            let apiCall;
            if (isEditMode) {
                // 编辑模式：使用 POST /api/users/update
                const update = {
                    username,
                    enabled: modal && modal.dataset.editEnabled === 'false' ? 'false' : 'true',
                    description
                };
                if (password) update.password = password;
                apiCall = apiPost('/api/users/update', update);
            } else {
                // 添加模式
                apiCall = apiPost('/api/users', { username, password, description });
            }

            return apiCall
                .then(res => {
                    if (res && res.success && typeof window.apiInvalidateCache === 'function') window.apiInvalidateCache('/api/users');
                    if (!active()) return;
                    if (res && res.success) {
                        Notification.success(`${username} ${isEditMode ? '修改' : '创建'}成功`, isEditMode ? '编辑用户' : '添加成功');
                        // 关闭 modal 并重置状态
                        if (modal) {
                            AppState.hideModal(modal);
                            modal.dataset.editMode = 'add';
                            modal.dataset.editUsername = '';
                            modal.dataset.editEnabled = '';
                        }
                        // 重置用户名输入框状态
                        const usernameInput = document.getElementById('add-username-input');
                        if (usernameInput) usernameInput.disabled = false;
                        this.loadUsers({ noCache: true }); // 刷新列表
                    } else {
                        showErr((res && res.error) || (isEditMode ? '修改用户失败' : '添加用户失败，用户名可能已存在'));
                    }
                })
                .catch((err) => active() && this._notifyUserActionError(
                    err,
                    isEditMode ? '修改用户失败' : '添加用户失败',
                    '用户管理'
                ))
                .finally(() => {
                    this._userSavePending = false;
                    if (btn && btn.isConnected) {
                        btn.disabled = false;
                        btn.textContent = modal.dataset.editMode === 'edit' ? '保存修改' : '确认添加';
                    }
                });
        },

        // ============ 编辑用户（复用添加用户弹窗）============
        async showEditUserModal(user) {

            // 复用添加用户的 modal
            const seq = this._userEditorSeq = (this._userEditorSeq || 0) + 1;
            const modal = await this._ensureModalForAction('add-user-modal');
            if (seq !== this._userEditorSeq || this.currentPage !== 'users') return;
            if (!modal) {
                console.error('[showEditUserModal] Modal not found!');
                Notification.info(`编辑用户: ${user.username}`, '编辑用户');
                return;
            }

            // 先标记为编辑模式，防止状态丢失
            modal.dataset.editMode = 'edit';
            modal.dataset.editUsername = user.username;
            modal.dataset.editEnabled = user.enabled === false ? 'false' : 'true';

            // 修改标题
            const title = document.getElementById('add-user-title');
            if (title) title.textContent = '修改密码';

            // 填充用户信息
            const usernameInput = document.getElementById('add-username-input');
            if (usernameInput) {
                usernameInput.value = user.username;
                usernameInput.disabled = true;  // 用户名不可修改
            }

            // 填充描述
            const descInput = document.getElementById('add-description-input');
            if (descInput) descInput.value = user.description || '';

            // 清空密码字段（编辑时密码可选）
            const pwdInput = document.getElementById('add-password-input');
            const confirmInput = document.getElementById('add-confirm-password-input');
            if (pwdInput) pwdInput.value = '';
            if (confirmInput) confirmInput.value = '';
            this._configureUserPasswordFields(true);

            // 修改确认按钮文本
            const confirmBtn = document.getElementById('confirm-add-user-btn');
            if (confirmBtn) confirmBtn.textContent = '保存修改';

            // 显示弹窗
            AppState.showModal(modal);
        
            // 清除错误提示
            AppState.clearInlineError('add-user-error');
        },

        // ============ 切换用户状态（启用/禁用）============
        toggleUserStatus(username, enable) {
            const confirmMsg = enable ? '确定要启用用户' : '确定要禁用用户';
            if (!confirm(`${confirmMsg} ${username} 吗？`)) return;

            apiPost('/api/users/update', { username, enabled: enable ? 'true' : 'false' })
                .then(res => {
                    if (res && res.success) {
                        Notification.success(`${username} ${enable ? '已启用' : '已禁用'}`, '状态更新');
                        this.loadUsers({ noCache: true });
                    } else {
                        Notification.error((res && res.error) || '操作失败', '操作失败');
                    }
                })
                .catch((err) => this._notifyUserActionError(
                    err,
                    (enable ? '启用' : '禁用') + '用户失败',
                    '用户管理'
                ));
        },

        // ============ 解锁用户 ============
        unlockUser(username) {
            if (!confirm(`确定要解锁账户 ${username} 吗？`)) return;
            apiPost('/api/users/unlock-account', { username })
                .then(res => {
                    if (res && res.success) {
                        Notification.success(`${username} 已解锁`, '解锁成功');
                        this.loadUsers({ noCache: true });
                    } else {
                        Notification.error((res && res.error) || '操作失败', '操作失败');
                    }
                })
                .catch((err) => this._notifyUserActionError(err, '解锁用户失败', '用户管理'));
        },

        // ============ 删除用户 ============
        deleteUser(username) {
            if (!confirm(`确定要删除用户 ${username} 吗？`)) return;

            apiDelete('/api/users/' + encodeURIComponent(username))
                .then(res => {
                    if (res && res.success) {
                        Notification.success(`${username} 已删除`, '删除成功');
                        this.loadUsers({ noCache: true });
                    } else {
                        Notification.error((res && res.error) || '操作失败', '操作失败');
                    }
                })
                .catch((err) => this._notifyUserActionError(err, '删除用户失败', '用户管理'));
        }
    });

    // 自动绑定事件
    if (typeof AppState.setupUsersEvents === 'function') {
        AppState.setupUsersEvents();
    }
})();
