// ============================================================
// AppState 子模块: 主题管理
// 从 state.js 拆分，通过全局 AppState 对象注册
// 依赖: state.js (AppState 全局对象)
// ============================================================

(function() {
    'use strict';

    /**
     * 初始化主题设置
     * 优先级: 用户手动设置 > 系统偏好
     */
    AppState.setupTheme = function() {
        const savedTheme = localStorage.getItem('theme');
        const systemPrefersDark = window.matchMedia('(prefers-color-scheme: dark)').matches;

        let theme;
        if (savedTheme) {
            theme = savedTheme;
        } else {
            theme = systemPrefersDark ? 'dark' : 'light';
            // 标记为自动模式（未手动设置）
            localStorage.setItem('theme-auto', 'true');
        }

        this.setTheme(theme, false);
        this.updateThemeToggleIcon(theme);

        // 绑定主题切换按钮 (下拉菜单中的)
        const themeToggleItem = document.getElementById('theme-toggle-item');
        if (themeToggleItem) {
            themeToggleItem.addEventListener('click', () => this.toggleTheme());
        }

        // 监听系统主题变化（仅在自动模式下）
        window.matchMedia('(prefers-color-scheme: dark)').addEventListener('change', (e) => {
            const isAutoMode = localStorage.getItem('theme-auto') === 'true';
            if (isAutoMode) {
                const newTheme = e.matches ? 'dark' : 'light';
                this.setTheme(newTheme, false);
                this.updateThemeToggleIcon(newTheme);
            }
        });
    };

    /**
     * 设置主题
     * @param {string} theme - 'dark' 或 'light'
     * @param {boolean} [isManual=true] - 是否为手动设置
     */
    AppState.setTheme = function(theme, isManual) {
        if (typeof isManual === 'undefined') isManual = true;
        document.documentElement.setAttribute('data-theme', theme);
        localStorage.setItem('theme', theme);

        // 如果是手动设置，清除自动模式标记
        if (isManual) {
            localStorage.removeItem('theme-auto');
        }

        this.updateThemeToggleIcon(theme);
    };

    /**
     * 切换主题（dark ↔ light）
     */
    AppState.toggleTheme = function() {
        const current = document.documentElement.getAttribute('data-theme');
        const newTheme = current === 'dark' ? 'light' : 'dark';
        this.setTheme(newTheme, true);
    };

    /**
     * 更新主题切换按钮的图标和文字
     * @param {string} theme - 当前主题
     */
    AppState.updateThemeToggleIcon = function(theme) {
        const themeToggleItem = document.getElementById('theme-toggle-item');
        if (themeToggleItem) {
            const iconSpan = themeToggleItem.querySelector('.item-icon');
            const textSpan = themeToggleItem.querySelector('span:not(.item-icon)');

            if (iconSpan) {
                iconSpan.textContent = theme === 'dark' ? '☀' : '🌙';
            }
            if (textSpan) {
                // 获取当前语言的翻译
                const key = theme === 'dark' ? 'theme-light' : 'theme-dark';
                const translated = window.i18n ? window.i18n.t(key) : (theme === 'dark' ? '浅色模式' : '深色模式');
                textSpan.textContent = translated;
            }
        }
    };

})();
