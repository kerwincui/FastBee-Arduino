/**
 * @file EthernetAdapter.cpp
 * @brief 以太网适配器实现 (W5500 SPI)
 * @author kerwincui
 * @date 2026-06-02
 *
 * 使用 ESP-IDF 底层 ethernet 驱动 API 初始化 W5500，
 * 兼容 Arduino-ESP32 Core 2.x / 3.x。
 */

#include "network/EthernetAdapter.h"

#if FASTBEE_ENABLE_ETHERNET

#include "systems/LoggerSystem.h"
#include <SPI.h>
#include <ETH.h>
#include <WiFi.h>

// ESP-IDF 底层以太网驱动 API（Core 2.x 回退路径需要）
#if !defined(ETH_SPI_SUPPORTS_CUSTOM)
#include "esp_eth.h"
#include "esp_eth_mac.h"
#include "esp_eth_phy.h"
#include "esp_event.h"
#include "driver/spi_master.h"
#endif

// 兼容 Arduino-ESP32 Core 2.x 和 3.x 的 SPI Host 定义
#if defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32C6)
  #ifndef SPI2_HOST
    #define SPI2_HOST FSPI
  #endif
#else  // ESP32 classic / C3
  #ifndef SPI2_HOST
    #define SPI2_HOST HSPI
  #endif
#endif

// 静态实例指针（用于事件回调）
EthernetAdapter* EthernetAdapter::_instance = nullptr;

EthernetAdapter::EthernetAdapter() {
    _instance = this;
}

EthernetAdapter::~EthernetAdapter() {
    disconnect();
    if (_spi) {
        _spi->end();
        delete _spi;
        _spi = nullptr;
    }
    _instance = nullptr;
}

bool EthernetAdapter::begin(const WiFiConfig& config) {
    if (_initialized) {
        LOG_WARNING("EthernetAdapter: Already initialized");
        return true;
    }

    _pinConfig = config.ethernet;

    LOG_INFO("EthernetAdapter: Initializing W5500 SPI Ethernet...");
    LOGGER.infof("EthernetAdapter: SPI pins - SCK:%d, MOSI:%d, MISO:%d",
                 _pinConfig.spiSck, _pinConfig.spiMosi, _pinConfig.spiMiso);
    LOGGER.infof("EthernetAdapter: Control pins - CS:%d, RST:%d, INT:%d",
                 _pinConfig.csPin, _pinConfig.rstPin, _pinConfig.intPin);

    // 注册以太网事件回调
    WiFi.onEvent(onEthEvent);

    // 初始化 SPI
    _spi = new SPIClass(SPI2_HOST);
    _spi->begin(_pinConfig.spiSck, _pinConfig.spiMiso, _pinConfig.spiMosi, _pinConfig.csPin);

    // 硬件复位 W5500
    if (_pinConfig.rstPin >= 0) {
        pinMode(_pinConfig.rstPin, OUTPUT);
        digitalWrite(_pinConfig.rstPin, LOW);
        delay(50);
        digitalWrite(_pinConfig.rstPin, HIGH);
        delay(200);
    }

    // 使用 ETH.begin() - 兼容不同版本的 Arduino Core
#if defined(ETH_SPI_SUPPORTS_CUSTOM)
    // Arduino-ESP32 Core 3.x+ (pioarduino)
    if (!ETH.begin(ETH_PHY_W5500, 0, _pinConfig.csPin, _pinConfig.intPin, _pinConfig.rstPin, *_spi)) {
        LOG_ERROR("EthernetAdapter: ETH.begin() failed");
        delete _spi;
        _spi = nullptr;
        return false;
    }
#else
    // Arduino-ESP32 Core 2.x - 使用 ESP-IDF 底层 API
    // 初始化 SPI 总线
    spi_bus_config_t buscfg = {};
    buscfg.mosi_io_num = _pinConfig.spiMosi;
    buscfg.miso_io_num = _pinConfig.spiMiso;
    buscfg.sclk_io_num = _pinConfig.spiSck;
    buscfg.quadwp_io_num = -1;
    buscfg.quadhd_io_num = -1;
    buscfg.max_transfer_sz = 4096;

    // 初始化 SPI 总线（如果已被 Arduino SPIClass 初始化则可能返回 ESP_ERR_INVALID_STATE）
    esp_err_t ret = spi_bus_initialize((spi_host_device_t)SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        LOG_ERROR("EthernetAdapter: SPI bus init failed");
        delete _spi;
        _spi = nullptr;
        return false;
    }

    // 添加 SPI 设备
    spi_device_interface_config_t devcfg = {};
    devcfg.command_bits = 16;   // W5500 地址段
    devcfg.address_bits = 8;    // W5500 控制段
    devcfg.mode = 0;
    devcfg.clock_speed_hz = 20000000; // 20 MHz
    devcfg.spics_io_num = _pinConfig.csPin;
    devcfg.queue_size = 20;

    spi_device_handle_t spi_handle = NULL;
    ret = spi_bus_add_device((spi_host_device_t)SPI2_HOST, &devcfg, &spi_handle);
    if (ret != ESP_OK) {
        LOG_ERROR("EthernetAdapter: SPI add device failed");
        delete _spi;
        _spi = nullptr;
        return false;
    }

    // 配置 W5500 MAC
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.reset_gpio_num = _pinConfig.rstPin;

    eth_w5500_config_t w5500_config = ETH_W5500_DEFAULT_CONFIG(spi_handle);
    w5500_config.int_gpio_num = _pinConfig.intPin;

    esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&w5500_config, &mac_config);
    if (!mac) {
        LOG_ERROR("EthernetAdapter: esp_eth_mac_new_w5500 failed");
        delete _spi;
        _spi = nullptr;
        return false;
    }

    esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&phy_config);
    if (!phy) {
        LOG_ERROR("EthernetAdapter: esp_eth_phy_new_w5500 failed");
        delete _spi;
        _spi = nullptr;
        return false;
    }

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth_handle = NULL;
    if (esp_eth_driver_install(&eth_config, &eth_handle) != ESP_OK) {
        LOG_ERROR("EthernetAdapter: esp_eth_driver_install failed");
        delete _spi;
        _spi = nullptr;
        return false;
    }

    // 将 eth_handle 附加到 TCP/IP 层 (netif)
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *eth_netif = esp_netif_new(&netif_cfg);
    esp_eth_netif_glue_handle_t glue = esp_eth_new_netif_glue(eth_handle);
    esp_netif_attach(eth_netif, glue);

    if (esp_eth_start(eth_handle) != ESP_OK) {
        LOG_ERROR("EthernetAdapter: esp_eth_start failed");
        delete _spi;
        _spi = nullptr;
        return false;
    }
#endif

    // 配置 IP（DHCP 或静态）
    if (config.ipConfigType == IPConfigType::STATIC &&
        !config.staticIP.isEmpty() && !config.gateway.isEmpty() && !config.subnet.isEmpty()) {
        IPAddress ip, gw, sn, dns1, dns2;
        if (ip.fromString(config.staticIP) && gw.fromString(config.gateway) && sn.fromString(config.subnet)) {
            dns1.fromString(config.dns1.isEmpty() ? "8.8.8.8" : config.dns1);
            dns2.fromString(config.dns2.isEmpty() ? "8.8.4.4" : config.dns2);
            ETH.config(ip, gw, sn, dns1, dns2);
            LOG_INFO("EthernetAdapter: Static IP configured: " + config.staticIP);
        }
    }

    _initialized = true;
    LOG_INFO("EthernetAdapter: Initialized successfully, waiting for link...");
    return true;
}

bool EthernetAdapter::waitForConnection(uint32_t timeoutMs) {
    if (!_initialized) return false;

    unsigned long start = millis();
    while (!_connected && (millis() - start < timeoutMs)) {
        delay(100);
    }

    if (_connected) {
        LOGGER.infof("EthernetAdapter: Connected! IP: %s", localIP().toString().c_str());
    } else {
        LOG_WARNING("EthernetAdapter: Connection timeout");
    }
    return _connected;
}

bool EthernetAdapter::isConnected() const {
    return _connected;
}

IPAddress EthernetAdapter::localIP() const {
    return ETH.localIP();
}

String EthernetAdapter::macAddress() const {
    return ETH.macAddress();
}

void EthernetAdapter::disconnect() {
    _connected = false;
    _initialized = false;
    LOG_INFO("EthernetAdapter: Disconnected");
}

Client* EthernetAdapter::getClient() {
    return &_ethClient;
}

void EthernetAdapter::update() {
    // ETH 事件由回调驱动
}

String EthernetAdapter::getStatusString() const {
    if (!_initialized) return "uninitialized";
    if (!_connected) return "disconnected";
    return "connected";
}

void EthernetAdapter::onEthEvent(arduino_event_id_t event, arduino_event_info_t info) {
    if (!_instance) return;

    switch (event) {
        case ARDUINO_EVENT_ETH_START:
            LOG_INFO("EthernetAdapter: ETH Started");
            break;
        case ARDUINO_EVENT_ETH_CONNECTED:
            LOG_INFO("EthernetAdapter: ETH Link Up");
            break;
        case ARDUINO_EVENT_ETH_GOT_IP:
            _instance->_connected = true;
            LOGGER.infof("EthernetAdapter: Got IP - %s",
                         ETH.localIP().toString().c_str());
            break;
        case ARDUINO_EVENT_ETH_DISCONNECTED:
            _instance->_connected = false;
            LOG_WARNING("EthernetAdapter: ETH Disconnected (was connected, link lost or DHCP lease expired)");
            break;
        default:
            break;
    }
}

#endif // FASTBEE_ENABLE_ETHERNET
