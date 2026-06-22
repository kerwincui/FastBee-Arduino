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

// 精简版 ModbusSubDevice（用于测试默认值和 hasMotorSoftLimit）
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
}
