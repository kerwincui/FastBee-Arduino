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

        // Prefer the HttpOnly session cookie. A legacy bearer token is still
        // accepted when present so an already-open tab can migrate cleanly.
        const savedRemember = localStorage.getItem('remember');
        const savedUsername = localStorage.getItem('username');

        apiGet('/api/auth/session')
            .then(res => {
                if (res && res.success && res.data && res.data.sessionValid) {
                    this.currentUser.name = res.data.username || 'Admin';
                    this._bootDashboardAfterAuth();
                } else {
                    // 会话无效，仅基于 sessionid 重试（不再依赖密码）
                    this._showLoginPage();
                }
            })
            .catch(() => {
                // 会话验证失败（如 401），仅基于 sessionid 重试
                this._showLoginPage();
            });
    };

    /**
     * 尝试使用保存的 Session ID 自动登录（基于 token 而非密码）
     * @param {string} remember - 是否记住登录状态
     * @param {string} username - 用户名
     * @private
     */
    AppState._tryAutoLogin = function(remember, username) {
        localStorage.removeItem('auth_token');
        localStorage.removeItem('sessionId');
        localStorage.removeItem('remember');
        this._showLoginPage();
    };

    /**
     * 显示登录页面
     * @private
     */
    AppState._showLoginPage = function() {
        // Release the long-lived event stream as soon as the session leaves the
        // application. This prevents closed/expired browser sessions from
        // retaining an SSE socket on memory-constrained targets.
        if (typeof this.disconnectSSE === 'function') this.disconnectSSE();
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

        // 预填充已保存的用户名，默认不勾选"保持登录"
        const savedUsername = localStorage.getItem('username');
        const savedRemember = localStorage.getItem('remember') || 'false';
        const usernameInput = document.getElementById('username');
        const rememberCheckbox = document.getElementById('remember');
        if (usernameInput && savedUsername) usernameInput.value = savedUsername;
        if (rememberCheckbox) rememberCheckbox.checked = (savedRemember === 'true');
        
        // 清理历史密码字段 (安全加固)
        if (localStorage.getItem('password')) {
            const oldPwd = localStorage.getItem('password');
            localStorage.removeItem('password');
            console.warn('[WP-13] Cleared legacy password from localStorage for security');
        }
    };

    AppState._bootDashboardAfterAuth = async function() {
        try {
            await this._showAppPage();
        } catch (e) {
            console.error('[Auth] Failed to prepare app shell:', e);
        }

        // 按功能配置显示/隐藏左侧菜单项（失败静默，不阻塞启动）
        if (typeof this.applyFeatureMenuVisibility === 'function') {
            this.applyFeatureMenuVisibility();
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

        });

        var self = this;
        setTimeout(function() {
            self._loadModals();
        }, 3000);

        // Background page/API preloading is useful on larger targets but can
        // starve the classic ESP32/C3 during repeated browser sessions. Read
        // the public health snapshot first and only preload when there is a
        // meaningful memory reserve (PSRAM is preferred when present).
        setTimeout(function() {
            var allowBackgroundPreload = function() {
                return fetch('/api/health', { cache: 'no-store' })
                    .then(function(resp) { return resp.ok ? resp.json() : null; })
                    .then(function(payload) {
                        var memory = payload && payload.data && payload.data.memory;
                        if (!memory) return false;
                        var psramTotal = Number(memory.psramTotal || 0);
                        var psramFree = Number(memory.psramFree || 0);
                        if (psramTotal > 0) return psramFree >= 64 * 1024;
                        return Number(memory.heapFree || payload.data.freeHeap || 0) >= 64 * 1024;
                    })
                    .catch(function() { return false; });
            };

            allowBackgroundPreload().then(function(allow) {
                if (!allow) {
                    console.info('[Preloader] skipped on constrained memory target');
                    return;
                }
                if (typeof ApiPreloader !== 'undefined' && typeof ApiPreloader.preloadPageData === 'function') {
                    ApiPreloader.preloadPageData('device');
                }
                if (typeof PageLoader !== 'undefined' && typeof PageLoader.preloadPages === 'function') {
                    PageLoader.preloadPages([
                        'device-page',
                        'network-page',
                        'peripheral-page',
                        'protocol-page'
                    ], self._pageMapping, { delayMs: 1200 }).catch(function() {});
                }
            });
        }, 5000);
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
        }, 2000);
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
                    // Authentication is carried by the HttpOnly cookie set by
                    // the server; no session token is persisted in storage.
                    if (remember) {
                        localStorage.setItem('remember', 'true');
                        localStorage.setItem('username', username);
                        // 不再保存密码到 localStorage!
                    } else {
                        ['remember', 'username', 'sessionId', 'auth_token'].forEach(k => localStorage.removeItem(k));
                        // 同时清理密码字段 (即使之前存在)
                        if (localStorage.getItem('password')) {
                            localStorage.removeItem('password');
                            console.warn('[WP-13] Removed password field from localStorage');
                        }
                    }

                    this.currentUser.name = res.username || username;
                    sessionStorage.setItem('currentUsername', this.currentUser.name);

                    this._bootDashboardAfterAuth();
                    Notification.success('登录成功', '欢迎');
                } else {
                    Notification.error(this._loginErrorText(res && res.error), '登录失败');
                }
            })
            .catch((err) => {
                // 区分超时/连接错误/凭据错误
                if (err && err.name === 'AbortError') {
                    Notification.warning('请求超时，请检查设备是否在线后重试', '登录超时');
                } else if (err instanceof TypeError) {
                    Notification.error('无法连接设备，请检查网络或设备 IP', '连接失败');
                } else {
                    // 登录失败，显示错误信息
                    const errorMsg = this._loginErrorText(err && err.data && err.data.error);
                    Notification.error(errorMsg, '登录失败');
                }
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

})();
