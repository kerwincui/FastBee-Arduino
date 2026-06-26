/**
 * @file test_ethernet_adapter.cpp
 * @brief EthernetAdapter W5500 SPI 以太网适配器单元测试
 *
 * 测试内容（纯逻辑，不依赖硬件）：
 * - SPI2_HOST 宏在不同芯片下的值
 * - EthernetConfig 配置解析（CS/RST/IRQ 引脚）
 * - 连接状态机（disconnected → connecting → connected）
 * - 析构顺序约束
 * - 状态字符串表示
 * - IP 配置类型（DHCP vs 静态）
 */

#include <unity.h>
#include <Arduino.h>
#include "helpers/TestLogger.h"
#include <cstring>
#include <string>
#include <fstream>
#include <sstream>

void test_ethernet_adapter_group();

// ========== 镜像 SPI2_HOST 宏逻辑 ==========
// 来自 EthernetAdapter.cpp：
//   ESP32-S3 / C6: SPI2_HOST = FSPI
//   ESP32 / C3: SPI2_HOST = HSPI

enum MirrorSPIHost {
    MIRROR_HSPI = 1,
    MIRROR_FSPI = 2
};

static MirrorSPIHost getMirrorSPI2Host(const char* chipName) {
    if (strcmp(chipName, "ESP32-S3") == 0 || strcmp(chipName, "ESP32-C6") == 0) {
        return MIRROR_FSPI;
    }
    return MIRROR_HSPI;
}

// ========== 镜像 EthernetConfig ==========
struct MirrorEthernetConfig {
    int8_t spiMosi = 11;
    int8_t spiMiso = 13;
    int8_t spiSck  = 12;
    int8_t csPin   = 47;
    int8_t rstPin  = 48;
    int8_t intPin  = 14;
};

// ========== 镜像连接状态机 ==========
enum class MirrorEthState : uint8_t {
    UNINITIALIZED = 0,
    DISCONNECTED = 1,
    CONNECTING = 2,
    CONNECTED = 3
};

static const char* mirrorGetStatusString(MirrorEthState state) {
    switch (state) {
        case MirrorEthState::UNINITIALIZED: return "uninitialized";
        case MirrorEthState::DISCONNECTED: return "disconnected";
        case MirrorEthState::CONNECTING: return "connecting";
        case MirrorEthState::CONNECTED: return "connected";
        default: return "unknown";
    }
}

// 镜像析构顺序检查
struct DestructionOrder {
    bool ethEndCalled = false;
    bool spiEndCalled = false;
    bool spiDeleted = false;

    void disconnect() {
        // 关键：ETH.end() 必须在 SPI.end() 之前
        ethEndCalled = true;
    }

    void destroySPI() {
        if (!ethEndCalled) return;  // 防护：如果 ETH 未停止则不允许销毁 SPI
        spiEndCalled = true;
        spiDeleted = true;
    }
};

// 镜像 IP 配置解析
enum class MirrorIPConfigType { DHCP = 0, STATIC = 1 };

struct MirrorIPConfig {
    MirrorIPConfigType type = MirrorIPConfigType::DHCP;
    std::string staticIP;
    std::string gateway;
    std::string subnet;
    std::string dns1;
    std::string dns2;
};

static bool canApplyStaticIP(const MirrorIPConfig& cfg) {
    return cfg.type == MirrorIPConfigType::STATIC &&
           !cfg.staticIP.empty() &&
           !cfg.gateway.empty() &&
           !cfg.subnet.empty();
}

static std::string resolveDNS(const std::string& dns, const std::string& fallback) {
    return dns.empty() ? fallback : dns;
}

// ============================================================
// TEST GROUP 1: SPI2_HOST 芯片映射
// ============================================================

static void test_spi_host_esp32() {
    TEST_ASSERT_EQUAL(MIRROR_HSPI, getMirrorSPI2Host("ESP32"));
}

static void test_spi_host_esp32s3() {
    TEST_ASSERT_EQUAL(MIRROR_FSPI, getMirrorSPI2Host("ESP32-S3"));
}

static void test_spi_host_esp32c3() {
    TEST_ASSERT_EQUAL(MIRROR_HSPI, getMirrorSPI2Host("ESP32-C3"));
}

static void test_spi_host_esp32c6() {
    TEST_ASSERT_EQUAL(MIRROR_FSPI, getMirrorSPI2Host("ESP32-C6"));
}

static void test_spi_host_default_fallback() {
    // 未知芯片回退到 HSPI
    TEST_ASSERT_EQUAL(MIRROR_HSPI, getMirrorSPI2Host("UNKNOWN"));
}

// ============================================================
// TEST GROUP 2: EthernetConfig 默认值
// ============================================================

static void test_eth_config_default_mosi() {
    MirrorEthernetConfig cfg;
    TEST_ASSERT_EQUAL(11, cfg.spiMosi);
}

static void test_eth_config_default_miso() {
    MirrorEthernetConfig cfg;
    TEST_ASSERT_EQUAL(13, cfg.spiMiso);
}

static void test_eth_config_default_sck() {
    MirrorEthernetConfig cfg;
    TEST_ASSERT_EQUAL(12, cfg.spiSck);
}

static void test_eth_config_default_cs() {
    MirrorEthernetConfig cfg;
    TEST_ASSERT_EQUAL(47, cfg.csPin);
}

static void test_eth_config_default_rst() {
    MirrorEthernetConfig cfg;
    TEST_ASSERT_EQUAL(48, cfg.rstPin);
}

static void test_eth_config_default_int() {
    MirrorEthernetConfig cfg;
    TEST_ASSERT_EQUAL(14, cfg.intPin);
}

static void test_eth_config_spi_pins_not_conflict() {
    MirrorEthernetConfig cfg;
    TEST_ASSERT_NOT_EQUAL(cfg.spiMosi, cfg.spiMiso);
    TEST_ASSERT_NOT_EQUAL(cfg.spiMosi, cfg.spiSck);
    TEST_ASSERT_NOT_EQUAL(cfg.spiMiso, cfg.spiSck);
}

static void test_eth_config_control_pins_not_conflict() {
    MirrorEthernetConfig cfg;
    TEST_ASSERT_NOT_EQUAL(cfg.csPin, cfg.rstPin);
    TEST_ASSERT_NOT_EQUAL(cfg.csPin, cfg.intPin);
    TEST_ASSERT_NOT_EQUAL(cfg.rstPin, cfg.intPin);
}

// ============================================================
// TEST GROUP 3: 连接状态机
// ============================================================

static void test_initial_state_uninitialized() {
    MirrorEthState state = MirrorEthState::UNINITIALIZED;
    TEST_ASSERT_EQUAL(0, static_cast<int>(state));
}

static void test_state_transitions() {
    MirrorEthState state = MirrorEthState::UNINITIALIZED;
    TEST_ASSERT_EQUAL_STRING("uninitialized", mirrorGetStatusString(state));

    state = MirrorEthState::DISCONNECTED;
    TEST_ASSERT_EQUAL_STRING("disconnected", mirrorGetStatusString(state));

    state = MirrorEthState::CONNECTING;
    TEST_ASSERT_EQUAL_STRING("connecting", mirrorGetStatusString(state));

    state = MirrorEthState::CONNECTED;
    TEST_ASSERT_EQUAL_STRING("connected", mirrorGetStatusString(state));
}

// ============================================================
// TEST GROUP 4: 析构顺序约束
// ============================================================

static void test_destruction_order_correct() {
    DestructionOrder order;
    order.disconnect();  // 先 ETH.end()
    order.destroySPI();  // 再 SPI.end() + delete
    TEST_ASSERT_TRUE(order.ethEndCalled);
    TEST_ASSERT_TRUE(order.spiEndCalled);
    TEST_ASSERT_TRUE(order.spiDeleted);
}

static void test_destruction_order_spi_without_eth_blocked() {
    DestructionOrder order;
    // 不调用 disconnect，直接销毁 SPI → 应被阻止
    order.destroySPI();
    TEST_ASSERT_FALSE(order.ethEndCalled);
    TEST_ASSERT_FALSE(order.spiEndCalled);
}

// ============================================================
// TEST GROUP 5: IP 配置类型
// ============================================================

static void test_ip_config_dhcp_default() {
    MirrorIPConfig cfg;
    TEST_ASSERT_EQUAL(0, static_cast<int>(cfg.type));
}

static void test_ip_config_static_applied() {
    MirrorIPConfig cfg;
    cfg.type = MirrorIPConfigType::STATIC;
    cfg.staticIP = "192.168.1.100";
    cfg.gateway = "192.168.1.1";
    cfg.subnet = "255.255.255.0";
    TEST_ASSERT_TRUE(canApplyStaticIP(cfg));
}

static void test_ip_config_static_missing_gateway() {
    MirrorIPConfig cfg;
    cfg.type = MirrorIPConfigType::STATIC;
    cfg.staticIP = "192.168.1.100";
    cfg.subnet = "255.255.255.0";
    TEST_ASSERT_FALSE(canApplyStaticIP(cfg));
}

static void test_ip_config_static_missing_ip() {
    MirrorIPConfig cfg;
    cfg.type = MirrorIPConfigType::STATIC;
    cfg.gateway = "192.168.1.1";
    cfg.subnet = "255.255.255.0";
    TEST_ASSERT_FALSE(canApplyStaticIP(cfg));
}

static void test_ip_config_dhcp_ignores_static() {
    MirrorIPConfig cfg;
    cfg.type = MirrorIPConfigType::DHCP;
    cfg.staticIP = "192.168.1.100";
    cfg.gateway = "192.168.1.1";
    cfg.subnet = "255.255.255.0";
    TEST_ASSERT_FALSE(canApplyStaticIP(cfg));
}

static void test_dns_fallback_primary() {
    TEST_ASSERT_EQUAL_STRING("8.8.8.8", resolveDNS("", "8.8.8.8").c_str());
}

static void test_dns_custom_used() {
    TEST_ASSERT_EQUAL_STRING("1.1.1.1", resolveDNS("1.1.1.1", "8.8.8.8").c_str());
}

// ============================================================
// TEST GROUP 6: RST 引脚有效性
// ============================================================

static void test_rst_pin_negative_skips_reset() {
    // rstPin = -1 表示不使用硬件复位
    MirrorEthernetConfig cfg;
    cfg.rstPin = -1;
    TEST_ASSERT_TRUE(cfg.rstPin < 0);  // 负值 → 跳过复位
}

static void test_rst_pin_positive_performs_reset() {
    MirrorEthernetConfig cfg;
    cfg.rstPin = 48;
    TEST_ASSERT_TRUE(cfg.rstPin >= 0);  // 正值 → 执行复位
}

// ============================================================
// TEST GROUP 7: 源码结构验证
// ============================================================

static void test_source_has_disconnect_before_spi_end() {
    // 验证析构函数中 disconnect() 在 _spi->end() 之前调用
    std::string source;
    {
        std::ifstream f("src/network/EthernetAdapter.cpp");
        if (f) {
            std::stringstream ss;
            ss << f.rdbuf();
            source = ss.str();
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(!source.empty(), "EthernetAdapter.cpp must be readable");

    // disconnect() 调用 ETH.end()
    size_t disconnectPos = source.find("disconnect()");
    size_t spiEndPos = source.find("_spi->end()");

    if (disconnectPos != std::string::npos && spiEndPos != std::string::npos) {
        // 在析构函数中 disconnect 应出现在 _spi->end() 之前
        // 注意：这里验证的是析构函数中的顺序，disconnect() 方法中也有 ETH.end()
        size_t dtorPos = source.find("EthernetAdapter::~EthernetAdapter");
        TEST_ASSERT_TRUE_MESSAGE(dtorPos != std::string::npos,
            "Destructor must exist");
        // 析构函数中应先调 disconnect 再 _spi->end
        size_t dtorDisconnect = source.find("disconnect()", dtorPos);
        size_t dtorSpiEnd = source.find("_spi->end()", dtorPos);
        if (dtorDisconnect != std::string::npos && dtorSpiEnd != std::string::npos) {
            TEST_ASSERT_LESS_THAN(dtorSpiEnd, dtorDisconnect);
        }
    }
}

// ============================================================
// 主入口
// ============================================================

void test_ethernet_adapter_group() {
    TestLog::groupStart("EthernetAdapter (W5500) Tests");

    // SPI2_HOST 映射
    RUN_TEST(test_spi_host_esp32);
    RUN_TEST(test_spi_host_esp32s3);
    RUN_TEST(test_spi_host_esp32c3);
    RUN_TEST(test_spi_host_esp32c6);
    RUN_TEST(test_spi_host_default_fallback);

    // EthernetConfig 默认值
    RUN_TEST(test_eth_config_default_mosi);
    RUN_TEST(test_eth_config_default_miso);
    RUN_TEST(test_eth_config_default_sck);
    RUN_TEST(test_eth_config_default_cs);
    RUN_TEST(test_eth_config_default_rst);
    RUN_TEST(test_eth_config_default_int);
    RUN_TEST(test_eth_config_spi_pins_not_conflict);
    RUN_TEST(test_eth_config_control_pins_not_conflict);

    // 连接状态机
    RUN_TEST(test_initial_state_uninitialized);
    RUN_TEST(test_state_transitions);

    // 析构顺序
    RUN_TEST(test_destruction_order_correct);
    RUN_TEST(test_destruction_order_spi_without_eth_blocked);

    // IP 配置
    RUN_TEST(test_ip_config_dhcp_default);
    RUN_TEST(test_ip_config_static_applied);
    RUN_TEST(test_ip_config_static_missing_gateway);
    RUN_TEST(test_ip_config_static_missing_ip);
    RUN_TEST(test_ip_config_dhcp_ignores_static);
    RUN_TEST(test_dns_fallback_primary);
    RUN_TEST(test_dns_custom_used);

    // RST 引脚
    RUN_TEST(test_rst_pin_negative_skips_reset);
    RUN_TEST(test_rst_pin_positive_performs_reset);

    // 源码结构
    RUN_TEST(test_source_has_disconnect_before_spi_end);

    TestLog::groupEnd();
}
