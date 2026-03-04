#ifndef I_NETWORK_MANAGER_H
#define I_NETWORK_MANAGER_H

/**
 * @brief 网络管理器接口
 * @details 定义网络管理的基本操作
 */
class INetworkManager {
public:
    virtual ~INetworkManager() = default;
    
    /**
     * @brief 初始化网络管理器
     * @return 是否初始化成功
     */
    virtual bool initialize() = 0;
    
    /**
     * @brief 断开所有网络连接
     */
    virtual void disconnect() = 0;
    
    /**
     * @brief 更新网络状态
     */
    virtual void update() = 0;
    
    /**
     * @brief 扫描网络
     * @return 网络列表的 JSON 字符串
     */
    virtual String scanNetworks() = 0;
    
    /**
     * @brief 连接到指定网络
     * @param ssid SSID
     * @param password 密码
     * @return 是否连接成功
     */
    virtual bool connectToNetwork(const String& ssid, const String& password) = 0;
    
    /**
     * @brief 断开网络连接
     */
    virtual void disconnectNetwork() = 0;
    
    /**
     * @brief 重启网络
     * @return 是否重启成功
     */
    virtual bool restartNetwork() = 0;
    
    /**
     * @brief 检查互联网连接
     * @return 是否有互联网连接
     */
    virtual bool checkInternetConnection() = 0;
    
    /**
     * @brief 检查IP冲突
     * @return 是否检测到IP冲突
     */
    virtual bool checkIPConflict() = 0;
};

#endif // I_NETWORK_MANAGER_H