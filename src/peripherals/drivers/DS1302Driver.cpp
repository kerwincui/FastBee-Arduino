#include "core/FeatureFlags.h"
#if FASTBEE_ENABLE_DS1302

#include "peripherals/drivers/DS1302Driver.h"
#include "core/RuntimeFeatureManager.h"
#include "core/FeatureInstanceRegistry.h"
#include "systems/LoggerSystem.h"

// 单例实现
DS1302Driver& DS1302Driver::getInstance()
{
    static DS1302Driver instance;
    static const FastBee::FeatureInstanceObserver observer{
        "ds1302", [](const void* context) {
            const auto* driver = static_cast<const DS1302Driver*>(context);
            const bool initialized = driver->_initialized.load(std::memory_order_acquire);
            return FastBee::FeatureInstanceSnapshot{initialized
                ? FastBee::FeatureInstanceState::Initialized
                : FastBee::FeatureInstanceState::NotInitialized,
                static_cast<uint16_t>(initialized ? 1 : 0)};
        }, &instance};
    static const bool registered = FastBee::FeatureInstanceRegistry::instance().registerObserver(&observer);
    (void)registered; // Failure retains the truthful 'unobserved' status.
    return instance;
}

DS1302Driver::~DS1302Driver()
{
    end();
}

// ========== 初始化与配置 ==========

bool DS1302Driver::begin(uint8_t cePin, uint8_t ioPin, uint8_t sclkPin)
{
    // 运行时功能门控：用户可在 Web UI 禁用 ds1302
    if (!FastBee::RuntimeFeatureManager::instance().shouldEnable("ds1302")) {
        LOG_INFO("DS1302: Compiled but runtime-disabled, skip init");
        return false;
    }

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
    return readClockSnapshot().dateTime;
}

DS1302ClockSnapshot DS1302Driver::readClockSnapshot()
{
    DS1302ClockSnapshot snapshot;
    DS1302DateTime& dt = snapshot.dateTime;
    
    if (!_initialized) {
        LOG_WARNING("DS1302: Not initialized, returning zero datetime");
        return snapshot;
    }
    
    // 拉高 CE 开始通信
    digitalWrite(_cePin, HIGH);
    delayMicroseconds(1);
    
    // 发送时钟突发读取命令 (0xBF)
    _writeByte(REG_CLOCK_BURST | 0x01);
    
    // 读取 8 个字节：秒、分、时、日、月、星期、年、写保护
    const uint8_t secondsReg = _readByte();
    dt.second = _bcdToDec(secondsReg & 0x7F);
    dt.minute = _bcdToDec(_readByte() & 0x7F);
    
    uint8_t hourReg = _readByte();
    if (hourReg & 0x80) {
        // 12 小时制
        dt.hour = _bcdToDec(hourReg & 0x1F);
        if (dt.hour < 1 || dt.hour > 12) {
            dt.hour = 255; // Keep invalid data invalid through calendar validation.
        } else {
            dt.hour %= 12; // 12 AM is midnight; 12 PM is noon, not hour 24.
            if (hourReg & 0x20) dt.hour += 12;
        }
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
    
    if (!dt.isValid()) return DS1302ClockSnapshot{};
    snapshot.valid = true;
    snapshot.running = !(secondsReg & 0x80);
    snapshot.mode24 = !(hourReg & 0x80);
    return snapshot;
}

bool DS1302Driver::setDateTime(const DS1302DateTime& dt, bool running)
{
    if (!_initialized) {
        LOG_ERROR("DS1302: Not initialized");
        return false;
    }
    
    // 验证参数
    if (!dt.isValid()) {
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
    _writeByte(static_cast<uint8_t>(_decToBcd(dt.second) | (running ? 0 : 0x80)));
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
    
    const uint8_t original = _readRegister(REG_HOURS);
    uint8_t hour = _bcdToDec(original & ((original & 0x80) ? 0x1F : 0x3F));
    if (original & 0x80) {
        if (hour < 1 || hour > 12) return;
        hour %= 12;
        if (original & 0x20) hour += 12;
    } else if (hour > 23) {
        return;
    }
    // Convert the represented hour, not only the mode bit.
    const uint8_t converted = mode24 ? _decToBcd(hour) :
        static_cast<uint8_t>(0x80 | (hour >= 12 ? 0x20 : 0) |
            _decToBcd(hour % 12 == 0 ? 12 : hour % 12));
    if (converted != original) _writeRegister(REG_HOURS, converted);
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
    if ((val & 0x0F) > 9 || (val >> 4) > 9) return 255;
    return ((val >> 4) * 10) + (val & 0x0F);
}

// ========== DS1302DateTime 实现 ==========

bool DS1302DateTime::isValid() const
{
    if (year > 99 || month < 1 || month > 12 || day < 1 ||
        hour > 23 || minute > 59 || second > 59 ||
        dayOfWeek < 1 || dayOfWeek > 7) return false;
    static const uint8_t days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    const uint8_t maximum = days[month - 1] +
        ((month == 2 && year % 4 == 0) ? 1 : 0);
    return day <= maximum;
}

String DS1302DateTime::toString(const char* format) const
{
    if (!isValid() || !format) return String();
    String result = String(format);
    // Disambiguate the documented default's minutes before replacing months.
    // Keep the existing explicit nn token compatible.
    result.replace("HH:MM", "HH:nn");
    
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
    
    // 分钟：默认 HH:MM 已在月份替换前规范化为 HH:nn。
    snprintf(buf, sizeof(buf), "%02d", minute);
    result.replace("nn", String(buf));
    
    // 替换秒
    snprintf(buf, sizeof(buf), "%02d", second);
    result.replace("SS", String(buf));
    
    return result;
}

uint32_t DS1302DateTime::toUnixTimestamp() const
{
    if (!isValid()) return 0;
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
