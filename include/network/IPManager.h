#ifndef IP_MANAGER_H
#define IP_MANAGER_H

#include <vector>
#include <WiFi.h>



/**
 * @brief IP 冲突缓存条目
 */
struct ConflictCache {
    String ip;
    bool conflicted;
    unsigned long timestamp;
};

/**
 * @brief IP 管理器类
 * @details 负责 IP 配置、IP 冲突检测和故障转移
 */
class IPManager {
public:
    IPManager();
    ~IPManager();
    
    /**
     * @brief 初始化 IP 管理器
     * @return 是否初始化成功
     */
    bool initialize();
    
    /**
     * @brief 更新 IP 冲突状态
     */
    void updateIPConflictStatus();
    
    /**
     * @brief 执行故障转移
     * @return 是否成功
     */
    bool performFailover();
    
    /**
     * @brief 切换到备用 IP
     * @return 是否成功
     */
    bool switchToBackupIP();
    
    /**
     * @brief 切换到随机 IP
     * @return 是否成功
     */
    bool switchToRandomIP();
    
    /**
     * @brief 切换到 DHCP
     * @return 是否成功
     */
    bool switchToDHCP();
    
    /**
     * @brief 生成备用 IP 列表
     */
    void generateBackupIPs();
    
    /**
     * @brief 获取指定范围内的随机 IP
     * @param network 网络地址
     * @param mask 子网掩码
     * @return 随机 IP 字符串
     */
    String getRandomIPInRange(const String& network, const String& mask);
    
    /**
     * @brief 清理冲突缓存
     */
    void cleanupConflictCache();
    
    /**
     * @brief 检查 IP 冲突
     * @return 是否检测到冲突
     */
    bool checkIPConflict();
    
    /**
     * @brief 测试 IP 可用性
     * @param ip IP 地址
     * @return 是否可用
     */
    bool testIPAvailability(const String& ip);
    
    // 配置属性
    bool autoFailover = true;
    uint16_t conflictCheckInterval = 30000;
    uint8_t maxFailoverAttempts = 3;
    uint8_t conflictThreshold = 2;
    bool fallbackToDHCP = true;
    String staticIP = "";
    String gateway = "";
    String subnet = "";
    std::vector<String> backupIPs;
    
private:
    bool ipConflictDetected = false;
    uint8_t currentFailoverAttempts = 0;
    uint8_t currentBackupIPIndex = 0;
    uint8_t conflictDetectionCount = 0;
    unsigned long lastConflictCheck = 0;
    std::vector<ConflictCache> conflictCache;
    
    /**
     * @brief 通过 ARP 检测 IP 冲突
     * @param ip IP 地址
     * @return 是否冲突
     */
    bool detectConflictByARP(const String& ip);
    
    /**
     * @brief 通过 Ping 检测 IP 冲突
     * @param ip IP 地址
     * @return 是否冲突
     */
    bool detectConflictByPing(const String& ip);
    
    /**
     * @brief 通过网关检测 IP 冲突
     * @param ip IP 地址
     * @return 是否冲突
     */
    bool detectConflictByGateway(const String& ip);
    
    /**
     * @brief 选择下一个 IP
     * @return 下一个 IP 字符串
     */
    String selectNextIP();
};

#endif // IP_MANAGER_H