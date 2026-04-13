// 消息通知系统
const Notification = {
    container: null,
    notifications: [],
    
    init() {
        this.container = document.getElementById('notification-container');
        if (!this.container) {
            this.container = document.createElement('div');
            this.container.id = 'notification-container';
            this.container.className = 'notification-container';
            document.body.appendChild(this.container);
        }
    },
    
    /**
     * 显示通知
     * @param {Object} options 配置选项
     * @param {string} options.type 类型: primary, success, warning, error, info
     * @param {string} options.title 标题
     * @param {string} options.message 消息内容
     * @param {number} options.duration 持续时间(毫秒)，默认3000
     * @param {boolean} options.autoClose 是否自动关闭，默认true
     */
    show(options) {
        const id = 'notification-' + Date.now();
        const duration = options.duration || 3000;
        
        const notification = document.createElement('div');
        notification.id = id;
        notification.className = `notification notification-${options.type || 'info'}`;
        
        const icons = {
            primary: '✅',
            success: '✅',
            warning: '⚠️',
            error: '❌',
            info: 'ℹ️'
        };

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
        body.innerHTML = options.message || '';

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
        
        // 添加到通知列表
        const notificationObj = {
            id,
            element: notification,
            timeout: null
        };
        
        this.notifications.push(notificationObj);
        
        // 设置自动关闭
        if (options.autoClose !== false) {
            notificationObj.timeout = setTimeout(() => {
                this.close(id);
            }, duration);
        }
        
        return id;
    },
    
    /**
     * 关闭通知
     * @param {string} id 通知ID
     */
    close(id) {
        const notification = document.getElementById(id);
        if (notification) {
            notification.classList.add('hiding');
            
            // 清除对应的定时器
            const notificationObj = this.notifications.find(n => n.id === id);
            if (notificationObj && notificationObj.timeout) {
                clearTimeout(notificationObj.timeout);
            }
            
            // 动画结束后移除元素
            setTimeout(() => {
                if (notification.parentNode) {
                    notification.parentNode.removeChild(notification);
                }
                
                // 从列表中移除
                this.notifications = this.notifications.filter(n => n.id !== id);
            }, 300);
        }
    },
    
    /**
     * 关闭所有通知
     */
    closeAll() {
        this.notifications.forEach(notification => {
            this.close(notification.id);
        });
    },
    
    /**
     * 获取默认标题
     * @param {string} type 通知类型
     * @returns {string} 默认标题
     */
    getDefaultTitle(type) {
        const titles = {
            primary: '提示',
            success: '成功',
            warning: '警告',
            error: '错误',
            info: '信息'
        };
        return titles[type] || '通知';
    },
    
    /**
     * 快速显示主要通知
     * @param {string} message 消息内容
     * @param {string} title 标题
     */
    primary(message, title) {
        return this.show({
            type: 'primary',
            title: title || '提示',
            message: message
        });
    },
    
    /**
     * 快速显示成功通知
     * @param {string} message 消息内容
     * @param {string} title 标题
     */
    success(message, title) {
        return this.show({
            type: 'success',
            title: title || '成功',
            message: message
        });
    },
    
    /**
     * 快速显示警告通知
     * @param {string} message 消息内容
     * @param {string} title 标题
     */
    warning(message, title) {
        return this.show({
            type: 'warning',
            title: title || '警告',
            message: message
        });
    },
    
    /**
     * 快速显示错误通知
     * @param {string} message 消息内容
     * @param {string} title 标题
     */
    error(message, title) {
        return this.show({
            type: 'error',
            title: title || '错误',
            message: message
        });
    },
    
    /**
     * 快速显示信息通知
     * @param {string} message 消息内容
     * @param {string} title 标题
     */
    info(message, title) {
        return this.show({
            type: 'info',
            title: title || '信息',
            message: message
        });
    }
};
