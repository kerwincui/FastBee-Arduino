// ============================================================
// Notification: 消息通知 UI 框架
// ============================================================
const Notification = {
    container: null, notifications: [],
    init() {
        this.container = document.getElementById('notification-container');
        if (!this.container) { this.container = document.createElement('div'); this.container.id = 'notification-container'; this.container.className = 'notification-container'; document.body.appendChild(this.container); }
    },
    show(options) {
        const id = 'notification-' + Date.now(); const duration = options.duration || 3000;
        const notification = document.createElement('div'); notification.id = id;
        notification.className = 'notification notification-' + (options.type || 'info');
        const icons = { primary: '✅', success: '✅', warning: '⚠️', error: '❌', info: 'ℹ️' };

        const header = document.createElement('div');
        header.className = 'notification-header';

        const title = document.createElement('div');
        title.className = 'notification-title';
        const icon = document.createElement('i');
        icon.textContent = icons[options.type] || icons.info;
        const titleText = document.createElement('span');
        titleText.textContent = options.title || this.getDefaultTitle(options.type);
        title.appendChild(icon);
        title.appendChild(titleText);

        const closeBtn = document.createElement('button');
        closeBtn.type = 'button';
        closeBtn.className = 'notification-close';
        closeBtn.textContent = '×';
        closeBtn.addEventListener('click', () => this.close(id));

        header.appendChild(title);
        header.appendChild(closeBtn);

        const body = document.createElement('div');
        body.className = 'notification-body';
        if (options.html === true) { body.innerHTML = options.message || ''; }
        else { body.textContent = options.message || ''; }

        const progress = document.createElement('div');
        progress.className = 'notification-progress';
        const progressBar = document.createElement('div');
        progressBar.className = 'notification-progress-bar';
        progressBar.style.setProperty('--notification-duration', duration + 'ms');
        progress.appendChild(progressBar);

        notification.appendChild(header);
        notification.appendChild(body);
        notification.appendChild(progress);

        this.container.appendChild(notification);
        const notificationObj = { id: id, element: notification, timeout: null }; this.notifications.push(notificationObj);
        if (options.autoClose !== false) { notificationObj.timeout = setTimeout(() => { this.close(id); }, duration); }
        return id;
    },
    close(id) {
        const notification = document.getElementById(id);
        if (notification) { notification.classList.add('hiding');
            const notificationObj = this.notifications.find(n => n.id === id);
            if (notificationObj && notificationObj.timeout) { clearTimeout(notificationObj.timeout); }
            setTimeout(() => { if (notification.parentNode) { notification.parentNode.removeChild(notification); } this.notifications = this.notifications.filter(n => n.id !== id); }, 300);
        }
    },
    closeAll() { this.notifications.forEach(n => { this.close(n.id); }); },
    getDefaultTitle(type) { const t = { primary: '提示', success: '成功', warning: '警告', error: '错误', info: '信息' }; return t[type] || '通知'; },
    primary(message, title) { return this.show({ type: 'primary', title: title || '提示', message: message }); },
    success(message, title) { return this.show({ type: 'success', title: title || '成功', message: message }); },
    warning(message, title) { return this.show({ type: 'warning', title: title || '警告', message: message }); },
    error(message, title) { return this.show({ type: 'error', title: title || '错误', message: message }); },
    info(message, title) { return this.show({ type: 'info', title: title || '信息', message: message }); }
};
