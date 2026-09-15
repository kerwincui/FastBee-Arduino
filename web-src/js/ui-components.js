/**
 * FastBee UI 组件库
 * 提供常用 UI 元素的 DOM 创建方法，替代 innerHTML 拼接
 */
(function() {
    'use strict';

    var FB = window.FB || {};

    FB.ui = {
        // 创建按钮
        createButton: function(text, options) {
            options = options || {};
            var btn = document.createElement('button');
            btn.type = options.type || 'button';
            btn.className = options.className || 'fb-btn fb-btn-primary';
            btn.textContent = text;
            if (options.onclick) btn.addEventListener('click', options.onclick);
            if (options.disabled) btn.disabled = true;
            if (options.dataset) {
                var keys = Object.keys(options.dataset);
                for (var i = 0; i < keys.length; i++) {
                    btn.dataset[keys[i]] = options.dataset[keys[i]];
                }
            }
            return btn;
        },

        // 创建表格行
        createTableRow: function(cells, options) {
            options = options || {};
            var tr = document.createElement('tr');
            if (options.className) tr.className = options.className;
            for (var i = 0; i < cells.length; i++) {
                var cell = cells[i];
                var td = document.createElement('td');
                if (typeof cell === 'string') {
                    td.textContent = cell;  // 安全：使用 textContent
                } else if (cell instanceof Node) {
                    td.appendChild(cell);
                } else if (cell && cell.html) {
                    td.innerHTML = cell.html;  // 明确标记为 HTML
                } else if (cell && cell.className) {
                    td.className = cell.className;
                    if (cell.text) td.textContent = cell.text;
                }
                if (options.cellClassNames && options.cellClassNames[i]) {
                    td.className = options.cellClassNames[i];
                }
                tr.appendChild(td);
            }
            return tr;
        },

        // 将管理表格增强为移动端纵向列表。列标题写入 data-label，
        // 具体布局统一由 main.css 控制，动态加载的页面也可重复调用。
        enhanceResponsiveTables: function(root) {
            root = root || document;
            var ui = this;
            var tables = [];
            if (root.matches && root.matches('.fb-table')) tables.push(root);
            if (root.closest) {
                var ownerTable = root.closest('.fb-table');
                if (ownerTable && tables.indexOf(ownerTable) === -1) tables.push(ownerTable);
            }
            if (root.querySelectorAll) {
                tables = tables.concat(Array.prototype.slice.call(root.querySelectorAll('.fb-table')));
            }

            tables.forEach(function(table) {
                var headers = Array.prototype.map.call(table.querySelectorAll('thead th'), function(th) {
                    return (th.textContent || '').trim();
                });
                if (!headers.length) return;
                var hasActions = headers.some(function(label) {
                    return /^(操作|动作|Action|Actions)$/i.test(label);
                });
                // CRUD tables opt in through an action column; non-CRUD tables
                // must opt in explicitly so compact status/key-value tables stay tabular.
                if (!hasActions && !table.classList.contains('fb-table-mobile')) return;

                table.classList.add('fb-table-responsive');
                // Protocol pages historically use u-overflow-x while other
                // management pages use table-container/u-table-wrap. Mark all
                // shared wrappers so the mobile list cannot retain a desktop
                // horizontal-scrolling container.
                var container = table.closest('.table-container, .u-table-wrap, .u-overflow-x, .u-overflow-x-auto');
                if (container) container.classList.add('fb-table-responsive-container');
                table.querySelectorAll('tbody tr').forEach(function(row) {
                    var cells = Array.prototype.filter.call(row.children, function(child) {
                        return child.tagName === 'TD';
                    });
                    cells.forEach(function(cell, index) {
                        if (Number(cell.getAttribute('colspan') || 1) > 1) {
                            cell.classList.add('fb-table-span-cell');
                            return;
                        }
                        var label = headers[index] || '';
                        if (label && !cell.hasAttribute('data-label')) {
                            cell.setAttribute('data-label', label);
                        }
                        if (/^(操作|动作|Action|Actions)$/i.test(label)) {
                            cell.classList.add('fb-table-action-cell', 'fb-table-action-cell--compact');
                        }
                    });
                });
                ui.enhanceMobileActionMenus(table);
            });
        },

        // CRUD 操作按钮在窄屏收进统一菜单。按钮节点本身没有被复制或替换，
        // 因此业务模块已有的事件代理、禁用状态和 data-* 属性都能继续工作。
        enhanceMobileActionMenus: function(root) {
            if (!root || !root.querySelectorAll) return;
            var actionCells = [];
            if (root.matches && root.matches('.fb-table-action-cell')) actionCells.push(root);
            actionCells = actionCells.concat(Array.prototype.slice.call(
                root.querySelectorAll('.fb-table-action-cell')
            ));

            actionCells.forEach(function(cell) {
                if (cell.classList.contains('fb-mobile-action-ready') || !cell.querySelector('button, a')) return;

                var menu = document.createElement('div');
                menu.className = 'fb-table-action-menu';
                var toggle = document.createElement('button');
                toggle.type = 'button';
                toggle.className = 'fb-table-action-menu-toggle';
                toggle.setAttribute('aria-label', '更多操作');
                toggle.setAttribute('aria-haspopup', 'menu');
                toggle.setAttribute('aria-expanded', 'false');
                toggle.title = '更多操作';
                toggle.textContent = '\u22ee';

                var panel = document.createElement('div');
                panel.className = 'fb-table-action-menu-panel';
                panel.setAttribute('role', 'menu');
                panel.setAttribute('aria-label', '操作菜单');
                while (cell.firstChild) panel.appendChild(cell.firstChild);

                menu.appendChild(toggle);
                menu.appendChild(panel);
                cell.appendChild(menu);
                cell.classList.add('fb-mobile-action-ready');

                panel.querySelectorAll('button, a').forEach(function(action) {
                    action.setAttribute('role', 'menuitem');
                });
            });
        },

        // 创建通知
        createNotification: function(type, message, duration) {
            duration = duration || 3000;
            var div = document.createElement('div');
            div.className = 'notification notification-' + type;
            div.textContent = message;
            document.body.appendChild(div);
            setTimeout(function() { div.remove(); }, duration);
            return div;
        },

        // 创建 DocumentFragment（批量 DOM 操作）
        createFragment: function() {
            return document.createDocumentFragment();
        },

        // 清空容器并批量添加子元素
        replaceChildren: function(container, children) {
            var fragment = document.createDocumentFragment();
            for (var i = 0; i < children.length; i++) {
                fragment.appendChild(children[i]);
            }
            container.innerHTML = '';
            container.appendChild(fragment);
        }
    };

    window.FB = FB;

    function getActionMenuButtons(menu) {
        return Array.prototype.filter.call(menu.querySelectorAll('.fb-table-action-menu-panel button, .fb-table-action-menu-panel a'), function(button) {
            return !button.disabled && button.getAttribute('aria-disabled') !== 'true';
        });
    }

    function closeActionMenus(except) {
        document.querySelectorAll('.fb-table-action-menu.is-open').forEach(function(menu) {
            if (menu === except) return;
            menu.classList.remove('is-open');
            var toggle = menu.querySelector('.fb-table-action-menu-toggle');
            if (toggle) toggle.setAttribute('aria-expanded', 'false');
        });
    }

    function openActionMenu(menu, focusFirst) {
        closeActionMenus(menu);
        menu.classList.add('is-open');
        var toggle = menu.querySelector('.fb-table-action-menu-toggle');
        if (toggle) toggle.setAttribute('aria-expanded', 'true');
        if (focusFirst) {
            var buttons = getActionMenuButtons(menu);
            if (buttons.length) buttons[0].focus();
        }
    }

    function startMobileActionMenuEvents() {
        document.addEventListener('click', function(event) {
            var toggle = event.target.closest && event.target.closest('.fb-table-action-menu-toggle');
            if (toggle) {
                var menu = toggle.closest('.fb-table-action-menu');
                if (!menu) return;
                event.preventDefault();
                if (menu.classList.contains('is-open')) closeActionMenus();
                else openActionMenu(menu, false);
                return;
            }

            var menuItem = event.target.closest && event.target.closest('.fb-table-action-menu-panel button, .fb-table-action-menu-panel a');
            if (menuItem) {
                closeActionMenus();
                return;
            }
            if (!event.target.closest || !event.target.closest('.fb-table-action-menu')) closeActionMenus();
        });

        document.addEventListener('keydown', function(event) {
            var toggle = event.target.closest && event.target.closest('.fb-table-action-menu-toggle');
            var item = event.target.closest && event.target.closest('.fb-table-action-menu-panel button, .fb-table-action-menu-panel a');
            var menu = (toggle || item) && (toggle || item).closest('.fb-table-action-menu');
            if (!menu) return;

            if (event.key === 'Escape') {
                event.preventDefault();
                closeActionMenus();
                var menuToggle = menu.querySelector('.fb-table-action-menu-toggle');
                if (menuToggle) menuToggle.focus();
                return;
            }
            if (toggle && (event.key === 'ArrowDown' || event.key === 'Enter' || event.key === ' ')) {
                event.preventDefault();
                openActionMenu(menu, true);
                return;
            }
            if (item && (event.key === 'ArrowDown' || event.key === 'ArrowUp')) {
                var buttons = getActionMenuButtons(menu);
                var index = buttons.indexOf(item);
                if (index === -1 || !buttons.length) return;
                event.preventDefault();
                buttons[(index + (event.key === 'ArrowDown' ? 1 : buttons.length - 1)) % buttons.length].focus();
            }
        });
    }

    function startResponsiveTableObserver() {
        FB.ui.enhanceResponsiveTables(document);
        startMobileActionMenuEvents();
        if (!document.body || typeof MutationObserver === 'undefined') return;
        var observer = new MutationObserver(function(records) {
            records.forEach(function(record) {
                record.addedNodes.forEach(function(node) {
                    if (node && node.nodeType === 1) FB.ui.enhanceResponsiveTables(node);
                });
            });
        });
        observer.observe(document.body, { childList: true, subtree: true });
    }

    if (document.readyState === 'loading') {
        document.addEventListener('DOMContentLoaded', startResponsiveTableObserver, { once: true });
    } else {
        startResponsiveTableObserver();
    }
})();
