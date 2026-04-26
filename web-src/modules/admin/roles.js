/**
 * 角色管理模块
 * 包含角色列表、权限管理、角色 CRUD
 */
(function() {
    AppState.registerModule('roles', {

        // ============ 事件绑定 ============
        setupRolesEvents() {
            const closeId = (id) => { this.hideModal(id); };

            // 添加角色按钮
            const addRoleBtn = document.getElementById('add-role-btn');
            if (addRoleBtn) addRoleBtn.addEventListener('click', () => this.showAddRoleModal());

            // 角色模态窗关闭
            ['close-role-modal', 'cancel-role-btn'].forEach(id => {
                const el = document.getElementById(id);
                if (el) el.addEventListener('click', () => closeId('role-modal'));
            });

            // 确认保存角色
            const confirmRole = document.getElementById('confirm-role-btn');
            if (confirmRole) confirmRole.addEventListener('click', () => this.saveRole());
            // 刷新按钮
            const rolesRefreshBtn = document.getElementById('roles-refresh-btn');
            if (rolesRefreshBtn) rolesRefreshBtn.addEventListener('click', () => this._refreshRolesList());
        },

        // ============ 角色管理 ============

        _refreshRolesList() {
            var btn = document.getElementById('roles-refresh-btn');
            if (btn && btn.disabled) return;
            if (btn) { btn.disabled = true; btn.innerHTML = '<span class="fb-spin">&#x21bb;</span> 加载中...'; }
            if (typeof window.apiInvalidateCache === 'function') {
                window.apiInvalidateCache('/api/roles');
            }
            this.loadRoles();
            setTimeout(function() {
                if (btn) { btn.disabled = false; btn.innerHTML = '&#x21bb; 刷新'; }
            }, 2000);
        },

        loadRoles() {
            apiGet('/api/roles')
                .then(res => {
                    if (!res || !res.success) return;
                    const roles = (res.data && res.data.roles) ? res.data.roles : [];
                    const permissions = (res.data && res.data.permissions) ? res.data.permissions : [];
                    this._permDefs = permissions;  // 保存权限定义
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

                // 角色ID
                const tdId = document.createElement('td');
                tdId.textContent = role.id;
                row.appendChild(tdId);

                // 角色名称 - 翻译内置角色名
                const tdName = document.createElement('td');
                const _rnm = {'管理员': 'role-admin', '操作员': 'role-operator', '查看者': 'role-viewer'};
                const dName = _rnm[role.name] ? i18n.t(_rnm[role.name]) : role.name;
                tdName.textContent = dName;
                row.appendChild(tdName);

                // 描述
                const tdDesc = document.createElement('td');
                tdDesc.textContent = role.description || '—';
                tdDesc.className = 'role-desc-cell';
                row.appendChild(tdDesc);

                // 权限数
                const tdPermCount = document.createElement('td');
                const permCount = (role.permissions || []).length;
                var pillSpan = document.createElement('span');
                pillSpan.className = 'role-pill role-pill--count';
                pillSpan.textContent = permCount;
                tdPermCount.appendChild(pillSpan);
                row.appendChild(tdPermCount);

                // 类型
                const tdType = document.createElement('td');
                var typeSpan = document.createElement('span');
                if (role.id === 'admin') {
                    typeSpan.className = 'role-pill role-pill--super';
                    typeSpan.textContent = i18n.t('role-type-super');
                } else if (role.isBuiltin) {
                    typeSpan.className = 'role-pill role-pill--builtin';
                    typeSpan.textContent = i18n.t('role-type-builtin');
                } else {
                    typeSpan.className = 'role-pill role-pill--custom';
                    typeSpan.textContent = i18n.t('role-type-custom');
                }
                tdType.appendChild(typeSpan);
                row.appendChild(tdType);

                // 操作
                const tdAction = document.createElement('td');
                tdAction.className = 'u-toolbar-sm';

                // 查看权限按钮
                const viewBtn = document.createElement('button');
                viewBtn.className = 'fb-btn fb-btn-sm fb-btn-secondary';
                viewBtn.textContent = i18n.t('role-view-perms');
                viewBtn.addEventListener('click', () => this.showRolePermissions(role));
                tdAction.appendChild(viewBtn);

                // 仅 admin 角色不可编辑/删除
                if (role.id !== 'admin') {
                    // 编辑按钮
                    const editBtn = document.createElement('button');
                    editBtn.className = 'fb-btn fb-btn-sm fb-btn-primary fb-mr-1';
                    editBtn.textContent = i18n.t('role-edit');
                    editBtn.addEventListener('click', () => this.showEditRoleModal(role.id));
                    tdAction.appendChild(editBtn);

                    // 删除按钮
                    const delBtn = document.createElement('button');
                    delBtn.className = 'fb-btn fb-btn-sm fb-btn-danger';
                    delBtn.textContent = i18n.t('role-delete');
                    delBtn.addEventListener('click', () => {
                        const _rnm2 = {'管理员': 'role-admin', '操作员': 'role-operator', '查看者': 'role-viewer'};
                        const dName2 = _rnm2[role.name] ? i18n.t(_rnm2[role.name]) : role.name;
                        this.deleteRole(role.id, dName2);
                    });
                    tdAction.appendChild(delBtn);
                }

                row.appendChild(tdAction);
                tbody.appendChild(row);
            });
        },

        // 查看角色权限详情
        showRolePermissions(role) {
            const permDefs = this._permDefs || [];
            const rolePerms = new Set(role.permissions || []);

            // 权限组名中文→英文映射key（同时支持中英文）
            const _gpk = {
                '设备':'device', 'Device':'device',
                '网络':'network', 'Network':'network',
                '系统':'system', 'System':'system',
                '用户':'user', 'Users':'user',
                '文件':'file', 'Files':'file',
                '协议':'protocol', 'Protocol':'protocol',
                '审计':'audit', 'Audit':'audit',
                'GPIO':'gpio',
                '外设':'peripheral', 'Peripheral':'peripheral'
            };

            // 按分组整理
            const permGroups = {};
            permDefs.forEach(p => {
                if (!permGroups[p.group]) permGroups[p.group] = [];
                permGroups[p.group].push(p);
            });

            let html = `<div class="role-perm-sheet">`;
            Object.keys(permGroups).sort().forEach(group => {
                const gKey = _gpk[group] ? i18n.t('perm-group-' + _gpk[group]) : group;
                html += `<div class="role-perm-group">`;
                html += `<h4 class="role-perm-group-title">${gKey}</h4>`;
                html += `<div class="role-perm-chip-list">`;
                permGroups[group].forEach(perm => {
                    const hasPerm = rolePerms.has(perm.id);
                    const pName = i18n.t('perm-' + perm.id) !== ('perm-' + perm.id) ? i18n.t('perm-' + perm.id) : perm.name;
                    const pDesc = i18n.t('perm-' + perm.id) !== ('perm-' + perm.id) ? i18n.t('perm-' + perm.id) : perm.description;
                    html += `<span class="role-perm-chip${hasPerm ? ' is-enabled' : ''}">`;
                    html += `<span class="role-perm-chip-indicator">${hasPerm ? '✓' : '✗'}</span>`;
                    html += `<span title="${escapeHtml(pDesc)}">${escapeHtml(pName)}</span></span>`;
                });
                html += `</div></div>`;
            });
            html += `</div>`;

            // 翻译角色名
            const roleNameMap = {'管理员': 'role-admin', '操作员': 'role-operator', '查看者': 'role-viewer'};
            const displayRoleName = roleNameMap[role.name] ? i18n.t(roleNameMap[role.name]) : role.name;

            const overlay = document.createElement('div');
            overlay.className = 'role-perm-overlay';
            overlay.innerHTML = `
                <div class="role-perm-dialog">
                    <div class="role-perm-dialog-header">
                        <h3 class="role-perm-dialog-title">${escapeHtml(displayRoleName)}${escapeHtml(i18n.t('role-detail-suffix'))}</h3>
                        <button type="button" class="role-perm-dialog-close">×</button>
                    </div>
                    <div class="role-perm-dialog-body">${html}</div>
                </div>
            `;
            overlay.addEventListener('click', (e) => {
                if (e.target === overlay) overlay.remove();
            });
            const closeBtn = overlay.querySelector('.role-perm-dialog-close');
            if (closeBtn) {
                closeBtn.addEventListener('click', () => overlay.remove());
            }
            const dialog = overlay.querySelector('.role-perm-dialog');
            if (dialog) {
                dialog.addEventListener('click', (e) => e.stopPropagation());
            }
            document.body.appendChild(overlay);
        },

        // ============ 角色管理 CRUD ============

        showAddRoleModal() {
            const modal = document.getElementById('role-modal');
            if (!modal) return;

            // 设置标题
            const title = document.getElementById('role-modal-title');
            if (title) title.textContent = i18n.t('role-add-title');

            // 清空输入
            const idInput = document.getElementById('role-id-input');
            const nameInput = document.getElementById('role-name-input');
            const descInput = document.getElementById('role-desc-input');
            if (idInput) { idInput.value = ''; idInput.disabled = false; }
            if (nameInput) nameInput.value = '';
            if (descInput) descInput.value = '';

            // 标记为新增模式
            modal.dataset.editMode = 'add';
            modal.dataset.editRoleId = '';

            // 渲染权限复选框
            this._renderPermissionCheckboxes([]);
        
            // 清除错误提示
            AppState.clearInlineError('role-error');
        
            AppState.showModal(modal);
        },
        
        showEditRoleModal(roleId) {
            const modal = document.getElementById('role-modal');
            if (!modal) {
                console.error('[showEditRoleModal] modal not found!');
                return;
            }


            // 先设置编辑模式，防止API回调前用户点击保存导致调用错误的API
            modal.dataset.editMode = 'edit';
            modal.dataset.editRoleId = roleId;

            // 获取角色信息
            apiGet('/api/roles')
                .then(res => {
                    if (!res || !res.success) return;
                    const roles = (res.data && res.data.roles) ? res.data.roles : [];
                    const role = roles.find(r => r.id === roleId);
                    if (!role) {
                        Notification.error(i18n.t('role-not-exist'), i18n.t('error-title'));
                        return;
                    }

                    // 设置标题
                    const title = document.getElementById('role-modal-title');
                    if (title) title.textContent = i18n.t('role-edit-title');

                    // 填充数据
                    const idInput = document.getElementById('role-id-input');
                    const nameInput = document.getElementById('role-name-input');
                    const descInput = document.getElementById('role-desc-input');
                    if (idInput) { idInput.value = role.id; idInput.disabled = true; }
                    if (nameInput) nameInput.value = role.name;
                    if (descInput) descInput.value = role.description || '';


                    // 渲染权限复选框
                    this._renderPermissionCheckboxes(role.permissions || []);
                
                    // 清除错误提示
                    AppState.clearInlineError('role-error');
                
                    AppState.showModal(modal);
                })
                .catch(() => {});
        },

        _renderPermissionCheckboxes(selectedPerms) {
            const container = document.getElementById('role-permissions-container');
            if (!container) return;
            container.innerHTML = '';

            const permDefs = this._permDefs || [];
            const selectedSet = new Set(selectedPerms);

            // 权限组名中文→英文映射key（同时支持中英文）
            const _gpk = {
                '设备':'device', 'Device':'device',
                '网络':'network', 'Network':'network',
                '系统':'system', 'System':'system',
                '用户':'user', 'Users':'user',
                '文件':'file', 'Files':'file',
                '协议':'protocol', 'Protocol':'protocol',
                '审计':'audit', 'Audit':'audit',
                'GPIO':'gpio',
                '外设':'peripheral', 'Peripheral':'peripheral'
            };

            // 按分组整理
            const permGroups = {};
            permDefs.forEach(p => {
                if (!permGroups[p.group]) permGroups[p.group] = [];
                permGroups[p.group].push(p);
            });

            Object.keys(permGroups).sort().forEach(group => {
                const groupDiv = document.createElement('div');
                groupDiv.className = 'role-perm-group';

                const gKey = _gpk[group] ? i18n.t('perm-group-' + _gpk[group]) : group;
                const groupTitle = document.createElement('h5');
                groupTitle.className = 'role-perm-group-title';
                groupTitle.textContent = gKey;
                groupDiv.appendChild(groupTitle);

                const permList = document.createElement('div');
                permList.className = 'role-perm-list';

                permGroups[group].forEach(perm => {
                    const pName = i18n.t('perm-' + perm.id) !== ('perm-' + perm.id) ? i18n.t('perm-' + perm.id) : perm.name;
                    const pDesc = i18n.t('perm-' + perm.id) !== ('perm-' + perm.id) ? i18n.t('perm-' + perm.id) : perm.description;
                    const label = document.createElement('label');
                    label.className = 'role-perm-label';
                    var checkbox = document.createElement('input');
                    checkbox.type = 'checkbox';
                    checkbox.name = 'role-perm';
                    checkbox.value = perm.id;
                    if (selectedSet.has(perm.id)) checkbox.checked = true;
                    label.appendChild(checkbox);
                    var labelText = document.createElement('span');
                    labelText.title = pDesc;
                    labelText.textContent = pName;
                    label.appendChild(labelText);
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
                AppState.showInlineError('role-error', msg);
                Notification.error(msg, isEditMode ? i18n.t('role-fail-edit') : i18n.t('role-fail-add'));
            };

            if (!id) return showErr(i18n.t('role-validate-id'));
            if (!name) return showErr(i18n.t('role-validate-name'));
            
            // 获取选中的权限
            const permCheckboxes = document.querySelectorAll('input[name="role-perm"]:checked');
            const permissions = Array.from(permCheckboxes).map(cb => cb.value).join(',');
            
            AppState.clearInlineError('role-error');

            const btn = document.getElementById('confirm-role-btn');
            if (btn) { btn.disabled = true; btn.textContent = i18n.t('role-saving-text'); }


            let apiCall;
            if (isEditMode) {
                // 编辑模式：先更新角色信息，再更新权限
                apiCall = apiPutForm('/api/roles/' + encodeURIComponent(id), { name, description })
                    .then(res => {
                        if (res && res.success) {
                            return apiPutForm('/api/roles/' + encodeURIComponent(id) + '/permissions', { permissions });
                        }
                        throw new Error(res.error || i18n.t('role-fail-update-msg'));
                    });
            } else {
                // 新增模式
                apiCall = apiPost('/api/roles', { id, name, description })
                    .then(res => {
                        if (res && res.success) {
                            return apiPutForm('/api/roles/' + encodeURIComponent(id) + '/permissions', { permissions });
                        }
                        throw new Error(res.error || i18n.t('role-fail-create-msg'));
                    });
            }

            apiCall
                .then(res => {
                    if (res && res.success) {
                        Notification.success(`${i18n.t('role-mgmt-title')}: ${name} ${isEditMode ? i18n.t('role-updated') : i18n.t('role-created')}${i18n.t('role-success-suffix')}`, i18n.t('role-mgmt-title'));
                        if (modal) AppState.hideModal(modal);
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

            apiDelete('/api/roles/' + encodeURIComponent(roleId))
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

    // 自动绑定事件
    if (typeof AppState.setupRolesEvents === 'function') {
        AppState.setupRolesEvents();
    }
})();
