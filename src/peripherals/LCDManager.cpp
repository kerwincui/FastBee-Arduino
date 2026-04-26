/**
 * @description: LCD/OLED 显示屏管理器实现
 * @author: kerwincui
 * @copyright: FastBee All rights reserved.
 * @date: 2025-04-15
 */

#include "peripherals/LCDManager.h"
#include "systems/LoggerSystem.h"
#include "core/ChipConfig.h"
#include "core/FeatureFlags.h"
#include <WiFi.h>

#if FASTBEE_ENABLE_LCD

// 单例实现
LCDManager& LCDManager::getInstance()
{
    static LCDManager instance;
    return instance;
}

// ========== 初始化与配置 ==========

bool LCDManager::initialize(const PeripheralConfig& config)
{
    LOG_INFO("LCD Manager: Initializing...");
    
    // 如果已初始化，先反初始化
    if (_initialized)
    {
        deinitialize();
    }
    
    // 提取配置参数
    _width = config.params.lcd.width;
    _height = config.params.lcd.height;
    _interface = static_cast<DisplayInterface>(config.params.lcd.interface);
    
    // 根据引脚数量判断控制器类型
    if (config.pinCount >= 2 && config.pins[0] != 255 && config.pins[1] != 255)
    {
        // I2C 模式：pins[0] = SDA, pins[1] = SCL
        if (_interface == DisplayInterface::I2C_MODE)
        {
            // 从引脚配置推断 I2C 地址
            // 通常 SSD1306 地址为 0x3C 或 0x3D
            _i2cAddress = 0x3C;
            _resetPin = -1;
            
            // 初始化 I2C
            Wire.begin(config.pins[0], config.pins[1]);
            
            LOG_INFOF("LCD Manager: I2C mode, SDA=%d, SCL=%d, Addr=0x%02X", 
                      config.pins[0], config.pins[1], _i2cAddress);
        }
        // SPI 模式：pins[0] = MOSI, pins[1] = SCK, pins[2] = CS, pins[3] = DC
        else if (_interface == DisplayInterface::SPI_MODE && config.pinCount >= 4)
        {
            LOG_INFOF("LCD Manager: SPI mode, MOSI=%d, SCK=%d, CS=%d, DC=%d",
                      config.pins[0], config.pins[1], config.pins[2], config.pins[3]);
        }
    }
    
    // 创建显示屏对象
    if (!createDisplay(config))
    {
        LOG_ERROR("LCD Manager: Failed to create display");
        return false;
    }
    
    // 初始化显示屏
    _display->begin();
    
    // 设置默认字体
    setDefaultFont();
    
    // 清屏
    clear();
    
    _initialized = true;
    _lastUpdate = millis();
    
    LOG_INFOF("LCD Manager: Initialized successfully (%dx%d)", _width, _height);
    return true;
}

void LCDManager::deinitialize()
{
    if (_display)
    {
        delete _display;
        _display = nullptr;
    }
    
    _initialized = false;
    _contentChanged = false;
    
    LOG_INFO("LCD Manager: Deinitialized");
}

bool LCDManager::createDisplay(const PeripheralConfig& config)
{
    // 根据控制器类型和接口创建对应的 U8G2 对象
    // 目前主要支持 I2C 接口的 SSD1306
    
    if (_interface == DisplayInterface::I2C_MODE)
    {
        // 默认使用 SSD1306 128x64
        if (_width == 128 && _height == 64)
        {
            // 硬件 I2C，全缓冲（性能最优，内存占用约 1KB）
            _display = new U8G2_SSD1306_128X64_NONAME_F_HW_I2C(
                U8G2_R0, _resetPin, config.pins[1], config.pins[0]
            );
            _controller = DisplayController::SSD1306;
        }
        else if (_width == 128 && _height == 32)
        {
            _display = new U8G2_SSD1306_128X32_UNIVISION_F_HW_I2C(
                U8G2_R0, _resetPin, config.pins[1], config.pins[0]
            );
            _controller = DisplayController::SSD1306;
        }
        else if (_width == 64 && _height == 128)
        {
            // SH1106 1.3寸 OLED
            _display = new U8G2_SH1106_128X64_NONAME_F_HW_I2C(
                U8G2_R0, _resetPin, config.pins[1], config.pins[0]
            );
            _controller = DisplayController::SH1106;
        }
        else
        {
            // 默认 128x64
            _display = new U8G2_SSD1306_128X64_NONAME_F_HW_I2C(
                U8G2_R0, _resetPin, config.pins[1], config.pins[0]
            );
            _width = 128;
            _height = 64;
            _controller = DisplayController::SSD1306;
        }
    }
    else if (_interface == DisplayInterface::SPI_MODE)
    {
        // SPI 模式（预留）
        LOG_WARNING("LCD Manager: SPI mode not fully implemented yet");
        return false;
    }
    else
    {
        LOG_ERROR("LCD Manager: Unsupported interface");
        return false;
    }
    
    return _display != nullptr;
}

void LCDManager::setDefaultFont()
{
    if (!_display) return;
    
    // 默认使用中等字体，适合显示中文和英文
    _display->setFont(u8g2_font_ncenB08_tr);
    _currentFont = 1;
}

// ========== 显示操作 ==========

void LCDManager::clear()
{
    if (!_display || !_initialized) return;
    
    _display->clearBuffer();
    _contentChanged = true;
}

bool LCDManager::print(const String& text, uint8_t x, uint8_t y, TextAlign align)
{
    if (!_display || !_initialized)
    {
        LOG_WARNING("LCD Manager: Display not initialized");
        return false;
    }
    
    if (text.isEmpty())
    {
        return true;
    }
    
    // 计算对齐位置
    uint8_t textX = x;
    if (align == TextAlign::CENTER)
    {
        uint16_t textWidth = _display->getStrWidth(text.c_str());
        textX = (_width - textWidth) / 2;
    }
    else if (align == TextAlign::RIGHT)
    {
        uint16_t textWidth = _display->getStrWidth(text.c_str());
        textX = _width - textWidth - x;
    }
    
    // 绘制文本
    _display->drawStr(textX, y, text.c_str());
    _contentChanged = true;
    
    return true;
}

bool LCDManager::printLine(const String& text, uint8_t line)
{
    if (!_display || !_initialized) return false;
    
    uint8_t y = (line + 1) * getFontHeight();
    if (y > _height)
    {
        LOG_WARNINGF("LCD Manager: Line %d out of bounds (max %d)", line, getMaxLines() - 1);
        return false;
    }
    
    return print(text, 0, y, TextAlign::LEFT);
}

bool LCDManager::printLines(const String lines[], uint8_t lineCount)
{
    if (!_display || !_initialized) return false;
    
    clear();
    
    for (uint8_t i = 0; i < lineCount && i < getMaxLines(); i++)
    {
        printLine(lines[i], i);
    }
    
    return refresh();
}

bool LCDManager::showSensorData(const String& name, float value, const String& unit, uint8_t line)
{
    if (!_display || !_initialized) return false;
    
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%s: %.1f%s", name.c_str(), value, unit.c_str());
    
    return printLine(String(buffer), line);
}

bool LCDManager::showSystemInfo()
{
    if (!_display || !_initialized) return false;
    
    clear();
    
    String lines[8];
    uint8_t lineCount = 0;
    
    // 第1行：项目名称
    lines[lineCount++] = "FastBee IoT";
    
    // 第2行：IP 地址
    if (WiFi.status() == WL_CONNECTED)
    {
        lines[lineCount++] = "IP: " + WiFi.localIP().toString();
    }
    else
    {
        lines[lineCount++] = "WiFi: Disconnected";
    }
    
    // 第3行：内存使用
    uint32_t freeHeap = ESP.getFreeHeap();
    uint32_t totalHeap = 300000;  // 约 300KB
    uint8_t memUsage = (totalHeap - freeHeap) * 100 / totalHeap;
    lines[lineCount++] = "Mem: " + String(memUsage) + "% used";
    
    // 第4行：运行时间
    unsigned long uptime = millis() / 1000;
    int hours = uptime / 3600;
    int mins = (uptime % 3600) / 60;
    char uptimeStr[16];
    snprintf(uptimeStr, sizeof(uptimeStr), "Up: %dh %dm", hours, mins);
    lines[lineCount++] = String(uptimeStr);
    
    return printLines(lines, lineCount);
}

// ========== 图形绘制 ==========

bool LCDManager::drawLine(uint8_t x1, uint8_t y1, uint8_t x2, uint8_t y2)
{
    if (!_display || !_initialized) return false;
    
    _display->drawLine(x1, y1, x2, y2);
    _contentChanged = true;
    return true;
}

bool LCDManager::drawRect(uint8_t x, uint8_t y, uint8_t w, uint8_t h)
{
    if (!_display || !_initialized) return false;
    
    _display->drawFrame(x, y, w, h);
    _contentChanged = true;
    return true;
}

bool LCDManager::drawBox(uint8_t x, uint8_t y, uint8_t w, uint8_t h)
{
    if (!_display || !_initialized) return false;
    
    _display->drawBox(x, y, w, h);
    _contentChanged = true;
    return true;
}

bool LCDManager::drawCircle(uint8_t x, uint8_t y, uint8_t r)
{
    if (!_display || !_initialized) return false;
    
    _display->drawCircle(x, y, r);
    _contentChanged = true;
    return true;
}

bool LCDManager::drawDisc(uint8_t x, uint8_t y, uint8_t r)
{
    if (!_display || !_initialized) return false;
    
    _display->drawDisc(x, y, r);
    _contentChanged = true;
    return true;
}

// ========== 高级功能 ==========

bool LCDManager::setFont(uint8_t fontIndex)
{
    if (!_display || !_initialized) return false;
    
    switch (fontIndex)
    {
        case 0:  // 小字体
            _display->setFont(u8g2_font_5x7_tf);
            _currentFont = 0;
            break;
        case 1:  // 中字体（默认）
            _display->setFont(u8g2_font_ncenB08_tr);
            _currentFont = 1;
            break;
        case 2:  // 大字体
            _display->setFont(u8g2_font_ncenB10_tr);
            _currentFont = 2;
            break;
        default:
            return false;
    }
    
    return true;
}

bool LCDManager::setContrast(uint8_t contrast)
{
    if (!_display || !_initialized) return false;
    
    _display->setContrast(contrast);
    return true;
}

bool LCDManager::setFlip(bool flip)
{
    if (!_display || !_initialized) return false;
    
    if (flip)
    {
        _display->setDisplayRotation(U8G2_R2);
    }
    else
    {
        _display->setDisplayRotation(U8G2_R0);
    }
    
    return true;
}

bool LCDManager::setDisplayOn(bool on)
{
    if (!_display || !_initialized) return false;
    
    _display->setPowerSave(!on);
    return true;
}

bool LCDManager::refresh()
{
    if (!_display || !_initialized) return false;
    
    // 检查刷新间隔，避免频繁刷新
    if (!shouldUpdate())
    {
        return true;
    }
    
    _display->sendBuffer();
    _lastUpdate = millis();
    _contentChanged = false;
    
    return true;
}

// ========== 状态查询 ==========

uint8_t LCDManager::getFontHeight() const
{
    if (!_display) return 10;
    
    switch (_currentFont)
    {
        case 0: return 7;
        case 1: return 10;
        case 2: return 13;
        default: return 10;
    }
}

uint8_t LCDManager::getMaxLines() const
{
    return _height / getFontHeight();
}

bool LCDManager::shouldUpdate()
{
    unsigned long now = millis();
    return (now - _lastUpdate >= _minUpdateInterval) || _contentChanged;
}

#endif // FASTBEE_ENABLE_LCD
