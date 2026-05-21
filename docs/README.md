# FastBee-Arduino 文档导航

本文档目录面向当前代码功能整理，优先描述精简版默认构建、Web 管理界面、外设配置、外设执行、MQTT/Modbus RTU 和配置导入导出流程。

## 当前功能定位

FastBee-Arduino 默认面向 classic ESP32、ESP32-C3、ESP32-S3 提供精简版固件，保留设备监控、网络配置、设备配置、外设配置、外设执行、MQTT、Modbus RTU 主站、设备控制和配置导入导出等核心能力。精简版会关闭 OTA、文件管理、日志查看、多用户/角色、RuleScript、TCP/HTTP/CoAP、Modbus 从站等高资源或低频功能，以保证 Web 页面访问、低内存稳定性和长期运行。

`esp32s3-full` 用于完整功能验证，可保留 OTA、文件/日志、多用户/角色、RuleScript、TCP/HTTP/CoAP、Modbus 从站等扩展能力。Web 文件系统和固件必须使用同一功能档位配套构建和上传。

## 文档索引

| 文档 | 适用场景 |
| --- | --- |
| [配置导入导出指南](./config-transfer-guide.md) | 备份、恢复、按类型导入导出 `/config/*.json` |
| [外设配置指南](./peripheral-configuration-guide.md) | 外设类型、引脚、参数、编译开关和排错 |
| [外设执行配置指南](./periph-exec-configuration-guide.md) | 本地自动化规则、事件触发、传感器联动、动作配置 |
| [外设执行流程说明](./periph_exec_flow.md) | PeriphExec 内部流程、调度、动作执行细节 |
| [Modbus RTU 使用指南](./modbus_usage_guide.md) | Modbus RTU 主站、子设备、映射、继电器/电机控制 |
| [OLED/LCD 使用指南](./oled_usage_guide.md) | OLED/LCD 显示屏、变量显示、数码管显示 |
| [RuleScript 脚本指南](./script-guide.md) | 完整版 `esp32s3-full` 中的脚本规则能力 |
| [Web 精简版与稳定性说明](./web_refactor_plan.md) | 当前 Web 精简策略、构建产物、加载优化 |
| [Web 优化变更记录](./web_refactor_plan_2026-05-19.md) | Web 稳定性和体积优化的阶段性记录 |

## 构建与产物原则

- `web-src/` 是 Web 前端源码目录，页面、样式和模块都从这里维护。
- `data/www/` 只放上传到硬件的压缩产物，不建议手工编辑。
- 使用 `node scripts/gzip-www.js --web-slim --no-monitor` 生成并上传精简版 LittleFS。
- 使用 `node scripts/gzip-www.js --web-slim --no-upload --no-monitor` 仅生成精简版产物和 staging 镜像。
- 使用 `pio run -e esp32`、`pio run -e esp32c3`、`pio run -e esp32s3` 构建对应精简固件。
- 使用 `pio run -e esp32s3-full` 构建 ESP32-S3 完整版。

## 当前推荐配置流程

1. 烧录固件和 LittleFS。
2. 首次启动后连接设备 AP，进入 Web 管理界面。
3. 在“网络设置”配置 WiFi。
4. 在“外设配置”添加 UART/RS485、DHT11、蜂鸣器、OLED、数码管等硬件。
5. 在“通信协议”配置 MQTT 或 Modbus RTU。
6. 在“外设执行”配置定时、事件或传感器联动规则。
7. 在“设备配置 > 高级配置”按类型导出配置备份。
