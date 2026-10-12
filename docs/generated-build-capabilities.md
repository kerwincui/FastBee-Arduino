<!-- generated: scripts/generate-build-capabilities.js; do not edit tables -->
# 自动生成的构建环境与 Web 能力矩阵

> 本表从当前工作区 platformio.ini 和现有 Web 能力解析器生成，不是发布成功、实例可用或实物支持证明。

刷新：node scripts/generate-build-capabilities.js --write；只读校验：node scripts/generate-build-capabilities.js --check。
现有 validate-build-matrix.js / Host static 同时检查本表是否过期。运行时 features.json、预算、依赖、初始化和设备接线不参与本表生成。

## 构建环境

Flash/PSRAM 数字来自环境命名，不是设备探测；PlatformIO board 是构建定义，不是已验收的成品板修订。RGB 专用固件不在 canonical 表中。

| 环境 | PlatformIO board | 命名 Flash MB | 命名 PSRAM MB | 分区文件 |
| --- | --- | ---: | ---: | --- |
| esp32-F4R0 | esp32dev | 4 | 0 | partitions/4MB.csv |
| esp32-F8R0 | esp32dev | 8 | 0 | partitions/8MB.csv |
| esp32-F8R8 | esp32dev | 8 | 8 | partitions/8MB.csv |
| esp32c3-F4R0 | esp32-c3-devkitm-1 | 4 | 0 | partitions/4MB.csv |
| esp32s3-F8R0 | esp32-s3-devkitc-1 | 8 | 0 | partitions/8MB.csv |
| esp32c6-F4R0 | esp32-c6-devkitc-1 | 4 | 0 | partitions/4MB.csv |
| esp32c6-F8R0 | esp32-c6-devkitc-1 | 8 | 0 | partitions/8MB.csv |
| esp32s3-F8R8 | esp32-s3-devkitc-1 | 8 | 8 | partitions/8MB.csv |
| esp32s3-F16R8 | esp32-s3-devkitc1-n16r8 | 16 | 8 | partitions/16MB.csv |

## Web 发布能力

包含/裁剪表示显式构建宏经现有 Web resolver 解析后的资源选择。未声明表示宏未显式定义，Web resolver 按 false 处理；不能据此声称 C++ 默认宏或运行功能也关闭。
otaFs 的旧宏保留分区/发布配置兼容；即使宏为 1，当前无文件系统 OTA 写入实现，有效能力仍为 false，表中标为未实现，不影响 OTA 固件升级父能力。
某资源还可能有组合依赖；本表不将所有宏映射为独立页面。外设类型、驱动型号、动作目标、整板资源与 E2～E5 验收须另按生产契约核验。

| 能力 / 构建宏 | esp32-F4R0 | esp32-F8R0 | esp32-F8R8 | esp32c3-F4R0 | esp32s3-F8R0 | esp32c6-F4R0 | esp32c6-F8R0 | esp32s3-F8R8 | esp32s3-F16R8 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| webServer / FASTBEE_ENABLE_WEB_SERVER | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 |
| webStatic / FASTBEE_ENABLE_WEB_STATIC | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 |
| webApi / FASTBEE_ENABLE_WEB_API | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 |
| auth / FASTBEE_ENABLE_AUTH | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 |
| session / FASTBEE_ENABLE_SESSION | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 |
| mqtt / FASTBEE_ENABLE_MQTT | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 |
| mdns / FASTBEE_ENABLE_MDNS | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 |
| apMode / FASTBEE_ENABLE_AP_MODE | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 |
| periphExec / FASTBEE_ENABLE_PERIPH_EXEC | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 |
| gpio / FASTBEE_ENABLE_GPIO | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 |
| commandScript / FASTBEE_ENABLE_COMMAND_SCRIPT | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 |
| sensorDriver / FASTBEE_ENABLE_SENSOR_DRIVER | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 |
| ds18b20 / FASTBEE_ENABLE_DS18B20 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 |
| modbus / FASTBEE_ENABLE_MODBUS | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 |
| lcd / FASTBEE_ENABLE_LCD | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 |
| lcdGb2312 / FASTBEE_ENABLE_LCD_GB2312 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 |
| neopixel / FASTBEE_ENABLE_NEOPIXEL | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 |
| sevenSegment / FASTBEE_ENABLE_SEVEN_SEGMENT | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 |
| rfid / FASTBEE_ENABLE_RFID | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 |
| i2cSensors / FASTBEE_ENABLE_I2C_SENSORS | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 |
| ioExpander / FASTBEE_ENABLE_IO_EXPANDER | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 |
| irRemote / FASTBEE_ENABLE_IR_REMOTE | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 裁剪 | 包含 |
| rfLearn / FASTBEE_ENABLE_RF_LEARN | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 裁剪 | 包含 |
| ds1302 / FASTBEE_ENABLE_DS1302 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 |
| lcd1602 / FASTBEE_ENABLE_LCD1602 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 |
| ruleScript / FASTBEE_ENABLE_RULE_SCRIPT | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 |
| deviceControl / FASTBEE_ENABLE_DEVICE_CONTROL | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 |
| screenPanel / FASTBEE_ENABLE_SCREEN_PANEL | 裁剪 | 裁剪 | 裁剪 | 裁剪 | 包含 | 裁剪 | 裁剪 | 包含 | 包含 |
| remoteResources / FASTBEE_ENABLE_REMOTE_RESOURCES | 裁剪 | 包含 | 包含 | 裁剪 | 包含 | 裁剪 | 包含 | 包含 | 包含 |
| ota / FASTBEE_ENABLE_OTA | 裁剪 | 包含 | 包含 | 裁剪 | 包含 | 裁剪 | 包含 | 包含 | 包含 |
| otaFs / FASTBEE_ENABLE_OTA_FS | 裁剪 | 未实现（宏已声明启用） | 未实现（宏已声明启用） | 裁剪 | 未实现（宏已声明启用） | 裁剪 | 未实现（宏已声明启用） | 未实现（宏已声明启用） | 未实现（宏已声明启用） |
| logViewer / FASTBEE_ENABLE_LOG_VIEWER | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 |
| fileLogging / FASTBEE_ENABLE_FILE_LOGGING | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 |
| userAdmin / FASTBEE_ENABLE_USER_ADMIN | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 |
| configTransfer / FASTBEE_ENABLE_CONFIG_TRANSFER | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 |
| ble / FASTBEE_ENABLE_BLE | 裁剪 | 裁剪 | 裁剪 | 裁剪 | 裁剪 | 裁剪 | 裁剪 | 裁剪 | 裁剪 |
| ethernet / FASTBEE_ENABLE_ETHERNET | 裁剪 | 包含 | 包含 | 裁剪 | 包含 | 裁剪 | 裁剪 | 包含 | 包含 |
| cellular / FASTBEE_ENABLE_CELLULAR | 裁剪 | 包含 | 包含 | 裁剪 | 包含 | 裁剪 | 裁剪 | 包含 | 包含 |
| i18n / FASTBEE_ENABLE_I18N | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 | 包含 |

## 可重现来源

以下是去 BOM、CRLF 归一为 LF 后的 UTF-8 文本 SHA256，避免 Git 换行策略导致误报；不是镜像或原始字节 SHA256。没有时间戳、绝对路径或设备信息。

| 来源 | 文本 SHA256 |
| --- | --- |
| platformio.ini | 0fa17e317e104120a60b551375cddb126a3ba3aac2cff3d3274bd1917b22fe45 |
| scripts/web-profile.js | 745b699312e37e3f582a58f0d0c16410dae7ea99346b5b9ac507ddc8cb09adc3 |
| scripts/generate-build-capabilities.js | 2bbb9e1cc6337aac36e7fbe61dff28d8afa7d909f9c246110214845bd3019690 |

每环境指纹沿用 Web 发布清单算法，包含解析后的构建定义；不能替代候选输入/源码/镜像哈希。

| 环境 | Web capabilityFingerprint |
| --- | --- |
| esp32-F4R0 | 65ae4755485e45afd264aba474533a32b5498bf3c66f092def0cd3f6a6f70c20 |
| esp32-F8R0 | d08c148dc37fc8d64b86b4c56646af9bb129bac6577830bce56ca4670580db9f |
| esp32-F8R8 | e16f39473922a90fe5b7af162591793edce5fd7c264e759158ef65277839978b |
| esp32c3-F4R0 | d6bd418322a2af7540989e459bca7569095bd1c256cca53adb7a665d177d9f9a |
| esp32s3-F8R0 | 92c24d0dddc058bef609b13f425337d8e17372bfd4980ff386b334c6c183b352 |
| esp32c6-F4R0 | a6765707c5f92275cb8d0128a57cb5b97c926dcc4f5cd0dd9196286bc2552ac6 |
| esp32c6-F8R0 | e23beee75b5fd8c6f138890766afd40d26fea5f8f6915d0c5ab63358fc242349 |
| esp32s3-F8R8 | 0aef53ea15012fa73e2451004252f46dcdf3bbe9ef13d6295092ee19b425a2a3 |
| esp32s3-F16R8 | 4c9c90f6b0cc0c12fe55c182cbb4a1086c24b259006d5c5135a6be0d0d5ca773 |
