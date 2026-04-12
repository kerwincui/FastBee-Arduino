// admin-bundle.js - merged: users + roles + logs + files + rule-script

/**
 * 鐢ㄦ埛绠＄悊妯″潡
 * 鍖呭惈鐢ㄦ埛鍒楄〃銆佹坊鍔?缂栬緫/鍒犻櫎鐢ㄦ埛銆佸惎鐢?绂佺敤/瑙ｉ攣
 */
(function() {
    AppState.registerModule('users', {

        // ============ 浜嬩欢缁戝畾 ============
        setupUsersEvents() {
            // 娣诲姞鐢ㄦ埛鎸夐挳
            const addUserBtn = document.getElementById('add-user-btn');
            if (addUserBtn) addUserBtn.addEventListener('click', () => this.showAddUserModal());

            // 鍏抽棴鐢ㄦ埛娣诲姞/缂栬緫 modal 鏃堕噸缃姸鎬?
            const closeUserModal = () => {
                const modal = document.getElementById('add-user-modal');
                if (modal) {
                    modal.style.display = 'none';
                    modal.dataset.editMode = 'add';
                    modal.dataset.editUsername = '';
                }
                // 閲嶇疆鐢ㄦ埛鍚嶈緭鍏ユ鐘舵€?
                const usernameInput = document.getElementById('add-username-input');
                if (usernameInput) usernameInput.disabled = false;
            };
            ['close-add-user-modal', 'cancel-add-user-btn'].forEach(id => {
                const el = document.getElementById(id);
                if (el) el.addEventListener('click', closeUserModal);
            });

            // 纭娣诲姞鐢ㄦ埛
            const confirmAdd = document.getElementById('confirm-add-user-btn');
            if (confirmAdd) confirmAdd.addEventListener('click', () => this.addUser());
        },

        // ============ 鐢ㄦ埛鍒楄〃锛堜粠 API 鍔犺浇锛?===========
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
                tbody.innerHTML = `<tr><td colspan="5" style="text-align:center;color:#888">${i18n.t('no-users-data')}</td></tr>`;
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

                // 瑙掕壊鏍囩鏈湴鍖?
                const roleMap = { admin: i18n.t('admin'), operator: i18n.t('operator'), viewer: i18n.t('viewer') };
                const roleText = roleMap[user.role] || user.role || '鈥?;
                const lastLogin = user.lastLogin ? new Date(user.lastLogin * 1000).toLocaleString() : '鈥?;

                // 鐘舵€佹爣蹇?
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

                // 鎿嶄綔鎸夐挳鍖哄煙
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

                // 瑙ｉ攣鎸夐挳
                if (user.isLocked) {
                    const unlockBtn = document.createElement('button');
                    unlockBtn.className = 'btn btn-sm btn-enable';
                    unlockBtn.textContent = i18n.t('unlock-user');
                    unlockBtn.addEventListener('click', () => this.unlockUser(user.username));
                    actionCell.appendChild(unlockBtn);
                }

                // 鍒犻櫎鎸夐挳锛堜笉鑳藉垹闄?admin锛?
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

        // ============ 娣诲姞鐢ㄦ埛 ============
        showAddUserModal() {
            const modal = document.getElementById('add-user-modal');
            if (modal) {
                // 鍏堥噸缃负娣诲姞妯″紡锛屽啀鏄剧ず寮圭獥
                modal.dataset.editMode = 'add';
                modal.dataset.editUsername = '';
                modal.style.display = 'flex';
            }

            // 淇敼鏍囬
            const title = document.getElementById('add-user-title');
            if (title) title.textContent = i18n.t('add-user-modal-title');

            // 淇敼鎸夐挳鏂囨湰
            const confirmBtn = document.getElementById('confirm-add-user-btn');
            if (confirmBtn) confirmBtn.textContent = i18n.t('confirm-add-btn');

            // 娓呯┖杈撳叆妗?
            ['add-username-input', 'add-password-input', 'add-confirm-password-input'].forEach(id => {
                const el = document.getElementById(id); if (el) el.value = '';
            });

            // 鍚敤鐢ㄦ埛鍚嶈緭鍏?
            const usernameInput = document.getElementById('add-username-input');
            if (usernameInput) usernameInput.disabled = false;

            const sel = document.getElementById('add-role-select');
            if (sel) sel.value = 'operator';
            const errDiv = document.getElementById('add-user-error');
            if (errDiv) errDiv.style.display = 'none';
        },

        addUser() {
            const modal = document.getElementById('add-user-modal');
            const isEditMode = modal && modal.dataset.editMode === 'edit';
            const editUsername = modal ? modal.dataset.editUsername : '';

            // 璋冭瘯鏃ュ織
            console.log('[addUser] modal:', modal);
            console.log('[addUser] modal.dataset:', modal ? JSON.stringify(modal.dataset) : 'null');
            console.log('[addUser] isEditMode:', isEditMode, 'editUsername:', editUsername);

            const username = isEditMode ? editUsername : ((document.getElementById('add-username-input') || {}).value || '').trim();
            const password = (document.getElementById('add-password-input') || {}).value || '';
            const confirmPwd = (document.getElementById('add-confirm-password-input') || {}).value || '';
            const role = (document.getElementById('add-role-select') || {}).value || 'operator';
            const errDiv = document.getElementById('add-user-error');

            const showErr = (msg) => {
                if (errDiv) { errDiv.textContent = msg; errDiv.style.display = 'block'; }
                Notification.error(msg, isEditMode ? i18n.t('edit-user-fail') : i18n.t('add-user-fail'));
            };

            if (!username) return showErr(i18n.t('validate-username-empty'));

            // 鐢ㄦ埛鍚嶉暱搴﹂獙璇?
            if (username.length < 3 || username.length > 32) return showErr(i18n.t('validate-username-len'));

            // 瀵嗙爜楠岃瘉锛堟坊鍔犲拰缂栬緫妯″紡閮借姹傚瘑鐮佸繀濉級
            if (!password || !confirmPwd) return showErr(i18n.t('validate-pwd-empty'));
            if (password !== confirmPwd) return showErr(i18n.t('validate-pwd-mismatch'));
            if (password.length < 6) return showErr(i18n.t('validate-pwd-len'));

            if (errDiv) errDiv.style.display = 'none';

            const btn = document.getElementById('confirm-add-user-btn');
            if (btn) { btn.disabled = true; btn.textContent = isEditMode ? i18n.t('saving-btn') : i18n.t('adding-btn'); }

            // 鏍规嵁妯″紡閫夋嫨 API
            let apiCall;
            if (isEditMode) {
                // 缂栬緫妯″紡锛氫娇鐢?POST /api/users/update
                apiCall = apiPost('/api/users/update', { username, password, role, enabled: 'true' });
            } else {
                // 娣诲姞妯″紡
                apiCall = apiPost('/api/users', { username, password, role });
            }

            apiCall
                .then(res => {
                    if (res && res.success) {
                        Notification.success(`${username} ${isEditMode ? i18n.t('role-updated') : i18n.t('role-created')}${i18n.t('role-success-suffix')}`, isEditMode ? i18n.t('edit-user-success') : i18n.t('add-user-success'));
                        // 鍏抽棴 modal 骞堕噸缃姸鎬?
                        if (modal) {
                            modal.style.display = 'none';
                            modal.dataset.editMode = 'add';
                            modal.dataset.editUsername = '';
                        }
                        // 閲嶇疆鐢ㄦ埛鍚嶈緭鍏ユ鐘舵€?
                        const usernameInput = document.getElementById('add-username-input');
                        if (usernameInput) usernameInput.disabled = false;
                        this.loadUsers(); // 鍒锋柊鍒楄〃
                    } else {
                        showErr((res && res.error) || (isEditMode ? i18n.t('modify-user-fail-msg') : i18n.t('add-user-fail-msg')));
                    }
                })
                .catch(() => {})
                .finally(() => { if (btn) { btn.disabled = false; btn.textContent = i18n.t('confirm-add-btn'); } });
        },

        // ============ 缂栬緫鐢ㄦ埛锛堝鐢ㄦ坊鍔犵敤鎴峰脊绐楋級============
        showEditUserModal(user) {
            console.log('[showEditUserModal] Called with user:', user);

            // 澶嶇敤娣诲姞鐢ㄦ埛鐨?modal
            const modal = document.getElementById('add-user-modal');
            if (!modal) {
                console.error('[showEditUserModal] Modal not found!');
                Notification.info(`${i18n.t('edit-user-modal-title')}: ${user.username}`, i18n.t('edit-user-modal-title'));
                return;
            }

            // 鍏堟爣璁颁负缂栬緫妯″紡锛岄槻姝㈢姸鎬佷涪澶?
            modal.dataset.editMode = 'edit';
            modal.dataset.editUsername = user.username;
            console.log('[showEditUserModal] Set editMode:', modal.dataset.editMode, 'editUsername:', modal.dataset.editUsername);

            // 淇敼鏍囬
            const title = document.getElementById('add-user-title');
            if (title) title.textContent = i18n.t('edit-user-modal-title');

            // 濉厖鐢ㄦ埛淇℃伅
            const usernameInput = document.getElementById('add-username-input');
            if (usernameInput) {
                usernameInput.value = user.username;
                usernameInput.disabled = true;  // 鐢ㄦ埛鍚嶄笉鍙慨鏀?
            }

            // 娓呯┖瀵嗙爜瀛楁锛堢紪杈戞椂瀵嗙爜鍙€夛級
            const pwdInput = document.getElementById('add-password-input');
            const confirmInput = document.getElementById('add-confirm-password-input');
            if (pwdInput) pwdInput.value = '';
            if (confirmInput) confirmInput.value = '';

            // 璁剧疆瑙掕壊
            const roleSelect = document.getElementById('add-role-select');
            if (roleSelect) roleSelect.value = user.role || 'operator';

            // 淇敼纭鎸夐挳鏂囨湰
            const confirmBtn = document.getElementById('confirm-add-user-btn');
            if (confirmBtn) confirmBtn.textContent = i18n.t('confirm-save-btn');

            // 鏄剧ず寮圭獥
            modal.style.display = 'flex';

            // 娓呴櫎閿欒鎻愮ず
            const errDiv = document.getElementById('add-user-error');
            if (errDiv) errDiv.style.display = 'none';
        },

        // ============ 鍒囨崲鐢ㄦ埛鐘舵€侊紙鍚敤/绂佺敤锛?===========
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

        // ============ 瑙ｉ攣鐢ㄦ埛 ============
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

        // ============ 鍒犻櫎鐢ㄦ埛 ============
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

    // 鑷姩缁戝畾浜嬩欢
    if (typeof AppState.setupUsersEvents === 'function') {
        AppState.setupUsersEvents();
    }
})();

/**
 * 瑙掕壊绠＄悊妯″潡
 * 鍖呭惈瑙掕壊鍒楄〃銆佹潈闄愮鐞嗐€佽鑹?CRUD
 */
(function() {
    AppState.registerModule('roles', {

        // ============ 浜嬩欢缁戝畾 ============
        setupRolesEvents() {
            const closeId = (id) => { const el = document.getElementById(id); if (el) el.style.display = 'none'; };

            // 娣诲姞瑙掕壊鎸夐挳
            const addRoleBtn = document.getElementById('add-role-btn');
            if (addRoleBtn) addRoleBtn.addEventListener('click', () => this.showAddRoleModal());

            // 瑙掕壊妯℃€佺獥鍏抽棴
            ['close-role-modal', 'cancel-role-btn'].forEach(id => {
                const el = document.getElementById(id);
                if (el) el.addEventListener('click', () => closeId('role-modal'));
            });

            // 纭淇濆瓨瑙掕壊
            const confirmRole = document.getElementById('confirm-role-btn');
            if (confirmRole) confirmRole.addEventListener('click', () => this.saveRole());
        },

        // ============ 瑙掕壊绠＄悊 ============
        loadRoles() {
            apiGet('/api/roles')
                .then(res => {
                    if (!res || !res.success) return;
                    const roles = (res.data && res.data.roles) ? res.data.roles : [];
                    const permissions = (res.data && res.data.permissions) ? res.data.permissions : [];
                    this._permDefs = permissions;  // 淇濆瓨鏉冮檺瀹氫箟
                    this._renderRolesList(roles);
                })
                .catch(() => {});
        },

        _renderRolesList(roles) {
            const tbody = document.getElementById('roles-table-body');
            if (!tbody) return;
            tbody.innerHTML = '';

            roles.forEach(role => {
                const row = document.createElement('tr');

                // 瑙掕壊ID
                const tdId = document.createElement('td');
                tdId.textContent = role.id;
                row.appendChild(tdId);

                // 瑙掕壊鍚嶇О - 缈昏瘧鍐呯疆瑙掕壊鍚?
                const tdName = document.createElement('td');
                const _rnm = {'绠＄悊鍛?: 'role-admin', '鎿嶄綔鍛?: 'role-operator', '鏌ョ湅鑰?: 'role-viewer'};
                const dName = _rnm[role.name] ? i18n.t(_rnm[role.name]) : role.name;
                tdName.textContent = dName;
                row.appendChild(tdName);

                // 鎻忚堪
                const tdDesc = document.createElement('td');
                tdDesc.textContent = role.description || '鈥?;
                tdDesc.style.maxWidth = '200px';
                tdDesc.style.overflow = 'hidden';
                tdDesc.style.textOverflow = 'ellipsis';
                tdDesc.style.whiteSpace = 'nowrap';
                row.appendChild(tdDesc);

                // 鏉冮檺鏁?
                const tdPermCount = document.createElement('td');
                const permCount = (role.permissions || []).length;
                tdPermCount.innerHTML = `<span style="background: #e6f7ff; color: #1890ff; padding: 2px 8px; border-radius: 10px; font-size: 12px;">${permCount}</span>`;
                row.appendChild(tdPermCount);

                // 绫诲瀷
                const tdType = document.createElement('td');
                if (role.id === 'admin') {
                    tdType.innerHTML = `<span style="background: #fff1f0; color: #f5222d; padding: 2px 8px; border-radius: 4px; font-size: 12px;">${i18n.t('role-type-super')}</span>`;
                } else if (role.isBuiltin) {
                    tdType.innerHTML = `<span style="background: #f6ffed; color: #52c41a; padding: 2px 8px; border-radius: 4px; font-size: 12px;">${i18n.t('role-type-builtin')}</span>`;
                } else {
                    tdType.innerHTML = `<span style="background: #f0f5ff; color: #2f54eb; padding: 2px 8px; border-radius: 4px; font-size: 12px;">${i18n.t('role-type-custom')}</span>`;
                }
                row.appendChild(tdType);

                // 鎿嶄綔
                const tdAction = document.createElement('td');
                tdAction.className = 'u-toolbar-sm';

                // 鏌ョ湅鏉冮檺鎸夐挳
                const viewBtn = document.createElement('button');
                viewBtn.className = 'btn btn-sm btn-secondary';
                viewBtn.textContent = i18n.t('role-view-perms');
                viewBtn.addEventListener('click', () => this.showRolePermissions(role));
                tdAction.appendChild(viewBtn);

                // 浠?admin 瑙掕壊涓嶅彲缂栬緫/鍒犻櫎
                if (role.id !== 'admin') {
                    // 缂栬緫鎸夐挳
                    const editBtn = document.createElement('button');
                    editBtn.className = 'btn btn-sm btn-edit';
                    editBtn.textContent = i18n.t('role-edit');
                    editBtn.style.marginRight = '5px';
                    editBtn.addEventListener('click', () => this.showEditRoleModal(role.id));
                    tdAction.appendChild(editBtn);

                    // 鍒犻櫎鎸夐挳
                    const delBtn = document.createElement('button');
                    delBtn.className = 'btn btn-sm btn-delete';
                    delBtn.textContent = i18n.t('role-delete');
                    delBtn.addEventListener('click', () => {
                        const _rnm2 = {'绠＄悊鍛?: 'role-admin', '鎿嶄綔鍛?: 'role-operator', '鏌ョ湅鑰?: 'role-viewer'};
                        const dName2 = _rnm2[role.name] ? i18n.t(_rnm2[role.name]) : role.name;
                        this.deleteRole(role.id, dName2);
                    });
                    tdAction.appendChild(delBtn);
                }

                row.appendChild(tdAction);
                tbody.appendChild(row);
            });
        },

        // 鏌ョ湅瑙掕壊鏉冮檺璇︽儏
        showRolePermissions(role) {
            const permDefs = this._permDefs || [];
            const rolePerms = new Set(role.permissions || []);

            // 鏉冮檺缁勫悕涓枃鈫掕嫳鏂囨槧灏刱ey锛堝悓鏃舵敮鎸佷腑鑻辨枃锛?
            const _gpk = {
                '璁惧':'device', 'Device':'device',
                '缃戠粶':'network', 'Network':'network',
                '绯荤粺':'system', 'System':'system',
                '鐢ㄦ埛':'user', 'Users':'user',
                '鏂囦欢':'file', 'Files':'file',
                '鍗忚':'protocol', 'Protocol':'protocol',
                '瀹¤':'audit', 'Audit':'audit',
                'GPIO':'gpio',
                '澶栬':'peripheral', 'Peripheral':'peripheral'
            };

            // 鎸夊垎缁勬暣鐞?
            const permGroups = {};
            permDefs.forEach(p => {
                if (!permGroups[p.group]) permGroups[p.group] = [];
                permGroups[p.group].push(p);
            });

            let html = `<div style="max-height: 400px; overflow-y: auto;">`;
            Object.keys(permGroups).sort().forEach(group => {
                const gKey = _gpk[group] ? i18n.t('perm-group-' + _gpk[group]) : group;
                html += `<div style="margin-bottom: 15px;">`;
                html += `<h4 style="margin: 0 0 8px; font-size: 14px; color: #333; border-bottom: 1px solid #eee; padding-bottom: 5px;">${gKey}</h4>`;
                html += `<div style="display: flex; flex-wrap: wrap; gap: 8px;">`;
                permGroups[group].forEach(perm => {
                    const hasPerm = rolePerms.has(perm.id);
                    const pName = i18n.t('perm-' + perm.id) !== ('perm-' + perm.id) ? i18n.t('perm-' + perm.id) : perm.name;
                    const pDesc = i18n.t('perm-' + perm.id) !== ('perm-' + perm.id) ? i18n.t('perm-' + perm.id) : perm.description;
                    html += `<span style="display: inline-flex; align-items: center; padding: 4px 10px; border-radius: 4px; font-size: 12px; ${hasPerm ? 'background: #e6f7ff; color: #1890ff; border: 1px solid #91d5ff;' : 'background: #f5f5f5; color: #999; border: 1px solid #d9d9d9;'}">`;
                    html += `<span style="margin-right: 4px;">${hasPerm ? '鉁? : '鉁?}</span>`;
                    html += `<span title="${pDesc}">${pName}</span></span>`;
                });
                html += `</div></div>`;
            });
            html += `</div>`;

            // 缈昏瘧瑙掕壊鍚?
            const roleNameMap = {'绠＄悊鍛?: 'role-admin', '鎿嶄綔鍛?: 'role-operator', '鏌ョ湅鑰?: 'role-viewer'};
            const displayRoleName = roleNameMap[role.name] ? i18n.t(roleNameMap[role.name]) : role.name;

            // 浣跨敤绠€鍗曠殑 alert 寮圭獥鏄剧ず锛堟垨鍙互鍒涘缓涓€涓ā鎬佺獥锛?
            const div = document.createElement('div');
            div.innerHTML = `
                <div style="position: fixed; top: 0; left: 0; right: 0; bottom: 0; background: rgba(0,0,0,0.5); z-index: 9999; display: flex; justify-content: center; align-items: center;" onclick="this.remove()">
                    <div style="background: white; border-radius: 8px; max-width: 600px; width: 90%; max-height: 80vh; overflow: hidden;" onclick="event.stopPropagation()">
                        <div style="background: linear-gradient(135deg, #1890ff 0%, #096dd9 100%); color: white; padding: 15px 20px; display: flex; justify-content: space-between; align-items: center;">
                            <h3 style="margin: 0;">${displayRoleName}${i18n.t('role-detail-suffix')}</h3>
                            <button onclick="this.closest('div[style*=position]').remove()" style="background: none; border: none; color: white; font-size: 20px; cursor: pointer;">脳</button>
                        </div>
                        <div style="padding: 20px;">${html}</div>
                    </div>
                </div>
            `;
            document.body.appendChild(div);
        },

        // ============ 瑙掕壊绠＄悊 CRUD ============

        showAddRoleModal() {
            const modal = document.getElementById('role-modal');
            if (!modal) return;

            // 璁剧疆鏍囬
            const title = document.getElementById('role-modal-title');
            if (title) title.textContent = i18n.t('role-add-title');

            // 娓呯┖杈撳叆
            const idInput = document.getElementById('role-id-input');
            const nameInput = document.getElementById('role-name-input');
            const descInput = document.getElementById('role-desc-input');
            if (idInput) { idInput.value = ''; idInput.disabled = false; }
            if (nameInput) nameInput.value = '';
            if (descInput) descInput.value = '';

            // 鏍囪涓烘柊澧炴ā寮?
            modal.dataset.editMode = 'add';
            modal.dataset.editRoleId = '';

            // 娓叉煋鏉冮檺澶嶉€夋
            this._renderPermissionCheckboxes([]);

            // 娓呴櫎閿欒鎻愮ず
            const errDiv = document.getElementById('role-error');
            if (errDiv) errDiv.style.display = 'none';

            modal.style.display = 'flex';
        },

        showEditRoleModal(roleId) {
            const modal = document.getElementById('role-modal');
            if (!modal) {
                console.error('[showEditRoleModal] modal not found!');
                return;
            }

            console.log('[showEditRoleModal] Opening edit modal for roleId:', roleId);

            // 鍏堣缃紪杈戞ā寮忥紝闃叉API鍥炶皟鍓嶇敤鎴风偣鍑讳繚瀛樺鑷磋皟鐢ㄩ敊璇殑API
            modal.dataset.editMode = 'edit';
            modal.dataset.editRoleId = roleId;

            // 鑾峰彇瑙掕壊淇℃伅
            apiGet('/api/roles')
                .then(res => {
                    if (!res || !res.success) return;
                    const roles = (res.data && res.data.roles) ? res.data.roles : [];
                    const role = roles.find(r => r.id === roleId);
                    if (!role) {
                        Notification.error(i18n.t('role-not-exist'), i18n.t('error-title'));
                        return;
                    }

                    // 璁剧疆鏍囬
                    const title = document.getElementById('role-modal-title');
                    if (title) title.textContent = i18n.t('role-edit-title');

                    // 濉厖鏁版嵁
                    const idInput = document.getElementById('role-id-input');
                    const nameInput = document.getElementById('role-name-input');
                    const descInput = document.getElementById('role-desc-input');
                    if (idInput) { idInput.value = role.id; idInput.disabled = true; }
                    if (nameInput) nameInput.value = role.name;
                    if (descInput) descInput.value = role.description || '';

                    console.log('[showEditRoleModal] Set editMode:', modal.dataset.editMode, 'editRoleId:', modal.dataset.editRoleId);

                    // 娓叉煋鏉冮檺澶嶉€夋
                    this._renderPermissionCheckboxes(role.permissions || []);

                    // 娓呴櫎閿欒鎻愮ず
                    const errDiv = document.getElementById('role-error');
                    if (errDiv) errDiv.style.display = 'none';

                    modal.style.display = 'flex';
                })
                .catch(() => {});
        },

        _renderPermissionCheckboxes(selectedPerms) {
            const container = document.getElementById('role-permissions-container');
            if (!container) return;
            container.innerHTML = '';

            const permDefs = this._permDefs || [];
            const selectedSet = new Set(selectedPerms);

            // 鏉冮檺缁勫悕涓枃鈫掕嫳鏂囨槧灏刱ey锛堝悓鏃舵敮鎸佷腑鑻辨枃锛?
            const _gpk = {
                '璁惧':'device', 'Device':'device',
                '缃戠粶':'network', 'Network':'network',
                '绯荤粺':'system', 'System':'system',
                '鐢ㄦ埛':'user', 'Users':'user',
                '鏂囦欢':'file', 'Files':'file',
                '鍗忚':'protocol', 'Protocol':'protocol',
                '瀹¤':'audit', 'Audit':'audit',
                'GPIO':'gpio',
                '澶栬':'peripheral', 'Peripheral':'peripheral'
            };

            // 鎸夊垎缁勬暣鐞?
            const permGroups = {};
            permDefs.forEach(p => {
                if (!permGroups[p.group]) permGroups[p.group] = [];
                permGroups[p.group].push(p);
            });

            Object.keys(permGroups).sort().forEach(group => {
                const groupDiv = document.createElement('div');
                groupDiv.style.cssText = 'margin-bottom: 12px;';

                const gKey = _gpk[group] ? i18n.t('perm-group-' + _gpk[group]) : group;
                const groupTitle = document.createElement('h5');
                groupTitle.style.cssText = 'margin: 0 0 8px; font-size: 13px; color: #666; border-bottom: 1px solid #eee; padding-bottom: 5px;';
                groupTitle.textContent = gKey;
                groupDiv.appendChild(groupTitle);

                const permList = document.createElement('div');
                permList.style.cssText = 'display: flex; flex-wrap: wrap; gap: 10px;';

                permGroups[group].forEach(perm => {
                    const pName = i18n.t('perm-' + perm.id) !== ('perm-' + perm.id) ? i18n.t('perm-' + perm.id) : perm.name;
                    const pDesc = i18n.t('perm-' + perm.id) !== ('perm-' + perm.id) ? i18n.t('perm-' + perm.id) : perm.description;
                    const label = document.createElement('label');
                    label.style.cssText = 'display: flex; align-items: center; cursor: pointer; font-size: 12px;';
                    label.innerHTML = `
                        <input type="checkbox" name="role-perm" value="${perm.id}" ${selectedSet.has(perm.id) ? 'checked' : ''} style="margin-right: 4px;">
                        <span title="${pDesc}">${pName}</span>
                    `;
                    permList.appendChild(label);
                });

                groupDiv.appendChild(permList);
                container.appendChild(groupDiv);
            });
        },

        saveRole() {
            const modal = document.getElementById('role-modal');
            if (!modal) {
                Notification.error(i18n.t('role-modal-not-found'), i18n.t('error-title'));
                return;
            }

            const isEditMode = modal.dataset.editMode === 'edit';
            const editRoleId = modal.dataset.editRoleId || '';

            const idInput = document.getElementById('role-id-input');
            const id = isEditMode ? editRoleId : (idInput ? idInput.value.trim() : '');
            const name = ((document.getElementById('role-name-input') || {}).value || '').trim();
            const description = ((document.getElementById('role-desc-input') || {}).value || '').trim();
            const errDiv = document.getElementById('role-error');

            const showErr = (msg) => {
                if (errDiv) { errDiv.textContent = msg; errDiv.style.display = 'block'; }
                Notification.error(msg, isEditMode ? i18n.t('role-fail-edit') : i18n.t('role-fail-add'));
            };

            if (!id) return showErr(i18n.t('role-validate-id'));
            if (!name) return showErr(i18n.t('role-validate-name'));

            // 鑾峰彇閫変腑鐨勬潈闄?
            const permCheckboxes = document.querySelectorAll('input[name="role-perm"]:checked');
            const permissions = Array.from(permCheckboxes).map(cb => cb.value).join(',');

            if (errDiv) errDiv.style.display = 'none';

            const btn = document.getElementById('confirm-role-btn');
            if (btn) { btn.disabled = true; btn.textContent = i18n.t('role-saving-text'); }

            console.log('[saveRole] modal.dataset:', JSON.stringify(modal.dataset));
            console.log('[saveRole] isEditMode:', isEditMode, 'id:', id, 'editRoleId:', editRoleId);
            console.log('[saveRole] Will call API:', isEditMode ? '/api/roles/update' : '/api/roles');

            let apiCall;
            if (isEditMode) {
                // 缂栬緫妯″紡锛氬厛鏇存柊瑙掕壊淇℃伅锛屽啀鏇存柊鏉冮檺
                apiCall = apiPost('/api/roles/update', { id, name, description })
                    .then(res => {
                        if (res && res.success) {
                            return apiPost('/api/roles/permissions', { id, permissions });
                        }
                        throw new Error(res.error || i18n.t('role-fail-update-msg'));
                    });
            } else {
                // 鏂板妯″紡
                apiCall = apiPost('/api/roles', { id, name, description })
                    .then(res => {
                        if (res && res.success) {
                            return apiPost('/api/roles/permissions', { id, permissions });
                        }
                        throw new Error(res.error || i18n.t('role-fail-create-msg'));
                    });
            }

            apiCall
                .then(res => {
                    if (res && res.success) {
                        Notification.success(`${i18n.t('role-mgmt-title')}: ${name} ${isEditMode ? i18n.t('role-updated') : i18n.t('role-created')}${i18n.t('role-success-suffix')}`, i18n.t('role-mgmt-title'));
                        if (modal) modal.style.display = 'none';
                        this.loadRoles();
                    } else {
                        showErr((res && res.error) || i18n.t('role-op-fail'));
                    }
                })
                .catch(err => {
                    showErr(err.message || i18n.t('role-op-fail'));
                })
                .finally(() => {
                    if (btn) { btn.disabled = false; btn.textContent = i18n.t('role-save-text'); }
                });
        },

        deleteRole(roleId, roleName) {
            if (!confirm(`${i18n.t('confirm-delete-role-msg')} "${roleName || roleId}" ${i18n.t('confirm-delete-role-suffix')}`)) return;

            apiPost('/api/roles/delete', { id: roleId })
                .then(res => {
                    if (res && res.success) {
                        Notification.success(`${roleName || roleId} ${i18n.t('role-deleted-msg')}`, i18n.t('delete-success'));
                        this.loadRoles();
                    } else {
                        Notification.error((res && res.error) || i18n.t('operation-fail'), i18n.t('operation-fail'));
                    }
                })
                .catch(() => {});
        }
    });

    // 鑷姩缁戝畾浜嬩欢
    if (typeof AppState.setupRolesEvents === 'function') {
        AppState.setupRolesEvents();
    }
})();

/**
 * 鏃ュ織绠＄悊妯″潡
 * 鍖呭惈鏃ュ織鏂囦欢鍒楄〃銆佹棩蹇楀唴瀹瑰姞杞姐€佽嚜鍔ㄥ埛鏂般€佹竻绌?
 */
(function() {
    AppState.registerModule('logs', {

        // ============ 浜嬩欢缁戝畾 ============
        setupLogsEvents() {
            // 鍒锋柊鏃ュ織鏂囦欢鍒楄〃鎸夐挳
            const refreshLogListBtn = document.getElementById('log-refresh-list-btn');
            if (refreshLogListBtn) refreshLogListBtn.addEventListener('click', () => this.loadLogFileList());

            // 鍒锋柊鏃ュ織鎸夐挳
            const refreshLogsBtn = document.getElementById('refresh-logs-btn');
            if (refreshLogsBtn) refreshLogsBtn.addEventListener('click', () => this.loadLogs());

            // 娓呯┖鏃ュ織鎸夐挳
            const clearLogsBtn = document.getElementById('clear-logs-btn');
            if (clearLogsBtn) clearLogsBtn.addEventListener('click', () => this.clearLogs());

            // 鑷姩鍒锋柊澶嶉€夋
            const autoRefreshCheckbox = document.getElementById('log-auto-refresh');
            if (autoRefreshCheckbox) {
                autoRefreshCheckbox.addEventListener('change', (e) => {
                    if (e.target.checked) {
                        this.startLogAutoRefresh();
                    } else {
                        this.stopLogAutoRefresh();
                    }
                });
            }
        },

        // ============ 鏃ュ織绠＄悊 ============

        /**
         * 鍔犺浇鏃ュ織鏂囦欢鍒楄〃
         */
        loadLogFileList() {
            const listContainer = document.getElementById('log-file-list');
            if (!listContainer) return;

            apiGet('/api/logs/list')
                .then(res => {
                    if (!res || !res.success) {
                        listContainer.innerHTML = '<div style="color: #f56c6c; padding: 10px;">' + i18n.t('log-load-fail-html') + '</div>';
                        return;
                    }

                    const files = res.data || [];
                    if (files.length === 0) {
                        listContainer.innerHTML = '<div style="color: #666; padding: 10px;">' + i18n.t('log-empty-html') + '</div>';
                        return;
                    }

                    // 鎸夋枃浠跺悕鎺掑簭锛宻ystem.log 鎺掓渶鍓嶉潰
                    files.sort((a, b) => {
                        if (a.name === 'system.log') return -1;
                        if (b.name === 'system.log') return 1;
                        return b.name.localeCompare(a.name); // 鏂版枃浠跺湪鍓?
                    });

                    let html = '';
                    files.forEach(file => {
                        const sizeStr = file.size < 1024 ? `${file.size} B` : `${(file.size / 1024).toFixed(1)} KB`;
                        const isActive = file.name === this._currentLogFile;
                        const activeStyle = isActive ? 'background: #3a3a3a; color: #fff;' : 'color: #ccc;';
                        const icon = file.current ? '馃搫' : '馃搩';
                        html += `<div class="log-file-item" style="padding: 8px 10px; cursor: pointer; border-bottom: 1px solid #333; ${activeStyle}" data-file="${file.name}">
                            <span style="margin-right: 5px;">${icon}</span>
                            <span style="font-size: 12px;">${file.name}</span>
                            <span style="font-size: 11px; color: #888; float: right;">${sizeStr}</span>
                        </div>`;
                    });

                    listContainer.innerHTML = html;

                    // 缁戝畾鐐瑰嚮浜嬩欢
                    listContainer.querySelectorAll('.log-file-item').forEach(item => {
                        item.addEventListener('click', () => {
                            const fileName = item.dataset.file;
                            this._currentLogFile = fileName;
                            // 鏇存柊褰撳墠鏂囦欢鏄剧ず
                            const currentSpan = document.getElementById('current-log-file');
                            if (currentSpan) currentSpan.textContent = i18n.t('log-current-file-prefix') + fileName;
                            // 鏇存柊閫変腑鐘舵€?
                            listContainer.querySelectorAll('.log-file-item').forEach(i => {
                                i.style.background = '';
                                i.style.color = '#ccc';
                            });
                            item.style.background = '#3a3a3a';
                            item.style.color = '#fff';
                            // 鍔犺浇鏃ュ織鍐呭
                            this.loadLogs(500, fileName);
                        });

                        // 榧犳爣鎮仠鏁堟灉
                        item.addEventListener('mouseenter', () => {
                            if (item.style.background !== '#3a3a3a') {
                                item.style.background = '#2a2a2a';
                            }
                        });
                        item.addEventListener('mouseleave', () => {
                            if (item.style.background === '#2a2a2a') {
                                item.style.background = '';
                            }
                        });
                    });
                })
                .catch(err => {
                    console.error('Load log file list failed:', err);
                    listContainer.innerHTML = '<div style="color: #f56c6c; padding: 10px;">' + i18n.t('log-load-fail-html') + '</div>';
                });
        },

        /**
         * 鍔犺浇鏃ュ織鍐呭
         * @param {number} maxLines 鏈€澶ц鏁帮紝榛樿500
         * @param {string} fileName 鏃ュ織鏂囦欢鍚嶏紝榛樿涓哄綋鍓嶉€変腑鐨勬枃浠?
         */
        loadLogs(maxLines = 500, fileName = null) {
            const container = document.getElementById('device-log-container');
            const infoSpan = document.getElementById('log-info');

            if (!container) return;

            // 浣跨敤浼犲叆鐨勬枃浠跺悕鎴栧綋鍓嶉€変腑鐨勬枃浠?
            const logFile = fileName || this._currentLogFile || 'system.log';
            this._currentLogFile = logFile;

            apiGet('/api/logs', { lines: maxLines, file: logFile })
                .then(res => {
                    if (!res || !res.success) {
                        container.innerHTML = i18n.t('log-load-fail-html');
                        return;
                    }

                    const data = res.data || {};
                    const content = data.content || '';
                    const fileSize = data.size || 0;
                    const lineCount = data.lines || 0;
                    const truncated = data.truncated || false;

                    // 鏇存柊鏃ュ織淇℃伅
                    if (infoSpan) {
                        const sizeStr = fileSize < 1024 ? `${fileSize} B` :
                                       fileSize < 1024 * 1024 ? `${(fileSize / 1024).toFixed(1)} KB` :
                                       `${(fileSize / 1024 / 1024).toFixed(2)} MB`;
                        let infoText = `${lineCount}${i18n.t('log-line-unit')}${sizeStr}`;
                        if (truncated) infoText += i18n.t('log-truncated-suffix');
                        infoSpan.textContent = infoText;
                    }

                    // 鏍煎紡鍖栧苟鏄剧ず鏃ュ織鍐呭
                    if (!content || content.trim() === '') {
                        container.innerHTML = i18n.t('log-empty-html');
                    } else {
                        container.innerHTML = this._formatLogContent(content);
                        // 婊氬姩鍒板簳閮?
                        container.scrollTop = container.scrollHeight;
                    }
                })
                .catch(err => {
                    console.error('Load logs failed:', err);
                    container.innerHTML = i18n.t('log-load-fail-html');
                });
        },

        /**
         * 鏍煎紡鍖栨棩蹇楀唴瀹癸紝娣诲姞棰滆壊楂樹寒
         */
        _formatLogContent(content) {
            const lines = content.split('\n');
            return lines.map(line => {
                // 璺宠繃绌鸿
                if (!line.trim()) return '';

                // 鏍规嵁鏃ュ織绾у埆璁剧疆棰滆壊
                let className = '';
                if (line.includes('[ERROR]') || line.includes('[E]')) {
                    className = 'log-error';
                } else if (line.includes('[WARN]') || line.includes('[W]')) {
                    className = 'log-warn';
                } else if (line.includes('[INFO]') || line.includes('[I]')) {
                    className = 'log-info';
                } else if (line.includes('[DEBUG]') || line.includes('[D]')) {
                    className = 'log-debug';
                }

                // HTML 杞箟
                const escaped = line
                    .replace(/&/g, '&amp;')
                    .replace(/</g, '&lt;')
                    .replace(/>/g, '&gt;');

                return `<div class="${className}">${escaped}</div>`;
            }).filter(line => line).join('');
        },

        /**
         * 娓呯┖鏃ュ織
         */
        clearLogs() {
            if (!confirm(i18n.t('log-clear-confirm-msg'))) return;

            const btn = document.getElementById('clear-logs-btn');
            if (btn) {
                btn.disabled = true;
                btn.innerHTML = i18n.t('log-clearing-html');
            }

            apiPost('/api/logs/clear', {})
                .then(res => {
                    if (res && res.success) {
                        Notification.success(i18n.t('log-cleared-msg'), i18n.t('log-op-title'));
                        this.loadLogs();  // 閲嶆柊鍔犺浇
                    } else {
                        Notification.error((res && res.error) || i18n.t('log-op-fail'), i18n.t('log-op-fail'));
                    }
                })
                .catch(err => {
                    console.error('Clear logs failed:', err);
                    Notification.error(i18n.t('log-op-fail'), i18n.t('log-op-fail'));
                })
                .finally(() => {
                    if (btn) {
                        btn.disabled = false;
                        btn.innerHTML = i18n.t('log-clear-btn-html');
                    }
                });
        },

        /**
         * 鍚姩鏃ュ織鑷姩鍒锋柊
         */
        startLogAutoRefresh(interval = 2000) {
            // 鍏堝仠姝㈠凡鏈夌殑瀹氭椂鍣?
            this.stopLogAutoRefresh();

            // 鍚姩鏂扮殑瀹氭椂鍣?
            this._logAutoRefreshTimer = setInterval(() => {
                // 浠呭湪鏃ュ織椤甸潰婵€娲绘椂鍒锋柊
                if (this.currentPage === 'logs') {
                    this.loadLogs();
                }
            }, interval);

            console.log('[Logs] Auto refresh started with interval:', interval);
        },

        /**
         * 鍋滄鏃ュ織鑷姩鍒锋柊
         */
        stopLogAutoRefresh() {
            if (this._logAutoRefreshTimer) {
                clearInterval(this._logAutoRefreshTimer);
                this._logAutoRefreshTimer = null;
                console.log('[Logs] Auto refresh stopped');
            }
        }
    });

    // 鑷姩缁戝畾浜嬩欢
    if (typeof AppState.setupLogsEvents === 'function') {
        AppState.setupLogsEvents();
    }
})();

/**
 * 鏂囦欢绠＄悊妯″潡
 * 鍖呭惈鏂囦欢绯荤粺淇℃伅銆佹枃浠舵爲銆佹枃浠舵墦寮€/淇濆瓨/鍏抽棴
 */
(function() {
    AppState.registerModule('files', {

        // ============ 浜嬩欢缁戝畾 ============
        setupFilesEvents() {
            // 鍒锋柊鏂囦欢鍒楄〃鎸夐挳
            const fsRefreshBtn = document.getElementById('fs-refresh-btn');
            if (fsRefreshBtn) fsRefreshBtn.addEventListener('click', () => this.loadFileTree(this._currentDir || '/'));

            // 杩斿洖涓婄骇鎸夐挳
            const fsUpBtn = document.getElementById('fs-up-btn');
            if (fsUpBtn) fsUpBtn.addEventListener('click', () => this.navigateUp());

            // 淇濆瓨鏂囦欢鎸夐挳
            const fsSaveBtn = document.getElementById('fs-save-btn');
            if (fsSaveBtn) fsSaveBtn.addEventListener('click', () => this.saveCurrentFile());

            // 鍏抽棴鏂囦欢鎸夐挳
            const fsCloseBtn = document.getElementById('fs-close-btn');
            if (fsCloseBtn) fsCloseBtn.addEventListener('click', () => this.closeCurrentFile());
        },

        // ============ 鏂囦欢绠＄悊 ============

        /**
         * 鍔犺浇鏂囦欢绯荤粺淇℃伅
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
         * 鍔犺浇鏂囦欢鏍?
         */
        loadFileTree(path) {
            const treeContainer = document.getElementById('file-tree');
            if (!treeContainer) return;

            // 璁板綍褰撳墠璺緞
            this._currentDir = path;

            // 鏇存柊璺緞鏄剧ず
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

                    // 鐩綍
                    dirs.forEach(dir => {
                        html += `<div class="file-tree-item" style="padding: 3px 0; cursor: pointer;" data-path="${path}${dir.name}/" data-type="dir">
                            <span style="color: #e6a23c;">馃搧</span> ${dir.name}
                        </div>`;
                    });

                    // 鏂囦欢
                    files.forEach(file => {
                        const size = file.size < 1024 ? `${file.size} B` :
                                    file.size < 1024 * 1024 ? `${(file.size / 1024).toFixed(1)} KB` :
                                    `${(file.size / 1024 / 1024).toFixed(2)} MB`;
                        html += `<div class="file-tree-item" style="padding: 3px 0; cursor: pointer;" data-path="${path}${file.name}" data-type="file">
                            <span style="color: #409eff;">馃搫</span> ${file.name} <span style="color: #999; font-size: 11px;">(${size})</span>
                        </div>`;
                    });

                    if (dirs.length === 0 && files.length === 0) {
                        html = i18n.t('fs-empty-dir-html');
                    }

                    treeContainer.innerHTML = html;

                    // 缁戝畾鐐瑰嚮浜嬩欢
                    treeContainer.querySelectorAll('.file-tree-item').forEach(item => {
                        item.addEventListener('click', (e) => {
                            const path = e.currentTarget.dataset.path;
                            const type = e.currentTarget.dataset.type;

                            // 绉婚櫎鍏朵粬閫変腑鐘舵€?
                            treeContainer.querySelectorAll('.file-tree-item').forEach(i => {
                                i.style.background = '';
                            });
                            e.currentTarget.style.background = '#e6f7ff';

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
         * 鎵撳紑鏂囦欢
         */
        openFile(path) {
            const editor = document.getElementById('file-editor');
            const pathSpan = document.getElementById('current-file-path');
            const saveBtn = document.getElementById('fs-save-btn');
            const closeBtn = document.getElementById('fs-close-btn');
            const statusDiv = document.getElementById('file-status');

            if (!editor || !pathSpan) return;

            // 妫€鏌ユ槸鍚﹀彲缂栬緫
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
                    closeBtn.disabled = false;

                    if (!AppState.currentUser.canManageFs) {
                        // 鏃?fs.manage 鏉冮檺锛氫繚瀛樻寜閽案涔呯鐢紝缂栬緫鍣ㄥ彧璇?
                        saveBtn.disabled = true;
                        editor.readOnly = true;
                        editor.title = i18n.t('fs-no-perm-tip');
                        editor.oninput = null;
                    } else {
                        // 鏈夋潈闄愶細鎵撳紑鏂囦欢鍚庝繚瀛樻寜閽繚鎸佺鐢紝闇€缂栬緫鍚庢墠鍙繚瀛?
                        saveBtn.disabled = true;
                        editor.readOnly = false;
                        editor.title = '';
                        // 鐩戝惉缂栬緫锛屾湁鏀瑰姩鍚庢墠鍚敤淇濆瓨
                        editor.oninput = () => {
                            saveBtn.disabled = false;
                            this._currentFileModified = true;
                        };
                    }

                    const size = data.size < 1024 ? `${data.size} B` :
                                data.size < 1024 * 1024 ? `${(data.size / 1024).toFixed(1)} KB` :
                                `${(data.size / 1024 / 1024).toFixed(2)} MB`;
                    statusDiv.textContent = `${i18n.t('fs-file-ready-prefix')}${size}${i18n.t('fs-file-ready-suffix')}`;

                    this._currentFilePath = path;
                    this._currentFileModified = false;
                })
                .catch(err => {
                    console.error('Open file failed:', err);
                    statusDiv.textContent = i18n.t('fs-file-load-fail');
                });
        },

        /**
         * 杩斿洖涓婄骇鐩綍
         */
        navigateUp() {
            const currentPath = this._currentDir || '/';

            // 濡傛灉褰撳墠鏄牴鐩綍锛屼笉鍋氫换浣曟搷浣?
            if (currentPath === '/') return;

            // 绉婚櫎鏈熬鐨勬枩鏉?
            let path = currentPath;
            if (path.endsWith('/')) path = path.slice(0, -1);

            // 鎵惧埌鏈€鍚庝竴涓枩鏉犵殑浣嶇疆
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

        /**
         * 淇濆瓨褰撳墠鏂囦欢
         */
        saveCurrentFile() {
            if (!this._currentFilePath) return;

            // 鏉冮檺浜屾鏍￠獙
            if (!AppState.currentUser.canManageFs) {
                Notification.warning(i18n.t('fs-no-perm-tip'), i18n.t('fs-mgmt-title'));
                return;
            }

            const editor = document.getElementById('file-editor');
            const statusDiv = document.getElementById('file-status');
            const saveBtn = document.getElementById('fs-save-btn');

            if (!editor || !statusDiv) return;

            const content = editor.value;
            statusDiv.textContent = i18n.t('fs-saving-text');
            saveBtn.disabled = true;

            apiPost('/api/files/save', { path: this._currentFilePath, content: content })
                .then(res => {
                    if (res && res.success) {
                        statusDiv.textContent = i18n.t('fs-save-ok-text');
                        this._currentFileModified = false;
                        Notification.success(i18n.t('fs-save-ok-msg'), i18n.t('fs-mgmt-title'));
                    } else {
                        statusDiv.textContent = i18n.t('fs-save-fail-prefix') + (res.error || '');
                        Notification.error(res.error || i18n.t('fs-save-fail-text'), i18n.t('fs-mgmt-title'));
                    }
                })
                .catch(err => {
                    console.error('Save file failed:', err);
                    statusDiv.textContent = i18n.t('fs-save-fail-text');
                    Notification.error(i18n.t('fs-save-fail-text'), i18n.t('fs-mgmt-title'));
                })
                .finally(() => {
                    saveBtn.disabled = false;
                });
        },

        /**
         * 鍏抽棴褰撳墠鏂囦欢
         */
        closeCurrentFile() {
            const editor = document.getElementById('file-editor');
            const pathSpan = document.getElementById('current-file-path');
            const saveBtn = document.getElementById('fs-save-btn');
            const closeBtn = document.getElementById('fs-close-btn');
            const statusDiv = document.getElementById('file-status');

            if (this._currentFileModified) {
                if (!confirm(i18n.t('fs-modified-confirm'))) {
                    return;
                }
                this.saveCurrentFile();
            }

            if (editor) {
                editor.value = '';
                editor.disabled = true;
                editor.readOnly = false;
                editor.title = '';
                editor.oninput = null;  // 娓呴櫎缂栬緫鐩戝惉
            }
            if (pathSpan) pathSpan.textContent = i18n.t('fs-select-file-text');
            if (saveBtn) saveBtn.disabled = true;
            if (closeBtn) closeBtn.disabled = true;
            if (statusDiv) statusDiv.textContent = '';

            this._currentFilePath = null;
            this._currentFileModified = false;

            // 鍙栨秷鏂囦欢鏍戜腑鐨勯€変腑鐘舵€?
            const treeContainer = document.getElementById('file-tree');
            if (treeContainer) {
                treeContainer.querySelectorAll('.file-tree-item').forEach(i => {
                    i.style.background = '';
                });
            }
        }
    });

    // 鑷姩缁戝畾浜嬩欢
    if (typeof AppState.setupFilesEvents === 'function') {
        AppState.setupFilesEvents();
    }
})();

/**
 * 瑙勫垯鑴氭湰妯″潡
 * 鍖呭惈瑙勫垯鑴氭湰鐨?CRUD 鎿嶄綔
 */
(function() {
    AppState.registerModule('rule-script', {

        // ============ 浜嬩欢缁戝畾 ============
        setupRuleScriptEvents() {
            const closeRuleScriptModal = document.getElementById('close-rule-script-modal');
            if (closeRuleScriptModal) closeRuleScriptModal.addEventListener('click', () => this.closeRuleScriptModal());
            const cancelRuleScriptBtn = document.getElementById('cancel-rule-script-btn');
            if (cancelRuleScriptBtn) cancelRuleScriptBtn.addEventListener('click', () => this.closeRuleScriptModal());
            const saveRuleScriptBtn = document.getElementById('save-rule-script-btn');
            if (saveRuleScriptBtn) saveRuleScriptBtn.addEventListener('click', () => this.saveRuleScript());
        },

        // ============ 瑙勫垯鑴氭湰椤甸潰 ============

        loadRuleScriptPage() {
            const tbody = document.getElementById('rule-script-table-body');
            if (!tbody) return;
            apiGet('/api/rule-script').then(res => {
                if (!res || !res.success || !res.data) {
                    tbody.innerHTML = '<tr><td colspan="6" style="text-align:center;color:#999;">' + i18n.t('rule-script-no-data') + '</td></tr>';
                    return;
                }
                const rules = res.data;
                if (rules.length === 0) {
                    tbody.innerHTML = '<tr><td colspan="6" style="text-align:center;color:#999;">' + i18n.t('rule-script-no-data') + '</td></tr>';
                    return;
                }
                const triggerLabels = { 0: i18n.t('rule-script-trigger-receive'), 1: i18n.t('rule-script-trigger-report') };
                const protocolLabels = { 0: 'MQTT', 1: 'Modbus RTU', 2: 'Modbus TCP', 3: 'HTTP', 4: 'CoAP', 5: 'TCP' };
                let html = '';
                rules.forEach(r => {
                    const statusBadge = r.enabled
                        ? '<span class="badge badge-success">' + i18n.t('periph-exec-status-on') + '</span>'
                        : '<span class="badge badge-info">' + i18n.t('periph-exec-status-off') + '</span>';
                    const triggerText = triggerLabels[r.triggerType] || '?';
                    const protocolText = protocolLabels[r.protocolType] || '-';
                    const statsText = i18n.t('periph-exec-stats-count') + ': ' + (r.triggerCount || 0);
                    html += '<tr>';
                    html += '<td>' + (r.name || '') + '</td>';
                    html += '<td>' + statusBadge + '</td>';
                    html += '<td>' + triggerText + '</td>';
                    html += '<td>' + protocolText + '</td>';
                    html += '<td style="font-size:12px;">' + statsText + '</td>';
                    html += '<td class="u-toolbar-sm">';
                    html += '<button class="btn btn-sm btn-edit" onclick="app.editRuleScript(\'' + r.id + '\')">' + i18n.t('peripheral-edit') + '</button>';
                    if (r.enabled) {
                        html += '<button class="btn btn-sm btn-disable" onclick="app.toggleRuleScript(\'' + r.id + '\',false)">' + i18n.t('peripheral-disable') + '</button>';
                    } else {
                        html += '<button class="btn btn-sm btn-enable" onclick="app.toggleRuleScript(\'' + r.id + '\',true)">' + i18n.t('peripheral-enable') + '</button>';
                    }
                    html += '<button class="btn btn-sm btn-delete" onclick="app.deleteRuleScript(\'' + r.id + '\')">' + i18n.t('peripheral-delete') + '</button>';
                    html += '</td>';
                    html += '</tr>';
                });
                tbody.innerHTML = html;
            }).catch(() => {
                tbody.innerHTML = '<tr><td colspan="6" style="text-align:center;color:#999;">' + i18n.t('rule-script-no-data') + '</td></tr>';
            });
        },

        openRuleScriptModal(editId) {
            const modal = document.getElementById('rule-script-modal');
            if (!modal) return;
            const titleEl = document.getElementById('rule-script-modal-title');
            document.getElementById('rule-script-original-id').value = editId || '';
            document.getElementById('rule-script-error').style.display = 'none';
            if (editId) {
                if (titleEl) titleEl.textContent = i18n.t('rule-script-edit-title');
            } else {
                if (titleEl) titleEl.textContent = i18n.t('rule-script-add-title');
                document.getElementById('rule-script-form').reset();
                document.getElementById('rule-script-protocol-type').value = '0';
                document.getElementById('rule-script-content').value = '';
            }
            modal.style.display = 'flex';
        },

        closeRuleScriptModal() {
            const modal = document.getElementById('rule-script-modal');
            if (modal) modal.style.display = 'none';
        },

        saveRuleScript() {
            const errEl = document.getElementById('rule-script-error');
            errEl.style.display = 'none';
            const originalId = document.getElementById('rule-script-original-id').value;
            const isEdit = originalId !== '';
            const ruleData = {
                name: document.getElementById('rule-script-name').value.trim(),
                enabled: document.getElementById('rule-script-enabled').checked ? '1' : '0',
                triggerType: document.getElementById('rule-script-trigger-type').value,
                protocolType: document.getElementById('rule-script-protocol-type').value,
                scriptContent: document.getElementById('rule-script-content').value
            };
            if (!ruleData.name) {
                errEl.textContent = i18n.t('periph-exec-validate-name');
                errEl.style.display = 'block';
                return;
            }
            if (isEdit) ruleData.id = originalId;
            const url = isEdit ? '/api/rule-script/update' : '/api/rule-script';
            apiPost(url, ruleData).then(res => {
                if (res && res.success) {
                    Notification.success(i18n.t(isEdit ? 'rule-script-update-ok' : 'rule-script-add-ok'), i18n.t('rule-script-title'));
                    this.closeRuleScriptModal();
                    if (this.currentPage === 'rule-script') this.loadRuleScriptPage();
                } else {
                    errEl.textContent = res?.error || i18n.t('rule-script-save-fail');
                    errEl.style.display = 'block';
                }
            }).catch(() => {
                errEl.textContent = i18n.t('rule-script-save-fail');
                errEl.style.display = 'block';
            });
        },

        editRuleScript(id) {
            this.openRuleScriptModal(id);
            apiGet('/api/rule-script').then(res => {
                if (!res || !res.success || !res.data) return;
                const rule = res.data.find(r => r.id === id);
                if (!rule) return;
                document.getElementById('rule-script-name').value = rule.name || '';
                document.getElementById('rule-script-enabled').checked = !!rule.enabled;
                document.getElementById('rule-script-trigger-type').value = String(rule.triggerType);
                document.getElementById('rule-script-protocol-type').value = String(rule.protocolType || 0);
                document.getElementById('rule-script-content').value = rule.scriptContent || '';
            });
        },

        toggleRuleScript(id, enable) {
            const url = enable ? '/api/rule-script/enable' : '/api/rule-script/disable';
            apiPost(url, { id: id }).then(res => {
                if (res && res.success) {
                    if (this.currentPage === 'rule-script') this.loadRuleScriptPage();
                }
            });
        },

        deleteRuleScript(id) {
            if (!confirm(i18n.t('periph-exec-confirm-delete'))) return;
            apiDelete('/api/rule-script/', { id: id }).then(res => {
                if (res && res.success) {
                    Notification.success(i18n.t('rule-script-delete-ok'), i18n.t('rule-script-title'));
                    if (this.currentPage === 'rule-script') this.loadRuleScriptPage();
                }
            });
        }
    });

    // 鑷姩缁戝畾浜嬩欢
    if (typeof AppState.setupRuleScriptEvents === 'function') {
        AppState.setupRuleScriptEvents();
    }
})();

