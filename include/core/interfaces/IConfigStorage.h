#ifndef I_CONFIG_STORAGE_H
#define I_CONFIG_STORAGE_H

/**
 * @brief 配置存储接口
 * @details 定义配置存储的基本操作
 */
class IConfigStorage {
public:
    virtual ~IConfigStorage() = default;
    
    /**
     * @brief 初始化配置存储
     * @return 是否初始化成功
     */
    virtual bool initialize() = 0;
    
    /**
     * @brief 保存配置
     * @param key 配置键
     * @param value 配置值
     * @return 是否保存成功
     */
    virtual bool saveConfig(const String& key, const String& value) = 0;
    
    /**
     * @brief 读取配置
     * @param key 配置键
     * @param defaultValue 默认值
     * @return 配置值
     */
    virtual String readConfig(const String& key, const String& defaultValue = "") = 0;
    
    /**
     * @brief 删除配置
     * @param key 配置键
     * @return 是否删除成功
     */
    virtual bool deleteConfig(const String& key) = 0;
    
    /**
     * @brief 检查配置是否存在
     * @param key 配置键
     * @return 是否存在
     */
    virtual bool hasConfig(const String& key) = 0;
};

#endif // I_CONFIG_STORAGE_H