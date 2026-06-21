#include "core/FeatureFlags.h"
#if FASTBEE_ENABLE_DS1302

#include "peripherals/drivers/DS1302Driver.h"
#include "systems/LoggerSystem.h"

// 单例实现
DS1302Driver& DS1302Driver::getInstance()
{
    static DS1302Driver instance;
    return instance;
}

DS1302Driver::~DS1302Driver()
{
    end();
}

// ========== 初始化与配置 ==========

bool DS1302Driver::begin(uint8_t cePin, uint8_t ioPin, uint8_t sclkPin)
{
    LOG_INFO("DS1302: Initializing...");
    
    // 如果已初始化，先停止
    if (_initialized) {
        end();
    }
    
    // 验证引脚有效性
    if (cePin == 255 || ioPin == 255 || sclkPin == 255) {
        LOG_ERROR("DS1302: Invalid pin configuration");
        return false;
    }
    
    _cePin = cePin;
    _ioPin = ioPin;
    _sclkPin = sclkPin;
    
    // 配置引脚
    pinMode(_cePin, OUTPUT);
    digitalWrite(_cePin, LOW);
    
    pinMode(_sclkPin, OUTPUT);
    digitalWrite(_sclkPin, LOW);
    
    // IO 引脚初始为输入（读取时需要）
    pinMode(_ioPin, INPUT);
    
    // 禁用写保护
    _setWriteProtect(false);
    
    // 启用涓流充电（可选，用于备份电池）
    // 默认使用 1 二极管 + 2K 电阻
    // setTrickleCharge(DS1302TrickleCharge::DIODE_1_2K);
    
    _initialized = true;
    LOG_INFOF("DS1302: Initialized CE=%d IO=%d SCLK=%d", _cePin, _ioPin, _sclkPin);
    
    return true;
}

void DS1302Driver::end()
{
    if (!_initialized) return;
    
    // 恢复引脚状态
    pinMode(_cePin, INPUT);
    pinMode(_ioPin, INPUT);
    pinMode(_sclkPin, INPUT);
    
    _cePin = 255;
    _ioPin = 255;
    _sclkPin = 255;
    _initialized = false;
    
    LOG_INFO("DS1302: Stopped");
}

// ========== 时间操作 ==========

DS1302DateTime DS1302Driver::readDateTime()
{
    DS1302DateTime dt = {0, 0, 0, 0, 0, 0, 0};
    
    if (!_initialized) {
        LOG_WARNING("DS1302: Not initialized, returning zero datetime");
        return dt;
    }
    
    // 拉高 CE 开始通信
    digitalWrite(_cePin, HIGH);
    delayMicroseconds(1);
    
    // 发送时钟突发读取命令 (0xBF)
    _writeByte(REG_CLOCK_BURST | 0x01);
    
    // 读取 8 个字节：秒、分、时、日、月、星期、年、写保护
    dt.second = _bcdToDec(_readByte() & 0x7F);
    dt.minute = _bcdToDec(_readByte() & 0x7F);
    
    uint8_t hourReg = _readByte();
    if (hourReg & 0x80) {
        // 12 小时制
        dt.hour = _bcdToDec(hourReg & 0x1F);
        if (hourReg & 0x20) dt.hour += 12; // PM
    } else {
        // 24 小时制
        dt.hour = _bcdToDec(hourReg & 0x3F);
    }
    
    dt.day = _bcdToDec(_readByte() & 0x3F);
    dt.month = _bcdToDec(_readByte() & 0x1F);
    dt.dayOfWeek = _bcdToDec(_readByte() & 0x07);
    dt.year = _bcdToDec(_readByte());
    
    // 读取写保护寄存器（丢弃）
    _readByte();
    
    // 拉低 CE 结束通信
    digitalWrite(_cePin, LOW);
    
    return dt;
}

bool DS1302Driver::setDateTime(const DS1302DateTime& dt)
{
    if (!_initialized) {
        LOG_ERROR("DS1302: Not initialized");
        return false;
    }
    
    // 验证参数
    if (dt.month < 1 || dt.month > 12 ||
        dt.day < 1 || dt.day > 31 ||
        dt.hour > 23 || dt.minute > 59 || dt.second > 59) {
        LOG_ERROR("DS1302: Invalid datetime values");
        return false;
    }
    
    // 禁用写保护
    _setWriteProtect(false);
    
    // 拉高 CE 开始通信
    digitalWrite(_cePin, HIGH);
    delayMicroseconds(1);
    
    // 发送时钟突发写入命令 (0xBE)
    _writeByte(REG_CLOCK_BURST);
    
    // 写入 8 个字节
    _writeByte(_decToBcd(dt.second));
    _writeByte(_decToBcd(dt.minute));
    _writeByte(_decToBcd(dt.hour)); // 24 小时制
    _writeByte(_decToBcd(dt.day));
    _writeByte(_decToBcd(dt.month));
    _writeByte(_decToBcd(dt.dayOfWeek));
    _writeByte(_decToBcd(dt.year));
    _writeByte(0x00); // 写保护寄存器
    
    // 拉低 CE 结束通信
    digitalWrite(_cePin, LOW);
    
    LOG_INFOF("DS1302: DateTime set to 20%02d-%02d-%02d %02d:%02d:%02d",
              dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second);
    
    return true;
}

bool DS1302Driver::isRunning()
{
    if (!_initialized) return false;
    
    uint8_t sec = _readRegister(REG_SECONDS);
    return !(sec & 0x80); // CH 位为 0 表示运行中
}

void DS1302Driver::setRunning(bool run)
{
    if (!_initialized) return;
    
    uint8_t sec = _readRegister(REG_SECONDS);
    if (run) {
        sec &= ~0x80; // 清除 CH 位
    } else {
        sec |= 0x80;  // 设置 CH 位
    }
    _writeRegister(REG_SECONDS, sec);
}

void DS1302Driver::set24HourMode(bool mode24)
{
    if (!_initialized) return;
    
    uint8_t hour = _readRegister(REG_HOURS);
    if (mode24) {
        hour &= ~0x80; // 清除 12/24 位
    } else {
        hour |= 0x80;  // 设置 12/24 位
    }
    _writeRegister(REG_HOURS, hour);
}

bool DS1302Driver::is24HourMode()
{
    if (!_initialized) return true;
    
    uint8_t hour = _readRegister(REG_HOURS);
    return !(hour & 0x80);
}

void DS1302Driver::setTrickleCharge(DS1302TrickleCharge mode)
{
    if (!_initialized) return;
    
    _writeRegister(REG_TC, static_cast<uint8_t>(mode));
    LOG_INFOF("DS1302: Trickle charge set to 0x%02X", static_cast<uint8_t>(mode));
}

// ========== RAM 操作 ==========

bool DS1302Driver::writeRam(uint8_t address, uint8_t data)
{
    if (!_initialized || address > 30) return false;
    
    _setWriteProtect(false);
    _writeRegister(REG_RAM_BASE + (address * 2), data);
    return true;
}

uint8_t DS1302Driver::readRam(uint8_t address)
{
    if (!_initialized || address > 30) return 0xFF;
    
    return _readRegister(REG_RAM_BASE + (address * 2) + 1);
}

bool DS1302Driver::writeRamBurst(const uint8_t* data, uint8_t len)
{
    if (!_initialized || !data || len > 31) return false;
    
    _setWriteProtect(false);
    
    digitalWrite(_cePin, HIGH);
    delayMicroseconds(1);
    
    _writeByte(REG_RAM_BURST);
    for (uint8_t i = 0; i < len; i++) {
        _writeByte(data[i]);
    }
    
    digitalWrite(_cePin, LOW);
    return true;
}

bool DS1302Driver::readRamBurst(uint8_t* data, uint8_t len)
{
    if (!_initialized || !data || len > 31) return false;
    
    digitalWrite(_cePin, HIGH);
    delayMicroseconds(1);
    
    _writeByte(REG_RAM_BURST | 0x01);
    for (uint8_t i = 0; i < len; i++) {
        data[i] = _readByte();
    }
    
    digitalWrite(_cePin, LOW);
    return true;
}

// ========== 内部方法 ==========

void DS1302Driver::_writeByte(uint8_t data)
{
    pinMode(_ioPin, OUTPUT);
    
    for (uint8_t i = 0; i < 8; i++) {
        digitalWrite(_sclkPin, LOW);
        delayMicroseconds(1);
        
        // LSB first
        digitalWrite(_ioPin, (data & 0x01) ? HIGH : LOW);
        data >>= 1;
        
        digitalWrite(_sclkPin, HIGH);
        delayMicroseconds(1);
    }
}

uint8_t DS1302Driver::_readByte()
{
    pinMode(_ioPin, INPUT);
    
    uint8_t data = 0;
    for (uint8_t i = 0; i < 8; i++) {
        digitalWrite(_sclkPin, LOW);
        delayMicroseconds(1);
        
        // LSB first
        if (digitalRead(_ioPin)) {
            data |= (1 << i);
        }
        
        digitalWrite(_sclkPin, HIGH);
        delayMicroseconds(1);
    }
    
    return data;
}

void DS1302Driver::_writeRegister(uint8_t reg, uint8_t data)
{
    digitalWrite(_cePin, HIGH);
    delayMicroseconds(1);
    
    _writeByte(reg);
    _writeByte(data);
    
    digitalWrite(_cePin, LOW);
}

uint8_t DS1302Driver::_readRegister(uint8_t reg)
{
    digitalWrite(_cePin, HIGH);
    delayMicroseconds(1);
    
    _writeByte(reg | 0x01); // 读操作
    uint8_t data = _readByte();
    
    digitalWrite(_cePin, LOW);
    return data;
}

void DS1302Driver::_setWriteProtect(bool enable)
{
    _writeRegister(REG_WP, enable ? 0x80 : 0x00);
}

// ========== BCD 码转换 ==========

uint8_t DS1302Driver::_decToBcd(uint8_t val)
{
    return ((val / 10) << 4) | (val % 10);
}

uint8_t DS1302Driver::_bcdToDec(uint8_t val)
{
    return ((val >> 4) * 10) + (val & 0x0F);
}

// ========== DS1302DateTime 实现 ==========

String DS1302DateTime::toString(const char* format) const
{
    String result = String(format);
    
    char buf[5];
    
    // 替换年份
    snprintf(buf, sizeof(buf), "20%02d", year);
    result.replace("YYYY", String(buf));
    
    // 替换月份
    snprintf(buf, sizeof(buf), "%02d", month);
    result.replace("MM", String(buf));
    
    // 替换日
    snprintf(buf, sizeof(buf), "%02d", day);
    result.replace("DD", String(buf));
    
    // 替换时
    snprintf(buf, sizeof(buf), "%02d", hour);
    result.replace("HH", String(buf));
    
    // 替换分（注意 MM 已被替换为月份，这里用 NN 表示分钟）
    // 但标准格式用 MM:SS，所以在 HH:MM:SS 中第二次出现的 MM 是分
    // 为简化处理，假设格式中第一个 MM 是月份，后续用 nn 表示分
    snprintf(buf, sizeof(buf), "%02d", minute);
    result.replace("nn", String(buf));
    
    // 替换秒
    snprintf(buf, sizeof(buf), "%02d", second);
    result.replace("SS", String(buf));
    
    return result;
}

uint32_t DS1302DateTime::toUnixTimestamp() const
{
    // 简化的时间戳计算（从 2000-01-01 00:00:00 起算）
    static const uint8_t daysInMonth[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    
    uint32_t timestamp = 0;
    
    // 年份
    for (uint8_t y = 0; y < year; y++) {
        timestamp += (y % 4 == 0) ? 366 : 365;
    }
    
    // 月份
    for (uint8_t m = 1; m < month; m++) {
        timestamp += daysInMonth[m - 1];
        if (m == 2 && year % 4 == 0) timestamp++; // 闰年 2 月
    }
    
    // 日
    timestamp += (day - 1);
    
    // 转换为秒
    timestamp = timestamp * 86400UL + hour * 3600UL + minute * 60UL + second;
    
    // 加上 2000-01-01 到 1970-01-01 的偏移
    timestamp += 946684800UL;
    
    return timestamp;
}

#endif // FASTBEE_ENABLE_DS1302
