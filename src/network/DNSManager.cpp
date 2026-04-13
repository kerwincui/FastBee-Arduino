/**
 * @file DNSManager.cpp
 * @brief DNS 管理器实现
 * @author kerwincui
 * @date 2026-03-03
 */

#include "network/DNSManager.h"
#include "systems/LoggerSystem.h"
#include <WiFi.h>
#include <WiFiUdp.h>

/**
 * @brief 探测网络上是否已有指定 hostname 的 mDNS 设备
 * @param hostname 要探测的 hostname（不含 .local 后缀）
 * @param timeoutMs 等待响应的超时时间
 * @return true=已被占用，false=未被占用
 */
static bool probeMDNSHostname(const String& hostname, unsigned long timeoutMs = 500) {
    WiFiUDP udp;
    IPAddress mdnsMulticast(224, 0, 0, 251);
    const uint16_t mdnsPort = 5353;
    
    // 构建 mDNS query 包
    // DNS header: ID=0, Flags=0 (standard query), QDCOUNT=1, ANCOUNT=0, NSCOUNT=0, ARCOUNT=0
    // Question: <hostname>.local, Type=A (1), Class=IN (1)
    
    String fqdn = hostname + ".local";
    
    // 计算包大小
    // Header(12) + name(variable) + type(2) + class(2)
    uint8_t packet[256];
    memset(packet, 0, sizeof(packet));
    
    // DNS Header
    // Transaction ID: 0x0000
    packet[0] = 0x00; packet[1] = 0x00;
    // Flags: 0x0000 (standard query)
    packet[2] = 0x00; packet[3] = 0x00;
    // Questions: 1
    packet[4] = 0x00; packet[5] = 0x01;
    // Answer/Authority/Additional: 0
    packet[6] = 0x00; packet[7] = 0x00;
    packet[8] = 0x00; packet[9] = 0x00;
    packet[10] = 0x00; packet[11] = 0x00;
    
    // Question section: encode hostname.local as DNS name
    size_t pos = 12;
    
    // Encode hostname part
    const char* hn = hostname.c_str();
    size_t hnLen = strlen(hn);
    if (hnLen > 63) hnLen = 63;
    packet[pos++] = (uint8_t)hnLen;
    memcpy(packet + pos, hn, hnLen);
    pos += hnLen;
    
    // Encode "local" part
    packet[pos++] = 5; // length of "local"
    memcpy(packet + pos, "local", 5);
    pos += 5;
    
    // Terminator
    packet[pos++] = 0x00;
    
    // Type: A (1)
    packet[pos++] = 0x00; packet[pos++] = 0x01;
    // Class: IN (1) with QU bit set for unicast response
    packet[pos++] = 0x80; packet[pos++] = 0x01;
    
    // Send multicast query
    if (!udp.beginMulticast(mdnsMulticast, mdnsPort)) {
        return false; // 无法发送，假设未占用
    }
    
    udp.beginPacket(mdnsMulticast, mdnsPort);
    udp.write(packet, pos);
    udp.endPacket();
    
    // 等待响应
    unsigned long startTime = millis();
    bool found = false;
    
    while (millis() - startTime < timeoutMs) {
        int packetSize = udp.parsePacket();
        if (packetSize > 0) {
            uint8_t respBuf[512];
            int len = udp.read(respBuf, sizeof(respBuf));
            
            // 检查是否是 mDNS 响应（Flags 的 QR bit = 1，即 bit15）
            if (len >= 12) {
                uint8_t flags1 = respBuf[2];
                uint16_t anCount = (respBuf[6] << 8) | respBuf[7];
                
                // QR=1 表示响应，且有 answer
                if ((flags1 & 0x80) && anCount > 0) {
                    // 简单检查：响应包中是否包含我们查询的 hostname
                    // 通过字符串搜索（不够严谨但对 ESP32 足够实用）
                    String respStr;
                    for (int i = 12; i < len && i < 256; i++) {
                        if (respBuf[i] >= 32 && respBuf[i] < 127) {
                            respStr += (char)respBuf[i];
                        }
                    }
                    respStr.toLowerCase();
                    String lowerHostname = hostname;
                    lowerHostname.toLowerCase();
                    if (respStr.indexOf(lowerHostname) >= 0) {
                        found = true;
                        break;
                    }
                }
            }
        }
        delay(10);
    }
    
    udp.stop();
    return found;
}

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
    
    // 冲突探测 + 自动重命名（最多尝试 hostname, hostname-2, hostname-3）
    const int maxAttempts = 3;
    String tryHostname;
    
    for (int attempt = 0; attempt < maxAttempts; attempt++) {
        tryHostname = mdnsHostname;
        if (attempt > 0) {
            tryHostname += "-" + String(attempt + 1);
        }
        
        // 探测网络上是否已有此 hostname
        LOG_INFOF("DNSManager: Probing mDNS hostname '%s.local'...", tryHostname.c_str());
        bool occupied = probeMDNSHostname(tryHostname, 500);
        
        if (occupied) {
            LOG_WARNINGF("DNSManager: '%s.local' is already in use, trying next...", tryHostname.c_str());
            continue;
        }
        
        // 未被占用，尝试注册
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