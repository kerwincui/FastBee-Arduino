/**
 * @description: LCD/OLED 显示屏管理器实现
 * @author: kerwincui
 * @copyright: FastBee All rights reserved.
 * @date: 2025-04-15
 */

#include "core/FeatureFlags.h"
#if FASTBEE_ENABLE_LCD

#include "peripherals/LCDManager.h"
#include "core/SensorConfigEpoch.h"
#include "systems/LoggerSystem.h"
#include "core/ChipConfig.h"
#include "core/AsyncExecTypes.h"
#include "core/FeatureBudget.h"
#include "core/I2CBusManager.h"
#include "core/RuntimeFeatureManager.h"
#include <WiFi.h>

namespace {
#if FASTBEE_ENABLE_LCD_GB2312
const uint8_t* lcdChineseFont() {
    // ScreenRuntime already links the full WQY font. Reuse it instead of
    // linking a second 109KB subset; OLED-only builds keep the smaller font.
#if FASTBEE_ENABLE_SCREEN_UI && defined(ARDUINO_ARCH_ESP32)
    return u8g2_font_wqy12_t_gb2312;
#else
    return u8g2_font_wqy12_t_gb2312a;
#endif
}
#endif

const void* g_lcdI2cOwner = nullptr;
bool g_lcdI2cTransferLocked = false;
bool g_lcdI2cTransferFailed = false;
bool g_lcdI2cOperationFailed = false;
bool g_lcdI2cAttempted = false;
bool g_lcdForegroundActive = false;
unsigned long g_lcdForegroundStarted = 0;
struct ManagedLcdFrameBudget {
    ManagedLcdFrameBudget() {
        g_lcdForegroundActive = true;
        g_lcdForegroundStarted = millis();
    }
    ~ManagedLcdFrameBudget() { g_lcdForegroundActive = false; }
};

void abortManagedLcdTransfer() {
    g_lcdI2cTransferFailed = false;
    if (!g_lcdI2cTransferLocked) return;
    FastBee::I2CBusManager::instance().unlock();
    g_lcdI2cTransferLocked = false;
}

uint8_t managedU8g2I2cByte(u8x8_t* u8x8, uint8_t msg,
                           uint8_t argInt, void* argPtr) {
    auto& manager = FastBee::I2CBusManager::instance();
    const auto fail = []() -> uint8_t { g_lcdI2cOperationFailed = true; return 0; };
    if (!g_lcdI2cOwner) return fail();

    switch (msg) {
        case U8X8_MSG_BYTE_INIT:
            return manager.ownsAddress(g_lcdI2cOwner, u8x8_GetI2CAddress(u8x8) >> 1) ? 1 : fail();
        case U8X8_MSG_BYTE_SET_DC:
            return 1;
        case U8X8_MSG_BYTE_START_TRANSFER: {
            // Do not replay later U8g2 chunks after the first failed transaction.
            // Bound a foreground frame while PM owns its config and grant.
            if (g_lcdI2cOperationFailed || (g_lcdForegroundActive &&
                millis() - g_lcdForegroundStarted >= 250UL)) return fail();
            // Recover defensively if a prior U8g2 transfer aborted before END.
            if (g_lcdI2cTransferLocked) g_lcdI2cOperationFailed = true;
            abortManagedLcdTransfer();
            if (g_lcdI2cOperationFailed) return fail();
            if (!manager.lock(250)) return fail();
            g_lcdI2cTransferLocked = true;
            TwoWire* wire = manager.wire(g_lcdI2cOwner);
            if (!wire || !manager.ownsAddress(g_lcdI2cOwner, u8x8_GetI2CAddress(u8x8) >> 1)) {
                abortManagedLcdTransfer();
                return fail();
            }
            g_lcdI2cAttempted = true;
            wire->beginTransmission(u8x8_GetI2CAddress(u8x8) >> 1);
            return 1;
        }
        case U8X8_MSG_BYTE_SEND: {
            if (!g_lcdI2cTransferLocked) return fail();
            TwoWire* wire = manager.wire(g_lcdI2cOwner);
            if (g_lcdI2cTransferFailed || !wire || (argInt && !argPtr) ||
                !manager.ownsAddress(g_lcdI2cOwner, u8x8_GetI2CAddress(u8x8) >> 1) ||
                (argInt && wire->write(static_cast<uint8_t*>(argPtr), argInt) != argInt)) {
                g_lcdI2cTransferFailed = true;
                return fail();
            }
            return 1;
        }
        case U8X8_MSG_BYTE_END_TRANSFER: {
            if (!g_lcdI2cTransferLocked) return fail();
            TwoWire* wire = manager.wire(g_lcdI2cOwner);
            // Finish our attempted transaction even after a short buffer write,
            // but never turn that earlier error into a successful END callback.
            const bool owned = wire && manager.ownsAddress(
                g_lcdI2cOwner, u8x8_GetI2CAddress(u8x8) >> 1);
            const bool acknowledged = owned && wire->endTransmission() == 0;
            const bool success = acknowledged && !g_lcdI2cTransferFailed;
            abortManagedLcdTransfer();
            return success ? 1 : fail();
        }
        default:
            return 0;
    }
}
// U8g2's high-level void methods may ignore a callback failure and send later
// transactions. Keep the error across the complete manager operation.
void beginManagedLcdOperation() {
    abortManagedLcdTransfer();
    g_lcdI2cOperationFailed = false;
}

bool finishManagedLcdOperation() {
    if (g_lcdI2cTransferLocked) g_lcdI2cOperationFailed = true;
    abortManagedLcdTransfer();
    return !g_lcdI2cOperationFailed;
}
}  // namespace

// 单例实现
LCDManager& LCDManager::getInstance()
{
    static LCDManager instance;
    static const FastBee::FeatureInstanceObserver observer{"lcd", [](const void* context) {
        return static_cast<const LCDManager*>(context)->observeFeatureInstance();
    }, &instance};
    FastBee::FeatureInstanceRegistry::instance().registerObserver(&observer);
    return instance;
}

FastBee::FeatureInstanceSnapshot LCDManager::observeFeatureInstance() const {
    using namespace FastBee;
    if (!_displayMutex) return FeatureInstanceSnapshot::unavailable(
        FeatureInstanceReason::SynchronizationUnavailable, FeatureInstanceScope::ServiceLifecycle);
    RecursiveMutexGuard lock(_displayMutex, 0);
    if (!lock.isLocked()) return FeatureInstanceSnapshot::lifecycle(FeatureInstanceState::Busy);
    if (_initialized && !_display) return FeatureInstanceSnapshot::unavailable(
        FeatureInstanceReason::InvalidSnapshot, FeatureInstanceScope::ServiceLifecycle);
    // Stopped means no active renderer, not that retained caches/deferred I2C
    // leases or all heap storage have been released. Never drain the bus here.
    return FeatureInstanceSnapshot::lifecycle(_initialized ? FeatureInstanceState::Running :
        (_display || _busAcquired) ? FeatureInstanceState::Allocated : FeatureInstanceState::Stopped);
}

LCDManager::LCDManager()
    : _displayMutex(xSemaphoreCreateRecursiveMutex())
{
}

LCDManager::~LCDManager()
{
    abortManagedLcdTransfer();
    if (_display) {
        delete _display;
        _display = nullptr;
    }
    if (_busAcquired) {
        auto& bus = FastBee::I2CBusManager::instance();
        if (!bus.release(this, 100)) bus.deferRelease(this);
        _busAcquired = false;
    }
    if (g_lcdI2cOwner == this) g_lcdI2cOwner = nullptr;
    if (_displayMutex) {
        vSemaphoreDelete(_displayMutex);
        _displayMutex = nullptr;
    }
}

// LCD operations can originate from the scheduler, script worker, and the
// maintenance task. U8G2 and Arduino Wire share mutable transaction state, so
// every public operation must serialize at this boundary. Recursive locking
// keeps compound operations such as showCustomText()->print()->refresh() safe.
#define LCD_GUARD_OR_RETURN(value) \
    RecursiveMutexGuard lcdGuard(_displayMutex, pdMS_TO_TICKS(250)); \
    if (!lcdGuard.isLocked()) return value
#define LCD_GUARD_OR_RETURN_VOID() \
    RecursiveMutexGuard lcdGuard(_displayMutex, pdMS_TO_TICKS(250)); \
    if (!lcdGuard.isLocked()) return

// ========== 初始化与配置 ==========

bool LCDManager::initialize(const PeripheralConfig& config)
{
    LCD_GUARD_OR_RETURN(false);
    if (!FastBee::RuntimeFeatureManager::instance().shouldEnable("lcd")) {
        LOG_INFO("LCD Manager: Compiled but runtime-disabled, skip init");
        return false;
    }

    if (FastBee::FeatureBudgetRegistry::instance().isDenied("lcd")) {
        LOG_WARNING("LCD Manager: Disabled by memory budget, skip init");
        return false;
    }

    if (config.type != PeripheralType::LCD || config.id.isEmpty() || config.id.length() > 40 ||
        config.pinCount != 2 || config.pins[0] == config.pins[1] ||
        config.params.lcd.interface != 2) return false;
    // A second named profile must never replace the singleton's first owner.
    if (!_activeId.isEmpty() && _activeId != config.id) return false;
    LOG_INFO("LCD Manager: Initializing...");
    
    // 如果已初始化，先反初始化
    if (_initialized || _display || _busAcquired)
    {
        if (!deinitialize(config.id)) return false;
    }
    _activeId = config.id;
    if (_activeId != config.id) return false;
    _configuredWidth = config.params.lcd.width;
    _configuredHeight = config.params.lcd.height;
    _configuredContrast = config.params.lcd.contrast;

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
            
            // 验证引脚有效性后再初始化
            int sda = config.pins[0];
            int scl = config.pins[1];
            
            // 验证引脚有效性后再初始化（芯片差异统一由 ChipConfig.h 提供，
            // 禁止本地按 CONFIG_IDF_TARGET 派生；旧 C6 分支误认 GPIO24-30 有效已修复）
            bool pinsValid = (sda >= 0 && sda <= CHIP_MAX_GPIO) &&
                             (scl >= 0 && scl <= CHIP_MAX_GPIO);
            
            if (pinsValid) {
                _sdaPin = sda;
                _sclPin = scl;
                String busError;
                FastBee::I2CBusLock admission(100);
                if (!admission.locked() || !FastBee::I2CBusManager::instance().acquire(
                        this, sda, scl, 100000, busError, FastBee::ResourceConsumerRole::Display)) {
                    LOG_WARNINGF("LCD Manager: I2C lease failed: %s", busError.c_str());
                    _sdaPin = -1;
                    _sclPin = -1;
                    return false;
                }
                _busAcquired = true;
                if (!FastBee::I2CBusManager::instance().claimAddress(this, _i2cAddress, busError)) {
                    LOG_WARNINGF("LCD Manager: I2C address failed: %s", busError.c_str());
                    deinitialize();
                    return false;
                }
                g_lcdI2cOwner = this;
                LOG_INFOF("LCD Manager: I2C mode, SDA=%d, SCL=%d, Addr=0x%02X", 
                          sda, scl, _i2cAddress);
                // U8G2 sends a complete controller setup sequence from begin().
                // Fail fast when the configured panel is absent so a single bad
                // profile cannot stall boot or flood the log with one error per byte.
                if (!probeI2cDisplay(true)) {
                    LOG_WARNING("LCD Manager: Display probe failed, skipping U8G2 initialization");
                    deinitialize();
                    return false;
                }
            } else {
                LOG_WARNINGF("LCD Manager: Invalid I2C pins (SDA=%d, SCL=%d), skipping I2C init", sda, scl);
                return false;
            }
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
        deinitialize();
        return false;
    }

    if (_interface == DisplayInterface::I2C_MODE) {
        _display->getU8x8()->byte_cb = managedU8g2I2cByte;
        // U8g2 initializes every configured GPIO before its byte callback.
        // Arduino 3 pinMode() on SDA/SCL detaches the active Wire controller;
        // the managed byte callback deliberately never starts another bus.
        // Only I2CBusManager owns these pins, including during initDisplay()
        // maintenance. Keep U8g2's reset pin and delay callback intact.
        u8x8_SetPin(_display->getU8x8(), U8X8_PIN_I2C_CLOCK, U8X8_PIN_NONE);
        u8x8_SetPin(_display->getU8x8(), U8X8_PIN_I2C_DATA, U8X8_PIN_NONE);
        _display->setBusClock(100000);
    }
    
    // 初始化显示屏
    beginManagedLcdOperation();
    _display->begin();

    // 应用配置对比度（0 视为未配置，按最大亮度处理）
    uint8_t contrast = config.params.lcd.contrast;
    if (contrast == 0) contrast = 255;
    _contrast = contrast;
    _display->setContrast(contrast);

    // 确保退出省电模式（部分模块上电后处于 display off 状态）
    _display->setPowerSave(0);

    if (!finishManagedLcdOperation()) {
        LOG_WARNING("LCD Manager: Controller initialization transfer failed");
        deinitialize();
        return false;
    }

    // 设置默认字体
    setDefaultFont();
    
    // 清屏
    clear();
    
    _initialized = true;
    _foregroundFailed = false;
    _lastUpdate = millis();
    _initTime = _lastUpdate;
    _lastReassert = _lastUpdate;
    
    LOG_INFOF("LCD Manager: Initialized successfully (%dx%d, contrast=%d)", _width, _height, contrast);
    return true;
}

bool LCDManager::deinitialize(const String& expectedId)
{
    LCD_GUARD_OR_RETURN(false);
    if (!expectedId.isEmpty() && !_activeId.isEmpty() && expectedId != _activeId) return false;
    abortManagedLcdTransfer();
    if (_display)
    {
        delete _display;
        _display = nullptr;
    }
    
    _initialized = false;
    _contentChanged = false;
    if (_busAcquired) {
        auto& bus = FastBee::I2CBusManager::instance();
        if (!bus.release(this, 100)) bus.deferRelease(this);
        _busAcquired = false;
    }
    if (g_lcdI2cOwner == this) g_lcdI2cOwner = nullptr;
    _sdaPin = -1;
    _sclPin = -1;
    
    _activeId = "";
    _foregroundFailed = false;
    LOG_INFO("LCD Manager: Deinitialized");
    return true;
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
        else if (_width == 72 && _height == 40)
        {
            // ESP32-C3 OLED 开发板常见 0.42寸 SSD1306 72x40
            _display = new U8G2_SSD1306_72X40_ER_F_HW_I2C(
                U8G2_R0, _resetPin, config.pins[1], config.pins[0]
            );
            _controller = DisplayController::SSD1306;
        }
        else if (_width == 64 && _height == 128)
        {
            // SH1106 控制器 OLED（配置 width=64/height=128 作为选择开关）
            // 实际面板为 128x64，归一化尺寸保证文本布局正确
            _display = new U8G2_SH1106_128X64_NONAME_F_HW_I2C(
                U8G2_R0, _resetPin, config.pins[1], config.pins[0]
            );
            _width = 128;
            _height = 64;
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
    
#if FASTBEE_ENABLE_LCD_GB2312
    // 运行时功能门控：lcdGb2312 被用户禁用时回退 ASCII 字体
    if (FastBee::RuntimeFeatureManager::instance().shouldEnable("lcdGb2312")) {
        // 默认使用 WQY 12 像素中文字体，同时支持 ASCII，配合 drawUTF8 可显示中英文混排
        // 符号、单位（如 ℃）均包含在 GB2312 子集中
        _display->setFont(lcdChineseFont());
        _currentFont = 1;
    } else {
        _display->setFont(u8g2_font_5x7_tf);
        _currentFont = 0;
    }
#else
    // 未启用 GB2312 中文字库（单个占 ~107KB Flash）时默认 ASCII 小字体
    _display->setFont(u8g2_font_5x7_tf);
    _currentFont = 0;
#endif
}

// ========== 显示操作 ==========

void LCDManager::clear()
{
    LCD_GUARD_OR_RETURN_VOID();
    if (!_display || !_initialized) return;
    
    _display->clearBuffer();
    _contentChanged = true;
}

bool LCDManager::print(const String& text, uint8_t x, uint8_t y, TextAlign align)
{
    LCD_GUARD_OR_RETURN(false);
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
        uint16_t textWidth = _display->getUTF8Width(text.c_str());
        textX = (_width - textWidth) / 2;
    }
    else if (align == TextAlign::RIGHT)
    {
        uint16_t textWidth = _display->getUTF8Width(text.c_str());
        textX = _width - textWidth - x;
    }
    
    // 绘制文本（使用 UTF-8 渲染，支持 °/Latin-1 等扩展字符）
    _display->drawUTF8(textX, y, text.c_str());
    _contentChanged = true;
    
    return true;
}

bool LCDManager::printLine(const String& text, uint8_t line)
{
    LCD_GUARD_OR_RETURN(false);
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
    LCD_GUARD_OR_RETURN(false);
    if (!_display || !_initialized) return false;
    
    clear();
    
    for (uint8_t i = 0; i < lineCount && i < getMaxLines(); i++)
    {
        printLine(lines[i], i);
    }
    
    return refresh();
}

bool LCDManager::showCustomText(const String& content)
{
    LCD_GUARD_OR_RETURN(false);
    if (!_display || !_initialized) return false;

    // 最多显示行数：受屏高和字体限制，同时加上硬上限避免栈数组过大
    static constexpr uint8_t MAX_CUSTOM_LINES = 6;

    // 预扫描：统计有效行数（按 \n 切分），最多取 MAX_CUSTOM_LINES
    // 为了避免堆分配，直接在原串上用下标位记再 substring 推进
    clear();

    uint8_t fontH = getFontHeight();
    if (fontH == 0) fontH = 12;
    uint8_t maxPixelLines = getMaxLines();
    if (maxPixelLines == 0) maxPixelLines = 4;
    uint8_t maxLines = (maxPixelLines < MAX_CUSTOM_LINES) ? maxPixelLines : MAX_CUSTOM_LINES;

    // 切分行：记录每行的起始下标和长度，不产生 String 数组堆分配
    struct LineSlice { int start; int len; };
    LineSlice slices[MAX_CUSTOM_LINES];
    uint8_t lineCount = 0;
    int n = content.length();
    int cursor = 0;
    while (cursor <= n && lineCount < maxLines) {
        int nl = content.indexOf('\n', cursor);
        int end = (nl < 0) ? n : nl;
        slices[lineCount].start = cursor;
        slices[lineCount].len = end - cursor;
        lineCount++;
        if (nl < 0) break;
        cursor = nl + 1;
    }

    if (lineCount == 0) {
        return refresh();  // 空内容 = 清屏效果
    }

    // 判断首行是否为标题（以 # 开头）
    bool hasTitle = false;
    String titleText;
    uint8_t bodyStart = 0;
    if (slices[0].len > 0 && content.charAt(slices[0].start) == '#') {
        hasTitle = true;
        // 去掉 # 和前置空格
        int ts = slices[0].start + 1;
        while (ts < slices[0].start + slices[0].len && content.charAt(ts) == ' ') ts++;
        titleText = content.substring(ts, slices[0].start + slices[0].len);
        bodyStart = 1;
    }

    uint8_t y = fontH;
    if (hasTitle) {
        print(titleText, 0, y, TextAlign::CENTER);
        // 标题下方绘分隔线
        drawLine(0, y + 2, _width, y + 2);
        y += fontH + 2;
    }

    // 正文行：左对齐，超出屏高丢弃
    for (uint8_t i = bodyStart; i < lineCount; i++) {
        if (y > _height) break;
        String lineText = content.substring(slices[i].start, slices[i].start + slices[i].len);
        print(lineText, 0, y, TextAlign::LEFT);
        y += fontH;
    }

    return refresh();
}

bool LCDManager::showSensorData(const String& name, float value, const String& unit, uint8_t line)
{
    LCD_GUARD_OR_RETURN(false);
    if (!_display || !_initialized) return false;
    
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%s: %.1f%s", name.c_str(), value, unit.c_str());
    
    return printLine(String(buffer), line);
}

bool LCDManager::showSystemInfo()
{
    LCD_GUARD_OR_RETURN(false);
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
    char uptimeStr[32];
    snprintf(uptimeStr, sizeof(uptimeStr), "Up: %dh %dm", hours, mins);
    lines[lineCount++] = String(uptimeStr);
    
    return printLines(lines, lineCount);
}

// ========== 图形绘制 ==========

bool LCDManager::drawLine(uint8_t x1, uint8_t y1, uint8_t x2, uint8_t y2)
{
    LCD_GUARD_OR_RETURN(false);
    if (!_display || !_initialized) return false;
    
    _display->drawLine(x1, y1, x2, y2);
    _contentChanged = true;
    return true;
}

bool LCDManager::drawRect(uint8_t x, uint8_t y, uint8_t w, uint8_t h)
{
    LCD_GUARD_OR_RETURN(false);
    if (!_display || !_initialized) return false;
    
    _display->drawFrame(x, y, w, h);
    _contentChanged = true;
    return true;
}

bool LCDManager::drawBox(uint8_t x, uint8_t y, uint8_t w, uint8_t h)
{
    LCD_GUARD_OR_RETURN(false);
    if (!_display || !_initialized) return false;
    
    _display->drawBox(x, y, w, h);
    _contentChanged = true;
    return true;
}

bool LCDManager::drawCircle(uint8_t x, uint8_t y, uint8_t r)
{
    LCD_GUARD_OR_RETURN(false);
    if (!_display || !_initialized) return false;
    
    _display->drawCircle(x, y, r);
    _contentChanged = true;
    return true;
}

bool LCDManager::drawDisc(uint8_t x, uint8_t y, uint8_t r)
{
    LCD_GUARD_OR_RETURN(false);
    if (!_display || !_initialized) return false;
    
    _display->drawDisc(x, y, r);
    _contentChanged = true;
    return true;
}

// ========== 通用传感器数据显示模块 ==========

void LCDManager::updateSensorEntry(const String& id, const String& label, float value,
                                   const String& unit, uint8_t decimals, uint32_t configVersion)
{
    LCD_GUARD_OR_RETURN_VOID();
    if (!_initialized) return;
    if (configVersion && !FastBee::SensorConfigEpoch::matches(configVersion)) return;
    
    // 查找已有条目
    for (auto& entry : _sensorEntries)
    {
        if (entry.id == id)
        {
            entry.label = label;
            entry.value = value;
            entry.unit = unit;
            entry.decimals = decimals;
            entry.timestamp = millis();
            entry.valid = true;
            entry.configVersion = configVersion;
            return;
        }
    }
    
    // 新增条目（限制最大数量）
    if (_sensorEntries.size() >= MAX_SENSOR_ENTRIES)
    {
        LOG_WARNING("LCD Manager: Sensor entries full, ignoring new entry");
        return;
    }
    
    SensorDisplayEntry entry;
    entry.id = id;
    entry.label = label;
    entry.value = value;
    entry.unit = unit;
    entry.decimals = decimals;
    entry.timestamp = millis();
    entry.valid = true;
    entry.configVersion = configVersion;
    _sensorEntries.push_back(entry);
    
    LOG_INFOF("LCD Manager: Registered sensor entry '%s' (%s)", id.c_str(), label.c_str());
}

void LCDManager::invalidateSensorEntry(const String& id)
{
    LCD_GUARD_OR_RETURN_VOID();
    for (auto& entry : _sensorEntries)
    {
        if (entry.id == id)
        {
            entry.valid = false;
            return;
        }
    }
}

uint8_t LCDManager::getSensorPageCount() const
{
    LCD_GUARD_OR_RETURN(0);
    if (_sensorEntries.empty()) return 0;
    
    // 每页显示行数 = maxLines - 1（顶部标题行）
    uint8_t linesPerPage = getMaxLines() - 1;
    if (linesPerPage == 0) linesPerPage = 1;
    
    return (_sensorEntries.size() + linesPerPage - 1) / linesPerPage;
}

bool LCDManager::showSensorPage(int8_t page)
{
    LCD_GUARD_OR_RETURN(false);
    if (!_display || !_initialized) return false;
    if (_sensorEntries.empty()) return false;
    
    uint8_t linesPerPage = getMaxLines() - 1;  // 预留标题行
    if (linesPerPage == 0) linesPerPage = 1;
    
    uint8_t totalPages = getSensorPageCount();
    
    // 自动轮播模式
    if (page < 0)
    {
        page = _currentSensorPage;
    }
    
    if ((uint8_t)page >= totalPages)
    {
        page = 0;
    }
    
    clear();
    
    // 绘制标题行（含页码）
    uint8_t fontH = getFontHeight();
    char titleBuf[32];
    if (totalPages > 1)
    {
        snprintf(titleBuf, sizeof(titleBuf), "传感器数据 [%d/%d]", page + 1, totalPages);
    }
    else
    {
        snprintf(titleBuf, sizeof(titleBuf), "传感器数据");
    }
    print(String(titleBuf), 0, fontH, TextAlign::CENTER);
    
    // 绘制分隔线
    drawLine(0, fontH + 2, _width, fontH + 2);
    
    // 绘制传感器数据行
    uint8_t startIdx = (uint8_t)page * linesPerPage;
    uint8_t endIdx = startIdx + linesPerPage;
    if (endIdx > _sensorEntries.size()) endIdx = _sensorEntries.size();
    
    for (uint8_t i = startIdx; i < endIdx; i++)
    {
        uint8_t lineNum = (i - startIdx) + 1;  // 从第1行开始（第0行是标题）
        uint8_t y = (lineNum + 1) * fontH;
        
        const SensorDisplayEntry& entry = _sensorEntries[i];
        char lineBuf[40];
        
        if (entry.valid && (!entry.configVersion || FastBee::SensorConfigEpoch::matches(entry.configVersion)))
        {
            // 格式：“标签：数值单位”（全角冒号，适配中文 label）
            char valueBuf[16];
            dtostrf(entry.value, 0, entry.decimals, valueBuf);
            snprintf(lineBuf, sizeof(lineBuf), "%s：%s%s",
                     entry.label.c_str(), valueBuf, entry.unit.c_str());
        }
        else
        {
            // 数据无效
            snprintf(lineBuf, sizeof(lineBuf), "%s：--", entry.label.c_str());
        }
        
        print(String(lineBuf), 2, y, TextAlign::LEFT);
    }
    
    return refresh();
}

void LCDManager::autoRefreshSensorDisplay(unsigned long intervalMs)
{
    LCD_GUARD_OR_RETURN_VOID();
    if (!_initialized || _sensorEntries.empty()) return;
    
    unsigned long now = millis();
    
    // 检查是否到翻页时间
    if (now - _lastPageSwitch >= intervalMs)
    {
        _lastPageSwitch = now;
        _currentSensorPage++;
        
        uint8_t totalPages = getSensorPageCount();
        if (_currentSensorPage >= totalPages)
        {
            _currentSensorPage = 0;
        }
    }
    
    // 显示当前页
    showSensorPage(_currentSensorPage);
}

// ========== 高级功能 ==========

bool LCDManager::setFont(uint8_t fontIndex)
{
    LCD_GUARD_OR_RETURN(false);
    if (!_display || !_initialized) return false;
    
    switch (fontIndex)
    {
        case 0:  // 小字体（仅 ASCII，省带宽，不支持中文）
            _display->setFont(u8g2_font_5x7_tf);
            _currentFont = 0;
            break;
        case 1:  // 中字体（默认，WQY 12像素 GB2312 中文 + ASCII）
#if FASTBEE_ENABLE_LCD_GB2312
            if (FastBee::RuntimeFeatureManager::instance().shouldEnable("lcdGb2312")) {
                _display->setFont(lcdChineseFont());
                _currentFont = 1;
            } else {
                _display->setFont(u8g2_font_5x7_tf);  // 运行时禁用中文字库时回退 ASCII
                _currentFont = 0;
            }
#else
            _display->setFont(u8g2_font_5x7_tf);  // 未启用中文字库时回退 ASCII
            _currentFont = 0;
#endif
            break;
        case 2:  // 大字体（WQY 15像素中英文）
#if FASTBEE_ENABLE_LCD_GB2312
            if (FastBee::RuntimeFeatureManager::instance().shouldEnable("lcdGb2312")) {
                _display->setFont(u8g2_font_wqy15_t_gb2312a);
                _currentFont = 2;
            } else {
                _display->setFont(u8g2_font_5x7_tf);  // 运行时禁用中文字库时回退 ASCII
                _currentFont = 0;
            }
#else
            _display->setFont(u8g2_font_5x7_tf);  // 未启用中文字库时回退 ASCII
            _currentFont = 0;
#endif
            break;
        default:
            return false;
    }
    
    return true;
}

bool LCDManager::setContrast(uint8_t contrast)
{
    LCD_GUARD_OR_RETURN(false);
    if (!_display || !_initialized) return false;
    
    beginManagedLcdOperation();
    _display->setContrast(contrast);
    if (!finishManagedLcdOperation()) return false;
    _contrast = contrast;
    return true;
}

bool LCDManager::setFlip(bool flip)
{
    LCD_GUARD_OR_RETURN(false);
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
    LCD_GUARD_OR_RETURN(false);
    if (!_display || !_initialized) return false;
    
    beginManagedLcdOperation();
    _display->setPowerSave(!on);
    return finishManagedLcdOperation();
}

bool LCDManager::probeI2cDisplay(bool logWarning)
{
    if (_interface != DisplayInterface::I2C_MODE) return true;
    if (_sdaPin < 0 || _sclPin < 0) return false;

    FastBee::I2CBusLock busLock(100);
    TwoWire* wire = busLock.locked() && FastBee::I2CBusManager::instance().ownsAddress(this, _i2cAddress)
        ? FastBee::I2CBusManager::instance().wire(this) : nullptr;
    if (!wire) return false;
    g_lcdI2cAttempted = true;
    wire->beginTransmission(_i2cAddress);
    const uint8_t wireStatus = wire->endTransmission();
    if (wireStatus == 0) return true;

    const unsigned long now = millis();
    if (logWarning && (now - _lastBusWarning >= 30000UL || _lastBusWarning == 0)) {
        LOG_WARNINGF("LCD Manager: I2C display 0x%02X unavailable (status=%u)",
                     _i2cAddress, (unsigned)wireStatus);
        _lastBusWarning = now;
    }
    return false;
}

bool LCDManager::refresh()
{
    LCD_GUARD_OR_RETURN(false);
    if (!_display || !_initialized) return false;
    
    // 检查刷新间隔，避免频繁刷新
    if (!_forceRefresh && !shouldUpdate())
    {
        return true;
    }
    
    if (!probeI2cDisplay(true)) return false;

    beginManagedLcdOperation();
    _display->sendBuffer();
    if (!finishManagedLcdOperation()) {
        _contentChanged = true;
        return false;
    }
    _lastUpdate = millis();
    _contentChanged = false;
    
    return true;
}

void LCDManager::performMaintenance(const PeripheralConfig& config)
{
    // Maintenance is opportunistic. Never block the framework task behind a
    // foreground rule/script display transfer.
    RecursiveMutexGuard lcdGuard(_displayMutex, 0);
    if (!lcdGuard.isLocked()) return;
    if (!_display || !_initialized || _foregroundFailed || !matchesConfig(config)) return;
    // PM -> display -> shared I2C is the only order. Do not wait for a peer
    // while holding the PM lock, including a multi-chunk recovery sequence.
    FastBee::I2CBusLock busLock(0);
    if (!busLock.locked()) return;
    ManagedLcdFrameBudget frameBudget;

    unsigned long now = millis();

    // === 阶段 1：开机初期快速重申（前 30 秒，每 3 秒一次） ===
    // 上电、WiFi 启动等阶段的电压波动可能让面板丢失电荷泵/对比度配置而黑屏。
    // 注意：不能用 begin()，它内部的 clearDisplay() 会清零画面缓冲区，
    // 随后 sendBuffer() 就会把空白推上屏幕。initDisplay() 只发命令、不动缓冲区。
    static constexpr unsigned long REASSERT_WINDOW_MS = 30000;
    static constexpr unsigned long REASSERT_INTERVAL_MS = 3000;

    if (now - _initTime <= REASSERT_WINDOW_MS) {
        if (now - _lastReassert >= REASSERT_INTERVAL_MS) {
            if (!probeI2cDisplay(true)) return;
            _lastReassert = now; // Bound failed retries too; not a success timestamp.
            beginManagedLcdOperation();
            _display->initDisplay();
            _display->setPowerSave(0);
            _display->setContrast(_contrast);
            _display->sendBuffer();
            if (!finishManagedLcdOperation()) {
                _contentChanged = true;
                return;
            }
            LOG_INFOF("[LCD] re-assert display (t=%lums, contrast=%d)",
                      now - _initTime, _contrast);
            _lastReassert = now;
            _lastUpdate = now;
        }
        return;
    }

    // === 阶段 2：长期 I2C 总线恢复（每 5 分钟一次） ===
    // 长时间运行中 I2C 总线可能因干扰、NACK 后状态机卡死等原因失效，
    // U8G2 sendBuffer() 会静默失败。定期重置 I2C 外设 + 重发初始化命令可自愈。
    static constexpr unsigned long FULL_RECOVERY_INTERVAL_MS = 300000;  // 5 分钟

    if (_interface == DisplayInterface::I2C_MODE && _sdaPin >= 0 && _sclPin >= 0) {
        if (now - _lastFullRecovery >= FULL_RECOVERY_INTERVAL_MS) {
            // 全局锁内恢复控制器，保留其他 I2C 使用者的租约。
            String recoveryError;
            if (!FastBee::I2CBusManager::instance().recover(this, recoveryError)) {
                LOG_WARNINGF("LCD Manager: I2C recovery failed: %s", recoveryError.c_str());
                _lastFullRecovery = now;
                return;
            }
            if (!probeI2cDisplay(true)) {
                _lastFullRecovery = now;
                return;
            }
            beginManagedLcdOperation();
            _display->initDisplay();
            _display->setPowerSave(0);
            _display->setContrast(_contrast);
            _display->sendBuffer();
            _lastFullRecovery = now;
            if (!finishManagedLcdOperation()) {
                _contentChanged = true;
                return;
            }
            _lastUpdate = now;
            LOG_INFOF("[LCD] periodic I2C recovery (SDA=%d SCL=%d, uptime=%lus)",
                      _sdaPin, _sclPin, now / 1000);
        }
    }
}

// ========== 状态查询 ==========

uint8_t LCDManager::getFontHeight() const
{
    LCD_GUARD_OR_RETURN(10);
    if (!_display) return 10;
    
    switch (_currentFont)
    {
        case 0: return 8;
        case 1: return 13;   // WQY 12 像素行高约 12-13px
        case 2: return 16;   // WQY 15 像素行高约 15-16px
        default: return 13;
    }
}

uint8_t LCDManager::getMaxLines() const
{
    LCD_GUARD_OR_RETURN(0);
    return _height / getFontHeight();
}

bool LCDManager::shouldUpdate()
{
    unsigned long now = millis();
    return (now - _lastUpdate >= _minUpdateInterval) || _contentChanged;
}

#undef LCD_GUARD_OR_RETURN
#undef LCD_GUARD_OR_RETURN_VOID

#endif // FASTBEE_ENABLE_LCD

#if FASTBEE_ENABLE_LCD
bool LCDManager::matchesConfig(const PeripheralConfig& config) const {
    return _activeId == config.id && !_activeId.isEmpty() &&
        config.type == PeripheralType::LCD && config.pinCount == 2 &&
        config.pins[0] == _sdaPin && config.pins[1] == _sclPin &&
        config.params.lcd.interface == 2 && config.params.lcd.width == _configuredWidth &&
        config.params.lcd.height == _configuredHeight && config.params.lcd.contrast == _configuredContrast;
}

bool LCDManager::quiescent(const PeripheralConfig& config) const {
    RecursiveMutexGuard lock(_displayMutex, 0);
    return lock.isLocked() && matchesConfig(config) && !g_lcdForegroundActive;
}

FastBee::DisplayWriteResult LCDManager::write(const PeripheralConfig& config,
    const FastBee::LcdOutputRequest& output) {
    using Command = FastBee::LcdOutputCommand;
    RecursiveMutexGuard lock(_displayMutex, 0);
    if (!lock.isLocked()) return {false, false, "DISPLAY_BUSY"};
    if (!_initialized || !_display || !matchesConfig(config))
        return {false, false, "DISPLAY_IDENTITY_UNCONFIRMED"};
    if (output.command == Command::Font)
        return {false, setFont(static_cast<uint8_t>(output.value)), "DISPLAY_FONT_UNCONFIRMED"};
    if (output.command == Command::TextLine &&
        (output.value < 0 || output.value >= getMaxLines()))
        return {false, false, "DISPLAY_LINE_INVALID"};
    FastBee::I2CBusLock busLock(0);
    if (!busLock.locked()) return {false, false, "DISPLAY_BUS_BUSY"};
    g_lcdI2cAttempted = false;
    ManagedLcdFrameBudget frameBudget;
    _forceRefresh = true;
    bool acknowledged = false;
    switch (output.command) {
        case Command::CustomText: acknowledged = showCustomText(output.text); break;
        case Command::Clear: clear(); acknowledged = refresh(); break;
        case Command::SensorPage: acknowledged = showSensorPage(static_cast<int8_t>(output.value)); break;
        case Command::TextAt:
            acknowledged = print(output.text, output.x, output.y, static_cast<TextAlign>(output.align)) && refresh();
            break;
        case Command::TextLine:
            acknowledged = printLine(output.text, static_cast<uint8_t>(output.value)) && refresh();
            break;
        case Command::SystemInfo: acknowledged = showSystemInfo(); break;
        default: break;
    }
    _forceRefresh = false;
    // A NACK must not be retried later by maintenance under a different source.
    _foregroundFailed = !acknowledged;
    if (!acknowledged) {
        // Do not let a later incremental TextAt action re-send the failed
        // invocation's cached payload as part of its own full frame.
        _display->clearBuffer();
        _contentChanged = true;
    }
    return {g_lcdI2cAttempted, acknowledged, "DISPLAY_TRANSFER_UNCONFIRMED"};
}
#endif
