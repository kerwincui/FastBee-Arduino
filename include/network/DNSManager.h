#ifndef DNS_MANAGER_H
#define DNS_MANAGER_H

#include <ESPmDNS.h>
#include <DNSServer.h>

/**
 * @brief DNS 管理器类
 * @details 负责 DNS 服务器和 mDNS 服务的管理
 */
class DNSManager {
public:
    DNSManager();
    ~DNSManager();
    
    /**
     * @brief 初始化 DNS 管理器
     * @return 是否初始化成功
     */
    bool initialize();
    
    /**
     * @brief 启动 mDNS 服务
     * @param hostname 主机名
     * @return 是否启动成功
     */
    bool startMDNS(const String& hostname);
    
    /**
     * @brief 停止 mDNS 服务
     */
    void stopMDNS();
    
    /**
     * @brief 启动 DNS 服务器
     * @param apIP AP IP 地址
     * @return 是否启动成功
     */
    bool startDNSServer(const IPAddress& apIP);
    
    /**
     * @brief 停止 DNS 服务器
     */
    void stopDNSServer();
    
    /**
     * @brief 处理 DNS 请求
     */
    void processDNSRequests();
    
    /**
     * @brief 检查 mDNS 是否启动
     * @return 是否启动
     */
    bool isMDNSStarted() const;
    
    /**
     * @brief 检查 DNS 服务器是否启动
     * @return 是否启动
     */
    bool isDNSServerStarted() const;
    
    /**
     * @brief 设置自定义域名
     * @param domain 域名
     */
    void setCustomDomain(const String& domain);
    
    /**
     * @brief 获取自定义域名
     * @return 域名
     */
    String getCustomDomain() const;
    
    /**
     * @brief 启用或禁用 mDNS
     * @param enabled 是否启用
     */
    void setMDNSEnabled(bool enabled);
    
    /**
     * @brief 启用或禁用 DNS 服务器
     * @param enabled 是否启用
     */
    void setDNSEnabled(bool enabled);
    
    /**
     * @brief 检查 mDNS 健康状态
     * @return true=服务正常，false=服务异常或未启动
     */
    bool checkMDNSHealth();
    
    /**
     * @brief 重启 mDNS 服务
     * @param hostname 主机名（可选，默认使用customDomain）
     */
    void restartMDNS(const String& hostname = "");
    
    /**
     * @brief 获取实际注册的 mDNS hostname
     * @return 实际注册的 hostname（可能与 customDomain 不同）
     */
    String getActualHostname() const;
    
private:
    DNSServer dnsServer;
    bool mdnsStarted = false;
    bool dnsServerStarted = false;
    String customDomain = "fastbee";
    bool mdnsEnabled = true;
    bool dnsEnabled = true;
    String actualHostname = "";  // 实际注册成功的 mDNS hostname（可能带 -2/-3 后缀）
};

#endif // DNS_MANAGER_H