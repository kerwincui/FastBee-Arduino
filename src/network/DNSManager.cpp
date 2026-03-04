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
    if (mdnsStarted) {
        return true;
    }

    // 检查网络状态
    if (WiFi.getMode() != WIFI_MODE_STA && WiFi.getMode() != WIFI_MODE_APSTA) {
        LOG_WARNING("DNSManager: Cannot start mDNS - not in STA mode");
        return false;
    }
    
    if (WiFi.status() != WL_CONNECTED) {
        LOG_WARNING("DNSManager: Cannot start mDNS - WiFi not connected");
        return false;
    }
    
    IPAddress staIP = WiFi.localIP();
    if (staIP == INADDR_NONE || staIP == IPAddress(0,0,0,0)) {
        LOG_WARNING("DNSManager: Cannot start mDNS - no valid IP address");
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