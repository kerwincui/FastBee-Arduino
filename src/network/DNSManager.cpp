/**
 * @file DNSManager.cpp
 * @brief DNS 管理器实现
 * @author kerwincui
 * @date 2026-03-03
 */

#include "network/DNSManager.h"
#include "systems/LoggerSystem.h"
#include <WiFi.h>

DNSManager::DNSManager() {
}

DNSManager::~DNSManager() {
    stopMDNS();
    stopDNSServer();
}

bool DNSManager::initialize() {
    LOG_INFO("DNSManager: Initializing...");
    mdnsStarted = false;
    dnsServerStarted = false;
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
    
    if (!MDNS.begin(mdnsHostname.c_str())) {
        LOG_ERROR("DNSManager: Failed to start mDNS");
        return false;
    }
    
    MDNS.setInstanceName(mdnsHostname.c_str());
    
    // 添加服务
    MDNS.addService("http", "tcp", 80);
    // FastBee设备提供一个独特的服务标识
    MDNS.addService("fastbee", "tcp", 80);
    MDNS.addService("ws", "tcp", 81); 
    LOG_INFO("DNSManager: Added HTTP service to mDNS");
    
    mdnsStarted = true;
    LOG_INFO("DNSManager: mDNS started: " + mdnsHostname + ".local");
    return true;
}

void DNSManager::stopMDNS() {
    if (mdnsStarted) {
        MDNS.end();
        mdnsStarted = false;
        LOG_INFO("DNSManager: mDNS stopped");
    }
}

bool DNSManager::startDNSServer(const IPAddress& apIP) {
    if (dnsServerStarted) {
        return true;
    }

    dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
    if (!dnsServer.start(53, "*", apIP)) {
        LOG_ERROR("DNSManager: Failed to start DNS server");
        return false;
    }

    dnsServerStarted = true;
    LOG_INFO("DNSManager: DNS server started");
    return true;
}

void DNSManager::stopDNSServer() {
    if (dnsServerStarted) {
        dnsServer.stop();
        dnsServerStarted = false;
        LOG_INFO("DNSManager: DNS server stopped");
    }
}

void DNSManager::processDNSRequests() {
    if (dnsServerStarted) {
        dnsServer.processNextRequest();
    }
}

bool DNSManager::isMDNSStarted() const {
    return mdnsStarted;
}

bool DNSManager::isDNSServerStarted() const {
    return dnsServerStarted;
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

void DNSManager::setDNSEnabled(bool enabled) {
    dnsEnabled = enabled;
    if (!enabled && dnsServerStarted) {
        stopDNSServer();
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
                return startMDNS(customDomain);
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