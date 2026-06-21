#ifndef LCD1602_DRIVER_H
#define LCD1602_DRIVER_H

#include "core/FeatureFlags.h"
#if FASTBEE_ENABLE_LCD1602

#include <Arduino.h>

/**
 * @brief LCD1602 I2C 字符液晶驱动
 * 
 * 功能：
 *   - I2C 接口（通常通过 PCF8574/PCF8574A 扩展板）
 *   - 支持 16x2 和 20x4 字符液晶
 *   - I2C 地址自动检测（0x20-0x27, 0x38-0x3F）
 *   - 文本显示、光标控制、背光控制
 * 
 * 接线：
 *   - I2C SDA → ESP32 SDA (GPIO21)
 *   - I2C SCL → ESP32 SCL (GPIO22)
 *   - VCC → 5V
 *   - GND → GND
 * 
 * 编译条件：FASTBEE_ENABLE_LCD1602=1
 */

/**
 * @brief LCD1602 配置参数
 */
struct LCD1602Config {
    uint8_t cols = 16;           // 列数（16 或 20）
    uint8_t rows = 2;            // 行数（2 或 4）
    uint8_t i2cAddress = 0x27;   // I2C 地址（默认 0x27）
    bool backlight = true;       // 背光开关
};

class LCD1602Driver {
public:
    static LCD1602Driver& getInstance();
    
    /**
     * @brief 初始化 LCD1602
     * @param sdaPin SDA 引脚
     * @param sclPin SCL 引脚
     * @param config 配置参数
     * @return 初始化是否成功
     */
    bool begin(uint8_t sdaPin, uint8_t sclPin, const LCD1602Config& config = LCD1602Config());
    
    /**
     * @brief 停止 LCD1602
     */
    void end();
    
    // ========== 文本显示 ==========
    
    /**
     * @brief 清屏并移动光标到左上角
     */
    void clear();
    
    /**
     * @brief 移动光标到左上角
     */
    void home();
    
    /**
     * @brief 设置光标位置
     * @param col 列号 (0-based)
     * @param row 行号 (0-based)
     */
    void setCursor(uint8_t col, uint8_t row);
    
    /**
     * @brief 显示文本
     * @param text 文本内容
     * @return 显示是否成功
     */
    bool print(const String& text);
    
    /**
     * @brief 显示整数
     * @param value 整数值
     * @param base 进制 (10=十进制, 16=十六进制)
     */
    bool print(int value, int base = 10);
    
    /**
     * @brief 显示浮点数
     * @param value 浮点数值
     * @param decimals 小数位数
     */
    bool print(float value, uint8_t decimals = 2);
    
    /**
     * @brief 在指定行显示文本
     * @param row 行号 (0-based)
     * @param text 文本内容
     * @return 显示是否成功
     */
    bool printLine(uint8_t row, const String& text);
    
    /**
     * @brief 显示两行文本（16x2 模式）
     * @param line1 第一行文本
     * @param line2 第二行文本
     * @return 显示是否成功
     */
    bool printTwoLines(const String& line1, const String& line2);
    
    // ========== 光标控制 ==========
    
    /**
     * @brief 显示光标（下划线）
     */
    void cursor();
    
    /**
     * @brief 隐藏光标
     */
    void noCursor();
    
    /**
     * @brief 光标闪烁
     */
    void blink();
    
    /**
     * @brief 关闭光标闪烁
     */
    void noBlink();
    
    // ========== 背光控制 ==========
    
    /**
     * @brief 开启背光
     */
    void backlightOn();
    
    /**
     * @brief 关闭背光
     */
    void backlightOff();
    
    /**
     * @brief 设置背光开关
     */
    void setBacklight(bool on);
    
    // ========== 高级功能 ==========
    
    /**
     * @brief 自动滚动显示
     */
    void autoscroll();
    
    /**
     * @brief 关闭自动滚动
     */
    void noAutoscroll();
    
    /**
     * @brief 创建自定义字符
     * @param location 字符位置 (0-7)
     * @param charmap 字符数据（8字节）
     */
    void createChar(uint8_t location, uint8_t charmap[]);
    
    /**
     * @brief 检测 I2C 地址
     * @return 检测到的地址，未找到返回 0
     */
    uint8_t detectAddress();
    
    bool isInitialized() const { return _initialized; }
    uint8_t getCols() const { return _cols; }
    uint8_t getRows() const { return _rows; }
    
private:
    LCD1602Driver() = default;
    ~LCD1602Driver();
    LCD1602Driver(const LCD1602Driver&) = delete;
    LCD1602Driver& operator=(const LCD1602Driver&) = delete;
    
    void* _lcd = nullptr;  // LiquidCrystal_I2C* 或兼容对象
    bool _initialized = false;
    uint8_t _sdaPin = 255;
    uint8_t _sclPin = 255;
    uint8_t _cols = 16;
    uint8_t _rows = 2;
    uint8_t _address = 0x27;
    bool _backlightOn = true;
    
    // 内部方法
    void _sendCommand(uint8_t cmd);
    void _sendData(uint8_t data);
    void _pulseEnable();
    void _write4bits(uint8_t value);
    void _expanderWrite(uint8_t value);
    void _waitMicroseconds(uint16_t us);
};

#endif // FASTBEE_ENABLE_LCD1602
#endif // LCD1602_DRIVER_H
