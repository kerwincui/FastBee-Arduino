/**
 * @file OTAManager.h
 * @brief OTA管理器类，负责固件的无线更新功能
 * @author kerwincui
 * @date 2025-12-02
 */

#ifndef OTA_MANAGER_H
#define OTA_MANAGER_H

#include <Arduino.h>
#include <functional>

// 前向声明
class AsyncWebServer;
class AsyncWebServerRequest;

/**
 * @class OTAManager
 * @brief OTA管理器类，提供固件无线更新功能
 */
class OTAManager {
public:
    /**
     * @brief 构造函数
     * @param webServer Web服务器指针
     */
    explicit OTAManager(AsyncWebServer* webServer = nullptr);
    
    /**
     * @brief 初始化OTA管理器
     * @return 初始化是否成功
     */
    bool initialize();
    
    /**
     * @brief 处理OTA开始请求
     * @param request HTTP请求对象
     */
    void handleOTAStart(AsyncWebServerRequest *request);
    
    /**
     * @brief 处理OTA上传
     * @param request HTTP请求对象
     * @param filename 文件名
     * @param index 数据索引
     * @param data 数据指针
     * @param len 数据长度
     * @param final 是否为最后一部分
     */
    void handleOTAUpload(AsyncWebServerRequest *request, const String& filename, 
                        size_t index, uint8_t *data, size_t len, bool final);
    
    /**
     * @brief 处理OTA状态查询
     * @param request HTTP请求对象
     */
    void handleOTAStatus(AsyncWebServerRequest *request);
    
    /**
     * @brief 处理OTA取消请求
     * @param request HTTP请求对象
     */
    void handleOTACancel(AsyncWebServerRequest *request);
    
    /**
     * @brief 通过URL启动OTA更新
     * @param url 固件下载URL
     * @return 启动是否成功
     */
    bool startOTA(const String& url);
    
    /**
     * @brief 获取OTA状态字符串
     * @return 状态描述字符串
     */
    String getOTAStatus();
    
    /**
     * @brief 获取当前进度百分比
     * @return 进度(0-100)
     */
    uint8_t getProgress() const;
    
    /**
     * @brief 检查是否正在进行OTA
     * @return 是否正在进行OTA
     */
    bool isOTAInProgress() const;
    
    /**
     * @brief 获取已上传的大小
     * @return 已上传字节数
     */
    size_t getCurrentSize() const;
    
    /**
     * @brief 获取总大小
     * @return 总字节数
     */
    size_t getTotalSize() const;
    
    /**
     * @brief 获取已用时间
     * @return 已用时间（毫秒）
     */
    unsigned long getElapsedTime() const;

private:
    /**
     * @brief 设置OTA回调函数
     */
    void setOTACallbacks();
    
    // 成员变量
    AsyncWebServer* server;           ///< Web服务器指针
    bool otaInProgress;               ///< OTA是否进行中
    unsigned long otaStartTime;       ///< OTA开始时间
    size_t totalSize;                 ///< 固件总大小
    size_t currentSize;               ///< 当前已上传大小
    uint8_t progress;                 ///< 当前进度百分比
};

#endif 