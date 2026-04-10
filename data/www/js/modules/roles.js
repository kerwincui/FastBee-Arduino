/**
 * 角色管理模块
 * 包含角色列表、权限管理、角色 CRUD
 */
(function() {
    AppState.registerModule('roles', {

        // ============ 事件绑定 ============
        setupRolesEvents() {
            const closeId = (id) => { const el = document.getElementById(id); if (el) el.style.display = 'none'; };

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
        },

        // ============ 角色管理 ============
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
                tdDesc.style.maxWidth = '200px';
                tdDesc.style.overflow = 'hidden';
                tdDesc.style.textOverflow = 'ellipsis';
                tdDesc.style.whiteSpace = 'nowrap';
                row.appendChild(tdDesc);

                // 权限数
                const tdPermCount = document.createElement('td');
                const permCount = (role.permissions || []).length;
                tdPermCount.innerHTML = `<span style="background: #e6f7ff; color: #1890ff; padding: 2px 8px; border-radius: 10px; font-size: 12px;">${permCount}</span>`;
                row.appendChild(tdPermCount);

                // 类型
                const tdType = document.createElement('td');
                if (role.id === 'admin') {
                    tdType.innerHTML = `<span style="background: #fff1f0; color: #f5222d; padding: 2px 8px; border-radius: 4px; font-size: 12px;">${i18n.t('role-type-super')}</span>`;
                } else if (role.isBuiltin) {
                    tdType.innerHTML = `<span style="background: #f6ffed; color: #52c41a; padding: 2px 8px; border-radius: 4px; font-size: 12px;">${i18n.t('role-type-builtin')}</span>`;
                } else {
                    tdType.innerHTML = `<span style="background: #f0f5ff; color: #2f54eb; padding: 2px 8px; border-radius: 4px; font-size: 12px;">${i18n.t('role-type-custom')}</span>`;
                }
                row.appendChild(tdType);

                // 操作
                const tdAction = document.createElement('td');
                tdAction.className = 'u-toolbar-sm';

                // 查看权限按钮
                const viewBtn = document.createElement('button');
                viewBtn.className = 'btn btn-sm btn-secondary';
                viewBtn.textContent = i18n.t('role-view-perms');
                viewBtn.addEventListener('click', () => this.showRolePermissions(role));
                tdAction.appendChild(viewBtn);

                // 仅 admin 角色不可编辑/删除
                if (role.id !== 'admin') {
                    // 编辑按钮
                    const editBtn = document.createElement('button');
                    editBtn.className = 'btn btn-sm btn-edit';
                    editBtn.textContent = i18n.t('role-edit');
                    editBtn.style.marginRight = '5px';
                    editBtn.addEventListener('click', () => this.showEditRoleModal(role.id));
                    tdAction.appendChild(editBtn);

                    // 删除按钮
                    const delBtn = document.createElement('button');
                    delBtn.className = 'btn btn-sm btn-delete';
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
                    html += `<span style="margin-right: 4px;">${hasPerm ? '✓' : '✗'}</span>`;
                    html += `<span title="${pDesc}">${pName}</span></span>`;
                });
                html += `</div></div>`;
            });
            html += `</div>`;

            // 翻译角色名
            const roleNameMap = {'管理员': 'role-admin', '操作员': 'role-operator', '查看者': 'role-viewer'};
            const displayRoleName = roleNameMap[role.name] ? i18n.t(roleNameMap[role.name]) : role.name;

            // 使用简单的 alert 弹窗显示（或可以创建一个模态窗）
            const div = document.createElement('div');
            div.innerHTML = `
                <div style="position: fixed; top: 0; left: 0; right: 0; bottom: 0; background: rgba(0,0,0,0.5); z-index: 9999; display: flex; justify-content: center; align-items: center;" onclick="this.remove()">
                    <div style="background: white; border-radius: 8px; max-width: 600px; width: 90%; max-height: 80vh; overflow: hidden;" onclick="event.stopPropagation()">
                        <div style="background: linear-gradient(135deg, #1890ff 0%, #096dd9 100%); color: white; padding: 15px 20px; display: flex; justify-content: space-between; align-items: center;">
                            <h3 style="margin: 0;">${displayRoleName}${i18n.t('role-detail-suffix')}</h3>
                            <button onclick="this.closest('div[style*=position]').remove()" style="background: none; border: none; color: white; font-size: 20px; cursor: pointer;">×</button>
                        </div>
                        <div style="padding: 20px;">${html}</div>
                    </div>
                </div>
            `;
            document.body.appendChild(div);
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

                    console.log('[showEditRoleModal] Set editMode:', modal.dataset.editMode, 'editRoleId:', modal.dataset.editRoleId);

                    // 渲染权限复选框
                    this._renderPermissionCheckboxes(role.permissions || []);

                    // 清除错误提示
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

            // 获取选中的权限
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
                // 编辑模式：先更新角色信息，再更新权限
                apiCall = apiPost('/api/roles/update', { id, name, description })
                    .then(res => {
                        if (res && res.success) {
                            return apiPost('/api/roles/permissions', { id, permissions });
                        }
                        throw new Error(res.error || i18n.t('role-fail-update-msg'));
                    });
            } else {
                // 新增模式
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

    // 自动绑定事件
    if (typeof AppState.setupRolesEvents === 'function') {
        AppState.setupRolesEvents();
    }
})();
