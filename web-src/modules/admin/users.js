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
            if (addUserBtn) {
                addUserBtn.addEventListener('click', () => this.showAddUserModal());
            }

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
            if (btn) { btn.disabled = true; btn.textContent = '加载中...'; }
            if (typeof window.apiInvalidateCache === 'function') {
                window.apiInvalidateCache('/api/users');
            }
            this.loadUsers({ noCache: true });
            setTimeout(function() {
                if (btn) { btn.disabled = false; btn.innerHTML = '&#x21bb; 刷新'; }
            }, 2000);
        },

        loadUsers(options) {
            var getter = (options && options.noCache === true && typeof apiGetFresh === 'function') ? apiGetFresh : apiGet;
            getter('/api/users', { page: 1, limit: 100 })
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
                    delBtn.addEventListener('click', () => {
                        if (confirm(`确定要删除用户 ${user.username} 吗？`)) {
                            this.deleteUser(user.username);
                        }
                    });
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

            AppState.clearInlineError('add-user-error');
        },

        addUser() {
            const modal = document.getElementById('add-user-modal');
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

            // 密码验证（添加和编辑模式都要求密码必填）
            if (!password || !confirmPwd) return showErr('请输入密码！');
            if (password !== confirmPwd) return showErr('密码与确认密码不一致！');
            if (password.length < 6) return showErr('密码长度至少6位！');
            
            AppState.clearInlineError('add-user-error');

            const btn = document.getElementById('confirm-add-user-btn');
            if (btn) { btn.disabled = true; btn.textContent = isEditMode ? '保存中...' : '添加中...'; }

            // 根据模式选择 API
            let apiCall;
            if (isEditMode) {
                // 编辑模式：使用 POST /api/users/update
                apiCall = apiPost('/api/users/update', { username, password, enabled: 'true', description });
            } else {
                // 添加模式
                apiCall = apiPost('/api/users', { username, password, description });
            }

            apiCall
                .then(res => {
                    if (res && res.success) {
                        Notification.success(`${username} ${isEditMode ? '修改' : '创建'}成功`, isEditMode ? '编辑用户' : '添加成功');
                        // 关闭 modal 并重置状态
                        if (modal) {
                            AppState.hideModal(modal);
                            modal.dataset.editMode = 'add';
                            modal.dataset.editUsername = '';
                        }
                        // 重置用户名输入框状态
                        const usernameInput = document.getElementById('add-username-input');
                        if (usernameInput) usernameInput.disabled = false;
                        this.loadUsers({ noCache: true }); // 刷新列表
                    } else {
                        showErr((res && res.error) || (isEditMode ? '修改用户失败' : '添加用户失败，用户名可能已存在'));
                    }
                })
                .catch(() => {})
                .finally(() => { if (btn) { btn.disabled = false; btn.textContent = '确认添加'; } });
        },

        // ============ 编辑用户（复用添加用户弹窗）============
        showEditUserModal(user) {

            // 复用添加用户的 modal
            const modal = document.getElementById('add-user-modal');
            if (!modal) {
                console.error('[showEditUserModal] Modal not found!');
                Notification.info(`编辑用户: ${user.username}`, '编辑用户');
                return;
            }

            // 先标记为编辑模式，防止状态丢失
            modal.dataset.editMode = 'edit';
            modal.dataset.editUsername = user.username;

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
            const action = enable ? '启用' : '禁用';
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
                .catch(() => {});
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
                .catch(() => {});
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
                .catch(() => {});
        }
    });

    // 自动绑定事件
    if (typeof AppState.setupUsersEvents === 'function') {
        AppState.setupUsersEvents();
    }
})();
