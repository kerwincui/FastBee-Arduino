/**
 * @file IPManager.cpp
 * @brief IP 管理器实现
 * @author kerwincui
 * @date 2026-03-03
 */

#include "network/IPManager.h"
#include "systems/LoggerSystem.h"

IPManager::IPManager() {
    backupIPs.clear();
}

IPManager::~IPManager() {
    backupIPs.clear();
    conflictCache.clear();
}

bool IPManager::initialize() {
    LOG_INFO("IPManager: Initializing...");
    backupIPs.clear();
    conflictCache.clear();
    ipConflictDetected = false;
    currentFailoverAttempts = 0;
    currentBackupIPIndex = 0;
    conflictDetectionCount = 0;
    lastConflictCheck = 0;
    LOG_INFO("IPManager: Initialized successfully");
    return true;
}

void IPManager::updateIPConflictStatus() {
    String currentIP = WiFi.localIP().toString();
    if (currentIP.isEmpty() || currentIP == "0.0.0.0") {
        return;
    }

    // 检查缓存中是否有这个 IP 的记录
    bool cachedConflict = false;
    for (auto& cache : conflictCache) {
        if (cache.ip == currentIP && 
            (millis() - cache.timestamp < 300000)) { // 5分钟缓存
            cachedConflict = cache.conflicted;
            break;
        }
    }

    bool conflict = false;
    String detectionMethod = "";
    
    if (!cachedConflict) {
        // 默认使用ARP检测
        conflict = detectConflictByARP(currentIP);
        detectionMethod = "ARP";

        // 缓存结果
        ConflictCache cache = {currentIP, conflict, millis()};
        conflictCache.push_back(cache);
    } else {
        conflict = cachedConflict;
        detectionMethod = "Cached";
    }

    if (conflict) {
        conflictDetectionCount++;
        
        LOG_WARNING("IPManager: IP conflict detected (" + 
                   detectionMethod + ") on IP: " + currentIP);
        
        if (conflictDetectionCount >= conflictThreshold) {
            ipConflictDetected = true;
            
            // 自动故障转移
            if (autoFailover) {
                LOG_INFO("IPManager: Starting automatic failover...");
                performFailover();
            }
        }
    } else {
        conflictDetectionCount = 0;
    }
}

bool IPManager::performFailover() {
    if (currentFailoverAttempts >= maxFailoverAttempts) {
        LOG_ERROR("IPManager: Max failover attempts reached");
        
        if (fallbackToDHCP) {
            LOG_INFO("IPManager: Falling back to DHCP");
            return switchToDHCP();
        }
        
        return false;
    }

    currentFailoverAttempts++;
    
    LOG_INFO("IPManager: Failover attempt " + 
             String(currentFailoverAttempts) + "/" + 
             String(maxFailoverAttempts));

    String nextIP = selectNextIP();
    if (nextIP.isEmpty()) {
        LOG_WARNING("IPManager: No more backup IPs available");
        
        if (fallbackToDHCP) {
            return switchToDHCP();
        }
        
        return false;
    }

    LOG_INFO("IPManager: Switching to IP: " + nextIP);
    
    // 更新配置
    staticIP = nextIP;
    
    return true;
}

bool IPManager::switchToBackupIP() {
    return performFailover();
}

bool IPManager::switchToRandomIP() {
    String randomIP = getRandomIPInRange(staticIP, subnet);
    if (randomIP.isEmpty()) {
        return false;
    }
    
    staticIP = randomIP;
    return true;
}

bool IPManager::switchToDHCP() {
    LOG_INFO("IPManager: Switching to DHCP");
    return true;
}

void IPManager::generateBackupIPs() {
    backupIPs.clear();
    
    if (staticIP.isEmpty() || 
        subnet.isEmpty() || 
        gateway.isEmpty()) {
        return;
    }

    // 解析网络地址和掩码
    IPAddress ip, subnetAddr, gatewayAddr;
    ip.fromString(staticIP.c_str());
    subnetAddr.fromString(subnet.c_str());
    gatewayAddr.fromString(gateway.c_str());

    // 计算网络地址
    IPAddress network(ip[0] & subnetAddr[0], 
                     ip[1] & subnetAddr[1], 
                     ip[2] & subnetAddr[2], 
                     ip[3] & subnetAddr[3]);

    // 生成3个备用IP（避免.0、.1、.255和网关）
    int hostCount = 0;
    for (int i = 2; i < 254 && hostCount < 3; i++) {
        // 跳过网关
        if (i == gatewayAddr[3]) {
            continue;
        }
        
        // 跳过当前IP
        if (i == ip[3]) {
            continue;
        }
        
        IPAddress backupIP(network[0], network[1], network[2], i);
        backupIPs.push_back(backupIP.toString());
        hostCount++;
    }

    LOG_INFO("IPManager: Generated " + String(hostCount) + " backup IPs");
}

String IPManager::getRandomIPInRange(const String& network, const String& mask) {
    IPAddress net, subnetAddr;
    net.fromString(network.c_str());
    subnetAddr.fromString(mask.c_str());

    // 计算网络地址
    IPAddress networkAddr(net[0] & subnetAddr[0], 
                         net[1] & subnetAddr[1], 
                         net[2] & subnetAddr[2], 
                         net[3] & subnetAddr[3]);

    // 计算广播地址
    IPAddress broadcastAddr;
    for (int i = 0; i < 4; i++) {
        broadcastAddr[i] = networkAddr[i] | (~subnetAddr[i] & 0xFF);
    }

    // 生成随机IP（避免.0、.1、.255）
    IPAddress randomIP;
    do {
        for (int i = 0; i < 4; i++) {
            if (subnetAddr[i] == 255) {
                randomIP[i] = networkAddr[i];
            } else {
                uint8_t min = (i == 3) ? 2 : networkAddr[i];
                uint8_t max = (i == 3) ? 254 : broadcastAddr[i];
                randomIP[i] = random(min, max + 1);
            }
        }
    } while (randomIP == networkAddr || 
             randomIP == broadcastAddr || 
             randomIP[3] == 0 || 
             randomIP[3] == 1 || 
             randomIP[3] == 255);

    return randomIP.toString();
}

void IPManager::cleanupConflictCache() {
    unsigned long currentTime = millis();
    auto it = conflictCache.begin();
    while (it != conflictCache.end()) {
        if (currentTime - it->timestamp > 3600000) { // 1小时
            it = conflictCache.erase(it);
        } else {
            ++it;
        }
    }
}

bool IPManager::checkIPConflict() {
    return ipConflictDetected;
}

bool IPManager::testIPAvailability(const String& ip) {
    LOG_DEBUG("IPManager: Testing IP availability: " + ip);
    // 这里可以实现IP可用性测试逻辑
    // 例如：发送ARP请求或尝试连接
    return true; // 暂时返回true
}

bool IPManager::detectConflictByARP(const String& ip) {
    // 这是一个简化的ARP检测实现
    LOG_DEBUG("IPManager: ARP conflict detection for IP: " + ip);
    return false; // 暂时返回false，需要根据实际情况实现
}

bool IPManager::detectConflictByPing(const String& ip) {
    LOG_DEBUG("IPManager: Ping conflict detection for IP: " + ip);
    // 这里可以实现ICMP ping检测
    // 注意：ESP32的ping功能可能需要额外的库
    return false;
}

bool IPManager::detectConflictByGateway(const String& ip) {
    LOG_DEBUG("IPManager: Gateway conflict detection for IP: " + ip);
    
    // 尝试与网关通信，如果失败可能表示IP冲突
    String gatewayStr = WiFi.gatewayIP().toString();
    if (gatewayStr.isEmpty() || gatewayStr == "0.0.0.0") {
        return false;
    }
    
    // 简化的网关可达性检测
    // 实际实现可能需要尝试连接网关的特定端口
    return false;
}

String IPManager::selectNextIP() {
    if (backupIPs.empty()) {
        return "";
    }

    String selectedIP;
    
    // 默认使用顺序策略
    selectedIP = backupIPs[currentBackupIPIndex];
    currentBackupIPIndex = (currentBackupIPIndex + 1) % backupIPs.size();

    return selectedIP;
}