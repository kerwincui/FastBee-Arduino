# Web 控制台图文导览

本文汇总 FastBee-Arduino 设备端 Web 控制台的关键截图，适合首次验收、现场培训、问题排查和文档配图复用。当前截图来自一台已通过 COM6 烧录的 `esp32s3-F16R8` 实机，局域网访问地址为 `http://192.168.5.116/`。

![Web 控制台导航图](images/web-console-navigation-map.svg)

## 1. 登录与在线确认

设备启动并连入网络后，浏览器访问设备 IP 会先进入登录页。默认账号仅用于首次开箱验证，现场交付前应修改密码或创建新的运维账号。

![FastBee 登录页](images/fastbee-login.png)

登录后先看仪表盘。这里可以快速确认设备 IP、WiFi 状态、MQTT 状态、Flash/Heap/LittleFS 使用情况、芯片信息和运行时间。

![设备监控仪表盘](images/fastbee-dashboard.png)

移动端可以访问同一个地址，适合现场巡检时快速确认设备是否在线。

![移动端仪表盘](images/fastbee-mobile-dashboard.png)

验收要点：

- 页面能打开登录页，说明 LittleFS Web 文件系统和 HTTP 服务正常。
- 仪表盘有实时数据，说明登录会话、基础 API 和设备运行状态正常。
- 现场记录应包含设备 IP、固件环境、运行时间和当前网络状态。

## 2. 网络配置路径

网络配置页用于切换 STA/AP、查看当前联网状态，并维护 mDNS、静态 IP 等高级参数。首次配网时先确认 STA 基础配置，再按需调整 AP 和高级配置。

![网络基础配置](system/images/network-settings.png)

![热点配置](system/images/network-ap-settings.png)

![网络高级配置](system/images/network-advanced-settings.png)

操作顺序建议：

1. 在基础配置中确认当前 SSID、IP、信号强度和联网模式。
2. 需要现场临时维护时，保留 AP 热点配置，便于网络异常时重新进入设备。
3. 启用静态 IP 或 mDNS 前，先确认路由器地址段和现场命名规则。

## 3. 外设配置路径

外设配置页负责把实际接线抽象成可复用对象，例如 GPIO、ADC、传感器、显示屏、Modbus 子设备等。新增外设时建议先保存为禁用状态，确认引脚和供电后再启用。

![外设配置列表](system/images/peripheral-management.png)

![新增外设弹窗](system/images/peripheral-add-dialog.png)

阅读截图时重点看：

- 外设 ID 是否稳定且可读，后续规则会通过 ID 引用外设。
- 类型、引脚、总线地址和启用状态是否与实际接线一致。
- 继电器、电机、蜂鸣器等会产生实际动作的外设，上线前应先保持禁用。

## 4. 自动化规则路径

外设执行页面用于维护自动化规则。每条规则由触发器和动作组成，适合实现定时采集、阈值判断、继电器控制、数据显示、MQTT 联动和 Modbus 轮询。

![外设执行规则列表](system/images/periph-exec-management.png)

规则上线建议：

- 新增或导入规则后，先保持禁用，检查触发器、目标外设和动作参数。
- 调试时优先使用手动执行入口观察日志和外设状态。
- 规则稳定后再启用，并在设备日志中确认没有超时、参数错误或内存告警。

## 5. 协议配置路径

通信协议页面集中配置 MQTT 和 Modbus RTU。实际排障时应先确认网络在线，再检查协议参数。

![协议配置总览](system/images/protocol-config.png)

![MQTT 配置](system/images/protocol-mqtt-config.png)

![Modbus RTU 配置](system/images/protocol-modbus-rtu.png)

协议配置检查点：

- MQTT：服务器地址、端口、客户端 ID、用户名密码、主题前缀和连接状态。
- Modbus RTU：串口引脚、波特率、校验位、从站地址、寄存器映射和轮询周期。
- 如果协议页面参数正确但连接失败，先回到网络配置和设备日志确认底层链路。

## 6. 系统维护路径

Full 档位提供更多运维页面，包括设备配置、文件管理、设备日志、用户和角色维护。现场交付时建议把这些页面作为维保检查的固定截图项。

![设备配置页面](system/images/device-config.png)

![文件管理页面](system/images/file-management.png)

![设备日志页面](system/images/device-logs.png)

![用户管理页面](system/images/user-management.png)

![角色管理页面](system/images/role-management.png)

维护建议：

- 设备配置用于确认设备名称、编号、时间同步和开发环境开关。
- 文件管理用于导入导出配置、查看 LittleFS 文件和保留现场备份。
- 设备日志用于定位启动、网络、MQTT、外设初始化和规则执行问题。
- 用户和角色页面用于交付后的权限收敛，避免值守账号误改网络或外设参数。

## 7. 规则脚本路径

规则脚本页面适合管理可复用的脚本模板。脚本会把多步 GPIO、PWM、延时、变量读写等逻辑封装到一条动作中，上线前必须先在禁用状态下检查。

![规则脚本页面](system/images/rule-script.png)

脚本截图留档时，建议同时记录：

- 脚本名称、启用状态和最近修改时间。
- 调用脚本的外设执行规则。
- 涉及的 GPIO、PWM、继电器、电机或显示屏实际接线。

## 8. 截图复用索引

图片资产的命名、替换、脱敏和校验规范见 [文档图片资产维护指南](image-assets.md)。通用入口图片清单见 [docs/images 图片说明](images/README.md)，系统页面图片清单见 [docs/system/images 图片说明](system/images/README.md)。

| 场景 | 推荐图片 | 适合引用的文档 |
|---|---|---|
| 首次访问、登录验收 | `images/fastbee-login.png` | 快速开始、部署、发布检查 |
| 设备在线状态 | `images/fastbee-dashboard.png` | 概览、用户手册、测试、商业交付 |
| 移动巡检 | `images/fastbee-mobile-dashboard.png` | 概览、现场巡检说明 |
| 网络排障 | `system/images/network-settings.png` | 网络配置、MQTT、部署检查 |
| 外设建模 | `system/images/peripheral-management.png` | 外设手册、示例教程、硬件覆盖 |
| 新增外设 | `system/images/peripheral-add-dialog.png` | 外设配置指南、示例教程 |
| 自动化规则 | `system/images/periph-exec-management.png` | 外设执行、场景教程、规则排障 |
| MQTT 接入 | `system/images/protocol-mqtt-config.png` | 协议文档、平台对接 |
| Modbus 接入 | `system/images/protocol-modbus-rtu.png` | Modbus 文档、RS485 场景 |
| 维护备份 | `system/images/file-management.png` | 文件管理、配置迁移 |
| 日志排障 | `system/images/device-logs.png` | 设备日志、测试、稳定性检查 |

新增截图建议继续放在对应目录下：通用入口图放 `docs/images/`，系统页面图放 `docs/system/images/`。文件名建议使用页面和状态命名，例如 `periph-exec-edit-dialog.png`、`file-config-export.png`、`mqtt-test-result.png`。
