/**
 * @file SevenSegmentDriver.cpp
 * @brief TM1637 4 位数码管驱动实现（bit-bang）
 */

#include "peripherals/SevenSegmentDriver.h"

#if FASTBEE_ENABLE_SEVEN_SEGMENT

#include "systems/LoggerSystem.h"

// TM1637 指令
#define TM1637_CMD_DATA     0x40  // 数据命令：写入显示寄存器，地址自增
#define TM1637_CMD_ADDR     0xC0  // 地址命令：起始位 0
#define TM1637_CMD_DISP_CTL 0x88  // 显示控制：开显示 + 亮度(0-7)

// 协议时序：TM1637 最大时钟 ≈ 250kHz，安全取 100kHz（5us 半周期即可；为稳妥取 50us）
#define TM1637_BIT_DELAY_US 50

// 段码表：0-9
static const uint8_t SEG_DIGITS[10] = {
    0x3F, // 0
    0x06, // 1
    0x5B, // 2
    0x4F, // 3
    0x66, // 4
    0x6D, // 5
    0x7D, // 6
    0x07, // 7
    0x7F, // 8
    0x6F  // 9
};

SevenSegmentDriver& SevenSegmentDriver::instance() {
    static SevenSegmentDriver inst;
    return inst;
}

bool SevenSegmentDriver::exists(const String& id) const {
    return _instances.find(id) != _instances.end();
}

// ========== 底层 bit-bang ==========

void SevenSegmentDriver::bitDelay() const {
    delayMicroseconds(TM1637_BIT_DELAY_US);
}

void SevenSegmentDriver::start(const Instance& ins) {
    // CLK 高、DIO 高 → DIO 拉低 = start
    pinMode(ins.dio, OUTPUT);
    digitalWrite(ins.dio, HIGH);
    digitalWrite(ins.clk, HIGH);
    bitDelay();
    digitalWrite(ins.dio, LOW);
    bitDelay();
}

void SevenSegmentDriver::stop(const Instance& ins) {
    pinMode(ins.dio, OUTPUT);
    digitalWrite(ins.clk, LOW);
    bitDelay();
    digitalWrite(ins.dio, LOW);
    bitDelay();
    digitalWrite(ins.clk, HIGH);
    bitDelay();
    digitalWrite(ins.dio, HIGH);
    bitDelay();
}

uint8_t SevenSegmentDriver::writeByte(const Instance& ins, uint8_t b) {
    // LSB first
    pinMode(ins.dio, OUTPUT);
    for (uint8_t i = 0; i < 8; i++) {
        digitalWrite(ins.clk, LOW);
        bitDelay();
        digitalWrite(ins.dio, (b & 0x01) ? HIGH : LOW);
        bitDelay();
        digitalWrite(ins.clk, HIGH);
        bitDelay();
        b >>= 1;
    }
    // 第 9 个时钟读 ACK
    digitalWrite(ins.clk, LOW);
    pinMode(ins.dio, INPUT_PULLUP);
    bitDelay();
    digitalWrite(ins.clk, HIGH);
    bitDelay();
    uint8_t ack = digitalRead(ins.dio);
    digitalWrite(ins.clk, LOW);
    pinMode(ins.dio, OUTPUT);
    digitalWrite(ins.dio, LOW);
    bitDelay();
    return ack;  // 0 = 设备应答成功
}

bool SevenSegmentDriver::writeSegments(const Instance& ins,
                                       const uint8_t* segs, uint8_t len,
                                       uint8_t startAddr) {
    // 1) 数据命令：自增地址写
    start(ins);
    writeByte(ins, TM1637_CMD_DATA);
    stop(ins);

    // 2) 地址命令 + 数据
    start(ins);
    writeByte(ins, TM1637_CMD_ADDR | (startAddr & 0x03));
    for (uint8_t i = 0; i < len; i++) {
        writeByte(ins, segs[i]);
    }
    stop(ins);

    // 3) 显示控制
    return commitDisplayCtrl(ins);
}

bool SevenSegmentDriver::commitDisplayCtrl(const Instance& ins) {
    start(ins);
    writeByte(ins, TM1637_CMD_DISP_CTL | (ins.brightness & 0x07));
    stop(ins);
    return true;
}

// ========== 字符映射 ==========

uint8_t SevenSegmentDriver::charToSeg(char c) {
    if (c >= '0' && c <= '9') return SEG_DIGITS[c - '0'];
    if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
    switch (c) {
        case 'A': return 0x77;
        case 'B': return 0x7C;  // 小写 b
        case 'C': return 0x39;
        case 'D': return 0x5E;  // 小写 d
        case 'E': return 0x79;
        case 'F': return 0x71;
        case 'G': return 0x3D;
        case 'H': return 0x76;
        case 'I': return 0x06;
        case 'J': return 0x1E;
        case 'L': return 0x38;
        case 'N': return 0x54;  // 小写 n
        case 'O': return 0x3F;
        case 'P': return 0x73;
        case 'Q': return 0x67;
        case 'R': return 0x50;  // 小写 r
        case 'S': return 0x6D;  // 同 5
        case 'T': return 0x78;  // 小写 t
        case 'U': return 0x3E;
        case 'Y': return 0x6E;
        case '-': return 0x40;
        case '_': return 0x08;
        case ' ': return 0x00;
        default:  return 0x00;
    }
}

// ========== 公共接口 ==========

bool SevenSegmentDriver::begin(const String& id, uint8_t clkPin, uint8_t dioPin, uint8_t brightness) {
    if (id.isEmpty()) return false;
    if (brightness > 7) brightness = 7;

    Instance ins{clkPin, dioPin, brightness};

    pinMode(ins.clk, OUTPUT);
    pinMode(ins.dio, OUTPUT);
    digitalWrite(ins.clk, HIGH);
    digitalWrite(ins.dio, HIGH);
    delay(2);

    _instances[id] = ins;

    // 初始化清屏 + 设置亮度
    uint8_t blank[4] = {0, 0, 0, 0};
    bool ok = writeSegments(ins, blank, 4, 0);

    LOG_INFOF("[TM1637] begin id=%s clk=%d dio=%d bri=%d -> %s",
              id.c_str(), clkPin, dioPin, brightness, ok ? "ok" : "fail");
    return ok;
}

void SevenSegmentDriver::release(const String& id) {
    auto it = _instances.find(id);
    if (it == _instances.end()) return;
    // 清屏再释放
    uint8_t blank[4] = {0, 0, 0, 0};
    writeSegments(it->second, blank, 4, 0);
    _instances.erase(it);
    LOG_INFOF("[TM1637] release id=%s", id.c_str());
}

bool SevenSegmentDriver::setBrightness(const String& id, uint8_t brightness) {
    auto it = _instances.find(id);
    if (it == _instances.end()) return false;
    if (brightness > 7) brightness = 7;
    it->second.brightness = brightness;
    return commitDisplayCtrl(it->second);
}

bool SevenSegmentDriver::clear(const String& id) {
    auto it = _instances.find(id);
    if (it == _instances.end()) return false;
    uint8_t blank[4] = {0, 0, 0, 0};
    return writeSegments(it->second, blank, 4, 0);
}

bool SevenSegmentDriver::displayNumber(const String& id, const String& value) {
    auto it = _instances.find(id);
    if (it == _instances.end()) return false;

    // 解析规则：
    // - 按字符序逐个映射
    // - '.' 附加到前一位的 DP
    // - ':' 附加到 index=1 的 DP（TM1637 带冒号款）
    // - '-' 按减号段显示
    // - 其他非数字字符按 charToSeg 映射
    // 最终固定 4 位，长度不足右对齐（左侧填空）
    uint8_t segs[4] = {0, 0, 0, 0};
    int slot = 0;
    bool colon = false;

    // 第一遍：压缩串（去掉 '.' 和 ':'，记录 DP/冒号位置）
    struct Tok { char c; bool dp; };
    Tok tokens[8];
    int tokLen = 0;

    for (size_t i = 0; i < value.length() && tokLen < 8; i++) {
        char c = value.charAt(i);
        if (c == '.') {
            if (tokLen > 0) tokens[tokLen - 1].dp = true;
        } else if (c == ':') {
            colon = true;
        } else {
            tokens[tokLen].c = c;
            tokens[tokLen].dp = false;
            tokLen++;
        }
    }

    // 右对齐：把最后 4 个 token 放入 segs
    int startIdx = (tokLen > 4) ? (tokLen - 4) : 0;
    int padLeft = (tokLen < 4) ? (4 - tokLen) : 0;
    for (int i = startIdx; i < tokLen && slot + padLeft < 4; i++) {
        uint8_t s = charToSeg(tokens[i].c);
        if (tokens[i].dp) s |= 0x80;
        segs[padLeft + slot] = s;
        slot++;
    }

    // 冒号：TM1637 4 位带冒号模块的冒号位为 index=1 的 DP
    if (colon) {
        segs[1] |= 0x80;
    }

    return writeSegments(it->second, segs, 4, 0);
}

bool SevenSegmentDriver::displayText(const String& id, const String& text) {
    auto it = _instances.find(id);
    if (it == _instances.end()) return false;

    uint8_t segs[4] = {0, 0, 0, 0};
    int len = (int)text.length();
    if (len > 4) len = 4;

    // 文本左对齐
    for (int i = 0; i < len; i++) {
        segs[i] = charToSeg(text.charAt(i));
    }

    return writeSegments(it->second, segs, 4, 0);
}

#endif // FASTBEE_ENABLE_SEVEN_SEGMENT
