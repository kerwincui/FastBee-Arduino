#include "core/FeatureFlags.h"
#if FASTBEE_ENABLE_LCD1602

#include "peripherals/drivers/LCD1602Driver.h"
#include "systems/LoggerSystem.h"
#include <Wire.h>

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
    return instance;
}

LCD1602Driver::~LCD1602Driver()
{
    end();
}

// ========== 初始化与配置 ==========

bool LCD1602Driver::begin(uint8_t sdaPin, uint8_t sclPin, const LCD1602Config& config)
{
    LOG_INFO("LCD1602: Initializing...");
    
    // 如果已初始化，先停止
    if (_initialized) {
        end();
    }
    
    // 验证引脚有效性
    if (sdaPin == 255 || sclPin == 255) {
        LOG_ERROR("LCD1602: Invalid pin configuration");
        return false;
    }
    
    _sdaPin = sdaPin;
    _sclPin = sclPin;
    _cols = config.cols;
    _rows = config.rows;
    _address = config.i2cAddress;
    _backlightOn = config.backlight;
    
    // 初始化 I2C
    Wire.begin(_sdaPin, _sclPin);
    
    // 尝试检测地址
    if (_address == 0) {
        _address = detectAddress();
        if (_address == 0) {
            LOG_ERROR("LCD1602: No LCD found at I2C addresses");
            return false;
        }
        LOG_INFOF("LCD1602: Auto-detected I2C address 0x%02X", _address);
    }
    
    // 初始化 LCD（4位模式）
    // 上电延迟
    delay(50);
    
    // 发送初始化序列（按照 HD44780 规范）
    _expanderWrite(0x03 | (_backlightOn ? LCD_BACKLIGHT : LCD_NOBACKLIGHT));
    delay(5);
    _expanderWrite(0x03 | (_backlightOn ? LCD_BACKLIGHT : LCD_NOBACKLIGHT));
    delay(5);
    _expanderWrite(0x03 | (_backlightOn ? LCD_BACKLIGHT : LCD_NOBACKLIGHT));
    delay(1);
    
    // 设置为 4 位模式
    _expanderWrite(0x02 | (_backlightOn ? LCD_BACKLIGHT : LCD_NOBACKLIGHT));
    delay(1);
    
    // 功能设置：4位模式，2行，5x8点阵
    _sendCommand(LCD_FUNCTIONSET | LCD_4BITMODE | 
                 (_rows > 1 ? LCD_2LINE : LCD_1LINE) | LCD_5x8DOTS);
    
    // 显示控制：开显示，关光标，关闪烁
    _sendCommand(LCD_DISPLAYCONTROL | LCD_DISPLAYON | LCD_CURSOROFF | LCD_BLINKOFF);
    
    // 清屏
    clear();
    
    // 输入模式：左到右，无移位
    _sendCommand(LCD_ENTRYMODESET | LCD_ENTRYLEFT | LCD_ENTRYSHIFTDECREMENT);
    
    // 归位
    home();
    
    _initialized = true;
    LOG_INFOF("LCD1602: Initialized %dx%d at 0x%02X (SDA=%d, SCL=%d)", 
              _cols, _rows, _address, _sdaPin, _sclPin);
    
    return true;
}

void LCD1602Driver::end()
{
    if (!_initialized) return;
    
    // 关闭背光
    backlightOff();
    
    // 清屏
    clear();
    
    _initialized = false;
    _sdaPin = 255;
    _sclPin = 255;
    
    LOG_INFO("LCD1602: Stopped");
}

// ========== 文本显示 ==========

void LCD1602Driver::clear()
{
    if (!_initialized) return;
    _sendCommand(LCD_CLEARDISPLAY);
    _waitMicroseconds(2000);
}

void LCD1602Driver::home()
{
    if (!_initialized) return;
    _sendCommand(LCD_RETURNHOME);
    _waitMicroseconds(2000);
}

void LCD1602Driver::setCursor(uint8_t col, uint8_t row)
{
    if (!_initialized) return;
    if (row >= _rows) row = _rows - 1;
    if (col >= _cols) col = _cols - 1;
    _sendCommand(LCD_SETDDRAMADDR | (col + ROW_OFFSETS[row]));
}

bool LCD1602Driver::print(const String& text)
{
    if (!_initialized) return false;
    
    for (unsigned int i = 0; i < text.length(); i++) {
        _sendData(text.charAt(i));
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
    
    String str = String(value, decimals);
    return print(str);
}

bool LCD1602Driver::printLine(uint8_t row, const String& text)
{
    if (!_initialized || row >= _rows) return false;
    
    setCursor(0, row);
    
    // 清空当前行
    for (uint8_t i = 0; i < _cols; i++) {
        _sendData(' ');
    }
    
    // 回到行首并显示文本
    setCursor(0, row);
    
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

void LCD1602Driver::backlightOn()
{
    if (!_initialized) return;
    _backlightOn = true;
    _expanderWrite(LCD_BACKLIGHT);
}

void LCD1602Driver::backlightOff()
{
    if (!_initialized) return;
    _backlightOn = false;
    _expanderWrite(LCD_NOBACKLIGHT);
}

void LCD1602Driver::setBacklight(bool on)
{
    if (on) backlightOn();
    else backlightOff();
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
    if (!_initialized || location > 7) return;
    
    location &= 0x7;
    _sendCommand(LCD_SETCGRAMADDR | (location << 3));
    for (uint8_t i = 0; i < 8; i++) {
        _sendData(charmap[i]);
    }
}

uint8_t LCD1602Driver::detectAddress()
{
    // 扫描常见地址范围
    uint8_t addresses[] = {0x27, 0x3F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E};
    
    for (uint8_t addr : addresses) {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0) {
            return addr;
        }
    }
    
    return 0;
}

// ========== 内部方法 ==========

void LCD1602Driver::_sendCommand(uint8_t cmd)
{
    // RS=0, RW=0
    uint8_t highNibble = (cmd & 0xF0) | (_backlightOn ? LCD_BACKLIGHT : LCD_NOBACKLIGHT);
    uint8_t lowNibble = ((cmd & 0x0F) << 4) | (_backlightOn ? LCD_BACKLIGHT : LCD_NOBACKLIGHT);
    
    _write4bits(highNibble);
    _write4bits(lowNibble);
}

void LCD1602Driver::_sendData(uint8_t data)
{
    // RS=1, RW=0
    uint8_t highNibble = (data & 0xF0) | LCD_RS | (_backlightOn ? LCD_BACKLIGHT : LCD_NOBACKLIGHT);
    uint8_t lowNibble = ((data & 0x0F) << 4) | LCD_RS | (_backlightOn ? LCD_BACKLIGHT : LCD_NOBACKLIGHT);
    
    _write4bits(highNibble);
    _write4bits(lowNibble);
}

void LCD1602Driver::_pulseEnable()
{
    // 脉冲使能信号
    uint8_t currentValue = (_backlightOn ? LCD_BACKLIGHT : LCD_NOBACKLIGHT);
    
    _expanderWrite(currentValue & ~LCD_EN);
    _waitMicroseconds(1);
    _expanderWrite(currentValue | LCD_EN);
    _waitMicroseconds(1);
    _expanderWrite(currentValue & ~LCD_EN);
    _waitMicroseconds(50);
}

void LCD1602Driver::_write4bits(uint8_t value)
{
    _expanderWrite(value);
    _pulseEnable();
}

void LCD1602Driver::_expanderWrite(uint8_t value)
{
    Wire.beginTransmission(_address);
    Wire.write(value);
    Wire.endTransmission();
}

void LCD1602Driver::_waitMicroseconds(uint16_t us)
{
    delayMicroseconds(us);
}

#endif // FASTBEE_ENABLE_LCD1602
