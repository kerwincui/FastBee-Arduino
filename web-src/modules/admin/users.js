/**
 * 用户管理模块
 * 包含用户列表、添加/编辑/删除用户、启用/禁用/解锁
 */
(function() {
    AppState.registerModule('users', {

        // ============ 事件绑定 ============
        setupUsersEvents() {
            // 添加用户按钮
            const addUserBtn = document.getElementById('add-user-btn');
            if (addUserBtn) addUserBtn.addEventListener('click', () => this.showAddUserModal());

            // 关闭用户添加/编辑 modal 时重置状态
            const closeUserModal = () => {
                const modal = document.getElementById('add-user-modal');
                if (modal) {
                    AppState.hideModal(modal);
                    modal.dataset.editMode = 'add';
                    modal.dataset.editUsername = '';
                }
                // 重置用户名输入框状态
                const usernameInput = document.getElementById('add-username-input');
                if (usernameInput) usernameInput.disabled = false;
            };
            ['close-add-user-modal', 'cancel-add-user-btn'].forEach(id => {
                const el = document.getElementById(id);
                if (el) el.addEventListener('click', closeUserModal);
            });

            // 确认添加用户
            const confirmAdd = document.getElementById('confirm-add-user-btn');
            if (confirmAdd) confirmAdd.addEventListener('click', () => this.addUser());
            // 刷新按钮
            const usersRefreshBtn = document.getElementById('users-refresh-btn');
            if (usersRefreshBtn) usersRefreshBtn.addEventListener('click', () => this._refreshUsersList());
        },

        // ============ 用户列表（从 API 加载）============

        _refreshUsersList() {
            var btn = document.getElementById('users-refresh-btn');
            if (btn && btn.disabled) return;
            if (btn) { btn.disabled = true; btn.innerHTML = '<span class="fb-spin">&#x21bb;</span> 加载中...'; }
            if (typeof window.apiInvalidateCache === 'function') {
                window.apiInvalidateCache('/api/users');
            }
            this.loadUsers();
            setTimeout(function() {
                if (btn) { btn.disabled = false; btn.innerHTML = '&#x21bb; 刷新'; }
            }, 2000);
        },

        loadUsers() {
            apiGet('/api/users', { page: 1, limit: 100 })
                .then(res => {
                    if (!res || !res.success) return;
                    const users = (res.data && res.data.users) ? res.data.users : [];
                    this._renderUsers(users);
                })
                .catch(() => {});
        },

        _renderUsers(users) {
            const tbody = document.getElementById('users-table-body');
            if (!tbody) return;

            tbody.innerHTML = '';
            if (!users || users.length === 0) {
                this.renderEmptyTableRow(tbody, 5, i18n.t('no-users-data'));
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

                // 角色标签本地化
                const roleMap = { admin: i18n.t('admin'), operator: i18n.t('operator'), viewer: i18n.t('viewer') };
                const roleText = roleMap[user.role] || user.role || '—';
                const lastLogin = user.lastLogin ? new Date(user.lastLogin * 1000).toLocaleString() : '—';

                // 状态标志
                const badge = document.createElement('span');
                badge.className = 'badge';
                if (user.isLocked) {
                    badge.classList.add('badge-warning');
                    badge.textContent = i18n.t('user-locked-badge');
                } else if (!user.enabled) {
                    badge.classList.add('badge-danger');
                    badge.textContent = i18n.t('user-status-inactive');
                } else {
                    badge.classList.add('badge-success');
                    badge.textContent = i18n.t('user-status-active');
                }

                // 操作按钮区域
                const actionCell = document.createElement('td');
                actionCell.className = 'u-toolbar-sm';

                const editBtn = document.createElement('button');
                editBtn.className = 'btn btn-sm btn-edit';
                editBtn.textContent = i18n.t('edit-user');
                editBtn.addEventListener('click', () => this.showEditUserModal(user));
                actionCell.appendChild(editBtn);

                const toggleBtn = document.createElement('button');
                if (user.enabled && !user.isLocked) {
                    toggleBtn.className = 'btn btn-sm btn-disable';
                    toggleBtn.textContent = i18n.t('disable-user');
                    toggleBtn.addEventListener('click', () => this.toggleUserStatus(user.username, false));
                } else {
                    toggleBtn.className = 'btn btn-sm btn-enable';
                    toggleBtn.textContent = i18n.t('enable-user');
                    toggleBtn.addEventListener('click', () => this.toggleUserStatus(user.username, true));
                }
                actionCell.appendChild(toggleBtn);

                // 解锁按钮
                if (user.isLocked) {
                    const unlockBtn = document.createElement('button');
                    unlockBtn.className = 'btn btn-sm btn-enable';
                    unlockBtn.textContent = i18n.t('unlock-user');
                    unlockBtn.addEventListener('click', () => this.unlockUser(user.username));
                    actionCell.appendChild(unlockBtn);
                }

                // 删除按钮（不能删除 admin）
                if (user.username !== 'admin') {
                    const delBtn = document.createElement('button');
                    delBtn.className = 'btn btn-sm btn-delete';
                    delBtn.textContent = i18n.t('delete-user');
                    delBtn.addEventListener('click', () => {
                        if (confirm(`${i18n.t('confirm-delete-user-msg')} ${user.username} ${i18n.t('confirm-suffix')}`)) {
                            this.deleteUser(user.username);
                        }
                    });
                    actionCell.appendChild(delBtn);
                }

                row.appendChild(td(user.username));
                row.appendChild(td(roleText));
                row.appendChild(td(lastLogin));
                row.appendChild(td(badge));
                row.appendChild(actionCell);
                tbody.appendChild(row);
            });
        },

        // ============ 添加用户 ============
        showAddUserModal() {
            const modal = document.getElementById('add-user-modal');
            if (modal) {
                // 先重置为添加模式，再显示弹窗
                modal.dataset.editMode = 'add';
                modal.dataset.editUsername = '';
                AppState.showModal(modal);
            }

            // 修改标题
            const title = document.getElementById('add-user-title');
            if (title) title.textContent = i18n.t('add-user-modal-title');

            // 修改按钮文本
            const confirmBtn = document.getElementById('confirm-add-user-btn');
            if (confirmBtn) confirmBtn.textContent = i18n.t('confirm-add-btn');

            // 清空输入框
            ['add-username-input', 'add-password-input', 'add-confirm-password-input'].forEach(id => {
                const el = document.getElementById(id); if (el) el.value = '';
            });

            // 启用用户名输入
            const usernameInput = document.getElementById('add-username-input');
            if (usernameInput) usernameInput.disabled = false;

            const sel = document.getElementById('add-role-select');
            if (sel) sel.value = 'operator';
            AppState.clearInlineError('add-user-error');
        },

        addUser() {
            const modal = document.getElementById('add-user-modal');
            const isEditMode = modal && modal.dataset.editMode === 'edit';
            const editUsername = modal ? modal.dataset.editUsername : '';

            // 调试日志
            console.log('[addUser] modal:', modal);
            console.log('[addUser] modal.dataset:', modal ? JSON.stringify(modal.dataset) : 'null');
            console.log('[addUser] isEditMode:', isEditMode, 'editUsername:', editUsername);

            const username = isEditMode ? editUsername : ((document.getElementById('add-username-input') || {}).value || '').trim();
            const password = (document.getElementById('add-password-input') || {}).value || '';
            const confirmPwd = (document.getElementById('add-confirm-password-input') || {}).value || '';
            const role = (document.getElementById('add-role-select') || {}).value || 'operator';
            const errDiv = document.getElementById('add-user-error');

            const showErr = (msg) => {
                AppState.showInlineError('add-user-error', msg);
                Notification.error(msg, isEditMode ? i18n.t('edit-user-fail') : i18n.t('add-user-fail'));
            };

            if (!username) return showErr(i18n.t('validate-username-empty'));

            // 用户名长度验证
            if (username.length < 3 || username.length > 32) return showErr(i18n.t('validate-username-len'));

            // 密码验证（添加和编辑模式都要求密码必填）
            if (!password || !confirmPwd) return showErr(i18n.t('validate-pwd-empty'));
            if (password !== confirmPwd) return showErr(i18n.t('validate-pwd-mismatch'));
            if (password.length < 6) return showErr(i18n.t('validate-pwd-len'));
            
            AppState.clearInlineError('add-user-error');

            const btn = document.getElementById('confirm-add-user-btn');
            if (btn) { btn.disabled = true; btn.textContent = isEditMode ? i18n.t('saving-btn') : i18n.t('adding-btn'); }

            // 根据模式选择 API
            let apiCall;
            if (isEditMode) {
                // 编辑模式：使用 POST /api/users/update
                apiCall = apiPost('/api/users/update', { username, password, role, enabled: 'true' });
            } else {
                // 添加模式
                apiCall = apiPost('/api/users', { username, password, role });
            }

            apiCall
                .then(res => {
                    if (res && res.success) {
                        Notification.success(`${username} ${isEditMode ? i18n.t('role-updated') : i18n.t('role-created')}${i18n.t('role-success-suffix')}`, isEditMode ? i18n.t('edit-user-success') : i18n.t('add-user-success'));
                        // 关闭 modal 并重置状态
                        if (modal) {
                            AppState.hideModal(modal);
                            modal.dataset.editMode = 'add';
                            modal.dataset.editUsername = '';
                        }
                        // 重置用户名输入框状态
                        const usernameInput = document.getElementById('add-username-input');
                        if (usernameInput) usernameInput.disabled = false;
                        this.loadUsers(); // 刷新列表
                    } else {
                        showErr((res && res.error) || (isEditMode ? i18n.t('modify-user-fail-msg') : i18n.t('add-user-fail-msg')));
                    }
                })
                .catch(() => {})
                .finally(() => { if (btn) { btn.disabled = false; btn.textContent = i18n.t('confirm-add-btn'); } });
        },

        // ============ 编辑用户（复用添加用户弹窗）============
        showEditUserModal(user) {
            console.log('[showEditUserModal] Called with user:', user);

            // 复用添加用户的 modal
            const modal = document.getElementById('add-user-modal');
            if (!modal) {
                console.error('[showEditUserModal] Modal not found!');
                Notification.info(`${i18n.t('edit-user-modal-title')}: ${user.username}`, i18n.t('edit-user-modal-title'));
                return;
            }

            // 先标记为编辑模式，防止状态丢失
            modal.dataset.editMode = 'edit';
            modal.dataset.editUsername = user.username;
            console.log('[showEditUserModal] Set editMode:', modal.dataset.editMode, 'editUsername:', modal.dataset.editUsername);

            // 修改标题
            const title = document.getElementById('add-user-title');
            if (title) title.textContent = i18n.t('edit-user-modal-title');

            // 填充用户信息
            const usernameInput = document.getElementById('add-username-input');
            if (usernameInput) {
                usernameInput.value = user.username;
                usernameInput.disabled = true;  // 用户名不可修改
            }

            // 清空密码字段（编辑时密码可选）
            const pwdInput = document.getElementById('add-password-input');
            const confirmInput = document.getElementById('add-confirm-password-input');
            if (pwdInput) pwdInput.value = '';
            if (confirmInput) confirmInput.value = '';

            // 设置角色
            const roleSelect = document.getElementById('add-role-select');
            if (roleSelect) roleSelect.value = user.role || 'operator';

            // 修改确认按钮文本
            const confirmBtn = document.getElementById('confirm-add-user-btn');
            if (confirmBtn) confirmBtn.textContent = i18n.t('confirm-save-btn');

            // 显示弹窗
            AppState.showModal(modal);
        
            // 清除错误提示
            AppState.clearInlineError('add-user-error');
        },

        // ============ 切换用户状态（启用/禁用）============
        toggleUserStatus(username, enable) {
            const action = enable ? i18n.t('enable-user') : i18n.t('disable-user');
            const confirmMsg = enable ? i18n.t('confirm-enable-user') : i18n.t('confirm-disable-user');
            if (!confirm(`${confirmMsg} ${username} ${i18n.t('confirm-suffix')}`)) return;

            apiPost('/api/users/update', { username, enabled: enable ? 'true' : 'false' })
                .then(res => {
                    if (res && res.success) {
                        Notification.success(`${username} ${enable ? i18n.t('user-enabled-msg') : i18n.t('user-disabled-msg')}`, i18n.t('user-status-update'));
                        this.loadUsers();
                    } else {
                        Notification.error((res && res.error) || i18n.t('operation-fail'), i18n.t('operation-fail'));
                    }
                })
                .catch(() => {});
        },

        // ============ 解锁用户 ============
        unlockUser(username) {
            if (!confirm(`${i18n.t('confirm-unlock-user')} ${username} ${i18n.t('confirm-suffix')}`)) return;
            apiPost('/api/users/unlock-account', { username })
                .then(res => {
                    if (res && res.success) {
                        Notification.success(`${username} ${i18n.t('user-unlocked-msg')}`, i18n.t('unlock-success'));
                        this.loadUsers();
                    } else {
                        Notification.error((res && res.error) || i18n.t('operation-fail'), i18n.t('operation-fail'));
                    }
                })
                .catch(() => {});
        },

        // ============ 删除用户 ============
        deleteUser(username) {
            if (!confirm(`${i18n.t('confirm-delete-user-msg')} ${username} ${i18n.t('confirm-suffix')}`)) return;

            apiPost('/api/users/delete', { username })
                .then(res => {
                    if (res && res.success) {
                        Notification.success(`${username} ${i18n.t('user-deleted-msg')}`, i18n.t('delete-success'));
                        this.loadUsers();
                    } else {
                        Notification.error((res && res.error) || i18n.t('operation-fail'), i18n.t('operation-fail'));
                    }
                })
                .catch(() => {});
        }
    });

    // 自动绑定事件
    if (typeof AppState.setupUsersEvents === 'function') {
        AppState.setupUsersEvents();
    }
})();
