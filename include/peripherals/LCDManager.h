/**
 * @description: LCD/OLED 显示屏管理器
 * @author: kerwincui
 * @copyright: FastBee All rights reserved.
 * @date: 2025-04-15
 */

#ifndef LCD_MANAGER_H
#define LCD_MANAGER_H

#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <SPI.h>
#include <ArduinoJson.h>
#include "core/PeripheralConfig.h"
#include <vector>

/**
 * @brief 传感器显示数据条目
 * 
 * 通用传感器数据结构，支持任意类型传感器的显示。
 * 每条数据包含标识符、显示标签、数值、单位和更新时间戳。
 */
struct SensorDisplayEntry {
    String id;              // 唯一标识（如 "dht_01_temp"）
    String label;           // 显示标签（如 "温度"、"湿度"）
    float value;            // 当前数值
    String unit;            // 单位（如 "°C"、"%"）
    uint8_t decimals;       // 小数位数
    unsigned long timestamp; // 最后更新时间 (millis)
    bool valid;             // 数据是否有效
};

/**
 * @brief 显示屏接口类型
 */
enum class DisplayInterface : uint8_t {
    PARALLEL = 0,    // 并行接口
    SPI_MODE = 1,    // SPI接口
    I2C_MODE = 2     // I2C接口
};

/**
 * @brief 显示屏控制器类型
 */
enum class DisplayController : uint8_t {
    SSD1306 = 0,     // 0.96/1.3寸 OLED (最常见)
    SH1106 = 1,      // 1.3寸 OLED
    SSD1309 = 2,     // 2.42寸 OLED
    ST7567 = 3,      // LCD
    ST7920 = 4,      // 128x64 LCD
    PCD8544 = 5      // Nokia 5110 LCD
};

/**
 * @brief 显示内容对齐方式
 */
enum class TextAlign : uint8_t {
    LEFT = 1,
    CENTER = 2,
    RIGHT = 3
};

/**
 * @brief LCD/OLED 显示屏管理器
 * 
 * 支持多种 OLED/LCD 显示屏，提供统一的显示接口。
 * 主要支持 I2C 接口的 SSD1306/SH1106 等 OLED 显示屏。
 * 
 * @note 使用单例模式，通过 getInstance() 获取实例
 * @note 线程安全，支持在 FreeRTOS 任务中调用
 */
class LCDManager {
public:
    /**
     * @brief 获取单例实例
     */
    static LCDManager& getInstance();
    
    // 禁止拷贝
    LCDManager(const LCDManager&) = delete;
    LCDManager& operator=(const LCDManager&) = delete;
    
    // ========== 初始化与配置 ==========
    
    /**
     * @brief 初始化显示屏
     * 
     * @param config 外设配置
     * @return bool 成功返回true
     */
    bool initialize(const PeripheralConfig& config);
    
    /**
     * @brief 反初始化显示屏
     */
    void deinitialize();
    
    /**
     * @brief 检查是否已初始化
     */
    bool isInitialized() const { return _initialized; }
    
    // ========== 显示操作 ==========
    
    /**
     * @brief 清空屏幕
     */
    void clear();
    
    /**
     * @brief 显示文本
     * 
     * @param text 文本内容
     * @param x X坐标（像素）
     * @param y Y坐标（像素，基线位置）
     * @param align 对齐方式
     * @return bool 成功返回true
     */
    bool print(const String& text, uint8_t x = 0, uint8_t y = 0, 
               TextAlign align = TextAlign::LEFT);
    
    /**
     * @brief 显示文本（自动换行）
     * 
     * @param text 文本内容
     * @param line 行号（0开始）
     * @return bool 成功返回true
     */
    bool printLine(const String& text, uint8_t line = 0);
    
    /**
     * @brief 显示多行文本
     * 
     * @param lines 行内容数组
     * @param lineCount 行数
     * @return bool 成功返回true
     */
    bool printLines(const String lines[], uint8_t lineCount);
    
    /**
     * @brief 自定义多行文本显示（供外设执行规则使用）
     * 
     * 内容按 \n 切分，首行以 # 开头时视为居中标题（自动绘制分隔线），
     * 其余行默认左对齐。最多显示 MAX_CUSTOM_LINES 行，超出截断。
     * 
     * @param content 多行文本内容
     * @return bool 成功返回true
     */
    bool showCustomText(const String& content);
    
    /**
     * @brief 显示传感器数据（单条）
     * 
     * @param name 传感器名称
     * @param value 数值
     * @param unit 单位
     * @param line 显示行号
     * @return bool 成功返回true
     */
    bool showSensorData(const String& name, float value, const String& unit, uint8_t line = 0);
    
    // ========== 通用传感器数据显示模块 ==========
    
    /**
     * @brief 更新传感器数据（供外部模块推送数据）
     * 
     * 如果 id 已存在则更新，否则新增条目。
     * 
     * @param id 传感器唯一标识
     * @param label 显示标签
     * @param value 数值
     * @param unit 单位
     * @param decimals 小数位数
     */
    void updateSensorEntry(const String& id, const String& label, float value, 
                           const String& unit, uint8_t decimals = 1);
    
    /**
     * @brief 标记传感器数据无效（读取失败时调用）
     */
    void invalidateSensorEntry(const String& id);
    
    /**
     * @brief 显示传感器数据页面
     * 
     * 自动分页显示所有已注册的传感器数据。
     * 每页显示 maxLines-1 条数据（顶部保留标题行）。
     * 
     * @param page 页码（0开始），-1 表示自动轮播
     * @return bool 成功返回true
     */
    bool showSensorPage(int8_t page = -1);
    
    /**
     * @brief 获取已注册的传感器条目数
     */
    uint8_t getSensorEntryCount() const { return _sensorEntries.size(); }
    
    /**
     * @brief 获取传感器显示总页数
     */
    uint8_t getSensorPageCount() const;
    
    /**
     * @brief 自动刷新传感器页面（在主循环中调用）
     * 
     * 根据设定的轮播间隔自动切换页面并刷新显示。
     * 
     * @param intervalMs 页面轮播间隔（毫秒），默认5秒
     */
    void autoRefreshSensorDisplay(unsigned long intervalMs = 5000);
    
    /**
     * @brief 显示系统信息
     * 
     * 显示IP地址、WiFi状态、内存使用等信息
     */
    bool showSystemInfo();
    
    // ========== 图形绘制 ==========
    
    /**
     * @brief 绘制线条
     */
    bool drawLine(uint8_t x1, uint8_t y1, uint8_t x2, uint8_t y2);
    
    /**
     * @brief 绘制矩形
     */
    bool drawRect(uint8_t x, uint8_t y, uint8_t w, uint8_t h);
    
    /**
     * @brief 绘制填充矩形
     */
    bool drawBox(uint8_t x, uint8_t y, uint8_t w, uint8_t h);
    
    /**
     * @brief 绘制圆形
     */
    bool drawCircle(uint8_t x, uint8_t y, uint8_t r);
    
    /**
     * @brief 绘制填充圆形
     */
    bool drawDisc(uint8_t x, uint8_t y, uint8_t r);
    
    // ========== 高级功能 ==========
    
    /**
     * @brief 设置字体
     * 
     * @param fontIndex 字体索引（0-小字体，1-中字体，2-大字体）
     */
    bool setFont(uint8_t fontIndex);
    
    /**
     * @brief 设置对比度
     * 
     * @param contrast 对比度（0-255）
     */
    bool setContrast(uint8_t contrast);
    
    /**
     * @brief 翻转显示
     * 
     * @param flip true=翻转，false=正常
     */
    bool setFlip(bool flip);
    
    /**
     * @brief 设置显示开关
     */
    bool setDisplayOn(bool on);
    
    /**
     * @brief 刷新显示
     * 
     * 将缓冲区内容发送到显示屏
     */
    bool refresh();
    
    // ========== 状态查询 ==========
    
    /**
     * @brief 获取显示屏宽度
     */
    uint8_t getWidth() const { return _width; }
    
    /**
     * @brief 获取显示屏高度
     */
    uint8_t getHeight() const { return _height; }
    
    /**
     * @brief 获取当前字体高度
     */
    uint8_t getFontHeight() const;
    
    /**
     * @brief 获取可显示的最大行数
     */
    uint8_t getMaxLines() const;
    
private:
    LCDManager() = default;
    
    // 显示屏对象
    U8G2* _display = nullptr;
    
    // 配置参数
    uint8_t _width = 128;
    uint8_t _height = 64;
    DisplayInterface _interface = DisplayInterface::I2C_MODE;
    DisplayController _controller = DisplayController::SSD1306;
    uint8_t _i2cAddress = 0x3C;      // 默认 I2C 地址
    int8_t _resetPin = -1;           // Reset 引脚（-1表示共用 Arduino Reset）
    uint8_t _currentFont = 1;        // 当前字体索引
    
    // 状态标志
    bool _initialized = false;
    bool _contentChanged = false;
    unsigned long _lastUpdate = 0;
    const unsigned long _minUpdateInterval = 50;  // 最小刷新间隔（ms）
    
    // 传感器显示模块
    std::vector<SensorDisplayEntry> _sensorEntries;  // 传感器数据注册表
    uint8_t _currentSensorPage = 0;                  // 当前显示页码
    unsigned long _lastPageSwitch = 0;               // 上次翻页时间
    static constexpr uint8_t MAX_SENSOR_ENTRIES = 16; // 最大传感器条目数
    
    // 内部方法
    
    /**
     * @brief 根据配置创建显示屏对象
     */
    bool createDisplay(const PeripheralConfig& config);
    
    /**
     * @brief 设置默认字体
     */
    void setDefaultFont();
    
    /**
     * @brief 检查是否需要刷新
     */
    bool shouldUpdate();
};

#endif // LCD_MANAGER_H
