// ============================================================
// AppState 子模块: 会话管理
// 从 state.js 拆分，通过全局 AppState 对象注册
// 包含: 会话验证、登录、修改密码、退出登录
// 依赖: state.js (AppState 全局对象), fetch-api.js (apiGet/apiPost)
// ============================================================

(function() {
    'use strict';

    // ============ 会话验证 ============

    /**
     * 刷新页面 — 验证当前会话，若无效则尝试自动登录
     */
    AppState.refreshPage = function() {
        const token = localStorage.getItem('auth_token');

        // 在 API 调用前保存"记住密码"凭据到局部变量
        // （防止 401 处理器清除 localStorage 中的凭据）
        const savedRemember = localStorage.getItem('remember');
        const savedUsername = localStorage.getItem('username');
        const savedPassword = localStorage.getItem('password');

        if (!token) {
            // 没有 token，尝试使用保存的凭据自动登录
            this._tryAutoLogin(savedRemember, savedUsername, savedPassword);
            return;
        }
        apiGet('/api/auth/session')
            .then(res => {
                if (res && res.success && res.data && res.data.sessionValid) {
                    this.currentUser.name = res.data.username || 'Admin';
                    this.currentUser.role = res.data.role || 'VIEWER';
                    this.currentUser.canManageFs = res.data.canManageFs === true;
                    this._showAppPage();
                    this._loadModule('dashboard', () => {
                        this.renderDashboard();
                        this.loadSystemMonitor();
                    });
                } else {
                    // 会话无效，尝试使用保存的凭据重新登录
                    this._tryAutoLogin(savedRemember, savedUsername, savedPassword);
                }
            })
            .catch(() => {
                // 会话验证失败（如 401），尝试自动重新登录
                this._tryAutoLogin(savedRemember, savedUsername, savedPassword);
            });
    };

    /**
     * 尝试使用保存的凭据自动登录
     * @param {string} remember - 是否记住密码
     * @param {string} username - 用户名
     * @param {string} password - 密码
     * @private
     */
    AppState._tryAutoLogin = function(remember, username, password) {
        if (remember === 'true' && username && password) {
            apiPost('/api/auth/login', { username, password })
                .then(res => {
                    if (res && res.success) {
                        const sid = res.sessionId;
                        localStorage.setItem('auth_token', sid);
                        localStorage.setItem('sessionId', sid);
                        // 恢复"记住密码"凭据（可能已被 401 处理器清除）
                        localStorage.setItem('remember', 'true');
                        localStorage.setItem('username', username);
                        localStorage.setItem('password', password);

                        this.currentUser.name = res.username || username;
                        sessionStorage.setItem('currentUsername', this.currentUser.name);

                        // 获取角色和权限信息
                        apiGet('/api/auth/session').then(sr => {
                            if (sr && sr.success && sr.data) {
                                this.currentUser.role = sr.data.role || 'VIEWER';
                                this.currentUser.canManageFs = sr.data.canManageFs === true;
                            }
                        }).catch(() => {});

                        this._showAppPage();
                        this._loadModule('dashboard', () => {
                            this.renderDashboard();
                            this.loadSystemMonitor();
                        });
                    } else {
                        // 自动登录失败（如密码已更改），清除无效凭据并显示登录页
                        localStorage.removeItem('password');
                        localStorage.removeItem('remember');
                        this._showLoginPage();
                    }
                })
                .catch(() => {
                    this._showLoginPage();
                });
        } else {
            this._showLoginPage();
        }
    };

    /**
     * 显示登录页面
     * @private
     */
    AppState._showLoginPage = function() {
        document.getElementById('login-page').style.display = 'flex';
        document.getElementById('app-container').classList.add('fb-hidden');

        // 预填充已保存的用户名和"记住密码"状态
        const savedUsername = localStorage.getItem('username');
        const savedRemember = localStorage.getItem('remember');
        const usernameInput = document.getElementById('username');
        const rememberCheckbox = document.getElementById('remember');
        if (usernameInput && savedUsername) usernameInput.value = savedUsername;
        if (rememberCheckbox && savedRemember === 'true') rememberCheckbox.checked = true;
    };

    /**
     * 显示主应用页面
     * @private
     */
    AppState._showAppPage = async function() {
        document.getElementById('login-page').style.display = 'none';
        document.getElementById('app-container').classList.remove('fb-hidden');
        // 登录成功后将URL从 /login 等路径重定向到根路径 /
        if (location.pathname !== '/' || location.hash) {
            history.replaceState(null, '', '/');
        }
        // 并行加载模态框和仪表盘页面（消除串行 await 延迟）
        await Promise.all([
            this._loadModals(),
            this.loadPage('dashboard-page')
        ]);
        // 移除骨架屏
        var skeleton = document.getElementById('skeleton-screen');
        if (skeleton) skeleton.remove();
        // 处理 URL 参数指定的跳转（新标签页全屏等场景）
        if (typeof this._applyUrlParams === 'function') this._applyUrlParams();
    };

    // ============ 登录 ============

    /**
     * 处理登录表单提交
     */
    AppState.handleLogin = function() {
        const username = (document.getElementById('username') || {}).value;
        const password = (document.getElementById('password') || {}).value;
        const remember = (document.getElementById('remember') || {}).checked;

        if (!username || !password) {
            Notification.warning(i18n.t('login-empty-warning'), i18n.t('login-fail-title'));
            return;
        }

        const submitBtn = document.querySelector('#login-form button[type="submit"]');
        const originalText = submitBtn ? submitBtn.innerHTML : '';
        if (submitBtn) { submitBtn.innerHTML = i18n.t('login-logging-in-html'); submitBtn.disabled = true; }

        apiPost('/api/auth/login', { username, password })
            .then(res => {
                if (res && res.success) {
                    const sid = res.sessionId;

                    // 保存 token 到 localStorage，fetch-api.js 会自动注入 Authorization 头
                    localStorage.setItem('auth_token', sid);

                    // 记住密码
                    if (remember) {
                        localStorage.setItem('remember', 'true');
                        localStorage.setItem('username', username);
                        localStorage.setItem('password', password);  // 保存密码用于自动填充
                        localStorage.setItem('sessionId', sid);
                    } else {
                        ['remember', 'username', 'password', 'sessionId'].forEach(k => localStorage.removeItem(k));
                    }

                    this.currentUser.name = res.username || username;
                    sessionStorage.setItem('currentUsername', this.currentUser.name);

                    // 获取角色和权限信息
                    apiGet('/api/auth/session').then(sr => {
                        if (sr && sr.success && sr.data) {
                            this.currentUser.role = sr.data.role || 'VIEWER';
                            this.currentUser.canManageFs = sr.data.canManageFs === true;
                        }
                    }).catch(() => {});

                    this._showAppPage();
                    this._loadModule('dashboard', () => {
                        this.renderDashboard();
                        this.loadSystemMonitor();
                    });
                    Notification.success(i18n.t('login-success-msg'), i18n.t('login-welcome-title'));
                } else {
                    Notification.error((res && res.error) || i18n.t('login-fail-title'), i18n.t('login-fail-title'));
                }
            })
            .catch((err) => {
                // 登录失败，显示错误信息
                const errorMsg = (err && err.data && err.data.error) || i18n.t('login-fail-msg');
                Notification.error(errorMsg, i18n.t('login-fail-title'));
            })
            .finally(() => {
                if (submitBtn) { submitBtn.innerHTML = originalText; submitBtn.disabled = false; }
            });
    };

    // ============ 修改密码 ============

    /**
     * 显示修改密码模态框
     */
    AppState.showChangePasswordModal = function() {
        this.showModal('change-password-modal');
        ['current-password-input', 'new-password-input', 'confirm-password-input'].forEach(id => {
            const el = document.getElementById(id); if (el) el.value = '';
        });
        this.clearInlineError('password-error');
    };

    /**
     * 执行修改密码操作
     */
    AppState.changePassword = function() {
        const oldPwd = (document.getElementById('current-password-input') || {}).value || '';
        const newPwd = (document.getElementById('new-password-input') || {}).value || '';
        const confirmPwd = (document.getElementById('confirm-password-input') || {}).value || '';
        const showErr = (msg) => {
            this.showInlineError('password-error', msg);
            Notification.error(msg, i18n.t('change-pwd-fail'));
        };

        if (!oldPwd || !newPwd || !confirmPwd) return showErr(i18n.t('validate-all-fields'));
        if (newPwd !== confirmPwd) return showErr(i18n.t('password-error') || i18n.t('validate-new-pwd-mismatch'));
        if (newPwd.length < 6) return showErr(i18n.t('validate-new-pwd-len'));

        this.clearInlineError('password-error');

        const btn = document.getElementById('confirm-password-btn');
        if (btn) { btn.disabled = true; btn.textContent = i18n.t('change-pwd-submitting'); }

        apiPost('/api/auth/change-password', { oldPassword: oldPwd, newPassword: newPwd })
            .then(res => {
                if (res && res.success) {
                    Notification.success(i18n.t('change-pwd-success-msg'), i18n.t('change-pwd-success-title'));
                    this.hideModal('change-password-modal');
                    // 修改密码后后端会踢出所有会话，需重新登录
                    setTimeout(() => {
                        localStorage.removeItem('auth_token');
                        this._showLoginPage();
                    }, 1500);
                } else {
                    showErr((res && res.error) || i18n.t('change-pwd-fail-msg'));
                }
            })
            .catch(() => {})
            .finally(() => { if (btn) { btn.disabled = false; btn.textContent = i18n.t('confirm-change-btn'); } });
    };

    // ============ 退出登录 ============

    /**
     * 退出登录
     */
    AppState.logout = function() {
        if (!confirm(i18n.t('logout-confirm') || '确定要退出登录吗？')) return;

        this._showLoginPage();
        document.getElementById('login-form') && document.getElementById('login-form').reset();

        apiPost('/api/auth/logout', {})
            .then(() => {})
            .catch(() => {})
            .finally(() => {
                localStorage.removeItem('auth_token');
                localStorage.removeItem('sessionId');
                localStorage.removeItem('password');
                sessionStorage.removeItem('savedPassword');
                sessionStorage.removeItem('currentUsername');
                Notification.success(i18n.t('logout-success'), i18n.t('logout-title'));
            });
    };

})();
