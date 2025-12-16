// axios全局配置
axios.defaults.baseURL = 'http://fastbee.local';
axios.defaults.timeout = 3000; // 5秒超时

// 如果需要，可以设置每次请求默认携带的参数或认证信息

 // 1. 配置请求拦截器[citation:1]
axios.interceptors.request.use(
    function (config) {
        // 发送请求前可修改配置，如添加统一请求头[citation:2]
        console.log('请求即将发送:', config.url);
        config.headers['X-Requested-With'] = 'XMLHttpRequest';
        // 示例：添加认证令牌（需自行实现获取逻辑）
        // const token = localStorage.getItem('auth_token');
        // if (token) config.headers.Authorization = `Bearer ${token}`;
        const cookie = localStorage.getItem('Cookie');
        if (cookie) config.headers.Authorization = `bear 123456`;
        config.headers.Cookie = "fastbee_session=authenticated"
        config.withCredentials = true
        return config; // 必须返回配置对象[citation:2]
    },
    function (error) {
        // 处理请求错误（如配置无效）
        return Promise.reject(error);
    }
);

// 2. 配置响应拦截器[citation:1]
axios.interceptors.response.use(
    function (response) {
        // 对2xx范围内的响应状态码进行处理[citation:5]
        console.log('收到响应:', response.status);
        // 可以统一处理响应数据格式，例如直接返回data部分[citation:2]
        return response.data;
    },
    function (error) {
        // 处理2xx范围外的响应错误[citation:5]
        if (error.response) {
            // 服务器有响应但状态码错误
            console.error('请求失败，状态码:', error.response.status);
            // 可根据状态码进行统一处理，如401跳转登录[citation:4]
        } else if (error.request) {
            // 请求已发出但无响应（网络问题）
            console.error('网络错误，无服务器响应');
        } else {
            // 请求配置出错
            console.error('请求配置错误:', error.message);
        }
        return Promise.reject(error); // 将错误继续抛出，以便在具体请求的catch中处理[citation:2]
    }
);

// 系统状态
const systemState = {
    isAuthenticated: false,
    currentUser: null,
    currentSession: null,
    activePage: 'dashboard'
};

// 登录功能
document.getElementById('login-form').addEventListener('submit', async function(e) {
    e.preventDefault();
    // 调试使用
    // document.getElementById('login-page').style.display = 'none';
    // document.getElementById('app').style.display = 'flex';
    // return;

    const username = document.getElementById('username').value.trim();
    const password = document.getElementById('password').value;
    const rememberMe = document.getElementById('remember-me').checked;
    
    // 简单验证
    if (!username || !password) {
        showNotification('请输入用户名和密码', 'error');
        return;
    }
    
    // 显示加载状态
    const submitBtn = e.target.querySelector('button[type="submit"]');
    const originalText = submitBtn.innerHTML;
    submitBtn.innerHTML = '<i class="fas fa-spinner fa-spin"></i> 登录中...';
    submitBtn.disabled = true;
    
    // 登录请求
    axios.post('/login', new URLSearchParams({
        username: username,
        password: password
    })
    ).then(response => {
        console.log('登录请求', response);
        if(response.status==1){
            // 认证成功
            systemState.isAuthenticated = true;
            systemState.currentUser = {
                username: 'admin',
                role: 'ADMIN',
                email: 'admin@example.com'
            };
            
            // 保存登录状态
            if (rememberMe) {
                localStorage.setItem('rememberMe', 'true');
                localStorage.setItem('username', username);
                localStorage.setItem('password', password);
            } else {
                localStorage.removeItem('rememberMe');
                localStorage.removeItem('username');
                localStorage.removeItem('password');
            }
            
            // 保存会话
            sessionStorage.setItem('userRole', 'ADMIN');

            document.getElementById('login-page').style.display = 'none';
            document.getElementById('app').style.display = 'flex';

            showNotification(response.msg,"error");
        }else{
            showNotification(response.msg , "error");
        }
        
    }).catch(error=>{
        showNotification("登录发生错误" , "error");
    }).finally(function(){
        // 恢复按钮状态
        submitBtn.innerHTML = originalText;
        submitBtn.disabled = false;
    });

});

// 检查自动登录
function checkAutoLogin() {
    const rememberMe = localStorage.getItem('rememberMe');
    const savedUsername = localStorage.getItem('username');
    const savedPassowrd = localStorage.getItem('password');
    
    if (rememberMe === 'true' && savedUsername) {
        document.getElementById('username').value = savedUsername;
        document.getElementById('password').value = savedPassowrd;
        document.getElementById('remember-me').checked = true;
    }
}


// 菜单切换功能
const menuItems = document.querySelectorAll('.menu-item');
const pages = document.querySelectorAll('.page');
const pageTitle = document.querySelector('.page-title');

menuItems.forEach(item => {
    item.addEventListener('click', function() {
        // 更新菜单激活状态
        menuItems.forEach(mi => mi.classList.remove('active'));
        this.classList.add('active');
        
        // 显示对应页面
        const pageId = this.getAttribute('data-page');
        pages.forEach(page => {
            page.classList.remove('active');
            if (page.id === pageId) {
                page.classList.add('active');
            }
        });
        
        // 更新页面标题
        pageTitle.textContent = this.querySelector('span').textContent;
    });
});

// 退出登录功能
document.getElementById('logout-btn').addEventListener('click', function() {
    document.getElementById('app').style.display = 'none';
    document.getElementById('login-page').style.display = 'flex';
    document.getElementById('login-form').reset();
});

// 修改密码功能
const changePasswordBtn = document.getElementById('change-password-btn');
const changePasswordModal = document.getElementById('change-password-modal');
const cancelPasswordBtn = document.getElementById('cancel-password-btn');
const modalClose = document.querySelector('.modal-close');
const confirmPasswordBtn = document.getElementById('confirm-password-btn');

changePasswordBtn.addEventListener('click', function() {
    changePasswordModal.style.display = 'flex';
});

function closePasswordModal() {
    changePasswordModal.style.display = 'none';
    document.getElementById('change-password-form').reset();
    document.getElementById('password-error').style.display = 'none';
    document.getElementById('password-success').style.display = 'none';
}

cancelPasswordBtn.addEventListener('click', closePasswordModal);
modalClose.addEventListener('click', closePasswordModal);

confirmPasswordBtn.addEventListener('click', function() {
    const currentPassword = document.getElementById('current-password').value;
    const newPassword = document.getElementById('new-password').value;
    const confirmPassword = document.getElementById('confirm-password').value;
    const errorDiv = document.getElementById('password-error');
    const successDiv = document.getElementById('password-success');
    
    // 简单验证
    if (currentPassword !== 'admin') {
        errorDiv.textContent = '当前密码不正确';
        errorDiv.style.display = 'block';
        successDiv.style.display = 'none';
        return;
    }
    
    if (newPassword !== confirmPassword) {
        errorDiv.textContent = '新密码与确认密码不匹配';
        errorDiv.style.display = 'block';
        successDiv.style.display = 'none';
        return;
    }
    
    if (newPassword.length < 6) {
        errorDiv.textContent = '新密码长度至少6位';
        errorDiv.style.display = 'block';
        successDiv.style.display = 'none';
        return;
    }
    
    // 模拟修改密码成功
    errorDiv.style.display = 'none';
    successDiv.style.display = 'block';
    
    // 3秒后关闭模态框
    setTimeout(() => {
        closePasswordModal();
    }, 2000);
});

// 配置选项卡切换
const configTabs = document.querySelectorAll('.config-tab');
const configContents = document.querySelectorAll('.config-content');

configTabs.forEach(tab => {
    tab.addEventListener('click', function() {
        // 更新选项卡激活状态
        configTabs.forEach(t => t.classList.remove('active'));
        this.classList.add('active');
        
        // 显示对应内容
        const tabId = this.getAttribute('data-tab');
        configContents.forEach(content => {
            content.classList.remove('active');
            if (content.id === tabId) {
                content.classList.add('active');
            }
        });
    });
});

// 表单提交处理
const forms = document.querySelectorAll('form');
forms.forEach(form => {
    form.addEventListener('submit', function(e) {
        e.preventDefault();
        const successId = this.id.replace('-form', '-success');
        const successDiv = document.getElementById(successId);
        
        if (successDiv) {
            successDiv.style.display = 'block';
            setTimeout(() => {
                successDiv.style.display = 'none';
            }, 3000);
        }
    });
});

// 初始化页面
document.getElementById('password-error').style.display = 'none';
document.getElementById('password-success').style.display = 'none';

// 隐藏所有成功消息
const successAlerts = document.querySelectorAll('.alert-success');
successAlerts.forEach(alert => {
    alert.style.display = 'none';
});

// 显示通知
function showNotification(message, type = 'info') {
    // 创建通知元素
    const notification = document.createElement('div');
    notification.className = `toast-notification ${type}`;
    notification.innerHTML = `
        <div class="toast-icon">
            <i class="fas fa-${type === 'success' ? 'check-circle' : type === 'error' ? 'exclamation-circle' : 'info-circle'}"></i>
        </div>
        <div class="toast-content">
            <p>${message}</p>
        </div>
        <button class="toast-close">&times;</button>
    `;
    
    // 添加到页面
    document.body.appendChild(notification);
    
    // 添加关闭事件
    notification.querySelector('.toast-close').addEventListener('click', () => {
        notification.style.opacity = '0';
        setTimeout(() => notification.remove(), 300);
    });
    
    // 自动消失
    setTimeout(() => {
        if (notification.parentNode) {
            notification.style.opacity = '0';
            setTimeout(() => notification.remove(), 300);
        }
    }, 5000);
    
    // 添加样式
    if (!document.querySelector('#toast-styles')) {
        const style = document.createElement('style');
        style.id = 'toast-styles';
        style.textContent = `
            .toast-notification {
                position: fixed;
                top: 20px;
                right: 20px;
                background: white;
                border-radius: var(--border-radius);
                padding: 15px 20px;
                display: flex;
                align-items: center;
                gap: 15px;
                box-shadow: var(--shadow-lg);
                z-index: 9999;
                max-width: 350px;
                transform: translateX(0);
                opacity: 1;
                transition: all 0.3s ease;
            }
            
            .toast-notification.success {
                border-left: 4px solid var(--success-color);
            }
            
            .toast-notification.error {
                border-left: 4px solid var(--danger-color);
            }
            
            .toast-notification.info {
                border-left: 4px solid var(--primary-color);
            }
            
            .toast-icon i {
                font-size: 1.5rem;
            }
            
            .toast-notification.success .toast-icon i {
                color: var(--success-color);
            }
            
            .toast-notification.error .toast-icon i {
                color: var(--danger-color);
            }
            
            .toast-notification.info .toast-icon i {
                color: var(--primary-color);
            }
            
            .toast-content {
                flex: 1;
            }
            
            .toast-content p {
                margin: 0;
                color: var(--dark-color);
            }
            
            .toast-close {
                background: none;
                border: none;
                font-size: 1.2rem;
                color: var(--gray);
                cursor: pointer;
                padding: 0;
                width: 24px;
                height: 24px;
                display: flex;
                align-items: center;
                justify-content: center;
            }
        `;
        document.head.appendChild(style);
    }
}

// 调用一次 
toggleStaticIP();
// 切换静态IP显示
function toggleStaticIP() {
    let value = document.getElementById('use-static-ip').value;
    document.getElementById('static-ip-config').style.visibility = value=='enabled' ? 'visible' : 'hidden';
    document.getElementById('static-ip-config-1').style.visibility = value=='enabled' ? 'visible' : 'hidden';
}

// 打开WiFi扫描器
function openWifiScanner() {
    document.getElementById('wifi-scanner-modal').classList.add('active');
}

// 关闭WiFi扫描器
function closeWifiScanner() {
    document.getElementById('wifi-scanner-modal').classList.remove('active');
}

// 开始扫描
function startScan() {
    const scanningStatus = document.getElementById('scanning-status');
    const wifiList = document.getElementById('wifi-list');
    
    scanningStatus.style.display = 'block';
    wifiList.innerHTML = '';
    
    fetch('/api/network/scan')
        .then(response => response.json())
        .then(networks => {
            wifiNetworks = networks;
            displayWifiList();
            scanningStatus.style.display = 'none';
        })
        .catch(error => {
            console.error('扫描失败:', error);
            scanningStatus.style.display = 'none';
            showMessage('扫描失败: ' + error.message, 'danger');
        });
}

// 显示WiFi列表
function displayWifiList() {
    const wifiList = document.getElementById('wifi-list');
    
    if (!wifiNetworks || wifiNetworks.length === 0) {
        wifiList.innerHTML = '<div style="text-align: center; color: #666; padding: 20px;">未发现WiFi网络</div>';
        return;
    }
    
    let html = '';
    wifiNetworks.forEach(network => {
        const strength = getSignalStrength(network.rssi);
        const strengthBars = getStrengthBars(strength);
        const encryption = network.encryption === 'open' ? '开放网络' : '加密网络';
        
        html += `
            <div class="wifi-item" onclick="selectWifi('${network.ssid.replace(/'/g, "\\'")}')">
                <div class="wifi-ssid">${network.ssid}</div>
                <div class="wifi-info">
                    <div>
                        <span>${encryption}</span> · 
                        <span>信道 ${network.channel}</span>
                    </div>
                    <div class="wifi-strength">
                        <span>${strength}%</span>
                        ${strengthBars}
                    </div>
                </div>
            </div>
        `;
    });
    
    wifiList.innerHTML = html;
}

// 选择WiFi
function selectWifi(ssid) {
    document.getElementById('sta-ssid').value = ssid;
    closeWifiScanner();
    showMessage(`已选择WiFi: ${ssid}`, 'success');
}

// 获取信号强度
function getSignalStrength(rssi) {
    if (rssi >= -50) return 100;
    if (rssi <= -100) return 0;
    return 2 * (rssi + 100);
}

// 获取信号强度条
function getStrengthBars(strength) {
    let bars = '';
    for (let i = 1; i <= 4; i++) {
        const active = strength >= (i * 25);
        bars += `<div class="strength-bar ${active ? 'active' : ''}"></div>`;
    }
    return bars;
}

// 生成备用IP
function generateBackupIPs() {
    fetch('/api/network/generate-backup-ips', {
        method: 'POST',
        headers: {
            'Content-Type': 'application/json'
        },
        body: JSON.stringify({
            staticIP: document.getElementById('static-ip').value,
            subnet: document.getElementById('subnet').value,
            gateway: document.getElementById('gateway').value
        })
    })
    .then(response => response.json())
    .then(data => {
        if (data.success) {
            currentConfig.backupIPs = data.backupIPs;
            updateBackupIPList();
            showMessage('备用IP生成成功', 'success');
        } else {
            showMessage('生成失败: ' + data.message, 'danger');
        }
    })
    .catch(error => {
        console.error('生成失败:', error);
        showMessage('生成失败: ' + error.message, 'danger');
    });
}

// 切换到随机IP
function switchToRandomIP() {
    if (!confirm('确定要切换到随机IP吗？当前连接可能会中断。')) {
        return;
    }
    
    fetch('/api/network/switch-random-ip', {
        method: 'POST'
    })
    .then(response => response.json())
    .then(data => {
        if (data.success) {
            showMessage('已切换到随机IP，正在重新连接...', 'success');
            setTimeout(() => {
                loadCurrentConfig();
                updateNetworkStatus();
            }, 3000);
        } else {
            showMessage('切换失败: ' + data.message, 'danger');
        }
    })
    .catch(error => {
        console.error('切换失败:', error);
        showMessage('切换失败: ' + error.message, 'danger');
    });
}

// 收集配置数据
function collectConfig() {
    return {
        mode: parseInt(document.getElementById('wifi-mode').value),
        deviceName: document.getElementById('device-name').value,
        
        // STA配置
        staSSID: document.getElementById('sta-ssid').value,
        staPassword: document.getElementById('sta-password').value,
        ipConfigType: parseInt(document.getElementById('ip-config-type').value),
        
        // 静态IP配置
        staticIP: document.getElementById('static-ip').value,
        gateway: document.getElementById('gateway').value,
        subnet: document.getElementById('subnet').value,
        dns1: document.getElementById('dns1').value,
        dns2: document.getElementById('dns2').value,
        
        // AP配置
        apSSID: document.getElementById('ap-ssid').value,
        apPassword: document.getElementById('ap-password').value,
        apChannel: 1,
        apHidden: false,
        apMaxConnections: 4,
        
        // IP冲突检测
        conflictDetection: parseInt(document.getElementById('conflict-detection').value),
        autoFailover: document.getElementById('auto-failover').checked,
        conflictCheckInterval: 30000,
        maxFailoverAttempts: 3,
        conflictThreshold: 2,
        fallbackToDHCP: true,
        
        // 备用IP列表
        backupIPs: currentConfig.backupIPs || [],
        
        // 域名配置
        customDomain: document.getElementById('custom-domain').value,
        enableMDNS: document.getElementById('enable-mdns').checked,
        enableDNS: document.getElementById('enable-dns').checked,
        
        // 连接参数
        connectTimeout: parseInt(document.getElementById('connect-timeout').value),
        reconnectInterval: parseInt(document.getElementById('reconnect-interval').value),
        maxReconnectAttempts: parseInt(document.getElementById('max-reconnect-attempts').value)
    };
}

// 保存配置
function saveConfig() {
    const config = collectConfig();
    
    fetch('/api/network/config', {
        method: 'POST',
        headers: {
            'Content-Type': 'application/json'
        },
        body: JSON.stringify(config)
    })
    .then(response => response.json())
    .then(data => {
        if (data.success) {
            showMessage('配置保存成功', 'success');
            loadCurrentConfig();
        } else {
            showMessage('保存失败: ' + data.message, 'danger');
        }
    })
    .catch(error => {
        console.error('保存失败:', error);
        showMessage('保存失败: ' + error.message, 'danger');
    });
}

// 保存并应用配置
function applyConfig() {
    if (!confirm('应用配置将重启网络连接，确定要继续吗？')) {
        return;
    }
    
    const config = collectConfig();
    
    fetch('/api/network/config/apply', {
        method: 'POST',
        headers: {
            'Content-Type': 'application/json'
        },
        body: JSON.stringify(config)
    })
    .then(response => response.json())
    .then(data => {
        if (data.success) {
            showMessage('配置已应用，网络正在重新连接...', 'success');
            setTimeout(() => {
                loadCurrentConfig();
                updateNetworkStatus();
            }, 5000);
        } else {
            showMessage('应用失败: ' + data.message, 'danger');
        }
    })
    .catch(error => {
        console.error('应用失败:', error);
        showMessage('应用失败: ' + error.message, 'danger');
    });
}

// 测试连接
function testConnection() {
    fetch('/api/network/test-connection')
        .then(response => response.json())
        .then(data => {
            if (data.success) {
                showMessage('连接测试成功: ' + data.message, 'success');
            } else {
                showMessage('连接测试失败: ' + data.message, 'danger');
            }
        })
        .catch(error => {
            console.error('测试失败:', error);
            showMessage('测试失败: ' + error.message, 'danger');
        });
}

// 重置配置
function resetConfig() {
    if (!confirm('确定要重置为默认配置吗？所有自定义设置都将丢失。')) {
        return;
    }
    
    fetch('/api/network/reset', {
        method: 'POST'
    })
    .then(response => response.json())
    .then(data => {
        if (data.success) {
            showMessage('配置已重置', 'success');
            setTimeout(() => {
                loadCurrentConfig();
                updateNetworkStatus();
            }, 2000);
        } else {
            showMessage('重置失败: ' + data.message, 'danger');
        }
    })
    .catch(error => {
        console.error('重置失败:', error);
        showMessage('重置失败: ' + error.message, 'danger');
    });
}

// 导出配置
function exportConfig() {
    const config = collectConfig();
    const configStr = JSON.stringify(config, null, 2);
    
    const blob = new Blob([configStr], { type: 'application/json' });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = 'fastbee-network-config.json';
    document.body.appendChild(a);
    a.click();
    document.body.removeChild(a);
    URL.revokeObjectURL(url);
    
    showMessage('配置导出成功', 'success');
}

// 显示消息
function showMessage(message, type) {
    const container = document.getElementById('message-container');
    const alert = document.createElement('div');
    alert.className = `alert alert-${type}`;
    alert.innerHTML = `
        <i class="fas fa-${type === 'success' ? 'check-circle' : 'exclamation-circle'}"></i>
        <span>${message}</span>
    `;
    
    container.appendChild(alert);
    
    // 3秒后自动移除
    setTimeout(() => {
        alert.style.opacity = '0';
        alert.style.transition = 'opacity 0.3s';
        setTimeout(() => {
            if (alert.parentNode) {
                alert.parentNode.removeChild(alert);
            }
        }, 300);
    }, 3000);
}