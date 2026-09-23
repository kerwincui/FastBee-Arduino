# 设备配置样例与本地备份

本目录只保存可公开提交的设备配置样例和少量源码配套说明。完整的用户手册、烧录教程和 API 文档维护在 [FastBee 设备端文档](https://fastbee.cn/doc/device/)；这里不重复维护整套手册。

仓库中的配置分为两层：

- 下表中的五套公开样例可提交到 Git，设备身份、WiFi STA 和 MQTT 凭据必须保持为空。
- 仓库根目录的 `.device-config-backups/latest/<构建环境>/` 是被 Git 忽略的本机私有备份，每台设备只保留当前最新的一份真机配置。完整 JSON 可能包含设备身份、WiFi 或登录材料，不得打包、提交或发送给其他人；长期保存应移到加密存储。

## 设备配置样例

五套配置分别对应不同硬件，不能互换。样例中的设备身份、WiFi STA 和 MQTT 凭据均为空或已脱敏。

| 构建环境 | 配置来源 | JSON 内容 | 主要用途 |
| --- | --- | --- | --- |
| `esp32c3-F4R0` | [`esp32c3-deployment-config.json`](esp32c3-deployment-config.json) | bundle：`device`、`peripherals`、`periph_exec`、`protocol` | DHT11、GPIO LED、OLED 和 3 条联动规则 |
| `esp32c6-F4R0` | [`c6-rgb-led-f4r0/`](c6-rgb-led-f4r0/README.md) | `device`、`network`、`peripherals`、`periph_exec`、`protocol` | GPIO8 板载 RGB 和 2 条灯效规则 |
| `esp32-F4R0` | [`puzhong-esp32-config.json`](puzhong-esp32-config.json) | bundle：上述 5 类及 `rule_scripts` | 普中开发板 16 个外设和 20 条综合测试规则 |
| `esp32s3-F16R8` | [`ecm50a-s3-f16r8/`](ecm50a-s3-f16r8/README.md) | `device`、`network`、`peripherals`、`periph_exec`、`protocol` | ECM50-A 土壤 AI、互补光电 DI、本板继电器和六台 Modbus 设备；SHT30 可选禁用 |
| `esp32s3-F8R0` | [`waveshare-esp32s3-relay6ch/`](waveshare-esp32s3-relay6ch/README.md) | `device`、`network`、`peripherals`、`periph_exec`、`protocol` | Waveshare 六路继电器、RGB、RS485 和 Modbus 温湿度模板 |

表中的 bundle 是一个 JSON 文件，其中 `files[].content` 保存各配置文件内容。目录格式则直接保存独立 JSON。两种格式均由部署脚本原生支持，不要把某台设备的配置复制给另一台设备。

新增带屏设备测试配置见 [`waveshare-esp32s3-touch-lcd4/`](waveshare-esp32s3-touch-lcd4/README.md)，使用 `esp32s3-F16R8`，下载模式通常为 `COM8`、应用运行时当前为 `COM9`，板本为 Rev4.0。模板启用 RGB/GT911、SHT30 温湿度采集、最近十点和面板卡片；Modbus 使用地址 8、9600、8-N-1，预置速度、脉冲数、计数和波特率编码四个 FC03 单寄存器读取任务，且默认全部禁用。当前电机硬件断开，不试运转、不自动写寄存器。它与 COM15 的 ECM50-A 是不同设备，不纳入下面既有五设备自动映射；请按说明单独预检，不能仅凭相同构建环境选择备份。

下表只说明五套公开样例的构建环境和历史串口标签，不是当前实验室在线清单。DHCP 变化后以串口或路由器实际分配结果为准；实际部署时必须先按 MAC/芯片身份核对设备，再将 `<device-ip>` 替换为当前分配的 STA 地址：

| 环境 | 串口 | 参考 STA 地址 |
| --- | --- | --- |
| `esp32s3-F16R8` | `COM15` | `http://<device-ip>` |
| `esp32c3-F4R0` | `COM4` | `http://<device-ip>` |
| `esp32c6-F4R0` | `COM11` | `http://<device-ip>` |
| `esp32-F4R0` | `COM16` | `http://<device-ip>` |
| `esp32s3-F8R0` | `COM7` | `http://<device-ip>` |

样例保留设备原始导出的选择范围，不强行补入未导出的文件。部署时 `scripts/deploy-config-bundle.ps1` 会补齐缺失的工厂默认配置，并根据已启用的外设、规则、Modbus 和网络能力生成 `features.json`。最终写入 LittleFS 的常规文件为 `device.json`、`network.json`、`features.json`、`peripherals.json`、`periph_exec.json` 和 `protocol.json`；只有实际使用规则脚本时才需要 `rule_scripts.json`。`auth.json` 和 `users.json` 含认证材料，不能进入公开样例。

无 PSRAM 设备超过内存预算时，部署脚本会按内存策略禁用部分高占用外设或执行规则，并在部署报告中记录实际裁剪结果。源样例不会被静默改写。

## 单设备部署

先用 `-DryRun` 检查环境、串口、配置内容和内存裁剪结果：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/deploy-config-bundle.ps1 `
  -Env esp32c6-F4R0 -Port COM11 `
  -BundlePath docs/c6-rgb-led-f4r0 -DryRun
```

检查通过后移除 `-DryRun`。端口只是示例，应以当前系统识别结果为准。ECM50-A 还需增加 `-NetworkProfile ecm50a`。

公开配置不保存 STA 密码，部署时通过环境变量临时提供：

```powershell
$env:FASTBEE_STA_SSID = "your-ssid"
$env:FASTBEE_STA_PASSWORD = "your-password"
```

真机验证只访问设备获得的 STA 地址，不连接设备 AP；STA 联网失败时保留串口和离线配置检查，跳过 Web 检查。

## 五设备批量预检

先执行不写设备的预检，确认五个环境、串口和配置来源一一对应：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/deploy-multi-device.ps1 `
  -AllDevices -DryRun -SkipSerialCheck
```

实际烧录前仍需逐台确认系统识别的串口。五台设备的 USB 写入会串行执行，不要同时运行第二个烧录命令。设备上线后先确认 `/api/health`，再登录检查功能状态、外设、规则和协议页面；可选菜单只在对应功能实际为 `active` 时显示，“设备配置”等基础菜单始终保留。

## 配置维护规则

1. 修改样例后运行 `node scripts/validate-device-config-examples.js`，确认 JSON 合法、无凭据且五套配置语义指纹互不相同。
2. 运行 `node scripts/validate-project-hygiene.js`，确认没有把身份、WiFi、MQTT 或认证材料带入可提交文件。
3. 修改字段、外设类型或联动动作时运行 `npm test`，同步更新对应板卡说明。
4. 不要把现场导出的真实配置覆盖到公开样例，也不要使用 `git add -f` 提交仓库根目录的 `.device-config-backups/`；更新真机备份时替换 `latest/<构建环境>/`，不要继续累积按日期命名的副本。
5. 不要随意改动这五个配置路径；部署脚本和回归测试直接引用它们。

## 专项说明

- [通用 I²C 外设配置与验收](generic-i2c-peripherals/README.md)：PCF8574、INA226 与已有传感器的无凭据配置片段、接线/校准、迁移回退和九环境验收记录。
- [通用外设能力扩展计划（待审核）](generic-peripheral-extension-plan.md)：通过现有外设配置与外设执行扩展 IO 和传感器能力，附可选功能、分工及验收标准。
- [StamPLC 资料核对记录（历史评估）](stamplc/README.md)：保留硬件与官方源码证据，原整板适配范围已由通用扩展计划取代。
- [工业 AI / DI / DO / AO 字段参考](industrial-io-guide.md)
- [设备测试与发布流程](https://fastbee.cn/doc/device/deploy/testing.html)
- [外设配置说明](https://fastbee.cn/doc/device/peripherals/)
