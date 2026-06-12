/**
 * @file DNSManager.cpp
 * @brief DNS 管理器实现
 * @author kerwincui
 * @date 2026-03-03
 */

#include "network/DNSManager.h"
#include "systems/LoggerSystem.h"
#include <WiFi.h>
// DNS服务器已移除，仅保留mDNS功能
// 注意：ESP-IDF 内置 mDNS 组件自带 hostname 冲突检测和自动重命名（RFC 6762），
// 无需手动 UDP 探测。ESP-IDF 5.5.4 加强了 lwIP TCPIP 核心锁断言，
// 在非 loopTask 上下文中调用 WiFiUDP::beginMulticast() 会触发 udp_new_ip_type assert。

#if FASTBEE_ENABLE_MDNS

DNSManager::DNSManager() {
}

DNSManager::~DNSManager() {
    stopMDNS();
}

bool DNSManager::initialize() {
    LOG_INFO("DNSManager: Initializing...");
    mdnsStarted = false;
    LOG_INFO("DNSManager: Initialized successfully");
    return true;
}

bool DNSManager::startMDNS(const String& hostname) {
    // 如果已经启动，先检查是否仍然有效
    if (mdnsStarted) {
        // 在AP+STA模式下，只要任一接口可用就保持服务
        return true;
    }

    WiFiMode_t currentMode = WiFi.getMode();
    
    // 检查网络模式：AP、STA或AP+STA都可以启动mDNS
    if (currentMode != WIFI_MODE_STA && 
        currentMode != WIFI_MODE_AP && 
        currentMode != WIFI_MODE_APSTA) {
        LOG_WARNING("DNSManager: Cannot start mDNS - invalid WiFi mode");
        return false;
    }
    
    // 确定使用哪个IP地址
    // 在AP+STA模式下，优先使用STA IP，如果不可用则使用AP IP
    IPAddress bindIP;
    bool hasValidIP = false;
    
    if (currentMode == WIFI_MODE_APSTA || currentMode == WIFI_MODE_STA) {
        // 尝试使用STA IP
        if (WiFi.status() == WL_CONNECTED) {
            IPAddress staIP = WiFi.localIP();
            if (staIP != INADDR_NONE && staIP != IPAddress(0,0,0,0)) {
                bindIP = staIP;
                hasValidIP = true;
                LOG_DEBUGF("DNSManager: Using STA IP for mDNS: %s", staIP.toString().c_str());
            }
        }
    }
    
    // 如果没有有效的STA IP，检查AP IP（AP+STA和纯AP模式）
    if (!hasValidIP && (currentMode == WIFI_MODE_AP || currentMode == WIFI_MODE_APSTA)) {
        IPAddress apIP = WiFi.softAPIP();
        if (apIP != INADDR_NONE && apIP != IPAddress(0,0,0,0)) {
            bindIP = apIP;
            hasValidIP = true;
            LOG_DEBUGF("DNSManager: Using AP IP for mDNS: %s", apIP.toString().c_str());
        }
    }
    
    if (!hasValidIP) {
        LOG_WARNING("DNSManager: Cannot start mDNS - no valid IP address on any interface");
        return false;
    }

    String mdnsHostname = hostname.isEmpty() ? customDomain : hostname;
    
    // 清理无效字符
    mdnsHostname.replace(" ", "-");
    mdnsHostname.toLowerCase();
    
    // 直接使用 MDNS.begin()，ESP-IDF 内置 mDNS 组件自带冲突探测和自动重命名（RFC 6762）
    // 不再手动 UDP 探测，避免 ESP-IDF 5.5.4 lwIP TCPIP 核心锁断言崩溃
    const int maxAttempts = 3;
    String tryHostname;
    
    for (int attempt = 0; attempt < maxAttempts; attempt++) {
        tryHostname = mdnsHostname;
        if (attempt > 0) {
            tryHostname += "-" + String(attempt + 1);
        }
        
        LOG_INFOF("DNSManager: Starting mDNS with hostname '%s.local'...", tryHostname.c_str());
        
        if (MDNS.begin(tryHostname.c_str())) {
            actualHostname = tryHostname;
            MDNS.setInstanceName(tryHostname.c_str());
            
            // 添加服务
            MDNS.addService("http", "tcp", 80);
            MDNS.addService("fastbee", "tcp", 80);
            MDNS.addService("ws", "tcp", 81);
            LOG_INFO("DNSManager: Added HTTP service to mDNS");
            
            mdnsStarted = true;
            if (attempt > 0) {
                LOG_WARNINGF("DNSManager: mDNS conflict detected, renamed to: %s.local", tryHostname.c_str());
            }
            LOG_INFO("DNSManager: mDNS started: " + tryHostname + ".local");
            return true;
        }
        
        LOG_WARNINGF("DNSManager: MDNS.begin('%s') failed, trying next...", tryHostname.c_str());
    }
    
    LOG_ERROR("DNSManager: All mDNS hostname attempts failed");
    return false;
}

void DNSManager::stopMDNS() {
    if (mdnsStarted) {
        MDNS.end();
        mdnsStarted = false;
        LOG_INFO("DNSManager: mDNS stopped");
    }
}



bool DNSManager::isMDNSStarted() const {
    return mdnsStarted;
}

void DNSManager::setCustomDomain(const String& domain) {
    customDomain = domain;
}

String DNSManager::getCustomDomain() const {
    return customDomain;
}

void DNSManager::setMDNSEnabled(bool enabled) {
    mdnsEnabled = enabled;
    if (!enabled && mdnsStarted) {
        stopMDNS();
    }
}

bool DNSManager::checkMDNSHealth() {
    // 检查mDNS服务是否仍然有效
    if (!mdnsStarted) {
        // mDNS未启动，检查是否应该启动
        WiFiMode_t currentMode = WiFi.getMode();
        
        // 在AP、STA或AP+STA模式下，如果有有效IP就应该启动mDNS
        if (currentMode == WIFI_MODE_AP || 
            currentMode == WIFI_MODE_STA || 
            currentMode == WIFI_MODE_APSTA) {
            
            bool hasValidIP = false;
            
            // 检查STA IP
            if ((currentMode == WIFI_MODE_STA || currentMode == WIFI_MODE_APSTA) &&
                WiFi.status() == WL_CONNECTED) {
                IPAddress staIP = WiFi.localIP();
                if (staIP != INADDR_NONE && staIP != IPAddress(0,0,0,0)) {
                    hasValidIP = true;
                }
            }
            
            // 检查AP IP
            if (!hasValidIP && 
                (currentMode == WIFI_MODE_AP || currentMode == WIFI_MODE_APSTA)) {
                IPAddress apIP = WiFi.softAPIP();
                if (apIP != INADDR_NONE && apIP != IPAddress(0,0,0,0)) {
                    hasValidIP = true;
                }
            }
            
            if (hasValidIP && mdnsEnabled) {
                LOG_INFO("DNSManager: Health check - mDNS not running, attempting restart");
                return startMDNS(actualHostname.isEmpty() ? customDomain : actualHostname);
            }
        }
        return false;
    }
    
    // mDNS已启动，检查网络状态是否仍然有效
    WiFiMode_t currentMode = WiFi.getMode();
    
    // 检查是否有任一有效IP
    bool hasValidIP = false;
    
    // 检查STA IP
    if ((currentMode == WIFI_MODE_STA || currentMode == WIFI_MODE_APSTA) &&
        WiFi.status() == WL_CONNECTED) {
        IPAddress staIP = WiFi.localIP();
        if (staIP != INADDR_NONE && staIP != IPAddress(0,0,0,0)) {
            hasValidIP = true;
        }
    }
    
    // 检查AP IP
    if (!hasValidIP &&
        (currentMode == WIFI_MODE_AP || currentMode == WIFI_MODE_APSTA)) {
        IPAddress apIP = WiFi.softAPIP();
        if (apIP != INADDR_NONE && apIP != IPAddress(0,0,0,0)) {
            hasValidIP = true;
        }
    }
    
    // 如果没有任何有效IP，mDNS服务可能已失效
    if (!hasValidIP) {
        LOG_WARNING("DNSManager: Health check - no valid IP, mDNS may be unstable");
        // 不立即停止，等待网络恢复
        return false;
    }
    
    return true;  // mDNS服务正常
}

void DNSManager::restartMDNS(const String& hostname) {
    LOG_INFO("DNSManager: Restarting mDNS service...");
    stopMDNS();
    delay(100);  // 短暂延迟确保资源释放
    startMDNS(hostname.isEmpty() ? customDomain : hostname);
}

String DNSManager::getActualHostname() const {
    return actualHostname;
}

#else

DNSManager::DNSManager() = default;
DNSManager::~DNSManager() = default;

bool DNSManager::initialize() {
    LOG_INFO("DNSManager: mDNS disabled by feature flag");
    return true;
}

bool DNSManager::startMDNS(const String&) {
    return false;
}

void DNSManager::stopMDNS() {
}

bool DNSManager::isMDNSStarted() const {
    return false;
}

void DNSManager::setCustomDomain(const String& domain) {
    customDomain = domain;
}

String DNSManager::getCustomDomain() const {
    return customDomain;
}

void DNSManager::setMDNSEnabled(bool enabled) {
    mdnsEnabled = enabled;
}

bool DNSManager::checkMDNSHealth() {
    return false;
}

void DNSManager::restartMDNS(const String&) {
}

String DNSManager::getActualHostname() const {
    return "";
}

#endif
