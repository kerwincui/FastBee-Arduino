(function (){
AppState.registerModule('users',{
setupUsersEvents(){
const addUserBtn=document.getElementById('add-user-btn');
if(addUserBtn)addUserBtn.addEventListener('click',()=>this.showAddUserModal());
const closeUserModal=()=>{
const modal=document.getElementById('add-user-modal');
if(modal){
AppState.hideModal(modal);
modal.dataset.editMode='add';
modal.dataset.editUsername='';
}
const usernameInput=document.getElementById('add-username-input');
if(usernameInput)usernameInput.disabled=false;
};
['close-add-user-modal','cancel-add-user-btn'].forEach(id=>{
const el=document.getElementById(id);
if(el)el.addEventListener('click',closeUserModal);
});
const confirmAdd=document.getElementById('confirm-add-user-btn');
if(confirmAdd)confirmAdd.addEventListener('click',()=>this.addUser());
const usersRefreshBtn=document.getElementById('users-refresh-btn');
if(usersRefreshBtn)usersRefreshBtn.addEventListener('click',()=>this._refreshUsersList());
},
_refreshUsersList(){
var btn=document.getElementById('users-refresh-btn');
if(btn&&btn.disabled)return ;
if(btn){btn.disabled=true;btn.innerHTML='<span class="fb-spin">&#x21bb;</span> 加载中...';}
if(typeof window.apiInvalidateCache==='function'){
window.apiInvalidateCache('/api/users');
}
this.loadUsers();
setTimeout(function (){
if(btn){btn.disabled=false;btn.innerHTML='&#x21bb; 刷新';}
},2000);
},
loadUsers(){
apiGet('/api/users',{page:1,limit:100})
.then(res=>{
if(!res||!res.success)return ;
const users=(res.data&&res.data.users)?res.data.users:[];
this._renderUsers(users);
})
.catch(()=>{});
},
_renderUsers(users){
const tbody=document.getElementById('users-table-body');
if(!tbody)return ;
tbody.innerHTML='';
if(!users||users.length===0){
this.renderEmptyTableRow(tbody,5,i18n.t('no-users-data'));
return ;
}
users.forEach(user=>{
const row=document.createElement('tr');
const td=(content)=>{
const cell=document.createElement('td');
if(typeof content==='string'||typeof content==='number'){
cell.textContent=content;
}else{
cell.appendChild(content);
}
return cell;
};
const roleMap={admin :i18n.t('admin'),operator:i18n.t('operator'),viewer:i18n.t('viewer')};
const roleText=roleMap[user.role]||user.role||'—';
const lastLogin =user.lastLogin ?new Date(user.lastLogin *1000).toLocaleString():'—';
const badge=document.createElement('span');
badge.className='badge';
if(user.isLocked){
badge.classList.add('badge-warning');
badge.textContent=i18n.t('user-locked-badge');
}else if(!user.enabled){
badge.classList.add('badge-danger');
badge.textContent=i18n.t('user-status-inactive');
}else{
badge.classList.add('badge-success');
badge.textContent=i18n.t('user-status-active');
}
const actionCell=document.createElement('td');
actionCell.className='u-toolbar-sm';
const editBtn=document.createElement('button');
editBtn.className='btn btn-sm btn-edit';
editBtn.textContent=i18n.t('edit-user');
editBtn.addEventListener('click',()=>this.showEditUserModal(user));
actionCell.appendChild(editBtn);
const toggleBtn=document.createElement('button');
if(user.enabled&&!user.isLocked){
toggleBtn.className='btn btn-sm btn-disable';
toggleBtn.textContent=i18n.t('disable-user');
toggleBtn.addEventListener('click',()=>this.toggleUserStatus(user.username,false));
}else{
toggleBtn.className='btn btn-sm btn-enable';
toggleBtn.textContent=i18n.t('enable-user');
toggleBtn.addEventListener('click',()=>this.toggleUserStatus(user.username,true));
}
actionCell.appendChild(toggleBtn);
if(user.isLocked){
const unlockBtn=document.createElement('button');
unlockBtn.className='btn btn-sm btn-enable';
unlockBtn.textContent=i18n.t('unlock-user');
unlockBtn.addEventListener('click',()=>this.unlockUser(user.username));
actionCell.appendChild(unlockBtn);
}
if(user.username!=='admin'){
const delBtn=document.createElement('button');
delBtn.className='btn btn-sm btn-delete';
delBtn.textContent=i18n.t('delete-user');
delBtn.addEventListener('click',()=>{
if(confirm(`${i18n.t('confirm-delete-user-msg')}${user.username}${i18n.t('confirm-suffix')}`)){
this.deleteUser(user.username);
}
});
actionCell.appendChild(delBtn);
}
row.appendChild(td(user.username));
row.appendChild(td(roleText));
row.appendChild(td(lastLogin ));
row.appendChild(td(badge));
row.appendChild(actionCell);
tbody.appendChild(row);
});
},
showAddUserModal(){
const modal=document.getElementById('add-user-modal');
if(modal){
modal.dataset.editMode='add';
modal.dataset.editUsername='';
AppState.showModal(modal);
}
const title=document.getElementById('add-user-title');
if(title)title.textContent=i18n.t('add-user-modal-title');
const confirmBtn=document.getElementById('confirm-add-user-btn');
if(confirmBtn)confirmBtn.textContent=i18n.t('confirm-add-btn');
['add-username-input','add-password-input','add-confirm-password-input'].forEach(id=>{
const el=document.getElementById(id);if(el)el.value='';
});
const usernameInput=document.getElementById('add-username-input');
if(usernameInput)usernameInput.disabled=false;
const sel=document.getElementById('add-role-select');
if(sel)sel.value='operator';
AppState.clearInlineError('add-user-error');
},
addUser(){
const modal=document.getElementById('add-user-modal');
const isEditMode=modal&&modal.dataset.editMode==='edit';
const editUsername=modal?modal.dataset.editUsername:'';
console.log('[addUser] modal:',modal);
console.log('[addUser] modal.dataset:',modal?JSON.stringify(modal.dataset):'null');
console.log('[addUser] isEditMode:',isEditMode,'editUsername:',editUsername);
const username=isEditMode?editUsername:((document.getElementById('add-username-input')||{}).value||'').trim();
const password=(document.getElementById('add-password-input')||{}).value||'';
const confirmPwd=(document.getElementById('add-confirm-password-input')||{}).value||'';
const role=(document.getElementById('add-role-select')||{}).value||'operator';
const errDiv=document.getElementById('add-user-error');
const showErr=(msg)=>{
AppState.showInlineError('add-user-error',msg);
Notification.error(msg,isEditMode?i18n.t('edit-user-fail'):i18n.t('add-user-fail'));
};
if(!username)return showErr(i18n.t('validate-username-empty'));
if(username.length<3||username.length>32)return showErr(i18n.t('validate-username-len'));
if(!password||!confirmPwd)return showErr(i18n.t('validate-pwd-empty'));
if(password!==confirmPwd)return showErr(i18n.t('validate-pwd-mismatch'));
if(password.length<6)return showErr(i18n.t('validate-pwd-len'));
AppState.clearInlineError('add-user-error');
const btn=document.getElementById('confirm-add-user-btn');
if(btn){btn.disabled=true;btn.textContent=isEditMode?i18n.t('saving-btn'):i18n.t('adding-btn');}
let apiCall;
if(isEditMode){
apiCall=apiPost('/api/users/update',{username,password,role,enabled:'true'});
}else{
apiCall=apiPost('/api/users',{username,password,role});
}
apiCall
.then(res=>{
if(res&&res.success){
Notification.success(`${username}${isEditMode?i18n.t('role-updated'):i18n.t('role-created')}${i18n.t('role-success-suffix')}`,isEditMode?i18n.t('edit-user-success'):i18n.t('add-user-success'));
if(modal){
AppState.hideModal(modal);
modal.dataset.editMode='add';
modal.dataset.editUsername='';
}
const usernameInput=document.getElementById('add-username-input');
if(usernameInput)usernameInput.disabled=false;
this.loadUsers();
}else{
showErr((res&&res.error)||(isEditMode?i18n.t('modify-user-fail-msg'):i18n.t('add-user-fail-msg')));
}
})
.catch(()=>{})
.finally(()=>{if(btn){btn.disabled=false;btn.textContent=i18n.t('confirm-add-btn');}});
},
showEditUserModal(user){
console.log('[showEditUserModal] Called with user:',user);
const modal=document.getElementById('add-user-modal');
if(!modal){
console.error('[showEditUserModal] Modal not found!');
Notification.info(`${i18n.t('edit-user-modal-title')}:${user.username}`,i18n.t('edit-user-modal-title'));
return ;
}
modal.dataset.editMode='edit';
modal.dataset.editUsername=user.username;
console.log('[showEditUserModal] Set editMode:',modal.dataset.editMode,'editUsername:',modal.dataset.editUsername);
const title=document.getElementById('add-user-title');
if(title)title.textContent=i18n.t('edit-user-modal-title');
const usernameInput=document.getElementById('add-username-input');
if(usernameInput){
usernameInput.value=user.username;
usernameInput.disabled=true;
}
const pwdInput=document.getElementById('add-password-input');
const confirmInput=document.getElementById('add-confirm-password-input');
if(pwdInput)pwdInput.value='';
if(confirmInput)confirmInput.value='';
const roleSelect=document.getElementById('add-role-select');
if(roleSelect)roleSelect.value=user.role||'operator';
const confirmBtn=document.getElementById('confirm-add-user-btn');
if(confirmBtn)confirmBtn.textContent=i18n.t('confirm-save-btn');
AppState.showModal(modal);
AppState.clearInlineError('add-user-error');
},
toggleUserStatus(username,enable){
const action=enable?i18n.t('enable-user'):i18n.t('disable-user');
const confirmMsg=enable?i18n.t('confirm-enable-user'):i18n.t('confirm-disable-user');
if(!confirm(`${confirmMsg}${username}${i18n.t('confirm-suffix')}`))return ;
apiPost('/api/users/update',{username,enabled:enable?'true':'false'})
.then(res=>{
if(res&&res.success){
Notification.success(`${username}${enable?i18n.t('user-enabled-msg'):i18n.t('user-disabled-msg')}`,i18n.t('user-status-update'));
this.loadUsers();
}else{
Notification.error((res&&res.error)||i18n.t('operation-fail'),i18n.t('operation-fail'));
}
})
.catch(()=>{});
},
unlockUser(username){
if(!confirm(`${i18n.t('confirm-unlock-user')}${username}${i18n.t('confirm-suffix')}`))return ;
apiPost('/api/users/unlock-account',{username})
.then(res=>{
if(res&&res.success){
Notification.success(`${username}${i18n.t('user-unlocked-msg')}`,i18n.t('unlock-success'));
this.loadUsers();
}else{
Notification.error((res&&res.error)||i18n.t('operation-fail'),i18n.t('operation-fail'));
}
})
.catch(()=>{});
},
deleteUser(username){
if(!confirm(`${i18n.t('confirm-delete-user-msg')}${username}${i18n.t('confirm-suffix')}`))return ;
apiPost('/api/users/delete',{username})
.then(res=>{
if(res&&res.success){
Notification.success(`${username}${i18n.t('user-deleted-msg')}`,i18n.t('delete-success'));
this.loadUsers();
}else{
Notification.error((res&&res.error)||i18n.t('operation-fail'),i18n.t('operation-fail'));
}
})
.catch(()=>{});
}
});
if(typeof AppState.setupUsersEvents==='function'){
AppState.setupUsersEvents();
}
})();
(function (){
AppState.registerModule('roles',{
setupRolesEvents(){
const closeId=(id)=>{this.hideModal(id);};
const addRoleBtn=document.getElementById('add-role-btn');
if(addRoleBtn)addRoleBtn.addEventListener('click',()=>this.showAddRoleModal());
['close-role-modal','cancel-role-btn'].forEach(id=>{
const el=document.getElementById(id);
if(el)el.addEventListener('click',()=>closeId('role-modal'));
});
const confirmRole=document.getElementById('confirm-role-btn');
if(confirmRole)confirmRole.addEventListener('click',()=>this.saveRole());
const rolesRefreshBtn=document.getElementById('roles-refresh-btn');
if(rolesRefreshBtn)rolesRefreshBtn.addEventListener('click',()=>this._refreshRolesList());
},
_refreshRolesList(){
var btn=document.getElementById('roles-refresh-btn');
if(btn&&btn.disabled)return ;
if(btn){btn.disabled=true;btn.innerHTML='<span class="fb-spin">&#x21bb;</span> 加载中...';}
if(typeof window.apiInvalidateCache==='function'){
window.apiInvalidateCache('/api/roles');
}
this.loadRoles();
setTimeout(function (){
if(btn){btn.disabled=false;btn.innerHTML='&#x21bb; 刷新';}
},2000);
},
loadRoles(){
apiGet('/api/roles')
.then(res=>{
if(!res||!res.success)return ;
const roles=(res.data&&res.data.roles)?res.data.roles:[];
const permissions=(res.data&&res.data.permissions)?res.data.permissions:[];
this._permDefs=permissions;
this._renderRolesList(roles);
})
.catch(()=>{});
},
_renderRolesList(roles){
const tbody=document.getElementById('roles-table-body');
if(!tbody)return ;
tbody.innerHTML='';
roles.forEach(role=>{
const row=document.createElement('tr');
const tdId=document.createElement('td');
tdId.textContent=role.id;
row.appendChild(tdId);
const tdName=document.createElement('td');
const _rnm={'管理员':'role-admin','操作员':'role-operator','查看者':'role-viewer'};
const dName=_rnm[role.name]?i18n.t(_rnm[role.name]):role.name;
tdName.textContent=dName;
row.appendChild(tdName);
const tdDesc=document.createElement('td');
tdDesc.textContent=role.description||'—';
tdDesc.className='role-desc-cell';
row.appendChild(tdDesc);
const tdPermCount=document.createElement('td');
const permCount=(role.permissions||[]).length;
tdPermCount.innerHTML=`<span class="role-pill role-pill--count">${permCount}</span>`;
row.appendChild(tdPermCount);
const tdType=document.createElement('td');
if(role.id==='admin'){
tdType.innerHTML=`<span class="role-pill role-pill--super">${i18n.t('role-type-super')}</span>`;
}else if(role.isBuiltin ){
tdType.innerHTML=`<span class="role-pill role-pill--builtin">${i18n.t('role-type-builtin')}</span>`;
}else{
tdType.innerHTML=`<span class="role-pill role-pill--custom">${i18n.t('role-type-custom')}</span>`;
}
row.appendChild(tdType);
const tdAction=document.createElement('td');
tdAction.className='u-toolbar-sm';
const viewBtn=document.createElement('button');
viewBtn.className='btn btn-sm btn-secondary';
viewBtn.textContent=i18n.t('role-view-perms');
viewBtn.addEventListener('click',()=>this.showRolePermissions(role));
tdAction.appendChild(viewBtn);
if(role.id!=='admin'){
const editBtn=document.createElement('button');
editBtn.className='btn btn-sm btn-edit fb-mr-1';
editBtn.textContent=i18n.t('role-edit');
editBtn.addEventListener('click',()=>this.showEditRoleModal(role.id));
tdAction.appendChild(editBtn);
const delBtn=document.createElement('button');
delBtn.className='btn btn-sm btn-delete';
delBtn.textContent=i18n.t('role-delete');
delBtn.addEventListener('click',()=>{
const _rnm2={'管理员':'role-admin','操作员':'role-operator','查看者':'role-viewer'};
const dName2=_rnm2[role.name]?i18n.t(_rnm2[role.name]):role.name;
this.deleteRole(role.id,dName2);
});
tdAction.appendChild(delBtn);
}
row.appendChild(tdAction);
tbody.appendChild(row);
});
},
showRolePermissions(role){
const permDefs=this._permDefs||[];
const rolePerms=new Set(role.permissions||[]);
const _gpk={
'设备':'device','Device':'device',
'网络':'network','Network':'network',
'系统':'system','System':'system',
'用户':'user','Users':'user',
'文件':'file','Files':'file',
'协议':'protocol','Protocol':'protocol',
'审计':'audit','Audit':'audit',
'GPIO':'gpio',
'外设':'peripheral','Peripheral':'peripheral'
};
const permGroups={};
permDefs.forEach(p=>{
if(!permGroups[p.group])permGroups[p.group]=[];
permGroups[p.group].push(p);
});
let html=`<div class="role-perm-sheet">`;
Object.keys(permGroups).sort().forEach(group=>{
const gKey=_gpk[group]?i18n.t('perm-group-'+_gpk[group]):group;
html+=`<div class="role-perm-group">`;
html+=`<h4 class="role-perm-group-title">${gKey}</h4>`;
html+=`<div class="role-perm-chip-list">`;
permGroups[group].forEach(perm=>{
const hasPerm=rolePerms.has(perm.id);
const pName=i18n.t('perm-'+perm.id)!==('perm-'+perm.id)?i18n.t('perm-'+perm.id):perm.name;
const pDesc=i18n.t('perm-'+perm.id)!==('perm-'+perm.id)?i18n.t('perm-'+perm.id):perm.description;
html+=`<span class="role-perm-chip${hasPerm ? ' is-enabled' : ''}">`;
html+=`<span class="role-perm-chip-indicator">${hasPerm?'✓':'✗'}</span>`;
html+=`<span title="${pDesc}">${pName}</span></span>`;
});
html+=`</div></div>`;
});
html+=`</div>`;
const roleNameMap={'管理员':'role-admin','操作员':'role-operator','查看者':'role-viewer'};
const displayRoleName=roleNameMap[role.name]?i18n.t(roleNameMap[role.name]):role.name;
const overlay=document.createElement('div');
overlay.className='role-perm-overlay';
overlay.innerHTML=`
<div class="role-perm-dialog">
<div class="role-perm-dialog-header">
<h3 class="role-perm-dialog-title">${displayRoleName}${i18n.t('role-detail-suffix')}</h3>
<button type="button" class="role-perm-dialog-close">×</button>
</div>
<div class="role-perm-dialog-body">${html}</div>
</div>
`;
overlay.addEventListener('click',(e)=>{
if(e.target===overlay)overlay.remove();
});
const closeBtn=overlay.querySelector('.role-perm-dialog-close');
if(closeBtn){
closeBtn.addEventListener('click',()=>overlay.remove());
}
const dialog=overlay.querySelector('.role-perm-dialog');
if(dialog){
dialog.addEventListener('click',(e)=>e.stopPropagation());
}
document.body.appendChild(overlay);
},
showAddRoleModal(){
const modal=document.getElementById('role-modal');
if(!modal)return ;
const title=document.getElementById('role-modal-title');
if(title)title.textContent=i18n.t('role-add-title');
const idInput=document.getElementById('role-id-input');
const nameInput=document.getElementById('role-name-input');
const descInput=document.getElementById('role-desc-input');
if(idInput){idInput.value='';idInput.disabled=false;}
if(nameInput)nameInput.value='';
if(descInput)descInput.value='';
modal.dataset.editMode='add';
modal.dataset.editRoleId='';
this._renderPermissionCheckboxes([]);
AppState.clearInlineError('role-error');
AppState.showModal(modal);
},
showEditRoleModal(roleId){
const modal=document.getElementById('role-modal');
if(!modal){
console.error('[showEditRoleModal] modal not found!');
return ;
}
console.log('[showEditRoleModal] Opening edit modal for roleId:',roleId);
modal.dataset.editMode='edit';
modal.dataset.editRoleId=roleId;
apiGet('/api/roles')
.then(res=>{
if(!res||!res.success)return ;
const roles=(res.data&&res.data.roles)?res.data.roles:[];
const role=roles.find(r=>r.id===roleId);
if(!role){
Notification.error(i18n.t('role-not-exist'),i18n.t('error-title'));
return ;
}
const title=document.getElementById('role-modal-title');
if(title)title.textContent=i18n.t('role-edit-title');
const idInput=document.getElementById('role-id-input');
const nameInput=document.getElementById('role-name-input');
const descInput=document.getElementById('role-desc-input');
if(idInput){idInput.value=role.id;idInput.disabled=true;}
if(nameInput)nameInput.value=role.name;
if(descInput)descInput.value=role.description||'';
console.log('[showEditRoleModal] Set editMode:',modal.dataset.editMode,'editRoleId:',modal.dataset.editRoleId);
this._renderPermissionCheckboxes(role.permissions||[]);
AppState.clearInlineError('role-error');
AppState.showModal(modal);
})
.catch(()=>{});
},
_renderPermissionCheckboxes(selectedPerms){
const container=document.getElementById('role-permissions-container');
if(!container)return ;
container.innerHTML='';
const permDefs=this._permDefs||[];
const selectedSet=new Set(selectedPerms);
const _gpk={
'设备':'device','Device':'device',
'网络':'network','Network':'network',
'系统':'system','System':'system',
'用户':'user','Users':'user',
'文件':'file','Files':'file',
'协议':'protocol','Protocol':'protocol',
'审计':'audit','Audit':'audit',
'GPIO':'gpio',
'外设':'peripheral','Peripheral':'peripheral'
};
const permGroups={};
permDefs.forEach(p=>{
if(!permGroups[p.group])permGroups[p.group]=[];
permGroups[p.group].push(p);
});
Object.keys(permGroups).sort().forEach(group=>{
const groupDiv=document.createElement('div');
groupDiv.className='role-perm-group';
const gKey=_gpk[group]?i18n.t('perm-group-'+_gpk[group]):group;
const groupTitle=document.createElement('h5');
groupTitle.className='role-perm-group-title';
groupTitle.textContent=gKey;
groupDiv.appendChild(groupTitle);
const permList=document.createElement('div');
permList.className='role-perm-list';
permGroups[group].forEach(perm=>{
const pName=i18n.t('perm-'+perm.id)!==('perm-'+perm.id)?i18n.t('perm-'+perm.id):perm.name;
const pDesc=i18n.t('perm-'+perm.id)!==('perm-'+perm.id)?i18n.t('perm-'+perm.id):perm.description;
const label=document.createElement('label');
label.className='role-perm-label';
label.innerHTML=`
<input type="checkbox" name="role-perm" value="${perm.id}" ${selectedSet.has(perm.id)?'checked':''}>
<span title="${pDesc}">${pName}</span>
`;
permList.appendChild(label);
});
groupDiv.appendChild(permList);
container.appendChild(groupDiv);
});
},
saveRole(){
const modal=document.getElementById('role-modal');
if(!modal){
Notification.error(i18n.t('role-modal-not-found'),i18n.t('error-title'));
return ;
}
const isEditMode=modal.dataset.editMode==='edit';
const editRoleId=modal.dataset.editRoleId||'';
const idInput=document.getElementById('role-id-input');
const id=isEditMode?editRoleId:(idInput?idInput.value.trim():'');
const name=((document.getElementById('role-name-input')||{}).value||'').trim();
const description=((document.getElementById('role-desc-input')||{}).value||'').trim();
const errDiv=document.getElementById('role-error');
const showErr=(msg)=>{
AppState.showInlineError('role-error',msg);
Notification.error(msg,isEditMode?i18n.t('role-fail-edit'):i18n.t('role-fail-add'));
};
if(!id)return showErr(i18n.t('role-validate-id'));
if(!name)return showErr(i18n.t('role-validate-name'));
const permCheckboxes=document.querySelectorAll('input[name="role-perm"]:checked');
const permissions=Array.from(permCheckboxes).map(cb=>cb.value).join (',');
AppState.clearInlineError('role-error');
const btn=document.getElementById('confirm-role-btn');
if(btn){btn.disabled=true;btn.textContent=i18n.t('role-saving-text');}
console.log('[saveRole] modal.dataset:',JSON.stringify(modal.dataset));
console.log('[saveRole] isEditMode:',isEditMode,'id:',id,'editRoleId:',editRoleId);
console.log('[saveRole] Will call API:',isEditMode?'/api/roles/update':'/api/roles');
let apiCall;
if(isEditMode){
apiCall=apiPost('/api/roles/update',{id,name,description})
.then(res=>{
if(res&&res.success){
return apiPost('/api/roles/permissions',{id,permissions});
}
throw new Error(res.error||i18n.t('role-fail-update-msg'));
});
}else{
apiCall=apiPost('/api/roles',{id,name,description})
.then(res=>{
if(res&&res.success){
return apiPost('/api/roles/permissions',{id,permissions});
}
throw new Error(res.error||i18n.t('role-fail-create-msg'));
});
}
apiCall
.then(res=>{
if(res&&res.success){
Notification.success(`${i18n.t('role-mgmt-title')}:${name}${isEditMode?i18n.t('role-updated'):i18n.t('role-created')}${i18n.t('role-success-suffix')}`,i18n.t('role-mgmt-title'));
if(modal)AppState.hideModal(modal);
this.loadRoles();
}else{
showErr((res&&res.error)||i18n.t('role-op-fail'));
}
})
.catch(err=>{
showErr(err.message||i18n.t('role-op-fail'));
})
.finally(()=>{
if(btn){btn.disabled=false;btn.textContent=i18n.t('role-save-text');}
});
},
deleteRole(roleId,roleName){
if(!confirm(`${i18n.t('confirm-delete-role-msg')}"${roleName || roleId}" ${i18n.t('confirm-delete-role-suffix')}`))return ;
apiPost('/api/roles/delete',{id:roleId})
.then(res=>{
if(res&&res.success){
Notification.success(`${roleName||roleId}${i18n.t('role-deleted-msg')}`,i18n.t('delete-success'));
this.loadRoles();
}else{
Notification.error((res&&res.error)||i18n.t('operation-fail'),i18n.t('operation-fail'));
}
})
.catch(()=>{});
}
});
if(typeof AppState.setupRolesEvents==='function'){
AppState.setupRolesEvents();
}
})();
(function (){
AppState.registerModule('logs',{
setupLogsEvents(){
const refreshLogListBtn=document.getElementById('log-refresh-list-btn');
if(refreshLogListBtn)refreshLogListBtn.addEventListener('click',()=>this.loadLogFileList());
const refreshLogsBtn=document.getElementById('refresh-logs-btn');
if(refreshLogsBtn)refreshLogsBtn.addEventListener('click',()=>this.loadLogs());
const clearLogsBtn=document.getElementById('clear-logs-btn');
if(clearLogsBtn)clearLogsBtn.addEventListener('click',()=>this.clearLogs());
const autoRefreshCheckbox=document.getElementById('log-auto-refresh');
if(autoRefreshCheckbox){
autoRefreshCheckbox.addEventListener('change',(e)=>{
if(e.target.checked){
this.startLogAutoRefresh();
}else{
this.stopLogAutoRefresh();
}
});
}
this._setupLogVisibilityHandler();
},
_setupLogVisibilityHandler(){
if(this._logVisibilityHandlerBound)return ;
this._logVisibilityHandlerBound=true;
document.addEventListener('visibilitychange',()=>{
if(document.hidden){
if(this._logAutoRefreshTimer){
clearInterval(this._logAutoRefreshTimer);
this._logAutoRefreshTimer=null;
this._logPausedByVisibility=true;
console.log('[Logs] Auto refresh paused by visibility');
}
}else if(this._logPausedByVisibility){
this._logPausedByVisibility=false;
if(this.currentPage==='logs'){
this.loadLogs();
const autoRefreshCheckbox=document.getElementById('log-auto-refresh');
if(autoRefreshCheckbox&&autoRefreshCheckbox.checked){
this.startLogAutoRefresh();
}
console.log('[Logs] Auto refresh resumed by visibility');
}
}
});
},
loadLogFileList(){
const listContainer=document.getElementById("log-file-list");
if(!listContainer)return ;
apiGet("/api/logs/list")
.then(res=>{
if(!res||!res.success){
listContainer.innerHTML=`<div class="logs-state logs-state-error">${i18n.t("log-load-fail")}</div>`;
return ;
}
const files=res.data||[];
if(files.length===0){
listContainer.innerHTML=`<div class="logs-state">${i18n.t("log-empty")}</div>`;
return ;
}
files.sort((a,b)=>{
if(a.name==="system.log")return -1;
if(b.name==="system.log")return 1;
return b.name.localeCompare(a.name);
});
let html="";
files.forEach(file=>{
const sizeStr=file.size<1024?`${file.size}B`:`${(file.size / 1024).toFixed(1)}KB`;
const isActive=file.name===this._currentLogFile;
const safeName=typeof escapeHtml==="function"?escapeHtml(file.name):file.name;
const icon=file.current?"&#9679;":"&#9675;";
html+=`<div class="logs-file-item${isActive ? " is-active" : ""}" data-file="${encodeURIComponent(file.name)}">
<span class="logs-file-icon${file.current ? " is-current" : ""}">${icon}</span>
<span class="logs-file-label">${safeName}</span>
<span class="logs-file-size">${sizeStr}</span>
</div>`;
});
listContainer.innerHTML=html;
listContainer.querySelectorAll(".logs-file-item").forEach(item=>{
item.addEventListener("click",()=>{
const fileName=decodeURIComponent(item.dataset.file||"");
if(!fileName)return ;
this._currentLogFile=fileName;
const currentSpan=document.getElementById("current-log-file");
if(currentSpan)currentSpan.textContent=i18n.t("log-current-file-prefix")+fileName;
this.setExclusiveActive(listContainer,".logs-file-item",item);
this.loadLogs(500,fileName);
});
});
})
.catch(err=>{
console.error("Load log file list failed:",err);
listContainer.innerHTML=`<div class="logs-state logs-state-error">${i18n.t("log-load-fail")}</div>`;
});
},
loadLogs(maxLines=500,fileName=null){
const container=document.getElementById("device-log-container");
const infoSpan=document.getElementById("log-info");
if(!container)return ;
const logFile=fileName||this._currentLogFile||"system.log";
this._currentLogFile=logFile;
const currentSpan=document.getElementById("current-log-file");
if(currentSpan)currentSpan.textContent=i18n.t("log-current-file-prefix")+logFile;
apiGet("/api/logs",{lines:maxLines,file:logFile})
.then(res=>{
if(!res||!res.success){
container.innerHTML=`<div class="logs-state logs-state-error">${i18n.t("log-load-fail")}</div>`;
return ;
}
const data=res.data||{};
const content=data.content||"";
const fileSize=data.size||0;
const lineCount=data.lines||0;
const truncated=data.truncated||false;
if(infoSpan){
const sizeStr=fileSize<1024?`${fileSize}B`:
fileSize<1024*1024?`${(fileSize / 1024).toFixed(1)}KB`:
`${(fileSize / 1024 / 1024).toFixed(2)}MB`;
let infoText=`${lineCount}${i18n.t("log-line-unit")}${sizeStr}`;
if(truncated)infoText+=i18n.t("log-truncated-suffix");
infoSpan.textContent=infoText;
}
if(!content||!content.trim()){
container.innerHTML=`<div class="logs-state">${i18n.t("log-empty")}</div>`;
}else{
container.innerHTML=this._formatLogContent(content);
container.scrollTop=container.scrollHeight;
}
})
.catch(err=>{
console.error("Load logs failed:",err);
container.innerHTML=`<div class="logs-state logs-state-error">${i18n.t("log-load-fail")}</div>`;
});
},
_formatLogContent(content){
const lines=content.split('\n');
return lines.map(line=>{
if(!line.trim())return '';
let className='';
if(line.includes('[ERROR]')||line.includes('[E]')){
className='log-error';
}else if(line.includes('[WARN]')||line.includes('[W]')){
className='log-warn';
}else if(line.includes('[INFO]')||line.includes('[I]')){
className='log-info';
}else if(line.includes('[DEBUG]')||line.includes('[D]')){
className='log-debug';
}
const escaped=line
.replace(/&/g,'&amp;')
.replace(/</g,'&lt;')
.replace(/>/g,'&gt;');
return `<div class="${className}">${escaped}</div>`;
}).filter(line=>line).join ('');
},
clearLogs(){
if(!confirm(i18n.t('log-clear-confirm-msg')))return ;
const btn=document.getElementById('clear-logs-btn');
if(btn){
btn.disabled=true;
btn.innerHTML=i18n.t('log-clearing-html');
}
apiPost('/api/logs/clear',{})
.then(res=>{
if(res&&res.success){
Notification.success(i18n.t('log-cleared-msg'),i18n.t('log-op-title'));
this.loadLogs();
}else{
Notification.error((res&&res.error)||i18n.t('log-op-fail'),i18n.t('log-op-fail'));
}
})
.catch(err=>{
console.error('Clear logs failed:',err);
Notification.error(i18n.t('log-op-fail'),i18n.t('log-op-fail'));
})
.finally(()=>{
if(btn){
btn.disabled=false;
btn.innerHTML=i18n.t('log-clear-btn-html');
}
});
},
startLogAutoRefresh(interval=5000){
this.stopLogAutoRefresh();
this._logAutoRefreshTimer=setInterval(()=>{
if(this.currentPage==='logs'){
this.loadLogs();
}
},interval);
console.log('[Logs] Auto refresh started with interval:',interval);
},
stopLogAutoRefresh(){
if(this._logAutoRefreshTimer){
clearInterval(this._logAutoRefreshTimer);
this._logAutoRefreshTimer=null;
console.log('[Logs] Auto refresh stopped');
}
}
});
if(typeof AppState.setupLogsEvents==='function'){
AppState.setupLogsEvents();
}
})();
(function (){
AppState.registerModule('files',{
setupFilesEvents(){
const fsRefreshBtn=document.getElementById('fs-refresh-btn');
if(fsRefreshBtn)fsRefreshBtn.addEventListener('click',()=>this.loadFileTree(this._currentDir||'/'));
const fsUpBtn=document.getElementById('fs-up-btn');
if(fsUpBtn)fsUpBtn.addEventListener('click',()=>this.navigateUp());
const fsSaveBtn=document.getElementById('fs-save-btn');
if(fsSaveBtn)fsSaveBtn.addEventListener('click',()=>this.saveCurrentFile());
const fsCloseBtn=document.getElementById('fs-close-btn');
if(fsCloseBtn)fsCloseBtn.addEventListener('click',()=>this.closeCurrentFile());
},
loadFileSystemInfo(){
apiGet('/api/filesystem')
.then(res=>{
if(res&&res.success){
const d=res.data||{};
const infoSpan=document.getElementById('fs-info');
if(infoSpan){
const total=d.totalBytes?(d.totalBytes / 1024 / 1024).toFixed(2)+' MB':'N/A';
const used=d.usedBytes?(d.usedBytes / 1024 / 1024).toFixed(2)+' MB':'N/A';
infoSpan.textContent=`${i18n.t('fs-space-prefix')}${total}${i18n.t('fs-space-used-prefix')}${used}`;
}
}
})
.catch(()=>{});
},
loadFileTree(path){
const treeContainer=document.getElementById('file-tree');
if(!treeContainer)return ;
this._currentDir=path;
const pathEl=document.getElementById('current-dir-path');
if(pathEl)pathEl.textContent=path;
treeContainer.innerHTML=i18n.t('fs-loading-text');
apiGet('/api/files',{path:path})
.then(res=>{
if(!res||!res.success){
treeContainer.innerHTML=i18n.t('fs-load-fail-text');
return ;
}
const data=res.data||{};
const dirs=data.dirs||[];
const files=data.files||[];
let html='';
dirs.forEach(dir=>{
html+=`<div class="file-tree-item" data-path="${path}${dir.name}/" data-type="dir">
<span class="file-tree-icon-dir">📁</span>${dir.name}
</div>`;
});
files.forEach(file=>{
const size=file.size<1024?`${file.size}B`:
file.size<1024*1024?`${(file.size / 1024).toFixed(1)}KB`:
`${(file.size / 1024 / 1024).toFixed(2)}MB`;
html+=`<div class="file-tree-item" data-path="${path}${file.name}" data-type="file">
<span class="file-tree-icon-file">📄</span>${file.name}<span class="file-tree-size">(${size})</span>
</div>`;
});
if(dirs.length===0&&files.length===0){
html=i18n.t('fs-empty-dir-html');
}
treeContainer.innerHTML=html;
treeContainer.querySelectorAll('.file-tree-item').forEach(item=>{
item.addEventListener('click',(e)=>{
const path=e.currentTarget.dataset.path;
const type=e.currentTarget.dataset.type;
treeContainer.querySelectorAll('.file-tree-item').forEach(i=>{
i.classList.remove('is-active');
});
e.currentTarget.classList.add('is-active');
if(type==='dir'){
this.loadFileTree(path);
}else{
this.openFile(path);
}
});
});
})
.catch(err=>{
console.error('Load file tree failed:',err);
treeContainer.innerHTML=i18n.t('fs-load-fail-text');
});
},
openFile(path){
const editor=document.getElementById('file-editor');
const pathSpan=document.getElementById('current-file-path');
const saveBtn=document.getElementById('fs-save-btn');
const closeBtn=document.getElementById('fs-close-btn');
const statusDiv=document.getElementById('file-status');
if(!editor||!pathSpan)return ;
const editable=path.endsWith('.json')||path.endsWith('.txt')||path.endsWith('.log')||
path.endsWith('.html')||path.endsWith('.js')||path.endsWith('.css');
if(!editable){
statusDiv.textContent=i18n.t('fs-file-type-unsupported');
return ;
}
pathSpan.textContent=path;
statusDiv.textContent=i18n.t('fs-file-loading');
apiGet('/api/files/content',{path:path})
.then(res=>{
if(!res||!res.success){
statusDiv.textContent=i18n.t('fs-file-load-fail-prefix')+(res.error||i18n.t('fs-file-unknown-error'));
return ;
}
const data=res.data||{};
editor.value=data.content||'';
editor.disabled=false;
closeBtn.disabled=false;
if(!AppState.currentUser.canManageFs){
saveBtn.disabled=true;
editor.readOnly=true;
editor.title=i18n.t('fs-no-perm-tip');
editor.oninput=null;
}else{
saveBtn.disabled=true;
editor.readOnly=false;
editor.title='';
editor.oninput=()=>{
saveBtn.disabled=false;
this._currentFileModified=true;
};
}
const size=data.size<1024?`${data.size}B`:
data.size<1024*1024?`${(data.size / 1024).toFixed(1)}KB`:
`${(data.size / 1024 / 1024).toFixed(2)}MB`;
statusDiv.textContent=`${i18n.t('fs-file-ready-prefix')}${size}${i18n.t('fs-file-ready-suffix')}`;
this._currentFilePath=path;
this._currentFileModified=false;
})
.catch(err=>{
console.error('Open file failed:',err);
statusDiv.textContent=i18n.t('fs-file-load-fail');
});
},
navigateUp(){
const currentPath=this._currentDir||'/';
if(currentPath==='/')return ;
let path=currentPath;
if(path.endsWith('/'))path=path.slice(0,-1);
const lastSlashIndex=path.lastIndexOf('/');
if(lastSlashIndex===0){
this.loadFileTree('/');
}else if(lastSlashIndex>0){
const parentPath=path.substring(0,lastSlashIndex+1);
this.loadFileTree(parentPath);
}else{
this.loadFileTree('/');
}
},
saveCurrentFile(){
if(!this._currentFilePath)return ;
if(!AppState.currentUser.canManageFs){
Notification.warning(i18n.t('fs-no-perm-tip'),i18n.t('fs-mgmt-title'));
return ;
}
const editor=document.getElementById('file-editor');
const statusDiv=document.getElementById('file-status');
const saveBtn=document.getElementById('fs-save-btn');
if(!editor||!statusDiv)return ;
const content=editor.value;
statusDiv.textContent=i18n.t('fs-saving-text');
saveBtn.disabled=true;
apiPost('/api/files/save',{path:this._currentFilePath,content:content})
.then(res=>{
if(res&&res.success){
statusDiv.textContent=i18n.t('fs-save-ok-text');
this._currentFileModified=false;
Notification.success(i18n.t('fs-save-ok-msg'),i18n.t('fs-mgmt-title'));
}else{
statusDiv.textContent=i18n.t('fs-save-fail-prefix')+(res.error||'');
Notification.error(res.error||i18n.t('fs-save-fail-text'),i18n.t('fs-mgmt-title'));
}
})
.catch(err=>{
console.error('Save file failed:',err);
statusDiv.textContent=i18n.t('fs-save-fail-text');
Notification.error(i18n.t('fs-save-fail-text'),i18n.t('fs-mgmt-title'));
})
.finally(()=>{
saveBtn.disabled=false;
});
},
closeCurrentFile(){
const editor=document.getElementById('file-editor');
const pathSpan=document.getElementById('current-file-path');
const saveBtn=document.getElementById('fs-save-btn');
const closeBtn=document.getElementById('fs-close-btn');
const statusDiv=document.getElementById('file-status');
if(this._currentFileModified){
if(!confirm(i18n.t('fs-modified-confirm'))){
return ;
}
this.saveCurrentFile();
}
if(editor){
editor.value='';
editor.disabled=true;
editor.readOnly=false;
editor.title='';
editor.oninput=null;
}
if(pathSpan)pathSpan.textContent=i18n.t('fs-select-file-text');
if(saveBtn)saveBtn.disabled=true;
if(closeBtn)closeBtn.disabled=true;
if(statusDiv)statusDiv.textContent='';
this._currentFilePath=null;
this._currentFileModified=false;
const treeContainer=document.getElementById('file-tree');
if(treeContainer){
treeContainer.querySelectorAll('.file-tree-item').forEach(i=>{
i.style.background='';
});
}
}
});
if(typeof AppState.setupFilesEvents==='function'){
AppState.setupFilesEvents();
}
})();
(function (){
AppState.registerModule('rule-script',{
_ruleScriptEventsBound:false,
setupRuleScriptEvents(){
if(this._ruleScriptEventsBound)return ;
const closeRuleScriptModal=document.getElementById('close-rule-script-modal');
if(closeRuleScriptModal)closeRuleScriptModal.addEventListener('click',()=>this.closeRuleScriptModal());
const cancelRuleScriptBtn=document.getElementById('cancel-rule-script-btn');
if(cancelRuleScriptBtn)cancelRuleScriptBtn.addEventListener('click',()=>this.closeRuleScriptModal());
const saveRuleScriptBtn=document.getElementById('save-rule-script-btn');
if(saveRuleScriptBtn)saveRuleScriptBtn.addEventListener('click',()=>this.saveRuleScript());
const tableBody=document.getElementById('rule-script-table-body');
if(tableBody){
tableBody.addEventListener('click',(event)=>this._handleRuleScriptTableClick(event));
}
const rsRefreshBtn=document.getElementById('rule-script-refresh-btn');
if(rsRefreshBtn)rsRefreshBtn.addEventListener('click',()=>this._refreshRuleScriptList());
this._ruleScriptEventsBound=true;
},
_handleRuleScriptTableClick(event){
const button=event.target.closest('[data-rule-script-action]');
if(!button)return ;
const action=button.getAttribute('data-rule-script-action');
const id=button.getAttribute('data-id');
if(!action||!id)return ;
if(action==='edit')this.editRuleScript(id);
else if(action==='toggle')this.toggleRuleScript(id,button.getAttribute('data-enabled')==='true');
else if(action==='delete')this.deleteRuleScript(id);
},
_renderRuleScriptStatusBadge(enabled){
return enabled
?'<span class="badge badge-success">'+i18n.t('periph-exec-status-on')+'</span>'
:'<span class="badge badge-info">'+i18n.t('periph-exec-status-off')+'</span>';
},
_renderRuleScriptActionButton(action,id,label,className,enabledValue){
let attrs='data-rule-script-action="'+action+'" data-id="'+escapeHtml(id)+'"';
if(enabledValue!==undefined)attrs+=' data-enabled="'+(enabledValue?'true':'false')+'"';
return '<button class="btn btn-sm '+className+'" '+attrs+'>'+label+'</button>';
},
_renderRuleScriptRow(rule,triggerLabels,protocolLabels){
const triggerText=triggerLabels[rule.triggerType]||'?';
const protocolText=protocolLabels[rule.protocolType]||'-';
const statsText=i18n.t('periph-exec-stats-count')+': '+(rule.triggerCount||0);
let html='<tr>';
html+='<td>'+escapeHtml(rule.name||'')+'</td>';
html+='<td>'+this._renderRuleScriptStatusBadge(rule.enabled)+'</td>';
html+='<td>'+escapeHtml(triggerText)+'</td>';
html+='<td>'+escapeHtml(protocolText)+'</td>';
html+='<td class="rule-script-stats-cell">'+escapeHtml(statsText)+'</td>';
html+='<td class="u-toolbar-sm">';
html+=this._renderRuleScriptActionButton('edit',rule.id,i18n.t('peripheral-edit'),'btn-edit');
html+=rule.enabled
?this._renderRuleScriptActionButton('toggle',rule.id,i18n.t('peripheral-disable'),'btn-disable',false)
:this._renderRuleScriptActionButton('toggle',rule.id,i18n.t('peripheral-enable'),'btn-enable',true);
html+=this._renderRuleScriptActionButton('delete',rule.id,i18n.t('peripheral-delete'),'btn-delete');
html+='</td></tr>';
return html;
},
_refreshRuleScriptList(){
var btn=document.getElementById('rule-script-refresh-btn');
if(btn&&btn.disabled)return ;
if(btn){btn.disabled=true;btn.innerHTML='<span class="fb-spin">&#x21bb;</span> 加载中...';}
if(typeof window.apiInvalidateCache==='function'){
window.apiInvalidateCache('/api/rule-script');
}
this.loadRuleScriptPage();
setTimeout(function (){
if(btn){btn.disabled=false;btn.innerHTML='&#x21bb; 刷新';}
},2000);
},
loadRuleScriptPage(){
const tbody=document.getElementById('rule-script-table-body');
if(!tbody)return ;
apiGet('/api/rule-script').then(res=>{
if(!res||!res.success||!res.data){
this.renderEmptyTableRow(tbody,6,i18n.t('rule-script-no-data'));
return ;
}
const rules=res.data;
if(rules.length===0){
this.renderEmptyTableRow(tbody,6,i18n.t('rule-script-no-data'));
return ;
}
const triggerLabels={0:i18n.t('rule-script-trigger-receive'),1:i18n.t('rule-script-trigger-report')};
const protocolLabels={0:'MQTT',1:'Modbus RTU',2:'Modbus TCP',3:'HTTP',4:'CoAP',5:'TCP'};
let html='';
rules.forEach(r=>{
html+=this._renderRuleScriptRow(r,triggerLabels,protocolLabels);
});
tbody.innerHTML=html;
}).catch(()=>{
this.renderEmptyTableRow(tbody,6,i18n.t('rule-script-no-data'));
});
},
openRuleScriptModal(editId){
const modal=document.getElementById('rule-script-modal');
if(!modal)return ;
const titleEl=document.getElementById('rule-script-modal-title');
document.getElementById('rule-script-original-id').value=editId||'';
this.clearInlineError('rule-script-error');
if(editId){
if(titleEl)titleEl.textContent=i18n.t('rule-script-edit-title');
}else{
if(titleEl)titleEl.textContent=i18n.t('rule-script-add-title');
document.getElementById('rule-script-form').reset();
document.getElementById('rule-script-protocol-type').value='0';
document.getElementById('rule-script-content').value='';
}
this.showModal(modal);
},
closeRuleScriptModal(){
this.hideModal('rule-script-modal');
},
saveRuleScript(){
this.clearInlineError('rule-script-error');
const originalId=document.getElementById('rule-script-original-id').value;
const isEdit=originalId!=='';
const ruleData={
name:document.getElementById('rule-script-name').value.trim(),
enabled:document.getElementById('rule-script-enabled').checked?'1':'0',
triggerType:document.getElementById('rule-script-trigger-type').value,
protocolType:document.getElementById('rule-script-protocol-type').value,
scriptContent:document.getElementById('rule-script-content').value
};
if(!ruleData.name){
this.showInlineError('rule-script-error',i18n.t('periph-exec-validate-name'));
return ;
}
if(isEdit)ruleData.id=originalId;
const url=isEdit?'/api/rule-script/update':'/api/rule-script';
apiPost(url,ruleData).then(res=>{
if(res&&res.success){
Notification.success(i18n.t(isEdit?'rule-script-update-ok':'rule-script-add-ok'),i18n.t('rule-script-title'));
this.closeRuleScriptModal();
if(this.currentPage==='rule-script')this.loadRuleScriptPage();
}else{
this.showInlineError('rule-script-error',res?.error||i18n.t('rule-script-save-fail'));
}
}).catch(()=>{
this.showInlineError('rule-script-error',i18n.t('rule-script-save-fail'));
});
},
editRuleScript(id){
this.openRuleScriptModal(id);
apiGet('/api/rule-script').then(res=>{
if(!res||!res.success||!res.data)return ;
const rule=res.data.find(r=>r.id===id);
if(!rule)return ;
document.getElementById('rule-script-name').value=rule.name||'';
document.getElementById('rule-script-enabled').checked=!!rule.enabled;
document.getElementById('rule-script-trigger-type').value=String(rule.triggerType);
document.getElementById('rule-script-protocol-type').value=String(rule.protocolType||0);
document.getElementById('rule-script-content').value=rule.scriptContent||'';
});
},
toggleRuleScript(id,enable){
const url=enable?'/api/rule-script/enable':'/api/rule-script/disable';
apiPost(url,{id:id}).then(res=>{
if(res&&res.success){
if(this.currentPage==='rule-script')this.loadRuleScriptPage();
}
});
},
deleteRuleScript(id){
if(!confirm(i18n.t('periph-exec-confirm-delete')))return ;
apiDelete('/api/rule-script/',{id:id}).then(res=>{
if(res&&res.success){
Notification.success(i18n.t('rule-script-delete-ok'),i18n.t('rule-script-title'));
if(this.currentPage==='rule-script')this.loadRuleScriptPage();
}
});
}
});
if(typeof AppState.setupRuleScriptEvents==='function'){
AppState.setupRuleScriptEvents();
}
})();