#ifndef DS1302_DRIVER_H
#define DS1302_DRIVER_H

#include "core/FeatureFlags.h"
#if FASTBEE_ENABLE_DS1302

#include <Arduino.h>

/**
 * @brief DS1302 实时时钟驱动
 * 
 * 功能：
 *   - 3线接口（CE/IO/SCLK）位操作实现
 *   - 支持读取和设置日期时间
 *   - BCD码与十进制自动转换
 *   - 涓流充电控制（用于备份电池）
 *   - RAM读写（31字节用户RAM）
 * 
 * 接线：
 *   - DS1302 VCC → 3.3V 或 5V
 *   - DS1302 GND → GND
 *   - DS1302 CE (RST) → 配置的 cePin
 *   - DS1302 IO (DAT) → 配置的 ioPin
 *   - DS1302 SCLK (CLK) → 配置的 sclkPin
 * 
 * 编译条件：FASTBEE_ENABLE_DS1302=1
 */

/**
 * @brief 日期时间结构体
 */
struct DS1302DateTime {
    uint8_t year;       // 年 (0-99, 实际表示 2000-2099)
    uint8_t month;      // 月 (1-12)
    uint8_t day;        // 日 (1-31)
    uint8_t hour;       // 时 (0-23)
    uint8_t minute;     // 分 (0-59)
    uint8_t second;     // 秒 (0-59)
    uint8_t dayOfWeek;  // 星期 (1-7, 1=周一)
    
    /**
     * @brief 格式化为字符串
     * @param format 格式，如 "YYYY-MM-DD HH:MM:SS"
     * @return 格式化后的时间字符串
     */
    String toString(const char* format = "YYYY-MM-DD HH:MM:SS") const;
    
    /**
     * @brief 获取 Unix 时间戳（从 2000-01-01 起算）
     */
    uint32_t toUnixTimestamp() const;
};

/**
 * @brief 涓流充电控制参数
 */
enum class DS1302TrickleCharge : uint8_t {
    DISABLED = 0x00,           // 禁用涓流充电
    DIODE_1_2K = 0xA5,         // 1二极管 + 2K电阻
    DIODE_1_4K = 0xA6,         // 1二极管 + 4K电阻
    DIODE_1_8K = 0xA7,         // 1二极管 + 8K电阻
    DIODE_2_2K = 0xA9,         // 2二极管 + 2K电阻
    DIODE_2_4K = 0xAA,         // 2二极管 + 4K电阻
    DIODE_2_8K = 0xAB          // 2二极管 + 8K电阻
};

class DS1302Driver {
public:
    static DS1302Driver& getInstance();
    
    /**
     * @brief 初始化 DS1302
     * @param cePin CE (RST) 引脚
     * @param ioPin IO (DAT) 引脚
     * @param sclkPin SCLK (CLK) 引脚
     * @return 初始化是否成功
     */
    bool begin(uint8_t cePin, uint8_t ioPin, uint8_t sclkPin);
    
    /**
     * @brief 停止 DS1302 通信
     */
    void end();
    
    /**
     * @brief 读取当前日期时间
     * @return 日期时间结构体
     */
    DS1302DateTime readDateTime();
    
    /**
     * @brief 设置日期时间
     * @param dt 日期时间结构体
     * @return 设置是否成功
     */
    bool setDateTime(const DS1302DateTime& dt);
    
    /**
     * @brief 检查时钟是否正在运行
     * @return 时钟运行状态
     */
    bool isRunning();
    
    /**
     * @brief 设置时钟运行状态
     * @param run true=运行, false=暂停
     */
    void setRunning(bool run);
    
    /**
     * @brief 设置 24 小时制模式
     * @param mode24 true=24小时制, false=12小时制
     */
    void set24HourMode(bool mode24);
    
    /**
     * @brief 检查是否为 24 小时制模式
     */
    bool is24HourMode();
    
    /**
     * @brief 设置涓流充电模式（用于备份电池/超级电容）
     * @param mode 充电模式
     */
    void setTrickleCharge(DS1302TrickleCharge mode);
    
    /**
     * @brief 写入用户 RAM（31字节）
     * @param address 地址 (0-30)
     * @param data 数据字节
     * @return 写入是否成功
     */
    bool writeRam(uint8_t address, uint8_t data);
    
    /**
     * @brief 读取用户 RAM
     * @param address 地址 (0-30)
     * @return 数据字节，失败返回 0xFF
     */
    uint8_t readRam(uint8_t address);
    
    /**
     * @brief 批量写入用户 RAM
     * @param data 数据数组
     * @param len 数据长度 (最大31)
     * @return 写入是否成功
     */
    bool writeRamBurst(const uint8_t* data, uint8_t len);
    
    /**
     * @brief 批量读取用户 RAM
     * @param data 输出缓冲区
     * @param len 读取长度 (最大31)
     * @return 读取是否成功
     */
    bool readRamBurst(uint8_t* data, uint8_t len);
    
    bool isInitialized() const { return _initialized; }
    
private:
    DS1302Driver() = default;
    ~DS1302Driver();
    DS1302Driver(const DS1302Driver&) = delete;
    DS1302Driver& operator=(const DS1302Driver&) = delete;
    
    uint8_t _cePin = 255;
    uint8_t _ioPin = 255;
    uint8_t _sclkPin = 255;
    bool _initialized = false;
    
    // DS1302 寄存器地址
    static constexpr uint8_t REG_SECONDS = 0x80;
    static constexpr uint8_t REG_MINUTES = 0x82;
    static constexpr uint8_t REG_HOURS = 0x84;
    static constexpr uint8_t REG_DATE = 0x86;
    static constexpr uint8_t REG_MONTH = 0x88;
    static constexpr uint8_t REG_DAY = 0x8A;
    static constexpr uint8_t REG_YEAR = 0x8C;
    static constexpr uint8_t REG_WP = 0x8E;        // 写保护寄存器
    static constexpr uint8_t REG_TC = 0x90;        // 涓流充电寄存器
    static constexpr uint8_t REG_RAM_BASE = 0xC0;  // RAM 基地址
    static constexpr uint8_t REG_CLOCK_BURST = 0xBE;
    static constexpr uint8_t REG_RAM_BURST = 0xFE;
    
    // 内部方法
    void _writeByte(uint8_t data);
    uint8_t _readByte();
    void _writeRegister(uint8_t reg, uint8_t data);
    uint8_t _readRegister(uint8_t reg);
    void _setWriteProtect(bool enable);
    
    // BCD 码转换
    static uint8_t _decToBcd(uint8_t val);
    static uint8_t _bcdToDec(uint8_t val);
};

#endif // FASTBEE_ENABLE_DS1302
#endif // DS1302_DRIVER_H
