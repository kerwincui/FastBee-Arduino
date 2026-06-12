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
                    this.currentUser.permissions = res.data.permissions || [];
                    this._bootDashboardAfterAuth();
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
                                this.currentUser.permissions = sr.data.permissions || [];
                            }
                        }).catch(() => {});

                        this._bootDashboardAfterAuth();
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

        // 初始化登录页语言选择器
        var loginLangSelect = document.getElementById('login-language-select');
        if (loginLangSelect && !loginLangSelect._bound) {
            loginLangSelect._bound = true;
            var savedLang = localStorage.getItem('language') || 'zh-CN';
            loginLangSelect.value = savedLang;
            // 若已保存英文偏好，立即切换登录页语言
            if (savedLang !== 'zh-CN' && typeof i18n !== 'undefined') {
                i18n.setLanguage(savedLang);
            }
            loginLangSelect.addEventListener('change', function() {
                var lang = loginLangSelect.value;
                localStorage.setItem('language', lang);
                if (typeof i18n !== 'undefined') {
                    i18n.setLanguage(lang);
                }
            });
        }

        // 预填充已保存的用户名和“记住密码”状态
        const savedUsername = localStorage.getItem('username');
        const savedRemember = localStorage.getItem('remember');
        const usernameInput = document.getElementById('username');
        const rememberCheckbox = document.getElementById('remember');
        if (usernameInput && savedUsername) usernameInput.value = savedUsername;
        if (rememberCheckbox && savedRemember === 'true') rememberCheckbox.checked = true;
    };

    AppState._bootDashboardAfterAuth = async function() {
        try {
            await this._showAppPage();
        } catch (e) {
            console.error('[Auth] Failed to prepare app shell:', e);
        }

        this._loadModule('dashboard', () => {
            var self = this;
            if (typeof this.bootDashboardPage === 'function') {
                this.bootDashboardPage();
            } else {
                this.renderDashboard();
                setTimeout(function() {
                    self.loadSystemMonitor();
                }, 120);
            }

            setTimeout(function() {
                if (typeof ApiPreloader !== 'undefined' && typeof ApiPreloader.preloadPageData === 'function') {
                    ApiPreloader.preloadPageData('device');
                }
                if (typeof PageLoader !== 'undefined' && typeof PageLoader.preloadPages === 'function') {
                    PageLoader.preloadPages([
                        'device-page',
                        'network-page',
                        'peripheral-page',
                        'protocol-page'
                    ], self._pageMapping, { delayMs: 220 }).catch(function() {});
                }
            }, 1600);
        });

        var self = this;
        setTimeout(function() {
            self._loadModals();
        }, 900);
    };

    /**
     * 显示主应用页面
     * @private
     */
    AppState._showAppPage = async function() {
        document.getElementById('login-page').style.display = 'none';
        document.getElementById('app-container').classList.remove('fb-hidden');
        if (location.pathname !== '/' || location.hash) {
            history.replaceState(null, '', '/');
        }
        await this.loadPage('dashboard-page');
        if (typeof this._refreshDeveloperModeState === 'function') {
            this._refreshDeveloperModeState({ silent: true, noCache: true });
        }
        var skeleton = document.getElementById('skeleton-screen');
        if (skeleton) skeleton.remove();
        setTimeout(function() {
            var sidebarLogo = document.getElementById('sidebar-logo');
            if (!sidebarLogo || sidebarLogo.getAttribute('src')) return;
            var lazySrc = sidebarLogo.getAttribute('data-lazy-src');
            if (lazySrc) sidebarLogo.setAttribute('src', lazySrc);
        }, 1500);
        if (typeof this._applyUrlParams === 'function') this._applyUrlParams();
    };

    // ============ 登录 ============

    AppState._loginErrorText = function(message) {
        var text = String(message || '');
        if (!text) return '用户名或密码错误';
        var lower = text.toLowerCase();
        if (lower.indexOf('invalid username or password') !== -1 ||
            lower.indexOf('invalid credentials') !== -1 ||
            lower.indexOf('login failed') !== -1) {
            return '用户名或密码错误';
        }
        if (lower.indexOf('too many') !== -1 && lower.indexOf('attempt') !== -1) {
            return '登录失败次数过多，账号已锁定';
        }
        if (lower.indexOf('account') !== -1 && lower.indexOf('locked') !== -1) {
            return '账号已锁定，请稍后再试';
        }
        if (lower.indexOf('ip address not allowed') !== -1) {
            return '当前 IP 不允许登录';
        }
        return text;
    };

    /**
     * 处理登录表单提交
     */
    AppState.handleLogin = function() {
        const username = (document.getElementById('username') || {}).value;
        const password = (document.getElementById('password') || {}).value;
        const remember = (document.getElementById('remember') || {}).checked;

        if (!username || !password) {
            Notification.warning('请输入用户名和密码', '登录失败');
            return;
        }

        const submitBtn = document.querySelector('#login-form button[type="submit"]');
        const originalText = submitBtn ? submitBtn.innerHTML : '';
        if (submitBtn) { submitBtn.innerHTML = '… 登录中...'; submitBtn.disabled = true; }

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
                            this.currentUser.permissions = sr.data.permissions || [];
                        }
                    }).catch(() => {});

                    this._bootDashboardAfterAuth();
                    Notification.success('登录成功', '欢迎');
                } else {
                    Notification.error(this._loginErrorText(res && res.error), '登录失败');
                }
            })
            .catch((err) => {
                // 登录失败，显示错误信息
                const errorMsg = this._loginErrorText(err && err.data && err.data.error);
                Notification.error(errorMsg, '登录失败');
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
            Notification.error(msg, '修改密码失败');
        };

        if (!oldPwd || !newPwd || !confirmPwd) return showErr('请填写所有字段！');
        if (newPwd !== confirmPwd) return showErr('新密码与确认密码不一致！');
        if (newPwd.length < 6) return showErr('新密码长度至少6位！');

        this.clearInlineError('password-error');

        const btn = document.getElementById('confirm-password-btn');
        if (btn) { btn.disabled = true; btn.textContent = '提交中...'; }

        apiPost('/api/auth/change-password', { oldPassword: oldPwd, newPassword: newPwd })
            .then(res => {
                if (res && res.success) {
                    Notification.success('密码修改成功，请重新登录', '修改成功');
                    this.hideModal('change-password-modal');
                    // 修改密码后后端会踢出所有会话，需重新登录
                    setTimeout(() => {
                        localStorage.removeItem('auth_token');
                        this._showLoginPage();
                    }, 1500);
                } else {
                    showErr((res && res.error) || '密码修改失败，请检查原密码是否正确');
                }
            })
            .catch(() => {})
            .finally(() => { if (btn) { btn.disabled = false; btn.textContent = '确认修改'; } });
    };

    // ============ 退出登录 ============

    /**
     * 退出登录
     */
    AppState.logout = function() {
        if (!confirm('确定要退出登录吗？')) return;

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
                Notification.success('已成功退出登录', '退出登录');
            });
    };

    // ============ 权限工具方法 ============

    /**
     * 检查当前用户是否拥有指定权限
     * @param {string} permission - 权限标识符，如 'user.admin', 'config.edit'
     * @returns {boolean}
     */
    AppState.hasPermission = function(permission) {
        if (!permission) return false;
        var role = this.currentUser.role;
        // admin 角色拥有所有权限
        if (role === 'ADMIN') return true;
        var perms = this.currentUser.permissions;
        if (!perms || !perms.length) return false;
        return perms.indexOf(permission) !== -1;
    };

    /**
     * 检查当前用户是否拥有任一指定权限
     * @param {...string} permissions - 权限标识符列表
     * @returns {boolean}
     */
    AppState.hasAnyPermission = function() {
        if (this.currentUser.role === 'ADMIN') return true;
        var perms = this.currentUser.permissions;
        if (!perms || !perms.length) return false;
        for (var i = 0; i < arguments.length; i++) {
            if (perms.indexOf(arguments[i]) !== -1) return true;
        }
        return false;
    };

})();
