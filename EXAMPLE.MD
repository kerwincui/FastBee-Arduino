### 网络管理器示例
```
// 初始化网络管理器
NetworkManager networkManager(&server, &logger);
networkManager.initialize();

// 设置回调
networkManager.setConnectionCallback([](NetworkStatus status, const String& message) {
    Serial.println("Connected: " + message);
});

// 在主循环中更新
void loop() {
    networkManager.update();
    
    // 获取状态信息
    NetworkStatusInfo status = networkManager.getStatusInfo();
    if (status.status == NetworkStatus::CONNECTED) {
        // 执行网络操作
    }
}
```

### modbus使用示例
```
#include "protocols/ModbusHandler.h"

ModbusHandler modbus;

void setup() {
    Serial.begin(115200);
    
    // 方法1：从文件加载配置
    if (!modbus.begin()) {
        Serial.println("Failed to initialize Modbus from config file");
        return;
    }
    
    // 方法2：使用硬编码配置
    /*
    ModbusConfig config;
    config.slaveAddress = 1;
    config.baudRate = 9600;
    config.txPin = 17;
    config.rxPin = 16;
    config.dePin = 4;  // RS485方向控制
    
    if (!modbus.begin(config)) {
        Serial.println("Failed to initialize Modbus");
        return;
    }
    */
    
    // 设置回调函数
    modbus.setDataCallback([](uint8_t slaveAddr, const String& data) {
        Serial.printf("Modbus data received from slave %d: %s\n", slaveAddr, data.c_str());
    });
    
    modbus.setRegisterReadCallback([](uint16_t address) -> uint16_t {
        // 自定义寄存器读取逻辑
        return address * 2; // 示例：返回地址的两倍
    });
    
    modbus.setRegisterWriteCallback([](uint16_t address, uint16_t value) {
        // 自定义寄存器写入逻辑
        Serial.printf("Register %d written with value %d\n", address, value);
    });
    
    Serial.println(modbus.getStatus());
}

void loop() {
    modbus.handle();
    
    // 示例：写入数据到寄存器
    static unsigned long lastWrite = 0;
    if (millis() - lastWrite > 5000) {
        modbus.writeData(0, "Hello Modbus!");
        lastWrite = millis();
    }
    
    delay(10);
}
```



### ProtocolManager使用示例
```
#include <Arduino.h>
#include <WiFi.h>
#include "Protocols/ProtocolManager.h"

// WiFi配置
const char* ssid = "your_wifi_ssid";
const char* password = "your_wifi_password";

// 协议管理器实例
ProtocolManager protocolManager;

void setup() {
    Serial.begin(115200);
    
    // 连接WiFi
    WiFi.begin(ssid, password);
    Serial.print("Connecting to WiFi");
    while (WiFi.status() != WL_CONNECTED) {
        delay(1000);
        Serial.print(".");
    }
    Serial.println("\nConnected to WiFi");
    
    // 初始化协议管理器
    protocolManager.begin();
    
    // 添加协议
    protocolManager.addProtocol(ProtocolType::MQTT, "MQTT Client");
    protocolManager.addProtocol(ProtocolType::HTTP, "HTTP Client");
    
    // MQTT配置
    MQTTConfig mqttConfig;
    mqttConfig.server = "broker.hivemq.com";
    mqttConfig.port = 1883;
    mqttConfig.clientId = "esp32-client-" + String(random(0xffff), HEX);
    mqttConfig.username = "";
    mqttConfig.password = "";
    mqttConfig.topicPrefix = "/esp32/";
    mqttConfig.keepAlive = 60;
    
    // HTTP配置
    HTTPConfig httpConfig;
    httpConfig.baseURL = "http://httpbin.org";
    httpConfig.timeout = 10000;
    httpConfig.contentType = "application/json";
    
    // 设置协议配置
    protocolManager.setProtocolConfig(ProtocolType::MQTT, &mqttConfig);
    protocolManager.setProtocolConfig(ProtocolType::HTTP, &httpConfig);
    
    // 设置消息回调
    protocolManager.setMessageCallback([](ProtocolType type, const String& topic, const String& message) {
        Serial.println("Received message - Protocol: " + String((int)type) + 
                      ", Topic: " + topic + ", Message: " + message);
    });
    
    // 启动所有协议
    if (protocolManager.startAll()) {
        Serial.println("All protocols started successfully");
    } else {
        Serial.println("Some protocols failed to start");
    }
    
    // 订阅MQTT主题
    protocolManager.sendData(ProtocolType::MQTT, "subscribe", "test/topic");
}

void loop() {
    // 处理协议通信
    protocolManager.handle();
    
    // 示例：每隔30秒发送数据
    static unsigned long lastSend = 0;
    if (millis() - lastSend > 30000) {
        lastSend = millis();
        
        // 发送MQTT数据
        String data = "{\"timestamp\": " + String(millis()) + ", \"value\": " + String(random(100)) + "}";
        protocolManager.sendData(ProtocolType::MQTT, "sensor/data", data);
        
        // 发送HTTP数据
        protocolManager.sendData(ProtocolType::HTTP, "post", data);
        
        // 打印协议状态
        Serial.println("MQTT Status: " + protocolManager.getProtocolStatus(ProtocolType::MQTT));
        Serial.println("HTTP Status: " + protocolManager.getProtocolStatus(ProtocolType::HTTP));
    }
    
    delay(100);
}

```


### GPIO示例
```
#include "core/GPIOManager.h"
#include "config/gpio_config.h"

void setup() {
    Serial.begin(115200);
    
    // 初始化GPIO管理器
    if (!GPIOManager::getInstance().initialize()) {
        Serial.println("GPIO Manager initialization failed!");
        return;
    }
    
    // 打印GPIO状态
    GPIOManager::getInstance().printStatus();
}

void loop() {
    // 读取按钮状态
    GPIOState buttonState = GPIOManager::getInstance().readPin("USER_BUTTON");
    
    // 控制LED
    if (buttonState == GPIOState::LOW) { // 按钮按下（上拉电阻）
        GPIOManager::getInstance().writePin("SYSTEM_LED", GPIOState::HIGH);
    } else {
        GPIOManager::getInstance().writePin("SYSTEM_LED", GPIOState::LOW);
    }
    
    // 读取模拟传感器
    uint16_t tempValue = GPIOManager::getInstance().readAnalog("TEMP_SENSOR");
    uint16_t humidityValue = GPIOManager::getInstance().readAnalog("HUMIDITY_SENSOR");
    
    delay(100);
}
```

##### 高级功能
```
// 中断回调函数
void buttonInterruptCallback(uint8_t pin, GPIOState state) {
    Serial.printf("Button interrupt! Pin: %d, State: %d\n", pin, static_cast<int>(state));
    
    // 切换继电器状态
    static bool relayState = false;
    relayState = !relayState;
    
    GPIOManager::getInstance().writePin("RELAY_1", 
        relayState ? GPIOState::HIGH : GPIOState::LOW);
}

void setupAdvanced() {
    // 配置中断
    GPIOManager::getInstance().attachInterrupt(
        "USER_BUTTON", 
        GPIOMode::INTERRUPT_FALLING, 
        buttonInterruptCallback
    );
    
    // PWM控制
    GPIOConfig pwmConfig = {
        .pin = 12,
        .name = "PWM_LED",
        .mode = GPIOMode::PWM_OUTPUT,
        .pwmChannel = 1,
        .pwmFrequency = 5000,
        .pwmResolution = 12
    };
    
    GPIOManager::getInstance().configurePin(pwmConfig);
    
    // 渐变PWM
    for (int duty = 0; duty < 4096; duty += 16) {
        GPIOManager::getInstance().writePWM("PWM_LED", duty);
        delay(10);
    }
}
```

##### 特定
* 类型安全：使用枚举类避免魔法数字
* 集中管理：所有GPIO配置集中管理
* 运行时验证：引脚和模式有效性检查
* 易于扩展：支持新功能添加
* 调试友好：详细的日志输出
* 企业级特性：错误处理、状态管理、持久化支持


### TCP 
```
TCPHandler tcpHandler;

// 方法1：直接从配置文件初始化
if (tcpHandler.beginFromConfig("/tcp_config.json")) {
    tcpHandler.setMessageCallback([](const String& message) {
        Serial.println("Received: " + message);
    });
}

// 方法2：手动加载配置后初始化
if (tcpHandler.loadConfigFromLittleFS("/tcp_config.json")) {
    tcpHandler.begin(tcpHandler.getConfig());
}

void loop() {
    tcpHandler.handle();
}
```

### HTTP
```
HTTPClientWrapper httpClient;

// 方法1：直接从配置文件初始化
if (httpClient.beginFromConfig("/http_config.json")) {
    httpClient.setResponseCallback([](const String& endpoint, const String& response) {
        Serial.println("Response from " + endpoint + ": " + response);
    });
}

// 方法2：手动加载配置后初始化
if (httpClient.loadConfigFromLittleFS("/http_config.json")) {
    httpClient.begin(httpClient.getConfig());
}

// 使用示例
String response;
if (httpClient.get("/data", response)) {
    Serial.println("GET response: " + response);
}

if (httpClient.post("/data", "{\"value\": 123}")) {
    Serial.println("POST successful");
}
```

### COAP
```
CoAPHandler coapHandler;

// 方法1：直接从配置文件初始化
if (coapHandler.beginFromConfig("/coap_config.json")) {
    coapHandler.setMessageCallback([](const String& type, const String& message) {
        Serial.println("CoAP " + type + ": " + message);
    });
}

// 方法2：手动加载配置后初始化
if (coapHandler.loadConfigFromLittleFS("/coap_config.json")) {
    coapHandler.begin(coapHandler.getConfig());
}

void loop() {
    coapHandler.handle();
    
    // 发送示例
    static unsigned long lastSend = 0;
    if (millis() - lastSend > 10000) {
        // 使用资源映射
        coapHandler.send("temperature", "25.5");
        
        // 或者使用完整路径
        coapHandler.post("sensors/data", "{\"temp\": 25.5, \"hum\": 60}");
        
        lastSend = millis();
    }
}
```


### 单例模式日志模块
```
// 其他模块中使用日志系统
#include "systems/LoggerSystem.h"

void setup() {
    // 初始化日志系统
    LoggerSystem::getInstance().initialize();
    LoggerSystem::getInstance().setLogLevel(LOG_DEBUG);
    LoggerSystem::getInstance().enableFileLogging(true);
    
    // 或者使用宏定义（更简洁）
    LOGGER.initialize();
    LOGGER.setLogLevel(LOG_DEBUG);
    
    // 记录日志
    LoggerSystem::getInstance().info("System started", "Main");
    LoggerSystem::getInstance().error("Something went wrong", "Network");
    
    // 使用宏定义
    LOG_INFO("System started");
    LOG_ERROR("Something went wrong");
    
    // 格式化日志
    LoggerSystem::getInstance().infof("Device ID: %s, Free heap: %d", 
                                       "ESP32-001", ESP.getFreeHeap());
    
    // 或者
    LOGGER.infof("Temperature: %.2f°C", 23.45);
}

void loop() {
    // 模块中使用
    static int counter = 0;
    LOGGER.debugf("Counter: %d", counter++);
    delay(1000);
}
```

### 单例模式 配置存储
```
// 在main.cpp或setup函数中初始化
#include "systems/ConfigStorage.h"

void setup() {
    Serial.begin(115200);
    
    // 初始化配置存储
    if (!ConfigStorage::initialize()) {
        Serial.println("配置存储初始化失败!");
        return;
    }
    
    // 使用配置存储
    ConfigStorage& config = ConfigStorage::getInstance();
    
    // 保存配置
    config.saveString("wifi_ssid", "MyWiFi");
    config.saveInt("brightness", 80);
    
    // 读取配置
    String ssid = config.readString("wifi_ssid", "default");
    int brightness = config.readInt("brightness", 50);
    
    // 使用JSON配置
    StaticJsonDocument<512> doc;
    doc["device_name"] = "ESP32_Device";
    doc["version"] = "1.0.0";
    config.saveSystemConfig(doc);
    
    // 加载JSON配置
    StaticJsonDocument<512> loadDoc;
    if (config.loadSystemConfig(loadDoc)) {
        String deviceName = loadDoc["device_name"];
        Serial.printf("设备名称: %s\n", deviceName.c_str());
    }
}

// 在网络模块中使用
#include "systems/ConfigStorage.h"

class NetworkManager {
public:
    void loadNetworkSettings() {
        StaticJsonDocument<1024> doc;
        if (ConfigStorage::getInstance().loadNetworkConfig(doc)) {
            String ssid = doc["ssid"];
            String password = doc["password"];
            // 使用配置连接WiFi
        }
    }
    
    void saveNetworkSettings(const String& ssid, const String& password) {
        StaticJsonDocument<1024> doc;
        doc["ssid"] = ssid;
        doc["password"] = password;
        ConfigStorage::getInstance().saveNetworkConfig(doc);
    }
};

// 在用户管理模块中使用
#include "systems/ConfigStorage.h"

class UserManager {
public:
    bool authenticateUser(const String& username, const String& password) {
        StaticJsonDocument<2048> doc;
        if (ConfigStorage::getInstance().loadUserConfig(doc)) {
            JsonArray users = doc["users"];
            for (JsonObject user : users) {
                if (user["username"] == username && user["password"] == password) {
                    return true;
                }
            }
        }
        return false;
    }
};
```


### 字符串处理静态类
```
// main.cpp 或其他模块中
#include "utils/StringUtils.h"

void setup() {
    Serial.begin(115200);
    
    // 1. 字符串分割和连接
    String csvData = "apple,banana,orange,grape";
    std::vector<String> fruits = StringUtils::split(csvData, ',');
    Serial.print("分割结果: ");
    for (const auto& fruit : fruits) {
        Serial.print(fruit + " ");
    }
    Serial.println();
    
    // 重新连接
    String recombined = StringUtils::join(fruits, " | ");
    Serial.println("重新连接: " + recombined);
    
    // 2. 字符串修剪
    String dirtyString = "  Hello World!  ";
    String cleanString = StringUtils::trim(dirtyString);
    Serial.println("修剪后: '" + cleanString + "'");
    
    // 3. 大小写转换
    String mixedCase = "Hello World";
    Serial.println("小写: " + StringUtils::toLower(mixedCase));
    Serial.println("大写: " + StringUtils::toUpper(mixedCase));
    
    // 4. 字符串匹配
    String filename = "config.json";
    bool isJson = StringUtils::endsWith(filename, ".json");
    bool containsConfig = StringUtils::contains(filename, "config");
    Serial.println("是JSON文件: " + String(isJson));
    Serial.println("包含'config': " + String(containsConfig));
    
    // 5. 类型转换
    String numberStr = "123";
    int num = StringUtils::toInt(numberStr);
    float piStr = StringUtils::toFloat("3.14159");
    bool boolValue = StringUtils::toBool("true");
    Serial.printf("转换结果: int=%d, float=%.2f, bool=%d\n", num, piStr, boolValue);
    
    // 6. 字符串替换
    String text = "Hello World, Hello Universe";
    String replaced = StringUtils::replaceAll(text, "Hello", "Hi");
    Serial.println("替换后: " + replaced);
    
    // 7. URL编码/解码
    String url = "https://example.com/search?q=hello world";
    String encoded = StringUtils::urlEncode(url);
    String decoded = StringUtils::urlDecode(encoded);
    Serial.println("URL编码: " + encoded);
    Serial.println("URL解码: " + decoded);
    
    // 8. 字符串格式化
    String formatted = StringUtils::format("设备: %s, 温度: %.1f°C, 状态: %s", 
                                          "ESP32", 25.5, "正常");
    Serial.println("格式化: " + formatted);
    
    // 9. 哈希计算
    String password = "secret123";
    String hash = StringUtils::md5(password);
    Serial.println("MD5哈希: " + hash);
    
    // 10. 命名约定转换
    String snakeCase = "hello_world_example";
    String camelCase = StringUtils::toCamelCase(snakeCase, true);
    Serial.println("蛇形转驼峰: " + camelCase);
}

// 网络模块中使用
#include "utils/StringUtils.h"

class HttpRequest {
public:
    static String buildQueryString(const std::vector<std::pair<String, String>>& params) {
        std::vector<String> pairs;
        for (const auto& param : params) {
            String encodedKey = StringUtils::urlEncode(param.first);
            String encodedValue = StringUtils::urlEncode(param.second);
            pairs.push_back(encodedKey + "=" + encodedValue);
        }
        return StringUtils::join(pairs, "&");
    }
};

// JSON处理模块中使用
#include "utils/StringUtils.h"
#include <ArduinoJson.h>

class JsonParser {
public:
    static DynamicJsonDocument parseJson(const String& jsonString) {
        DynamicJsonDocument doc(1024);
        // 使用StringUtils清理JSON字符串
        String cleanJson = StringUtils::trim(jsonString);
        deserializeJson(doc, cleanJson);
        return doc;
    }
    
    static String toJsonString(const JsonDocument& doc) {
        String json;
        serializeJson(doc, json);
        return StringUtils::jsonEscape(json);
    }
};

// 配置文件解析
#include "utils/StringUtils.h"

class ConfigParser {
public:
    static std::map<String, String> parseKeyValue(const String& configText) {
        std::map<String, String> config;
        std::vector<String> lines = StringUtils::split(configText, '\n');
        
        for (const String& line : lines) {
            String trimmed = StringUtils::trim(line);
            if (trimmed.isEmpty() || trimmed.startsWith("#")) {
                continue; // 跳过空行和注释
            }
            
            std::vector<String> parts = StringUtils::split(trimmed, '=', 2);
            if (parts.size() == 2) {
                String key = StringUtils::trim(parts[0]);
                String value = StringUtils::trim(parts[1]);
                config[key] = value;
            }
        }
        
        return config;
    }
};

// 日志模块中使用
#include "utils/StringUtils.h"

class Logger {
public:
    void log(const String& level, const String& message, const String& source = "") {
        String timestamp = String(millis());
        String logEntry;
        
        if (!source.isEmpty()) {
            logEntry = StringUtils::format("[%s] [%s] [%s] %s",
                                          timestamp.c_str(),
                                          level.c_str(),
                                          source.c_str(),
                                          message.c_str());
        } else {
            logEntry = StringUtils::format("[%s] [%s] %s",
                                          timestamp.c_str(),
                                          level.c_str(),
                                          message.c_str());
        }
        
        // 输出到串口
        Serial.println(logEntry);
        
        // 写入文件
        FileUtils::appendToFile("/logs/app.log", logEntry + "\n");
    }
    
    void info(const String& message, const String& source = "") {
        log("INFO", message, source);
    }
    
    void error(const String& message, const String& source = "") {
        log("ERROR", message, source);
    }
    
    void debug(const String& message, const String& source = "") {
        #ifdef DEBUG
        log("DEBUG", message, source);
        #endif
    }
};
```

### AuthManager使用示例
```
#include "AuthManager.h"

// 创建实例
AuthManager authManager;

void setup() {
    // 初始化认证管理器
    if (!authManager.initialize()) {
        Serial.println("Failed to initialize auth manager!");
        return;
    }
    
    // 设置认证事件回调
    authManager.setAuthEventCallback([](const String& username, const String& event, const String& details) {
        Serial.println("Auth Event: " + username + " - " + event + " - " + details);
    });
}

void handleLogin() {
    // 用户登录
    AuthResult result = authManager.authenticate("admin", "password", "192.168.1.100", "Web Browser");
    
    if (result.success) {
        Serial.println("Login successful! Session: " + result.sessionId);
        
        // 保存会话令牌
        String sessionToken = result.sessionId;
        
        // 验证会话
        AuthResult verifyResult = authManager.verifySession(sessionToken);
        if (verifyResult.success) {
            Serial.println("Session is valid");
            
            // 检查权限
            if (authManager.checkSessionPermission(sessionToken, "system_config")) {
                Serial.println("User has system configuration permission");
            }
        }
        
        // 用户登出
        authManager.logout(sessionToken);
    } else {
        Serial.println("Login failed: " + result.message);
    }
}
```

* 完整的认证流程：用户登录、会话管理、权限验证
* 安全防护：暴力破解防护、账户锁定、会话超时
* 基于角色的访问控制 (RBAC)：细粒度的权限管理
* 会话管理：多会话支持、会话状态监控
* 安全配置：可配置的安全策略
* 审计日志：完整的认证事件记录


### 命名空间示例
```
// 使用网络配置
// WiFi.softAP(Network::DEFAULT_AP_SSID, Network::DEFAULT_AP_PASSWORD);
    
// 使用文件系统路径
// String configPath = FileSystem::SYSTEM_CONFIG_FILE;
```


### 文件系统
```
// 计算目录大小
size_t wwwSize = FileUtils::calculateFolderSize("/www");

// 读取配置文件
String config = FileUtils::readFile("/config/settings.json");

// 列出目录内容
std::vector<FileInfo> files = FileUtils::listDirectory("/www", true);

// 读取配置文件
String config = FileUtils::readFile("/config/settings.json");
    
// 检查目录大小
size_t wwwSize = FileUtils::getDirectorySize("/www");
Serial.printf("/www 目录大小: %d 字节 (%.2f KB)\n", wwwSize, wwwSize / 1024.0);

// 创建备份
String backupPath = FileUtils::backupFile("/config/settings.json", "/backup");
if (!backupPath.isEmpty()) {
    Serial.printf("创建备份: %s\n", backupPath.c_str());
}

// 写入日志
String logMessage = "[" + String(millis()) + "] System started\n";
FileUtils::appendToFile("/logs/system.log", logMessage);

// 检查存储空间
float usage = FileUtils::getSpaceUsage();
if (usage > 0.8) {
    Serial.println("Warning: Storage space is running low!");
}
```

### 规则脚本示例
```
// main.cpp
#include "FastBeeLuaIntegration.h"

FastBeeLuaIntegration* luaIntegration;

void setup() {
    Serial.begin(115200);
    
    // 初始化FastBee核心
    FastBeeCore core;
    core.init();
    
    // 初始化Lua集成
    luaIntegration = FastBeeLuaIntegration::getInstance();
    if (!luaIntegration->init(&core)) {
        Serial.println("Failed to initialize Lua integration");
        return;
    }
    
    // 加载启动脚本
    loadStartupScripts();
    
    Serial.println("Lua script system initialized");
}

void loop() {
    // 处理定时任务
    luaIntegration->processScheduledEvents();
    
    // 其他主循环逻辑...
    delay(100);
}

void loadStartupScripts() {
    ScriptManager* scriptManager = luaIntegration->getScriptManager();
    
    // 加载系统脚本
    scriptManager->loadScript("/scripts/system_monitor.lua");
    scriptManager->loadScript("/scripts/network_manager.lua");
    scriptManager->loadScript("/scripts/web_api.lua");
    
    // 启动自动启动的脚本
    scriptManager->startAllScripts();
}
```