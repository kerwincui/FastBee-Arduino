#include "core/FeatureFlags.h"
#if FASTBEE_ENABLE_LCD1602

#include "peripherals/drivers/LCD1602Driver.h"
#include "core/I2CBusManager.h"
#include "core/RuntimeFeatureManager.h"
#include "systems/LoggerSystem.h"
#include <cmath>
#include <cstdio>
#include <cstring>

// PCF8574 LCD 背光控制位
#define LCD_BACKLIGHT 0x08
#define LCD_NOBACKLIGHT 0x00

// PCF8574 引脚映射（标准映射）
#define LCD_RS 0x01    // Register Select
#define LCD_RW 0x02    // Read/Write
#define LCD_EN 0x04    // Enable
#define LCD_D4 0x10    // Data bit 4
#define LCD_D5 0x20    // Data bit 5
#define LCD_D6 0x40    // Data bit 6
#define LCD_D7 0x80    // Data bit 7

// LCD 命令
#define LCD_CLEARDISPLAY 0x01
#define LCD_RETURNHOME 0x02
#define LCD_ENTRYMODESET 0x04
#define LCD_DISPLAYCONTROL 0x08
#define LCD_CURSORSHIFT 0x10
#define LCD_FUNCTIONSET 0x20
#define LCD_SETCGRAMADDR 0x40
#define LCD_SETDDRAMADDR 0x80

// 显示控制位
#define LCD_DISPLAYON 0x04
#define LCD_DISPLAYOFF 0x00
#define LCD_CURSORON 0x02
#define LCD_CURSOROFF 0x00
#define LCD_BLINKON 0x01
#define LCD_BLINKOFF 0x00

// 模式设置位
#define LCD_ENTRYRIGHT 0x00
#define LCD_ENTRYLEFT 0x02
#define LCD_ENTRYSHIFTINCREMENT 0x01
#define LCD_ENTRYSHIFTDECREMENT 0x00

// 函数设置位
#define LCD_8BITMODE 0x10
#define LCD_4BITMODE 0x00
#define LCD_2LINE 0x08
#define LCD_1LINE 0x00
#define LCD_5x10DOTS 0x04
#define LCD_5x8DOTS 0x00

// 行偏移地址
static const uint8_t ROW_OFFSETS[] = {0x00, 0x40, 0x14, 0x54};

// 单例实现
LCD1602Driver& LCD1602Driver::getInstance()
{
    static LCD1602Driver instance;
    static const FastBee::FeatureInstanceObserver observer{"lcd1602", [](const void* context) {
        return static_cast<const LCD1602Driver*>(context)->observeFeatureInstance();
    }, &instance};
    FastBee::FeatureInstanceRegistry::instance().registerObserver(&observer);
    return instance;
}

FastBee::FeatureInstanceSnapshot LCD1602Driver::observeFeatureInstance() const {
    const bool initialized = _initialized.load(std::memory_order_acquire);
    // Only the atomic initialization flag is observable here. In particular,
    // false does not prove pending I2C retirement or heap cleanup completed.
    return {initialized ? FastBee::FeatureInstanceState::Initialized :
        FastBee::FeatureInstanceState::NotInitialized, static_cast<uint16_t>(initialized ? 1 : 0)};
}

LCD1602Driver::~LCD1602Driver()
{
    end();
}

// ========== 初始化与配置 ==========

bool LCD1602Driver::begin(uint8_t sdaPin, uint8_t sclPin, const LCD1602Config& config)
{
    // 运行时功能门控：用户可在 Web UI 禁用 lcd1602
    if (!FastBee::RuntimeFeatureManager::instance().shouldEnable("lcd1602")) {
        LOG_INFO("LCD1602: Compiled but runtime-disabled, skip init");
        return false;
    }

    LOG_INFO("LCD1602: Initializing...");
    
    // 如果已初始化，先停止
    if (_initialized || _busAcquired) {
        end();
    }
    
    // 验证引脚有效性
    if (sdaPin == 255 || sclPin == 255) {
        LOG_ERROR("LCD1602: Invalid pin configuration");
        return false;
    }
    
    // Match PeripheralManager admission for direct driver calls too.
    if (!((config.cols == 16 && config.rows == 2) ||
          (config.cols == 20 && config.rows == 4)) ||
        (config.i2cAddress != 0 &&
         !((config.i2cAddress >= 0x20 && config.i2cAddress <= 0x27) ||
           (config.i2cAddress >= 0x38 && config.i2cAddress <= 0x3F)))) {
        LOG_ERROR("LCD1602: Invalid dimensions or I2C address");
        return false;
    }
    _writeFailed = false;
    _sdaPin = sdaPin;
    _sclPin = sclPin;
    _cols = config.cols;
    _rows = config.rows;
    _address = config.i2cAddress;
    _backlightOn = config.backlight;
    
    String busError;
    if (!FastBee::I2CBusManager::instance().acquire(
            this, _sdaPin, _sclPin, 100000, busError, FastBee::ResourceConsumerRole::Display)) {
        LOG_ERRORF("LCD1602: I2C lease failed: %s", busError.c_str());
        _sdaPin = 255;
        _sclPin = 255;
        return false;
    }
    _busAcquired = true;
    
    // 尝试检测地址
    if (_address == 0) {
        _address = detectAddress();
        if (_address == 0) {
            LOG_ERROR("LCD1602: No LCD found at I2C addresses");
            auto& bus = FastBee::I2CBusManager::instance();
            if (!bus.release(this, 100)) bus.deferRelease(this);
            _busAcquired = false;
            return false;
        }
        LOG_INFOF("LCD1602: Auto-detected I2C address 0x%02X", _address);
    }
    
    // Reserve after optional address detection, before sending any LCD data.
    // Address-only probes do not establish ownership or prove device identity.
    {
        FastBee::I2CBusLock admission(100);
        if (!admission.locked() || !FastBee::I2CBusManager::instance().claimAddress(
                this, _address, busError)) {
            end();
            return false;
        }
    }

    // 初始化 LCD（4位模式）
    const bool ready = [&]() {
        FastBee::I2CBusLock lock(100);
        if (!lock.locked()) return _failWrite();
        const uint8_t light = _backlightOn ? LCD_BACKLIGHT : LCD_NOBACKLIGHT;
        // Arduino LiquidCrystal 1.0.7 / HD44780 four-bit initialization:
        // 3,3,3,2 are DATA nibbles (PCF P4..P7), each clocked with E.
        if (!_expanderWrite(light)) return false; // RS/RW/E initially low.
        delay(50);
        if (!_write4bits(0x30 | light)) return false;
        delay(5);
        if (!_write4bits(0x30 | light)) return false;
        delay(5);
        if (!_write4bits(0x30 | light)) return false;
        delay(1);
        if (!_write4bits(0x20 | light)) return false;
        delay(1);
        if (!_sendCommand(LCD_FUNCTIONSET | LCD_4BITMODE | LCD_2LINE | LCD_5x8DOTS) ||
            !_sendCommand(LCD_DISPLAYCONTROL | LCD_DISPLAYON | LCD_CURSOROFF | LCD_BLINKOFF) ||
            !_sendCommand(LCD_CLEARDISPLAY)) return false;
        _waitMicroseconds(2000);
        // Public clear()/home() are guarded by _initialized, still false here.
        if (!_sendCommand(LCD_ENTRYMODESET | LCD_ENTRYLEFT | LCD_ENTRYSHIFTDECREMENT) ||
            !_sendCommand(LCD_RETURNHOME)) return false;
        _waitMicroseconds(2000);
        return true;
    }();
    if (!ready) {
        LOG_ERROR("LCD1602: Initialization write failed");
        end();
        return false;
    }
    
    _initialized = true;
    LOG_INFOF("LCD1602: Initialized %dx%d at 0x%02X (SDA=%d, SCL=%d)", 
              _cols, _rows, _address, _sdaPin, _sclPin);
    
    return true;
}

void LCD1602Driver::end()
{
    if (_initialized) {
        // 关闭背光并清屏，同时租约仍然有效。
        backlightOff();
        clear();
    }
    
    _initialized = false;
    _sdaPin = 255;
    _sclPin = 255;
    if (_busAcquired) {
        auto& bus = FastBee::I2CBusManager::instance();
        if (!bus.release(this, 100)) bus.deferRelease(this);
        _busAcquired = false;
    }
    
    LOG_INFO("LCD1602: Stopped");
}

// ========== 文本显示 ==========

bool LCD1602Driver::clear()
{
    if (!_initialized) return false;
    FastBee::I2CBusLock lock(100);
    if (!lock.locked()) return _failWrite();
    if (!_sendCommand(LCD_CLEARDISPLAY)) return false;
    _waitMicroseconds(2000);
    return true;
}

void LCD1602Driver::home()
{
    if (!_initialized) return;
    FastBee::I2CBusLock lock(100);
    if (!lock.locked()) { _failWrite(); return; }
    if (!_sendCommand(LCD_RETURNHOME)) return;
    _waitMicroseconds(2000);
}

bool LCD1602Driver::setCursor(uint8_t col, uint8_t row)
{
    if (!_initialized) return false;
    if (row >= _rows) row = _rows - 1;
    if (col >= _cols) col = _cols - 1;
    return _sendCommand(LCD_SETDDRAMADDR | (col + ROW_OFFSETS[row]));
}

bool LCD1602Driver::printAt(uint8_t col, uint8_t row, const String& text)
{
    if (!_initialized || col >= _cols || row >= _rows || text.isEmpty() ||
        text.length() > 128 || text.length() != std::strlen(text.c_str())) return false;
    FastBee::I2CBusLock lock(100);
    if (!lock.locked()) return _failWrite();
    // Never print at an unconfirmed cursor; retain the first failed/partial byte.
    return setCursor(col, row) && print(text);
}

bool LCD1602Driver::print(const String& text)
{
    if (!_initialized) return false;
    
    for (unsigned int i = 0; i < text.length(); i++) {
        if (!_sendData(text.charAt(i))) return false;
    }
    return true;
}

bool LCD1602Driver::print(int value, int base)
{
    if (!_initialized) return false;
    
    String str;
    if (base == 16) {
        str = String(value, HEX);
    } else {
        str = String(value, DEC);
    }
    return print(str);
}

bool LCD1602Driver::print(float value, uint8_t decimals)
{
    if (!_initialized) return false;

    // Do not let large precision/value overflow a fixed stack buffer.
    char buf[32];
    if (!std::isfinite(value) || decimals > sizeof(buf) - 3) return false;
    const int length = std::snprintf(buf, sizeof(buf), "%.*f",
                                     static_cast<int>(decimals), static_cast<double>(value));
    if (length < 0 || static_cast<size_t>(length) >= sizeof(buf)) return false;
    return print(String(buf));
}

bool LCD1602Driver::printLine(uint8_t row, const String& text)
{
    if (!_initialized || row >= _rows) return false;
    FastBee::I2CBusLock lock(100);
    if (!lock.locked()) return _failWrite();
    
    if (!setCursor(0, row)) return false;
    
    // 清空当前行
    for (uint8_t i = 0; i < _cols; i++) {
        if (!_sendData(' ')) return false;
    }
    
    // 回到行首并显示文本
    if (!setCursor(0, row)) return false;
    
    // 限制显示长度
    String displayText = text;
    if (displayText.length() > _cols) {
        displayText = displayText.substring(0, _cols);
    }
    
    return print(displayText);
}

bool LCD1602Driver::printTwoLines(const String& line1, const String& line2)
{
    if (!_initialized) return false;
    
    return printLine(0, line1) && printLine(1, line2);
}

// ========== 光标控制 ==========

void LCD1602Driver::cursor()
{
    if (!_initialized) return;
    _sendCommand(LCD_DISPLAYCONTROL | LCD_DISPLAYON | LCD_CURSORON | LCD_BLINKOFF);
}

void LCD1602Driver::noCursor()
{
    if (!_initialized) return;
    _sendCommand(LCD_DISPLAYCONTROL | LCD_DISPLAYON | LCD_CURSOROFF | LCD_BLINKOFF);
}

void LCD1602Driver::blink()
{
    if (!_initialized) return;
    _sendCommand(LCD_DISPLAYCONTROL | LCD_DISPLAYON | LCD_CURSORON | LCD_BLINKON);
}

void LCD1602Driver::noBlink()
{
    if (!_initialized) return;
    _sendCommand(LCD_DISPLAYCONTROL | LCD_DISPLAYON | LCD_CURSOROFF | LCD_BLINKOFF);
}

// ========== 背光控制 ==========

bool LCD1602Driver::backlightOn()
{
    if (!_initialized) return false;
    FastBee::I2CBusLock lock(100);
    if (!lock.locked()) return _failWrite();
    if (!_expanderWrite(LCD_BACKLIGHT)) return false;
    _backlightOn = true;
    return true;
}

bool LCD1602Driver::backlightOff()
{
    if (!_initialized) return false;
    FastBee::I2CBusLock lock(100);
    if (!lock.locked()) return _failWrite();
    if (!_expanderWrite(LCD_NOBACKLIGHT)) return false;
    _backlightOn = false;
    return true;
}

bool LCD1602Driver::setBacklight(bool on)
{
    return on ? backlightOn() : backlightOff();
}

// ========== 高级功能 ==========

void LCD1602Driver::autoscroll()
{
    if (!_initialized) return;
    _sendCommand(LCD_ENTRYMODESET | LCD_ENTRYLEFT | LCD_ENTRYSHIFTINCREMENT);
}

void LCD1602Driver::noAutoscroll()
{
    if (!_initialized) return;
    _sendCommand(LCD_ENTRYMODESET | LCD_ENTRYLEFT | LCD_ENTRYSHIFTDECREMENT);
}

void LCD1602Driver::createChar(uint8_t location, uint8_t charmap[])
{
    if (!_initialized || location > 7 || !charmap) return;
    FastBee::I2CBusLock lock(100);
    if (!lock.locked()) { _failWrite(); return; }
    
    location &= 0x7;
    if (!_sendCommand(LCD_SETCGRAMADDR | (location << 3))) return;
    for (uint8_t i = 0; i < 8; i++) {
        if (!_sendData(charmap[i])) return;
    }
}

uint8_t LCD1602Driver::detectAddress()
{
    // 扫描常见地址范围
    uint8_t addresses[] = {0x27, 0x3F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E};
    FastBee::I2CBusLock lock(100);
    TwoWire* wire = lock.locked()
        ? FastBee::I2CBusManager::instance().wire(this) : nullptr;
    if (!wire) return 0;
    
    for (uint8_t addr : addresses) {
        wire->beginTransmission(addr);
        if (wire->endTransmission() == 0) {
            return addr;
        }
    }
    
    return 0;
}

// ========== 内部方法 ==========

bool LCD1602Driver::_sendCommand(uint8_t cmd)
{
    FastBee::I2CBusLock lock(100);
    if (!lock.locked()) return _failWrite();
    // RS=0, RW=0
    uint8_t highNibble = (cmd & 0xF0) | (_backlightOn ? LCD_BACKLIGHT : LCD_NOBACKLIGHT);
    uint8_t lowNibble = ((cmd & 0x0F) << 4) | (_backlightOn ? LCD_BACKLIGHT : LCD_NOBACKLIGHT);
    
    return _write4bits(highNibble) && _write4bits(lowNibble);
}

bool LCD1602Driver::_sendData(uint8_t data)
{
    FastBee::I2CBusLock lock(100);
    if (!lock.locked()) return _failWrite();
    // RS=1, RW=0
    uint8_t highNibble = (data & 0xF0) | LCD_RS | (_backlightOn ? LCD_BACKLIGHT : LCD_NOBACKLIGHT);
    uint8_t lowNibble = ((data & 0x0F) << 4) | LCD_RS | (_backlightOn ? LCD_BACKLIGHT : LCD_NOBACKLIGHT);
    
    return _write4bits(highNibble) && _write4bits(lowNibble);
}

bool LCD1602Driver::_pulseEnable(uint8_t value)
{
    // Keep DATA/RS/backlight unchanged across both E edges.
    _waitMicroseconds(1);
    if (!_expanderWrite(value | LCD_EN)) return false;
    _waitMicroseconds(1);
    if (!_expanderWrite(value & ~LCD_EN)) return false;
    _waitMicroseconds(50);
    return true;
}

bool LCD1602Driver::_write4bits(uint8_t value)
{
    FastBee::I2CBusLock lock(100);
    if (!lock.locked()) return _failWrite();
    value &= ~(LCD_EN | LCD_RW);
    return _expanderWrite(value) && _pulseEnable(value);
}

bool LCD1602Driver::_expanderWrite(uint8_t value)
{
    if (_writeFailed) return false;
    FastBee::I2CBusLock lock(100);
    TwoWire* wire = lock.locked()
        ? FastBee::I2CBusManager::instance().wire(this) : nullptr;
    if (!wire) return _failWrite();
    wire->beginTransmission(_address);
    const bool queued = wire->write(value) == 1;
    // Always end a begun transaction, including a short buffer write.
    const uint8_t status = wire->endTransmission();
    if (!queued || status != 0) return _failWrite();
    return true;
}

bool LCD1602Driver::_failWrite()
{
    _writeFailed = true;
    _initialized = false;
    return false;
}

void LCD1602Driver::_waitMicroseconds(uint16_t us)
{
    delayMicroseconds(us);
}

#endif // FASTBEE_ENABLE_LCD1602
