// ============================================================
// AppState 子模块: 会话管理
// 从 state.js 拆分，通过全局 AppState 对象注册
// 包含: 会话验证、登录、修改密码、退出登录
// 依赖: state.js (AppState 全局对象), fetch-api.js (apiGet/apiPost)
// ============================================================

(function() {
    'use strict';

    // The current firmware authenticates browser requests with an HttpOnly
    // session cookie.  Older releases stored a bearer token in localStorage;
    // keeping that token lets it override a fresh cookie because the server
    // intentionally gives an explicit Authorization header precedence.
    function clearLegacyAuthArtifacts() {
        localStorage.removeItem('auth_token');
        localStorage.removeItem('sessionId');
    }

    // ============ 会话验证 ============

    /**
     * 刷新页面 — 验证当前会话，若无效则尝试自动登录
     */
    AppState.refreshPage = function() {
        // Do not send a stale legacy bearer token alongside the cookie. The
        // server deliberately prefers Authorization when both are present.
        clearLegacyAuthArtifacts();
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
        // A failed/expired session must not leave a legacy token that can
        // poison the next cookie-authenticated login attempt.
        clearLegacyAuthArtifacts();
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
        var bootNavSeq = this._pageNavSeq || 0;
        var dashboardReady = false;
        try {
            dashboardReady = await this._showAppPage(bootNavSeq);
        } catch (e) {
            console.error('[Auth] Failed to prepare app shell:', e);
        }

        // 先完成轻量菜单状态请求，再加载首屏模块。C3 上两者并发会争用
        // AsyncTCP/JSON DRAM，导致 features 501/503 或 dashboard.js 被拒绝。
        if (typeof this.applyFeatureMenuVisibility === 'function') {
            try {
                await this.applyFeatureMenuVisibility();
            } catch (e) {
                // 菜单函数已 fail-closed；异常不能阻塞基础页面启动。
            }
        }

        if (dashboardReady && bootNavSeq === (this._pageNavSeq || 0) && this.currentPage === 'dashboard') {
            this._loadModule('dashboard', () => {
                if (bootNavSeq !== (this._pageNavSeq || 0) || this.currentPage !== 'dashboard') return;
                var self = this;
                if (typeof this.bootDashboardPage === 'function') {
                    this.bootDashboardPage();
                } else {
                    this.renderDashboard();
                    setTimeout(function() {
                        if (bootNavSeq === (self._pageNavSeq || 0) && self.currentPage === 'dashboard') {
                            self.loadSystemMonitor();
                        }
                    }, 120);
                }
            });
        }

        var self = this;
        var backgroundPreloadAllowance = null;
        var allowBackgroundPreload = function() {
            if (!backgroundPreloadAllowance) {
                backgroundPreloadAllowance = apiGetSilent('/api/health')
                    .then(function(payload) {
                        var memory = payload && payload.data && payload.data.memory;
                        if (!memory) return false;
                        var psramTotal = Number(memory.psramTotal || 0);
                        var psramFree = Number(memory.psramFree || 0);
                        if (psramTotal > 0) return psramFree >= 64 * 1024;
                        return Number(memory.heapFree || payload.data.freeHeap || 0) >= 64 * 1024;
                    })
                    .catch(function() { return false; });
            }
            return backgroundPreloadAllowance;
        };

        // Modal templates are fetched lazily on constrained targets. Pulling
        // the full template bundle during login competes with the first page's
        // runtime modules and can trigger an otherwise avoidable 503.
        setTimeout(function() {
            allowBackgroundPreload().then(function(allow) {
                if (!allow) {
                    console.info('[Preloader] modal templates deferred on constrained memory target');
                    return;
                }
                self._loadModals().catch(function() {
                    // PageLoader already logged the failure. Keep login usable;
                    // the first modal action will retry the shared lazy request.
                });
            });
        }, 3000);

        // Background page/API preloading is useful on larger targets but can
        // starve the classic ESP32/C3 during repeated browser sessions. Reuse
        // the same health snapshot as modal preloading so login adds one small
        // probe rather than two competing requests.
        setTimeout(function() {
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
    AppState._showAppPage = async function(expectedNavSeq) {
        var appNavSeq = expectedNavSeq === undefined ? (this._pageNavSeq || 0) : expectedNavSeq;
        document.getElementById('login-page').style.display = 'none';
        document.getElementById('app-container').classList.remove('fb-hidden');
        if (location.pathname !== '/' || location.hash) {
            history.replaceState(null, '', '/');
        }
        // Cache the first page without mounting it. A user can select another
        // menu item while this request is in flight; only the current
        // navigation is allowed to add a page root to #content-area.
        await this.loadPage('dashboard-page', { mount: false });
        var skeleton = document.getElementById('skeleton-screen');
        if (skeleton) skeleton.remove();
        if (appNavSeq !== (this._pageNavSeq || 0)) return false;

        if (typeof PageLoader !== 'undefined' && typeof PageLoader.unmountPages === 'function') {
            PageLoader.unmountPages();
        }
        var dashboardPage = (typeof PageLoader !== 'undefined' && typeof PageLoader.mountPage === 'function')
            ? PageLoader.mountPage('dashboard-page')
            : document.getElementById('dashboard-page');
        if (!dashboardPage && typeof PageLoader !== 'undefined' &&
            typeof PageLoader._renderUnavailablePage === 'function') {
            PageLoader._renderUnavailablePage('dashboard-page', new Error('Dashboard page unavailable'));
            dashboardPage = document.getElementById('dashboard-page');
        }
        if (!dashboardPage) return false;
        dashboardPage.classList.add('active');
        this.currentPage = 'dashboard';
        if (typeof this._refreshDeveloperModeState === 'function') {
            this._refreshDeveloperModeState({ silent: true, noCache: true });
        }
        setTimeout(function() {
            var sidebarLogo = document.getElementById('sidebar-logo');
            if (!sidebarLogo || sidebarLogo.getAttribute('src')) return;
            var lazySrc = sidebarLogo.getAttribute('data-lazy-src');
            if (lazySrc) sidebarLogo.setAttribute('src', lazySrc);
        }, 2000);
        if (typeof this._applyUrlParams === 'function') this._applyUrlParams();
        return !dashboardPage.querySelector('[data-page-fallback]');
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
                    // Login establishes an HttpOnly cookie. Always discard
                    // credentials left by pre-cookie firmware, including when
                    // "remember" is selected.
                    clearLegacyAuthArtifacts();
                    // Authentication is carried by the HttpOnly cookie set by
                    // the server; no session token is persisted in storage.
                    if (remember) {
                        localStorage.setItem('remember', 'true');
                        localStorage.setItem('username', username);
                        // 不再保存密码到 localStorage!
                    } else {
                        localStorage.removeItem('remember');
                        localStorage.removeItem('username');
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
    AppState.showChangePasswordModal = async function() {
        var modal = await this._ensureModalForAction('change-password-modal');
        if (!modal) return;
        this.showModal(modal);
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
