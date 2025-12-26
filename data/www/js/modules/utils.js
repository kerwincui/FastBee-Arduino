// 工具函数和WiFi功能

// WiFi扫描功能
window.openWifiScanner = function() {
    const wifiList = document.getElementById('wifi-list');
    wifiList.innerHTML = `
        <div class="wifi-item">
            <div class="wifi-ssid">MyWiFi</div>
            <div class="wifi-signal">
                <i class="fas fa-wifi"></i>
                <span>强</span>
            </div>
        </div>
        <div class="wifi-item">
            <div class="wifi-ssid">GuestWiFi</div>
            <div class="wifi-signal">
                <i class="fas fa-wifi"></i>
                <span>中</span>
            </div>
        </div>
        <div class="wifi-item">
            <div class="wifi-ssid">OfficeWiFi</div>
            <div class="wifi-signal">
                <i class="fas fa-wifi"></i>
                <span>弱</span>
            </div>
        </div>
    `;
    
    // 点击WiFi项自动填充SSID
    document.querySelectorAll('.wifi-item').forEach(item => {
        item.addEventListener('click', function() {
            const ssid = this.querySelector('.wifi-ssid').textContent;
            document.getElementById('wifi-ssid').value = ssid;
            
            Notification.primary(`已选择WiFi网络: ${ssid}`, '网络选择');
        });
    });
    
    Notification.success('已扫描到3个WiFi网络', '网络扫描完成');
};

// 静态IP切换功能
window.toggleStaticIP = function() {
    const useStaticIP = document.getElementById('use-static-ip').value;
    const staticIpSection = document.getElementById('static-ip-section');
    if (useStaticIP === 'enabled') {
        staticIpSection.style.display = 'block';
        Notification.info('已启用静态IP配置', '网络设置');
    } else {
        staticIpSection.style.display = 'none';
    }
};