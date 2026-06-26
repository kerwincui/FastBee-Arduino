/**
 * @file test_driver_registry.cpp
 * @brief DriverRegistry 传感器驱动注册表单元测试
 *
 * 测试内容（基于 DriverRegistry.h）：
 * - 注册/查找/创建驱动
 * - 重名检测（大小写不敏感）
 * - 最大注册数 MAX_SENSOR_DRIVERS=8 限制
 * - 工厂函数 nullptr 防御
 * - 枚举驱动列表
 */

#include <unity.h>
#include <Arduino.h>
#include "helpers/TestLogger.h"
#include <cstring>

void test_driver_registry_group();

// ========== 镜像 DriverRegistry ==========
// 简化版，独立于 FastBee 命名空间，避免头文件依赖

namespace MirrorRegistry {

static constexpr size_t MAX_SENSOR_DRIVERS = 8;

struct MockSensorDriver {
    const char* name;
    int id;
};

using MockFactory = MockSensorDriver* (*)();

struct DriverEntry {
    const char* name;
    MockFactory factory;
};

class TestDriverRegistry {
public:
    void reset() {
        _count = 0;
        memset(_drivers, 0, sizeof(_drivers));
    }

    bool registerDriver(const char* name, MockFactory factory) {
        if (!name || !factory) return false;
        if (_count >= MAX_SENSOR_DRIVERS) return false;
        for (size_t i = 0; i < _count; i++) {
            if (equalsName(_drivers[i].name, name)) return false;
        }
        _drivers[_count++] = {name, factory};
        return true;
    }

    MockSensorDriver* createDriver(const char* name) const {
        if (!name) return nullptr;
        for (size_t i = 0; i < _count; i++) {
            if (equalsName(_drivers[i].name, name)) {
                return _drivers[i].factory();
            }
        }
        return nullptr;
    }

    size_t getDriverCount() const { return _count; }

    const DriverEntry* getDriverEntry(size_t index) const {
        return (index < _count) ? &_drivers[index] : nullptr;
    }

    bool hasDriver(const char* name) const {
        if (!name) return false;
        for (size_t i = 0; i < _count; i++) {
            if (equalsName(_drivers[i].name, name)) return true;
        }
        return false;
    }

private:
    static bool equalsName(const char* a, const char* b) {
        if (!a || !b) return false;
        while (*a && *b) {
            char ca = *a, cb = *b;
            if (ca >= 'A' && ca <= 'Z') ca = char(ca - 'A' + 'a');
            if (cb >= 'A' && cb <= 'Z') cb = char(cb - 'A' + 'a');
            if (ca != cb) return false;
            ++a; ++b;
        }
        return *a == '\0' && *b == '\0';
    }

    DriverEntry _drivers[MAX_SENSOR_DRIVERS] = {};
    size_t _count = 0;
};

// 测试用工厂函数
static MockSensorDriver* createSHT31() {
    static MockSensorDriver d{"SHT31", 1};
    return &d;
}

static MockSensorDriver* createBMP280() {
    static MockSensorDriver d{"BMP280", 2};
    return &d;
}

static MockSensorDriver* createBH1750() {
    static MockSensorDriver d{"BH1750", 3};
    return &d;
}

} // namespace MirrorRegistry

static MirrorRegistry::TestDriverRegistry registry;

// ============================================================
// TEST GROUP 1: 基本注册与查找
// ============================================================

static void test_register_single_driver() {
    registry.reset();
    TEST_ASSERT_TRUE(registry.registerDriver("SHT31", MirrorRegistry::createSHT31));
    TEST_ASSERT_EQUAL(1, (int)registry.getDriverCount());
}

static void test_register_multiple_drivers() {
    registry.reset();
    TEST_ASSERT_TRUE(registry.registerDriver("SHT31", MirrorRegistry::createSHT31));
    TEST_ASSERT_TRUE(registry.registerDriver("BMP280", MirrorRegistry::createBMP280));
    TEST_ASSERT_TRUE(registry.registerDriver("BH1750", MirrorRegistry::createBH1750));
    TEST_ASSERT_EQUAL(3, (int)registry.getDriverCount());
}

static void test_has_driver_found() {
    registry.reset();
    registry.registerDriver("SHT31", MirrorRegistry::createSHT31);
    TEST_ASSERT_TRUE(registry.hasDriver("SHT31"));
}

static void test_has_driver_not_found() {
    registry.reset();
    registry.registerDriver("SHT31", MirrorRegistry::createSHT31);
    TEST_ASSERT_FALSE(registry.hasDriver("DHT11"));
}

static void test_create_driver_returns_correct() {
    registry.reset();
    registry.registerDriver("SHT31", MirrorRegistry::createSHT31);
    auto* d = registry.createDriver("SHT31");
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_EQUAL_STRING("SHT31", d->name);
    TEST_ASSERT_EQUAL(1, d->id);
}

static void test_create_driver_unknown_returns_null() {
    registry.reset();
    registry.registerDriver("SHT31", MirrorRegistry::createSHT31);
    auto* d = registry.createDriver("UNKNOWN");
    TEST_ASSERT_NULL(d);
}

// ============================================================
// TEST GROUP 2: 重名检测（大小写不敏感）
// ============================================================

static void test_duplicate_exact_case_rejected() {
    registry.reset();
    TEST_ASSERT_TRUE(registry.registerDriver("SHT31", MirrorRegistry::createSHT31));
    TEST_ASSERT_FALSE(registry.registerDriver("SHT31", MirrorRegistry::createSHT31));
    TEST_ASSERT_EQUAL(1, (int)registry.getDriverCount());
}

static void test_duplicate_lower_case_rejected() {
    registry.reset();
    TEST_ASSERT_TRUE(registry.registerDriver("SHT31", MirrorRegistry::createSHT31));
    TEST_ASSERT_FALSE(registry.registerDriver("sht31", MirrorRegistry::createSHT31));
}

static void test_duplicate_mixed_case_rejected() {
    registry.reset();
    TEST_ASSERT_TRUE(registry.registerDriver("SHT31", MirrorRegistry::createSHT31));
    TEST_ASSERT_FALSE(registry.registerDriver("Sht31", MirrorRegistry::createSHT31));
}

static void test_has_driver_case_insensitive() {
    registry.reset();
    registry.registerDriver("BMP280", MirrorRegistry::createBMP280);
    TEST_ASSERT_TRUE(registry.hasDriver("bmp280"));
    TEST_ASSERT_TRUE(registry.hasDriver("BMP280"));
    TEST_ASSERT_TRUE(registry.hasDriver("Bmp280"));
}

static void test_create_driver_case_insensitive() {
    registry.reset();
    registry.registerDriver("BMP280", MirrorRegistry::createBMP280);
    auto* d = registry.createDriver("bmp280");
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_EQUAL_STRING("BMP280", d->name);
}

// ============================================================
// TEST GROUP 3: 最大注册数限制
// ============================================================

static void test_max_drivers_constant() {
    TEST_ASSERT_EQUAL(8, (int)MirrorRegistry::MAX_SENSOR_DRIVERS);
}

static void test_register_up_to_max() {
    registry.reset();
    const char* names[] = {"D1", "D2", "D3", "D4", "D5", "D6", "D7", "D8"};
    for (int i = 0; i < 8; i++) {
        TEST_ASSERT_TRUE_MESSAGE(
            registry.registerDriver(names[i], MirrorRegistry::createSHT31),
            "Should register within limit");
    }
    TEST_ASSERT_EQUAL(8, (int)registry.getDriverCount());
}

static void test_register_over_max_rejected() {
    registry.reset();
    const char* names[] = {"D1", "D2", "D3", "D4", "D5", "D6", "D7", "D8"};
    for (int i = 0; i < 8; i++) {
        registry.registerDriver(names[i], MirrorRegistry::createSHT31);
    }
    TEST_ASSERT_FALSE_MESSAGE(
        registry.registerDriver("D9", MirrorRegistry::createSHT31),
        "Should reject when registry is full");
    TEST_ASSERT_EQUAL(8, (int)registry.getDriverCount());
}

// ============================================================
// TEST GROUP 4: nullptr 防御
// ============================================================

static void test_register_null_name_rejected() {
    registry.reset();
    TEST_ASSERT_FALSE(registry.registerDriver(nullptr, MirrorRegistry::createSHT31));
    TEST_ASSERT_EQUAL(0, (int)registry.getDriverCount());
}

static void test_register_null_factory_rejected() {
    registry.reset();
    TEST_ASSERT_FALSE(registry.registerDriver("SHT31", nullptr));
    TEST_ASSERT_EQUAL(0, (int)registry.getDriverCount());
}

static void test_register_both_null_rejected() {
    registry.reset();
    TEST_ASSERT_FALSE(registry.registerDriver(nullptr, nullptr));
}

static void test_create_null_name_returns_null() {
    registry.reset();
    registry.registerDriver("SHT31", MirrorRegistry::createSHT31);
    auto* d = registry.createDriver(nullptr);
    TEST_ASSERT_NULL(d);
}

static void test_has_null_name_returns_false() {
    registry.reset();
    registry.registerDriver("SHT31", MirrorRegistry::createSHT31);
    TEST_ASSERT_FALSE(registry.hasDriver(nullptr));
}

// ============================================================
// TEST GROUP 5: 枚举驱动列表
// ============================================================

static void test_enumerate_empty_registry() {
    registry.reset();
    TEST_ASSERT_NULL(registry.getDriverEntry(0));
}

static void test_enumerate_registered_drivers() {
    registry.reset();
    registry.registerDriver("SHT31", MirrorRegistry::createSHT31);
    registry.registerDriver("BMP280", MirrorRegistry::createBMP280);

    const auto* e0 = registry.getDriverEntry(0);
    TEST_ASSERT_NOT_NULL(e0);
    TEST_ASSERT_EQUAL_STRING("SHT31", e0->name);

    const auto* e1 = registry.getDriverEntry(1);
    TEST_ASSERT_NOT_NULL(e1);
    TEST_ASSERT_EQUAL_STRING("BMP280", e1->name);
}

static void test_enumerate_out_of_bounds_returns_null() {
    registry.reset();
    registry.registerDriver("SHT31", MirrorRegistry::createSHT31);
    TEST_ASSERT_NULL(registry.getDriverEntry(1));
    TEST_ASSERT_NULL(registry.getDriverEntry(100));
}

static void test_enumerate_order_preserved() {
    registry.reset();
    registry.registerDriver("C_First", MirrorRegistry::createSHT31);
    registry.registerDriver("A_Second", MirrorRegistry::createBMP280);
    registry.registerDriver("B_Third", MirrorRegistry::createBH1750);

    // 枚举应保持注册顺序，不是字母序
    TEST_ASSERT_EQUAL_STRING("C_First", registry.getDriverEntry(0)->name);
    TEST_ASSERT_EQUAL_STRING("A_Second", registry.getDriverEntry(1)->name);
    TEST_ASSERT_EQUAL_STRING("B_Third", registry.getDriverEntry(2)->name);
}

// ============================================================
// 主入口
// ============================================================

void test_driver_registry_group() {
    TestLog::groupStart("DriverRegistry Tests");

    // 基本注册与查找
    RUN_TEST(test_register_single_driver);
    RUN_TEST(test_register_multiple_drivers);
    RUN_TEST(test_has_driver_found);
    RUN_TEST(test_has_driver_not_found);
    RUN_TEST(test_create_driver_returns_correct);
    RUN_TEST(test_create_driver_unknown_returns_null);

    // 重名检测
    RUN_TEST(test_duplicate_exact_case_rejected);
    RUN_TEST(test_duplicate_lower_case_rejected);
    RUN_TEST(test_duplicate_mixed_case_rejected);
    RUN_TEST(test_has_driver_case_insensitive);
    RUN_TEST(test_create_driver_case_insensitive);

    // 最大注册数
    RUN_TEST(test_max_drivers_constant);
    RUN_TEST(test_register_up_to_max);
    RUN_TEST(test_register_over_max_rejected);

    // nullptr 防御
    RUN_TEST(test_register_null_name_rejected);
    RUN_TEST(test_register_null_factory_rejected);
    RUN_TEST(test_register_both_null_rejected);
    RUN_TEST(test_create_null_name_returns_null);
    RUN_TEST(test_has_null_name_returns_false);

    // 枚举
    RUN_TEST(test_enumerate_empty_registry);
    RUN_TEST(test_enumerate_registered_drivers);
    RUN_TEST(test_enumerate_out_of_bounds_returns_null);
    RUN_TEST(test_enumerate_order_preserved);

    TestLog::groupEnd();
}
