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
 * @note HTTP 路由统一由 OTARouteHandler 注册并委托到本类，
 *       本类不再自行注册路由（避免重复注册导致处理器被遮蔽）
 */
class OTAManager {
public:
    /**
     * @brief 构造函数
     * @param webServer Web服务器指针（保留参数兼容性，路由已移交 OTARouteHandler）
     */
    explicit OTAManager(AsyncWebServer* webServer = nullptr);
    
    /**
     * @brief 初始化OTA管理器
     * @return 初始化是否成功
     */
    bool initialize();

    // ===== OTA 核心原语（与 HTTP 层解耦，上传/URL 下载两条路径共用） =====

    /**
     * @brief 开始一次 OTA 写入会话
     * @param expectedSize 固件预期大小（字节）；0 表示大小未知，
     *        此时按最大可用 Sketch 空间预留（maxSketchSpace）
     * @return 是否成功进入写入状态（含内存门控检查）
     * @note 内部执行 DRAM≥20KB 硬门槛与 HealthMonitor SEVERE 级别门控，
     *       失败原因可通过 getErrorMessage() 获取
     */
    bool beginOTA(size_t expectedSize = 0);

    /**
     * @brief 写入一个固件数据分块
     * @param data 数据指针
     * @param len 数据长度
     * @return 实际写入字节数；写入失败返回 0 并终止会话
     */
    size_t writeData(const uint8_t* data, size_t len);

    /**
     * @brief 结束 OTA 写入并校验固件完整性
     * @return 校验通过返回 true（状态置为 OTA_COMPLETED）
     */
    bool endOTA();

    /**
     * @brief 取消当前 OTA 会话（丢弃已写入数据）
     */
    void cancelOTA();

    /**
     * @brief 内存门控检查：OTA 写入需要 ~4KB 临时缓冲
     * @param reason 输出拒绝原因（可为 nullptr）
     * @return DRAM≥20KB 且内存压力未达 SEVERE 时返回 true
     */
    bool checkMemoryGate(String* reason = nullptr);

    /**
     * @brief 获取状态 JSON（扁平结构，供测试与脚本使用）
     */
    String getStatusJSON() const;

    // ===== HTTP 处理器（由 OTARouteHandler 委托调用） =====
    
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
     * @brief 在独立后台任务中执行URL OTA下载（不阻塞 async_tcp）
     * @param url 固件下载URL
     * @return 任务创建是否成功（已有OTA进行中时返回 false）
     */
    bool startOTAAsync(const String& url);
    
    /**
     * @brief 检查并回收僵死的上传型 OTA（客户端中途断连后 otaInProgress 永久残留）
     * @note 在 begin/status/upload 入口惰性调用，无需额外定时任务
     */
    void checkStalledUpload();

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
    
    /**
     * @brief 获取错误信息
     * @return 错误信息字符串
     */
    String getErrorMessage() const;

private:
    /**
     * @brief 计算当前可用的最大 Sketch 升级空间
     */
    static size_t calcMaxSketchSpace();

    // OTA 内存门控硬门槛：至少 20KB 8BIT DRAM 可用
    static const uint32_t OTA_MIN_FREE_DRAM = 20480;

    // 成员变量
    bool otaInProgress;               ///< OTA是否进行中
    bool otaUploadMode;               ///< 是否为 Web 上传型 OTA（区别于 URL 下载型）
    unsigned long otaStartTime;       ///< OTA开始时间
    unsigned long otaLastChunkTime;   ///< 上传型 OTA 最近一个分块到达时间
    size_t totalSize;                 ///< 固件剩余大小（URL 下载路径递减，未知大小时为 0）
    size_t totalExpected;             ///< 固件总大小（初始值，用于进度计算，0=未知）
    size_t currentSize;               ///< 当前已写入大小
    uint8_t progress;                 ///< 当前进度百分比（未知大小时保持 0）
};

#endif 