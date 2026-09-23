/**
 * @file test_modbus_handler.cpp
 * @brief ModbusHandler 协议处理单元测试
 * 
 * 测试内容：
 * - Modbus CRC16 校验算法（标准多项式 0xA001）
 * - 数据 DJB2 Hash 计算（变化检测）
 * - 死区（Deadband）判断逻辑
 * - 功能码分类（读/写）
 * - ModbusException 与 OneShotError 枚举
 * - ModbusSubDevice 结构体默认值与 hasMotorSoftLimit()
 * - PollTask 结构体默认值
 * - 寄存器值数据类型转换（uint16/int16/uint32/int32/float32）
 */

#include <unity.h>
#include <Arduino.h>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

void test_modbus_handler_group();

// ========== 内联复现 Modbus 核心逻辑 ==========

// Modbus RTU CRC16（镜像 ModbusHandler::calculateCRC）
static uint16_t calculateCRC(const uint8_t* data, uint8_t length) {
    uint16_t crc = 0xFFFF;
    for (uint8_t pos = 0; pos < length; pos++) {
        crc ^= (uint16_t)data[pos];
        for (uint8_t i = 8; i != 0; i--) {
            if ((crc & 0x0001) != 0) {
                crc >>= 1;
                crc ^= 0xA001;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

// 镜像 computeDataHash（DJB2 算法）
static uint32_t computeDataHash(const uint16_t* data, size_t count) {
    uint32_t hash = 5381;
    for (size_t i = 0; i < count; i++) {
        hash = ((hash << 5) + hash) ^ (data[i] & 0xFF);
        hash = ((hash << 5) + hash) ^ ((data[i] >> 8) & 0xFF);
    }
    return hash;
}

// 镜像 isWithinDeadband
static bool isWithinDeadband(const uint16_t* newValues, const uint16_t* oldValues,
                             uint16_t count, float deadband) {
    if (!oldValues || deadband <= 0.0f) return false;
    for (uint16_t i = 0; i < count; i++) {
        float change = fabs((float)newValues[i] - (float)oldValues[i]);
        float threshold = fabs((float)oldValues[i]) * deadband / 100.0f;
        if (threshold < 1.0f) threshold = 1.0f;
        if (change > threshold) return false;
    }
    return true;
}

// 镜像 isReadFunctionCode
static bool isReadFunctionCode(uint8_t fc) {
    return fc == 0x01 || fc == 0x02 || fc == 0x03 || fc == 0x04;
}

// 镜像 clampValue
template <typename T>
static T clampValue(T value, T minVal, T maxVal) {
    if (value < minVal) return minVal;
    if (value > maxVal) return maxVal;
    return value;
}

// 镜像 ModbusException
enum ModbusException : uint8_t {
    MODBUS_EX_ILLEGAL_FUNCTION     = 0x01,
    MODBUS_EX_ILLEGAL_DATA_ADDRESS = 0x02,
    MODBUS_EX_ILLEGAL_DATA_VALUE   = 0x03,
    MODBUS_EX_SLAVE_DEVICE_FAILURE = 0x04
};

// 镜像 OneShotError
enum OneShotError : uint8_t {
    ONESHOT_SUCCESS         = 0,
    ONESHOT_TIMEOUT         = 1,
    ONESHOT_CRC_ERROR       = 2,
    ONESHOT_EXCEPTION       = 3,
    ONESHOT_NOT_INITIALIZED = 4,
    ONESHOT_BUSY            = 5
};

// ModbusSubDevice 测试替身（用于测试默认值和 hasMotorSoftLimit）
struct TestModbusSubDevice {
    char     name[32];
    uint8_t  slaveAddress;
    uint8_t  channelCount;
    bool     ncMode;
    uint8_t  controlProtocol;
    uint8_t  motorDecimals;
    int32_t  motorMinPosition;
    int32_t  motorMaxPosition;
    int32_t  motorCurrentPosition;
    int32_t  motorMoveStep;
    uint16_t motorLastPulse;
    bool     enabled;

    TestModbusSubDevice()
        : slaveAddress(1), channelCount(2), ncMode(false),
          controlProtocol(0), motorDecimals(0),
          motorMinPosition(0), motorMaxPosition(0),
          motorCurrentPosition(0), motorMoveStep(0), motorLastPulse(0),
          enabled(true) {
        memset(name, 0, sizeof(name));
        strncpy(name, "Device", sizeof(name) - 1);
    }

    bool hasMotorSoftLimit() const {
        return motorMaxPosition > motorMinPosition;
    }
};

// 寄存器数据类型（镜像 RegisterMapping::dataType）
enum RegisterDataType : uint8_t {
    DT_UINT16  = 0,
    DT_INT16   = 1,
    DT_UINT32  = 2,
    DT_INT32   = 3,
    DT_FLOAT32 = 4
};

// 从原始 uint16 寄存器读取各数据类型
static float convertRegisterValue(const uint16_t* regs, RegisterDataType dataType) {
    switch (dataType) {
        case DT_UINT16:
            return (float)regs[0];
        case DT_INT16:
            return (float)(int16_t)regs[0];
        case DT_UINT32: {
            uint32_t val = ((uint32_t)regs[0] << 16) | regs[1];
            return (float)val;
        }
        case DT_INT32: {
            int32_t val = (int32_t)(((uint32_t)regs[0] << 16) | regs[1]);
            return (float)val;
        }
        case DT_FLOAT32: {
            uint32_t raw = ((uint32_t)regs[0] << 16) | regs[1];
            float f;
            memcpy(&f, &raw, sizeof(f));
            return f;
        }
        default:
            return 0.0f;
    }
}

// ========== CRC16 测试 ==========

static void test_crc16_known_frame() {
    // 标准 Modbus RTU CRC 测试向量：地址 0x01 + FC 0x03 + 起始 0x00 0x00 + 数量 0x00 0x0A
    uint8_t frame[] = {0x01, 0x03, 0x00, 0x00, 0x00, 0x0A};
    uint16_t crc = calculateCRC(frame, 6);
    // CRC = 0xCDC5（低位字节在前）
    TEST_ASSERT_EQUAL_HEX16(0xCDC5, crc);
}

static void test_crc16_single_byte() {
    uint8_t data[] = {0x00};
    uint16_t crc = calculateCRC(data, 1);
    // CRC of single 0x00 byte = 0x40BF
    TEST_ASSERT_EQUAL_HEX16(0x40BF, crc);
}

static void test_crc16_all_zeros() {
    uint8_t data[] = {0x00, 0x00, 0x00, 0x00};
    uint16_t crc = calculateCRC(data, 4);
    // 4 字节全 0 的 CRC
    TEST_ASSERT_NOT_EQUAL(0, crc);  // CRC 不应为 0（初始值 0xFFFF）
}

static void test_crc16_all_ones() {
    uint8_t data[] = {0xFF, 0xFF, 0xFF, 0xFF};
    uint16_t crc = calculateCRC(data, 4);
    TEST_ASSERT_NOT_EQUAL(0xFFFF, crc);  // 不应等于初始值
}

static void test_crc16_deterministic() {
    uint8_t frame[] = {0x01, 0x04, 0x00, 0x01, 0x00, 0x02};
    uint16_t crc1 = calculateCRC(frame, 6);
    uint16_t crc2 = calculateCRC(frame, 6);
    TEST_ASSERT_EQUAL(crc1, crc2);
}

static void test_crc16_different_data() {
    uint8_t frame1[] = {0x01, 0x03, 0x00, 0x00, 0x00, 0x01};
    uint8_t frame2[] = {0x01, 0x03, 0x00, 0x00, 0x00, 0x02};
    uint16_t crc1 = calculateCRC(frame1, 6);
    uint16_t crc2 = calculateCRC(frame2, 6);
    TEST_ASSERT_NOT_EQUAL(crc1, crc2);
}

static void test_crc16_frame_validation() {
    // 构造带 CRC 的完整帧：地址 + FC + 数据 + CRC
    uint8_t frame[8];
    frame[0] = 0x01;  // 地址
    frame[1] = 0x03;  // FC
    frame[2] = 0x02;  // 字节数
    frame[3] = 0x00;  // 数据高
    frame[4] = 0x64;  // 数据低 (100)
    uint16_t crc = calculateCRC(frame, 5);
    frame[5] = (uint8_t)(crc & 0xFF);        // CRC 低字节
    frame[6] = (uint8_t)((crc >> 8) & 0xFF); // CRC 高字节
    
    // 验证：重新计算前 5 字节的 CRC，应匹配后 2 字节
    uint16_t verifyCrc = calculateCRC(frame, 5);
    TEST_ASSERT_EQUAL(crc, verifyCrc);
    TEST_ASSERT_EQUAL(frame[5], verifyCrc & 0xFF);
    TEST_ASSERT_EQUAL(frame[6], (verifyCrc >> 8) & 0xFF);
}

// ========== Data Hash 测试 ==========

static void test_data_hash_deterministic() {
    uint16_t data[] = {100, 200, 300};
    uint32_t h1 = computeDataHash(data, 3);
    uint32_t h2 = computeDataHash(data, 3);
    TEST_ASSERT_EQUAL(h1, h2);
}

static void test_data_hash_different_data() {
    uint16_t data1[] = {100, 200, 300};
    uint16_t data2[] = {100, 200, 301};
    uint32_t h1 = computeDataHash(data1, 3);
    uint32_t h2 = computeDataHash(data2, 3);
    TEST_ASSERT_NOT_EQUAL(h1, h2);
}

static void test_data_hash_empty() {
    // 空数据应返回初始值 5381
    uint32_t h = computeDataHash(nullptr, 0);
    TEST_ASSERT_EQUAL(5381, h);
}

static void test_data_hash_order_matters() {
    uint16_t data1[] = {100, 200};
    uint16_t data2[] = {200, 100};
    uint32_t h1 = computeDataHash(data1, 2);
    uint32_t h2 = computeDataHash(data2, 2);
    TEST_ASSERT_NOT_EQUAL(h1, h2);  // 顺序不同，hash 不同
}

// ========== Deadband 测试 ==========

static void test_deadband_within_threshold() {
    uint16_t oldVals[] = {1000, 2000, 3000};
    uint16_t newVals[] = {1001, 2002, 3003};  // 微小变化
    
    // deadband = 1.0%，1000 的阈值 = 10，2000 的阈值 = 20，3000 的阈值 = 30
    TEST_ASSERT_TRUE(isWithinDeadband(newVals, oldVals, 3, 1.0f));
}

static void test_deadband_exceeded() {
    uint16_t oldVals[] = {1000};
    uint16_t newVals[] = {1200};  // 变化 200，超过 1% 阈值（10）
    
    TEST_ASSERT_FALSE(isWithinDeadband(newVals, oldVals, 1, 1.0f));
}

static void test_deadband_zero_old_value() {
    // oldVal=0 时，threshold = max(0*deadband/100, 1) = 1
    // 任何 >1 的变化都应触发上报
    uint16_t oldVals[] = {0};
    uint16_t newVals[] = {5};
    TEST_ASSERT_FALSE(isWithinDeadband(newVals, oldVals, 1, 1.0f));
}

static void test_deadband_zero_disabled() {
    uint16_t oldVals[] = {1000};
    uint16_t newVals[] = {1000};
    
    // deadband=0 应禁用（始终返回 false，即"不在死区内"→触发上报）
    TEST_ASSERT_FALSE(isWithinDeadband(newVals, oldVals, 1, 0.0f));
}

static void test_deadband_null_old_values() {
    uint16_t newVals[] = {1000};
    TEST_ASSERT_FALSE(isWithinDeadband(newVals, nullptr, 1, 1.0f));
}

static void test_deadband_minimum_threshold() {
    // 即使 oldVal 很小，最小阈值也是 1
    uint16_t oldVals[] = {1};
    uint16_t newVals[] = {1};  // 无变化
    TEST_ASSERT_TRUE(isWithinDeadband(newVals, oldVals, 1, 0.5f));
    
    uint16_t newVals2[] = {3};  // 变化 2 > 阈值 1
    TEST_ASSERT_FALSE(isWithinDeadband(newVals2, oldVals, 1, 0.5f));
}

// ========== 功能码分类测试 ==========

static void test_read_function_codes() {
    TEST_ASSERT_TRUE(isReadFunctionCode(0x01));  // 读线圈
    TEST_ASSERT_TRUE(isReadFunctionCode(0x02));  // 读离散输入
    TEST_ASSERT_TRUE(isReadFunctionCode(0x03));  // 读保持寄存器
    TEST_ASSERT_TRUE(isReadFunctionCode(0x04));  // 读输入寄存器
}

static void test_write_function_codes_not_read() {
    TEST_ASSERT_FALSE(isReadFunctionCode(0x05));  // 写单个线圈
    TEST_ASSERT_FALSE(isReadFunctionCode(0x06));  // 写单个寄存器
    TEST_ASSERT_FALSE(isReadFunctionCode(0x0F));  // 写多个线圈
    TEST_ASSERT_FALSE(isReadFunctionCode(0x10));  // 写多个寄存器
}

static void test_invalid_function_codes() {
    TEST_ASSERT_FALSE(isReadFunctionCode(0x00));
    TEST_ASSERT_FALSE(isReadFunctionCode(0xFF));
}

// ========== ClampValue 测试 ==========

static void test_clamp_within_range() {
    TEST_ASSERT_EQUAL(50, clampValue<int>(50, 0, 100));
}

static void test_clamp_below_min() {
    TEST_ASSERT_EQUAL(0, clampValue<int>(-10, 0, 100));
}

static void test_clamp_above_max() {
    TEST_ASSERT_EQUAL(100, clampValue<int>(150, 0, 100));
}

static void test_clamp_at_boundary() {
    TEST_ASSERT_EQUAL(0, clampValue<int>(0, 0, 100));
    TEST_ASSERT_EQUAL(100, clampValue<int>(100, 0, 100));
}

static void test_clamp_float() {
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.5f, clampValue<float>(0.5f, 0.0f, 1.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, clampValue<float>(-0.5f, 0.0f, 1.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.0f, clampValue<float>(1.5f, 0.0f, 1.0f));
}

// ========== 枚举测试 ==========

static void test_modbus_exception_codes() {
    TEST_ASSERT_EQUAL(0x01, (int)MODBUS_EX_ILLEGAL_FUNCTION);
    TEST_ASSERT_EQUAL(0x02, (int)MODBUS_EX_ILLEGAL_DATA_ADDRESS);
    TEST_ASSERT_EQUAL(0x03, (int)MODBUS_EX_ILLEGAL_DATA_VALUE);
    TEST_ASSERT_EQUAL(0x04, (int)MODBUS_EX_SLAVE_DEVICE_FAILURE);
}

static void test_oneshot_error_codes() {
    TEST_ASSERT_EQUAL(0, (int)ONESHOT_SUCCESS);
    TEST_ASSERT_EQUAL(1, (int)ONESHOT_TIMEOUT);
    TEST_ASSERT_EQUAL(2, (int)ONESHOT_CRC_ERROR);
    TEST_ASSERT_EQUAL(3, (int)ONESHOT_EXCEPTION);
    TEST_ASSERT_EQUAL(4, (int)ONESHOT_NOT_INITIALIZED);
    TEST_ASSERT_EQUAL(5, (int)ONESHOT_BUSY);
}

// ========== 结构体默认值测试 ==========

static void test_subdevice_defaults() {
    TestModbusSubDevice dev;
    TEST_ASSERT_EQUAL(1, dev.slaveAddress);
    TEST_ASSERT_EQUAL(2, dev.channelCount);
    TEST_ASSERT_FALSE(dev.ncMode);
    TEST_ASSERT_EQUAL(0, dev.controlProtocol);
    TEST_ASSERT_TRUE(dev.enabled);
    TEST_ASSERT_EQUAL_STRING("Device", dev.name);
}

static void test_subdevice_motor_soft_limit_disabled() {
    TestModbusSubDevice dev;
    // 默认 min=max=0，软限位不启用
    TEST_ASSERT_FALSE(dev.hasMotorSoftLimit());
}

static void test_subdevice_motor_soft_limit_enabled() {
    TestModbusSubDevice dev;
    dev.motorMinPosition = 0;
    dev.motorMaxPosition = 1000;
    TEST_ASSERT_TRUE(dev.hasMotorSoftLimit());
}

static void test_subdevice_motor_soft_limit_equal() {
    TestModbusSubDevice dev;
    dev.motorMinPosition = 100;
    dev.motorMaxPosition = 100;
    TEST_ASSERT_FALSE(dev.hasMotorSoftLimit());  // max 必须 > min
}

static void test_subdevice_motor_new_fields_defaults() {
    TestModbusSubDevice dev;
    // motorDecimals/motorMoveStep/motorLastPulse 默认值均为 0
    TEST_ASSERT_EQUAL(0, dev.motorDecimals);
    TEST_ASSERT_EQUAL(0, dev.motorMoveStep);
    TEST_ASSERT_EQUAL(0, dev.motorLastPulse);
}

static void test_subdevice_motor_new_fields_assignment() {
    TestModbusSubDevice dev;
    dev.motorDecimals = 2;
    dev.motorMinPosition = -5000;
    dev.motorMaxPosition = 5000;
    dev.motorCurrentPosition = 100;
    dev.motorMoveStep = 1600;
    dev.motorLastPulse = 800;

    TEST_ASSERT_EQUAL(2, dev.motorDecimals);
    TEST_ASSERT_EQUAL(-5000, dev.motorMinPosition);
    TEST_ASSERT_EQUAL(5000, dev.motorMaxPosition);
    TEST_ASSERT_EQUAL(100, dev.motorCurrentPosition);
    TEST_ASSERT_EQUAL(1600, dev.motorMoveStep);
    TEST_ASSERT_EQUAL(800, dev.motorLastPulse);
    TEST_ASSERT_TRUE(dev.hasMotorSoftLimit());  // 5000 > -5000
}

// ========== 寄存器值类型转换测试 ==========

static void test_convert_uint16() {
    uint16_t regs[] = {0xFFFF};
    float val = convertRegisterValue(regs, DT_UINT16);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 65535.0f, val);
}

static void test_convert_int16_negative() {
    uint16_t regs[] = {(uint16_t)(-100)};  // 0xFF9C
    float val = convertRegisterValue(regs, DT_INT16);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, -100.0f, val);
}

static void test_convert_int16_positive() {
    uint16_t regs[] = {1000};
    float val = convertRegisterValue(regs, DT_INT16);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 1000.0f, val);
}

static void test_convert_uint32() {
    // 0x0001_0000 = 65536
    uint16_t regs[] = {0x0001, 0x0000};
    float val = convertRegisterValue(regs, DT_UINT32);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 65536.0f, val);
}

static void test_convert_int32_negative() {
    // -1 in int32 = 0xFFFFFFFF
    uint16_t regs[] = {0xFFFF, 0xFFFF};
    float val = convertRegisterValue(regs, DT_INT32);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, -1.0f, val);
}

static void test_convert_float32() {
    // 3.14 in IEEE 754 float32 = 0x4048F5C3
    // 高位字 = 0x4048，低位字 = 0xF5C3
    uint16_t regs[] = {0x4048, 0xF5C3};
    float val = convertRegisterValue(regs, DT_FLOAT32);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 3.14f, val);
}

static void test_convert_float32_zero() {
    uint16_t regs[] = {0x0000, 0x0000};
    float val = convertRegisterValue(regs, DT_FLOAT32);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, val);
}

static void test_convert_float32_negative() {
    // -273.15 in IEEE 754 = 0xC388_999A
    uint16_t regs[] = {0xC388, 0x999A};
    float val = convertRegisterValue(regs, DT_FLOAT32);
    TEST_ASSERT_FLOAT_WITHIN(0.1f, -273.15f, val);
}

// ========== Slave 地址有效性 ==========

static void test_valid_slave_addresses() {
    // Modbus 从站地址范围 1-247
    for (uint8_t addr = 1; addr <= 247; addr++) {
        TEST_ASSERT_TRUE(addr >= 1 && addr <= 247);
    }
}

static void test_invalid_slave_addresses() {
    TEST_ASSERT_FALSE(0 >= 1 && 0 <= 247);
    TEST_ASSERT_FALSE(248 >= 1 && 248 <= 247);
}

// ========== MasterConfig 硬编码默认值测试 ==========

// 内联复现 MasterConfig（符合实际硬编码默认值）
struct TestMasterConfig {
    uint16_t responseTimeout;
    uint8_t  maxRetries;
    uint16_t interPollDelay;
    uint8_t  taskCount;

    // 硬编码默认值：不再从 protocol.json 读取，由外设执行-轮询触发器覆盖实际执行参数
    TestMasterConfig()
        : responseTimeout(1000), maxRetries(2), interPollDelay(100), taskCount(0) {}
};

static void test_master_config_default_response_timeout() {
    TestMasterConfig cfg;
    TEST_ASSERT_EQUAL_UINT16(1000, cfg.responseTimeout);
}

static void test_master_config_default_max_retries() {
    TestMasterConfig cfg;
    TEST_ASSERT_EQUAL_UINT8(2, cfg.maxRetries);
}

static void test_master_config_default_inter_poll_delay() {
    TestMasterConfig cfg;
    TEST_ASSERT_EQUAL_UINT16(100, cfg.interPollDelay);
}

static void test_master_config_defaults_in_valid_range() {
    // 默认值在合理范围内：[100, 5000] / [0, 3] / [20, 1000]
    TestMasterConfig cfg;
    TEST_ASSERT_TRUE(cfg.responseTimeout >= 100 && cfg.responseTimeout <= 5000);
    TEST_ASSERT_TRUE(cfg.maxRetries <= 3);
    TEST_ASSERT_TRUE(cfg.interPollDelay >= 20 && cfg.interPollDelay <= 1000);
}

static void test_master_config_periph_exec_override_pattern() {
    // 验证覆盖模式： PeriphExec 传入的参数可替代默认值执行，完成后恢复
    TestMasterConfig cfg;
    uint16_t origTimeout = cfg.responseTimeout;
    uint8_t origRetries  = cfg.maxRetries;

    // 模拟 PeriphExec 临时覆盖
    cfg.responseTimeout = 500;
    cfg.maxRetries      = 1;
    TEST_ASSERT_EQUAL_UINT16(500, cfg.responseTimeout);
    TEST_ASSERT_EQUAL_UINT8(1, cfg.maxRetries);

    // 执行完成后恢复默认值
    cfg.responseTimeout = origTimeout;
    cfg.maxRetries      = origRetries;
    TEST_ASSERT_EQUAL_UINT16(1000, cfg.responseTimeout);
    TEST_ASSERT_EQUAL_UINT8(2, cfg.maxRetries);
}

static std::string readModbusProductionFile(const char* path) {
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) return {};
    std::ostringstream content;
    content << input.rdbuf();
    return content.str();
}

static size_t countTextOccurrences(const std::string& text, const std::string& needle) {
    size_t count = 0;
    size_t pos = 0;
    while ((pos = text.find(needle, pos)) != std::string::npos) {
        count++;
        pos += needle.length();
    }
    return count;
}

// Native builds intentionally do not link the hardware protocol sources. This
// contract test therefore inspects the exact production path that owns batch
// result routing, rather than testing a second copy of the aggregation logic.
static void test_raw_poll_batch_uses_atomic_per_frame_production_path() {
    const std::string handler = readModbusProductionFile("src/protocols/ModbusHandler.cpp");
    const std::string header = readModbusProductionFile("include/protocols/ModbusHandler.h");
    const std::string executor = readModbusProductionFile("src/core/PeriphExecExecutor.cpp");
    TEST_ASSERT_FALSE_MESSAGE(handler.empty(), "ModbusHandler.cpp must be readable");
    TEST_ASSERT_FALSE_MESSAGE(header.empty(), "ModbusHandler.h must be readable");
    TEST_ASSERT_FALSE_MESSAGE(executor.empty(), "PeriphExecExecutor.cpp must be readable");

    const size_t batchStart = handler.find("String ModbusHandler::executePollTaskBatch");
    const size_t batchEnd = handler.find("String ModbusHandler::getPollStatistics", batchStart);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(std::string::npos, batchStart,
                                  "production batch implementation must exist");
    TEST_ASSERT_NOT_EQUAL_MESSAGE(std::string::npos, batchEnd,
                                  "production batch implementation boundary must exist");
    const std::string batch = handler.substr(batchStart, batchEnd - batchStart);

    const size_t appendStart = batch.find("auto appendResult");
    const size_t rawBranch = batch.find("if (rawTransfer)", appendStart);
    const size_t frameDispatch = batch.find("rawFrameCallback(slaveAddress, result)", rawBranch);
    const size_t rawReturn = batch.find("return;", frameDispatch);
    const size_t jsonStrip = batch.find("result.substring(1, result.length() - 1)", appendStart);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, appendStart);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, rawBranch);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, frameDispatch);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, rawReturn);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, jsonStrip);
    TEST_ASSERT_TRUE_MESSAGE(rawBranch < frameDispatch && frameDispatch < rawReturn &&
                             rawReturn < jsonStrip,
                             "raw HEX must return after per-frame dispatch before JSON bracket stripping");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        0, static_cast<uint32_t>(countTextOccurrences(batch, "combined += r.substring")),
        "legacy unconditional HEX-corrupting batch concatenation must be absent");
    TEST_ASSERT_TRUE_MESSAGE(
        countTextOccurrences(batch, "return rawTransfer ? \"[]\" : combined;") >= 2,
        "raw batch output must never masquerade as an aggregated JSON payload");

    TEST_ASSERT_NOT_EQUAL_MESSAGE(
        std::string::npos, header.find("using PollFrameCallback"),
        "batch API must expose a per-frame raw result callback");
    TEST_ASSERT_NOT_EQUAL_MESSAGE(
        std::string::npos, header.find("PollFrameCallback rawFrameCallback = nullptr"),
        "batch API callback must remain optional for JSON callers");

    const size_t actionStart = executor.find("bool PeriphExecExecutor::executeModbusPollAction");
    const size_t actionEnd = executor.find("String PeriphExecExecutor::guessSensorDataField", actionStart);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, actionStart);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, actionEnd);
    const std::string actionPath = executor.substr(actionStart, actionEnd - actionStart);
    TEST_ASSERT_TRUE_MESSAGE(
        countTextOccurrences(actionPath, "&pollBatchComplete, dispatchRawFrame") == 1 &&
        countTextOccurrences(actionPath, "&legacyBatchComplete, dispatchRawFrame") == 1,
        "both structured and legacy PeriphExec poll paths must pass the raw frame callback");
    TEST_ASSERT_NOT_EQUAL_MESSAGE(
        std::string::npos,
        actionPath.find("slaveAddress, frame, ModbusDataSource::PeriphExecPoll"),
        "raw frames must use the queued PeriphExecPoll MQTT outlet with the real slave address");
}

static void test_raw_oneshot_read_uses_atomic_per_frame_production_path() {
    const std::string manager = readModbusProductionFile("src/protocols/ProtocolManager.cpp");
    TEST_ASSERT_FALSE_MESSAGE(manager.empty(), "ProtocolManager.cpp must be readable");

    const size_t readStart = manager.find("String ProtocolManager::executeModbusRead");
    const size_t readEnd = manager.find("String ProtocolManager::executeModbusRawSend", readStart);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(std::string::npos, readStart,
                                  "production Modbus one-shot read path must exist");
    TEST_ASSERT_NOT_EQUAL_MESSAGE(std::string::npos, readEnd,
                                  "production Modbus one-shot read boundary must exist");
    const std::string readPath = manager.substr(readStart, readEnd - readStart);

    const size_t rawMode = readPath.find("const bool rawTransfer =");
    const size_t frameCapture = readPath.find(
        "rawTransfer ? &rawResponseHex : nullptr", rawMode);
    const size_t frameDispatch = readPath.find(
        "slaveAddr, rawResponseHex, ModbusDataSource::PeriphExecPoll",
        frameCapture);
    const size_t jsonBranch = readPath.find("t.containsKey(\"mappings\")", frameDispatch);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, rawMode);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, frameCapture);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, frameDispatch);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, jsonBranch);
    TEST_ASSERT_TRUE_MESSAGE(rawMode < frameCapture && frameCapture < frameDispatch &&
                             frameDispatch < jsonBranch,
                             "each captured RAW one-shot ADU must be dispatched before the JSON mapping path");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        0, static_cast<uint32_t>(countTextOccurrences(readPath, "item[\"value\"] = hexStr")),
        "RAW RTU frames must not be wrapped in a JSON value field");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        0, static_cast<uint32_t>(countTextOccurrences(readPath, "formatRawHex(")),
        "RAW one-shot reads must not reconstruct the UART response");
}

static void test_raw_poll_preserves_uart_adu_without_json_suppression() {
    const std::string handler = readModbusProductionFile("src/protocols/ModbusHandler.cpp");
    const std::string header = readModbusProductionFile("include/protocols/ModbusHandler.h");
    const std::string manager = readModbusProductionFile("src/protocols/ProtocolManager.cpp");
    TEST_ASSERT_FALSE_MESSAGE(handler.empty(), "ModbusHandler.cpp must be readable");
    TEST_ASSERT_FALSE_MESSAGE(header.empty(), "ModbusHandler.h must be readable");
    TEST_ASSERT_FALSE_MESSAGE(manager.empty(), "ProtocolManager.cpp must be readable");

    TEST_ASSERT_NOT_EQUAL_MESSAGE(
        std::string::npos, header.find("String* rawResponseHex = nullptr"),
        "read API must optionally expose the exact validated response ADU");

    const size_t readStart = handler.find("OneShotResult ModbusHandler::readRegistersOnce");
    const size_t readEnd = handler.find("OneShotError ModbusHandler::_readRangeInto", readStart);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, readStart);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, readEnd);
    const std::string readPath = handler.substr(readStart, readEnd - readStart);
    const size_t transactionPos = readPath.find("result.error = _sendOneShotRequestRaw(");
    const size_t preservePos = readPath.find("*rawResponseHex += byteHex", transactionPos);
    const size_t errorReturnPos = readPath.find("if (result.error != ONESHOT_SUCCESS)", preservePos);
    TEST_ASSERT_TRUE_MESSAGE(
        transactionPos != std::string::npos && preservePos != std::string::npos &&
        errorReturnPos != std::string::npos &&
        transactionPos < preservePos && preservePos < errorReturnPos,
        "CRC-valid exception ADUs must be preserved before structured error return");
    TEST_ASSERT_NOT_EQUAL_MESSAGE(
        std::string::npos,
        readPath.find("Protocols::MODBUS_MAX_REGISTERS_PER_READ"),
        "RAW reads must support the full protocol read quantity, not the 32-value JSON cache limit");

    const size_t finalizeStart = handler.find("String ModbusHandler::_finalizePollResult");
    const size_t finalizeEnd = handler.find("String ModbusHandler::executePollTaskBatch", finalizeStart);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, finalizeStart);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, finalizeEnd);
    const std::string finalizePath = handler.substr(finalizeStart, finalizeEnd - finalizeStart);
    const size_t rawSuccessPos = finalizePath.find("if (config.transferType == 1) {");
    const size_t exactFramePos = finalizePath.find("const String& hexStr = *rawResponseHex", rawSuccessPos);
    const size_t reportPolicyPos = finalizePath.find("uint32_t newHash = computeDataHash", rawSuccessPos);
    TEST_ASSERT_TRUE_MESSAGE(
        rawSuccessPos != std::string::npos && exactFramePos != std::string::npos &&
        reportPolicyPos != std::string::npos && rawSuccessPos < exactFramePos &&
        exactFramePos < reportPolicyPos,
        "RAW polling must forward the captured UART ADU before JSON hash/deadband suppression");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        0, static_cast<uint32_t>(countTextOccurrences(
               finalizePath.substr(rawSuccessPos, reportPolicyPos - rawSuccessPos),
               "formatRawHex(")),
        "RAW polling must not reconstruct a response from decoded values");

    const size_t batchStart = handler.find("String ModbusHandler::executePollTaskBatch");
    const size_t batchEnd = handler.find("String ModbusHandler::getPollStatistics", batchStart);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, batchStart);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, batchEnd);
    const std::string batchPath = handler.substr(batchStart, batchEnd - batchStart);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(
        std::string::npos,
        batchPath.find("if (rawTransfer || !Protocols::MODBUS_MERGE_READS_ENABLED || candCount == 1)"),
        "RAW polling must bypass request merging so every reported ADU was actually on the UART");

    const size_t managerReadStart = manager.find("String ProtocolManager::executeModbusRead");
    const size_t managerReadEnd = manager.find("String ProtocolManager::executeModbusRawSend", managerReadStart);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, managerReadStart);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, managerReadEnd);
    const std::string managerRead = manager.substr(managerReadStart, managerReadEnd - managerReadStart);
    const size_t managerCapturePos = managerRead.find("rawTransfer ? &rawResponseHex : nullptr");
    const size_t managerDispatchPos = managerRead.find(
        "slaveAddr, rawResponseHex, ModbusDataSource::PeriphExecPoll", managerCapturePos);
    const size_t managerErrorPos = managerRead.find("if (readResult.error != ONESHOT_SUCCESS)", managerDispatchPos);
    TEST_ASSERT_TRUE_MESSAGE(
        managerCapturePos != std::string::npos && managerDispatchPos != std::string::npos &&
        managerErrorPos != std::string::npos && managerCapturePos < managerDispatchPos &&
        managerDispatchPos < managerErrorPos,
        "RAW one-shot reads must forward captured exception ADUs before JSON error handling");
}

static void test_rtu_frame_gap_uses_millisecond_units() {
    const std::string handler = readModbusProductionFile("src/protocols/ModbusHandler.cpp");
    TEST_ASSERT_FALSE_MESSAGE(handler.empty(), "ModbusHandler.cpp must be readable");

    const size_t helperStart = handler.find("static unsigned long modbusRtuFrameGapMs");
    const size_t helperEnd = handler.find("}", helperStart);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(std::string::npos, helperStart,
                                  "RTU frame-gap conversion helper must exist");
    TEST_ASSERT_NOT_EQUAL(std::string::npos, helperEnd);
    const std::string helper = handler.substr(helperStart, helperEnd - helperStart);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(
        std::string::npos, helper.find("35000UL / baudRate"),
        "35 bit-times must be converted to milliseconds before millis comparison");
    TEST_ASSERT_NOT_EQUAL_MESSAGE(
        std::string::npos, helper.find("35000UL % baudRate"),
        "RTU frame-gap conversion must round up partial milliseconds");
    TEST_ASSERT_TRUE_MESSAGE(
        countTextOccurrences(handler, "modbusRtuFrameGapMs(config.baudRate)") >= 2,
        "master and slave receive paths must share the corrected frame-gap units");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        0, static_cast<uint32_t>(countTextOccurrences(
               handler, "35000000UL / config.baudRate")),
        "microsecond-valued timeout must never be compared directly with millis");
}

static void test_per_slave_poll_backoff_isolation_contract() {
    const std::string handler = readModbusProductionFile("src/protocols/ModbusHandler.cpp");
    const std::string header = readModbusProductionFile("include/protocols/ModbusHandler.h");
    TEST_ASSERT_FALSE_MESSAGE(handler.empty(), "ModbusHandler.cpp must be readable");
    TEST_ASSERT_FALSE_MESSAGE(header.empty(), "ModbusHandler.h must be readable");

    TEST_ASSERT_NOT_EQUAL_MESSAGE(
        std::string::npos,
        header.find("SlaveBackoffState _slaveBackoff[SLAVE_BACKOFF_SLOT_COUNT]"),
        "poll breaker state must use a bounded per-slave table");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        0, static_cast<uint32_t>(countTextOccurrences(header, "_consecutiveTimeouts")),
        "the global timeout breaker must not remain");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        0, static_cast<uint32_t>(countTextOccurrences(header, "std::map")),
        "breaker state must not allocate map nodes during long-running polling");

    const size_t deadlineStart = handler.find("bool ModbusHandler::_deadlineReached");
    const size_t findStateStart = handler.find(
        "ModbusHandler::SlaveBackoffState* ModbusHandler::_findSlaveBackoffState",
        deadlineStart);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, deadlineStart);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, findStateStart);
    const std::string deadline = handler.substr(deadlineStart, findStateStart - deadlineStart);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(
        std::string::npos,
        deadline.find("static_cast<int32_t>(now - deadline) >= 0"),
        "cooldown deadlines must remain safe when millis wraps");

    const size_t transactionStart = handler.find(
        "OneShotError ModbusHandler::_executeModbusTransactionRaw");
    const size_t requestStart = handler.find(
        "OneShotResult ModbusHandler::sendOneShotRequest", transactionStart);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, transactionStart);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, requestStart);
    const std::string transaction = handler.substr(
        transactionStart, requestStart - transactionStart);
    const size_t bypassCheck = transaction.find("!bypassSlaveBackoff");
    const size_t readOnlyCheck = transaction.find("isReadFunctionCode(request[1])");
    const size_t deferCheck = transaction.find("_shouldDeferSlaveRequest");
    const size_t serialWrite = transaction.find("modbusSerial->write");
    TEST_ASSERT_TRUE_MESSAGE(
        bypassCheck != std::string::npos && readOnlyCheck != std::string::npos &&
        deferCheck != std::string::npos && serialWrite != std::string::npos &&
        bypassCheck < deferCheck && readOnlyCheck < deferCheck && deferCheck < serialWrite,
        "only non-control reads may be deferred, before UART traffic starts");
    TEST_ASSERT_NOT_EQUAL_MESSAGE(
        std::string::npos, transaction.find("error == ONESHOT_CRC_ERROR ? 20U : 50U"),
        "in-request retry delay must distinguish CRC and timeout failures");
    TEST_ASSERT_NOT_EQUAL_MESSAGE(
        std::string::npos, transaction.find("_recordSlaveOutcome(expectedSlaveAddr, error"),
        "each real poll-read outcome must update only its slave state");

    const size_t outcomeStart = handler.find("void ModbusHandler::_recordSlaveOutcome");
    const size_t destructorStart = handler.find("ModbusHandler::~ModbusHandler", outcomeStart);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, outcomeStart);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, destructorStart);
    const std::string outcome = handler.substr(outcomeStart, destructorStart - outcomeStart);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, outcome.find("ONESHOT_TIMEOUT"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, outcome.find("ONESHOT_CRC_ERROR"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, outcome.find("ONESHOT_EXCEPTION"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, outcome.find("state->consecutiveFailures = 0"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, outcome.find("state->backoffLevel = 0"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, outcome.find("state->cooldownUntil = 0"));

    const size_t controlStart = handler.find("OneShotResult ModbusHandler::sendControlRequest");
    const size_t nextWriteStart = handler.find("OneShotResult ModbusHandler::writeCoilOnce", controlStart);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, controlStart);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, nextWriteStart);
    const std::string control = handler.substr(controlStart, nextWriteStart - controlStart);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(
        std::string::npos, control.find("retriesOverride, true"),
        "control requests must explicitly bypass poll backoff");

    const size_t statsStart = handler.find("String ModbusHandler::getPollStatistics");
    const size_t cacheStart = handler.find(
        "const ModbusHandler::PollTaskCache* ModbusHandler::getTaskCache", statsStart);
    const std::string stats = handler.substr(statsStart, cacheStart - statsStart);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, stats.find("backoffSkips"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, stats.find("slaveHealth"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, stats.find("crcErrors"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, stats.find("exceptions"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, stats.find("halfOpenProbes"));
}

// ========== 测试组入口 ==========

void test_modbus_handler_group() {
    // CRC16
    RUN_TEST(test_crc16_known_frame);
    RUN_TEST(test_crc16_single_byte);
    RUN_TEST(test_crc16_all_zeros);
    RUN_TEST(test_crc16_all_ones);
    RUN_TEST(test_crc16_deterministic);
    RUN_TEST(test_crc16_different_data);
    RUN_TEST(test_crc16_frame_validation);
    
    // Data Hash
    RUN_TEST(test_data_hash_deterministic);
    RUN_TEST(test_data_hash_different_data);
    RUN_TEST(test_data_hash_empty);
    RUN_TEST(test_data_hash_order_matters);
    
    // Deadband
    RUN_TEST(test_deadband_within_threshold);
    RUN_TEST(test_deadband_exceeded);
    RUN_TEST(test_deadband_zero_old_value);
    RUN_TEST(test_deadband_zero_disabled);
    RUN_TEST(test_deadband_null_old_values);
    RUN_TEST(test_deadband_minimum_threshold);
    
    // 功能码分类
    RUN_TEST(test_read_function_codes);
    RUN_TEST(test_write_function_codes_not_read);
    RUN_TEST(test_invalid_function_codes);
    
    // ClampValue
    RUN_TEST(test_clamp_within_range);
    RUN_TEST(test_clamp_below_min);
    RUN_TEST(test_clamp_above_max);
    RUN_TEST(test_clamp_at_boundary);
    RUN_TEST(test_clamp_float);
    
    // 枚举
    RUN_TEST(test_modbus_exception_codes);
    RUN_TEST(test_oneshot_error_codes);
    
    // 结构体默认值
    RUN_TEST(test_subdevice_defaults);
    RUN_TEST(test_subdevice_motor_soft_limit_disabled);
    RUN_TEST(test_subdevice_motor_soft_limit_enabled);
    RUN_TEST(test_subdevice_motor_soft_limit_equal);
    RUN_TEST(test_subdevice_motor_new_fields_defaults);
    RUN_TEST(test_subdevice_motor_new_fields_assignment);
    
    // 寄存器值类型转换
    RUN_TEST(test_convert_uint16);
    RUN_TEST(test_convert_int16_negative);
    RUN_TEST(test_convert_int16_positive);
    RUN_TEST(test_convert_uint32);
    RUN_TEST(test_convert_int32_negative);
    RUN_TEST(test_convert_float32);
    RUN_TEST(test_convert_float32_zero);
    RUN_TEST(test_convert_float32_negative);
    
    // 从站地址
    RUN_TEST(test_valid_slave_addresses);
    RUN_TEST(test_invalid_slave_addresses);

    // MasterConfig 硬编码默认值
    RUN_TEST(test_master_config_default_response_timeout);
    RUN_TEST(test_master_config_default_max_retries);
    RUN_TEST(test_master_config_default_inter_poll_delay);
    RUN_TEST(test_master_config_defaults_in_valid_range);
    RUN_TEST(test_master_config_periph_exec_override_pattern);
    RUN_TEST(test_raw_poll_batch_uses_atomic_per_frame_production_path);
    RUN_TEST(test_raw_oneshot_read_uses_atomic_per_frame_production_path);
    RUN_TEST(test_raw_poll_preserves_uart_adu_without_json_suppression);
    RUN_TEST(test_rtu_frame_gap_uses_millisecond_units);
    RUN_TEST(test_per_slave_poll_backoff_isolation_contract);
}
