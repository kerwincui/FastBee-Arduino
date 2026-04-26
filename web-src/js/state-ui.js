// ============================================================
// AppState 子模块: UI 工具方法
// 从 state.js 拆分，通过全局 AppState 对象注册
// 依赖: state.js (AppState 全局对象)
// 包含: 元素显隐、模态框、表格辅助、按钮状态、侧边栏、用户下拉菜单
// ============================================================

(function() {
    'use strict';

    if (typeof AppState === 'undefined') {
        console.error('[state-ui] AppState not found');
        return;
    }

    // ============ 元素获取 ============
    AppState.getEl = function(ref) {
        if (!ref) return null;
        return typeof ref === 'string' ? document.getElementById(ref) : ref;
    };

    // ============ 元素显隐 ============
    AppState.showElement = function(ref, displayValue) {
        var el = this.getEl(ref);
        if (!el) return null;
        el.classList.remove('is-hidden');
        if (displayValue) {
            el.style.display = displayValue;
        } else {
            el.style.removeProperty('display');
        }
        return el;
    };

    AppState.hideElement = function(ref) {
        var el = this.getEl(ref);
        if (!el) return null;
        el.classList.add('is-hidden');
        el.style.display = 'none';
        return el;
    };

    AppState.toggleVisible = function(ref, show) {
        var el = this.getEl(ref);
        if (!el) return null;
        if (show) {
            return this.showElement(el);
        } else {
            return this.hideElement(el);
        }
    };

    // ============ 模态框 ============
    AppState.showModal = function(ref) {
        var el = this.showElement(ref, 'flex');
        if (el) {
            var self = this;
            // 点击遮罩层关闭
            el._modalOverlayHandler = function(e) {
                if (e.target === el) {
                    self.hideModal(ref);
                }
            };
            el.addEventListener('click', el._modalOverlayHandler);
                
            // 绑定关闭按钮
            var closeBtn = el.querySelector('.modal-close, .modal-close-btn');
            if (closeBtn) {
                closeBtn._modalCloseHandler = function() {
                    self.hideModal(ref);
                };
                closeBtn.addEventListener('click', closeBtn._modalCloseHandler);
            }
        }
        return el;
    };

    AppState.hideModal = function(ref) {
        var el = this.getEl(ref);
        if (!el) return null;
        // 清理遮罩点击事件
        if (el._modalOverlayHandler) {
            el.removeEventListener('click', el._modalOverlayHandler);
            el._modalOverlayHandler = null;
        }
        // 清理关闭按钮事件
        var closeBtn = el.querySelector('.modal-close, .modal-close-btn');
        if (closeBtn && closeBtn._modalCloseHandler) {
            closeBtn.removeEventListener('click', closeBtn._modalCloseHandler);
            closeBtn._modalCloseHandler = null;
        }
        el.classList.add('is-hidden');
        el.style.display = 'none';
        return el;
    };

    // ============ 内联错误提示 ============
    AppState.showInlineError = function(ref, message) {
        var el = this.getEl(ref);
        if (!el) return null;
        el.textContent = message || '';
        return this.showElement(el, 'block');
    };

    AppState.clearInlineError = function(ref) {
        var el = this.getEl(ref);
        if (!el) return null;
        el.textContent = '';
        return this.hideElement(el);
    };

    // ============ 表格辅助 ============
    AppState.renderEmptyTableRow = function(tbodyRef, colspan, text, className) {
        var tbody = this.getEl(tbodyRef);
        if (!tbody) return null;
        tbody.innerHTML = '';
        var row = document.createElement('tr');
        var cell = document.createElement('td');
        cell.colSpan = colspan;
        cell.className = className || 'u-empty-cell';
        cell.textContent = text || '';
        row.appendChild(cell);
        tbody.appendChild(row);
        return row;
    };

    AppState.renderPagination = function(containerRef, options) {
        var container = this.getEl(containerRef);
        if (!container) return null;

        var total = Math.max(0, Number(options && options.total) || 0);
        var pageSize = Math.max(1, Number(options && options.pageSize) || 10);
        var totalPages = Math.max(1, Math.ceil(total / pageSize));
        var currentPage = Math.min(Math.max(1, Number(options && options.page) || 1), totalPages);
        var maxVisiblePages = Math.max(3, Number(options && options.maxVisiblePages) || 5);
        var summaryText = (options && options.summaryText) || '';
        var onPageChange = (options && typeof options.onPageChange === 'function') ? options.onPageChange : null;

        container.innerHTML = '';

        var wrap = document.createElement('div');
        wrap.className = 'pagination u-pagination';

        var appendButton = function(label, targetPage, disabled, active) {
            var button = document.createElement('button');
            button.type = 'button';
            button.className = 'fb-btn fb-btn-sm' + (active ? ' fb-btn-primary' : '');
            button.textContent = label;
            button.disabled = !!disabled;
            if (!button.disabled && onPageChange) {
                (function(pg) {
                    button.addEventListener('click', function() { onPageChange(pg); });
                })(targetPage);
            }
            wrap.appendChild(button);
        };

        var appendEllipsis = function() {
            var ellipsis = document.createElement('span');
            ellipsis.className = 'u-pagination-ellipsis';
            ellipsis.textContent = '...';
            wrap.appendChild(ellipsis);
        };

        var appendSummary = function() {
            if (!summaryText) return;
            var summary = document.createElement('span');
            summary.className = 'u-pagination-summary';
            summary.textContent = summaryText;
            wrap.appendChild(summary);
        };

        if (totalPages <= 1) {
            appendSummary();
            container.appendChild(wrap);
            return wrap;
        }

        appendButton('«', currentPage - 1, currentPage <= 1, false);

        var startPage = Math.max(1, currentPage - Math.floor(maxVisiblePages / 2));
        var endPage = Math.min(totalPages, startPage + maxVisiblePages - 1);
        if (endPage - startPage + 1 < maxVisiblePages) {
            startPage = Math.max(1, endPage - maxVisiblePages + 1);
        }

        if (startPage > 1) {
            appendButton('1', 1, false, false);
            if (startPage > 2) appendEllipsis();
        }

        for (var page = startPage; page <= endPage; page++) {
            appendButton(String(page), page, page === currentPage, page === currentPage);
        }

        if (endPage < totalPages) {
            if (endPage < totalPages - 1) appendEllipsis();
            appendButton(String(totalPages), totalPages, false, false);
        }

        appendButton('»', currentPage + 1, currentPage >= totalPages, false);
        appendSummary();

        container.appendChild(wrap);
        return wrap;
    };

    // ============ 按钮状态 ============
    AppState.setLoading = function(ref, text) {
        var el = this.getEl(ref);
        if (!el) return null;
        if (!el.hasAttribute('data-original-text')) {
            el.setAttribute('data-original-text', el.textContent);
        }
        el.disabled = true;
        el.textContent = text || (typeof i18n !== 'undefined' ? i18n.t('saving') : '保存中...');
        el.classList.add('is-loading');
        return el;
    };

    AppState.restoreButton = function(ref, text) {
        var el = this.getEl(ref);
        if (!el) return null;
        el.disabled = false;
        el.textContent = text || el.getAttribute('data-original-text') || '';
        el.classList.remove('is-loading');
        el.removeAttribute('data-original-text');
        return el;
    };

    // ============ 徽章 ============
    AppState.renderBadge = function(type, text) {
        var span = document.createElement('span');
        span.className = 'badge badge-' + (type || 'info');
        span.textContent = text || '';
        return span;
    };

    // ============ 排他激活 ============
    AppState.setExclusiveActive = function(containerRef, selector, activeEl, className) {
        var container = this.getEl(containerRef);
        if (!container) return;
        var activeClass = className || 'is-active';
        container.querySelectorAll(selector).forEach(function(el) {
            el.classList.toggle(activeClass, el === activeEl);
        });
    };

    // ============ 用户下拉菜单 ============
    AppState.setupUserDropdown = function() {
        var dropdownBtn = document.getElementById('user-dropdown-btn');
        var dropdownMenu = document.getElementById('user-dropdown-menu');

        if (!dropdownBtn || !dropdownMenu) return;

        // 切换下拉菜单显示
        dropdownBtn.addEventListener('click', function(e) {
            e.stopPropagation();
            var dropdown = dropdownBtn.closest('.user-dropdown');
            dropdown.classList.toggle('open');
        });

        // 点击外部关闭下拉菜单
        document.addEventListener('click', function(e) {
            if (!dropdownBtn.contains(e.target) && !dropdownMenu.contains(e.target)) {
                var dropdown = dropdownBtn.closest('.user-dropdown');
                if (dropdown) dropdown.classList.remove('open');
            }
        });

        // 点击菜单项后关闭下拉菜单
        dropdownMenu.querySelectorAll('.dropdown-item').forEach(function(item) {
            item.addEventListener('click', function() {
                var dropdown = dropdownBtn.closest('.user-dropdown');
                if (dropdown) dropdown.classList.remove('open');
            });
        });
    };

    // ============ 通用折叠面板 ============
    AppState.toggleSection = function(bodyId) {
        var body = document.getElementById(bodyId);
        var icon = document.getElementById(bodyId + '-icon');
        if (!body) return;
        if (body.classList.contains('fb-hidden')) {
            body.classList.remove('fb-hidden');
            body.style.display = '';
            if (icon) icon.innerHTML = '&#9660;';
        } else {
            body.classList.add('fb-hidden');
            body.style.display = '';
            if (icon) icon.innerHTML = '&#9654;';
        }
    };

    // ============ 侧边栏 ============
    AppState.setupSidebarToggle = function() {
        var self = this;
        var btn = document.getElementById('sidebar-toggle');
        if (btn) btn.addEventListener('click', function() { self.toggleSidebar(); });
        if (localStorage.getItem('sidebarCollapsed') === 'true') this.collapseSidebar();
    };

    AppState.toggleSidebar = function() {
        // 移动端使用 expanded 类，桌面端使用 collapsed 类
        var sidebar = document.getElementById('sidebar');
        if (!sidebar) return;

        // 检测是否为移动端
        var isMobile = window.innerWidth <= 768;

        if (isMobile) {
            // 移动端：切换 expanded 类
            if (sidebar.classList.contains('expanded')) {
                sidebar.classList.remove('expanded');
                this.sidebarCollapsed = true;
                var btn = document.getElementById('sidebar-toggle');
                if (btn) btn.textContent = '☰';
            } else {
                sidebar.classList.add('expanded');
                this.sidebarCollapsed = false;
                var btn2 = document.getElementById('sidebar-toggle');
                if (btn2) btn2.textContent = '✕';
            }
        } else {
            // 桌面端：使用原有逻辑
            this.sidebarCollapsed ? this.expandSidebar() : this.collapseSidebar();
        }
    };

    AppState.collapseSidebar = function() {
        var sidebar = document.getElementById('sidebar');
        if (sidebar) {
            sidebar.classList.add('collapsed');
            sidebar.classList.remove('expanded');
            this.sidebarCollapsed = true;
            localStorage.setItem('sidebarCollapsed', 'true');
            var btn = document.getElementById('sidebar-toggle');
            if (btn) btn.textContent = '☰';
        }
    };

    AppState.expandSidebar = function() {
        var sidebar = document.getElementById('sidebar');
        if (sidebar) {
            sidebar.classList.remove('collapsed');
            this.sidebarCollapsed = false;
            localStorage.setItem('sidebarCollapsed', 'false');
            var btn = document.getElementById('sidebar-toggle');
            if (btn) btn.textContent = '✕';
        }
    };

})();
