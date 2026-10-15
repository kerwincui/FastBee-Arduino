# 设备配置与本地备份

本目录每台设备只维护一套公开配置。四台当前测试设备统一放在 [device-test-configs/](device-test-configs/README.md)，另两种板卡保留各自唯一的样例。公开配置已脱敏，不能用空凭据覆盖现场配置；完整用户手册维护在 [FastBee 设备端文档](https://fastbee.cn/doc/device/)。

## 根目录文件索引

以下按分组登记 `docs/` 根目录当前保留的文件；设备配置样例目录的入口见下方[唯一配置入口](#唯一配置入口)表。

### 计划与审计

- [项目审计与改进计划（2026-10-13）](project-audit-and-improvement-plan-20261013.md)：当前系统改进任务的执行清单与在途事项接管记录

其余历史计划与审计文档已于 2026-10-13 清理，在途事项由上述计划接管，历史见 git 记录。

### 契约与生成物

- [资源与稳定性验收约束](generic-resource-stability-contract.md)：公共链路的容量、调度、恢复与验收约束（AGENTS.md 引用）
- [构建环境与 Web 能力矩阵](generated-build-capabilities.md)：由 `scripts/generate-build-capabilities.js` 自动生成，勿手改表格
- [动作与外设目标契约](generated-action-target-contracts.md)：由 `scripts/generate-action-contracts.py` 自动生成，勿手改表格
- `repository-retention-manifest.json`：仓库保留/隔离条目的处置清单（由 retention 门禁校验，勿手改约定）

### 指南与约束

- [工业接口字段参考（AI / DI / DO / AO）](https://fastbee.cn/doc/device/peripherals/adc-input.html)：与源码实现相关的字段、公式和边界（已迁移至设备端文档站）
- [设备屏幕使用说明](https://fastbee.cn/doc/device/system/screen-panel.html)：touch-editor-r2 编辑工作区使用说明（已迁移至设备端文档站）

触控屏历史实施/部署/验证记录已于 2026-10-13 清理，历史见 git 记录。

### 设备配置样例（根目录 JSON）

以下示例配置保持 `enabled: false`，用于字段和结构参考，不是可直接部署的完整配置：

- `ads1115-example-20261002.json`：ADS1115 电压采集外设示例
- `ads1115-scan-example-20261004.json`：ADS1115 四路输入扫描示例
- `mcp23017-disabled-example-20261003.json`：MCP23017 I²C 总线与复位引脚示例
- `rtc-ntp-writeback-disabled-example-20261004.json`：RTC 网络同步后复制的规则示例


## 唯一配置入口

| 设备 / 历史端口 | 构建环境 | 配置来源 | 配置范围 |
| --- | --- | --- | --- |
| C3 开发板 / `COM4` | `esp32c3-F4R0` | [device-test-configs/com4-esp32c3-f4r0/config](device-test-configs/com4-esp32c3-f4r0/README.md) | 3 个外设、3 条规则，含实际功能配置 |
| 普中 ESP32 / `COM16` | `esp32-F4R0` | [device-test-configs/com16-esp32-f4r0/config](device-test-configs/com16-esp32-f4r0/README.md) | 11 个外设、6 条规则及规则脚本；不恢复旧全量负载 |
| ECM50-A 工业 S3 / `COM15` | `esp32s3-F16R8` | [device-test-configs/com15-esp32s3-f16r8/config](device-test-configs/com15-esp32s3-f16r8/README.md) | 8 个外设、9 条规则、Modbus 和现场配置 |
| Waveshare 触控屏 / `COM8/9` | `esp32s3-F16R8`，需 RGB 板级构建 | [device-test-configs/com8-com9-esp32s3-touch/config](device-test-configs/com8-com9-esp32s3-touch/README.md) | 4 个外设、6 条规则（1 条启用）、布局与历史；COM8/9 是同一台设备 |
| C6 RGB 板 / `COM11` | `esp32c6-F4R0` | [c6-rgb-led-f4r0](c6-rgb-led-f4r0/README.md) | GPIO8 RGB 和 2 条灯效规则 |
| Waveshare 六路继电器板 / `COM7` | `esp32s3-F8R0` | [waveshare-esp32s3-relay6ch](waveshare-esp32s3-relay6ch/README.md) | 六路继电器、RGB、RS485 和温湿度模板 |

上表是配置归属和历史端口标签，不是当前在线清单。DHCP 变化后以串口或路由器实际分配结果为准；访问设备前重新核对 MAC、芯片、端口和当前 STA 地址 `http://<device-ip>`。普通工业 S3 与 RGB 触控屏不能按相同构建环境混用配置或私有备份。

每套配置由多个运行 JSON 组成；设备目录中的 README、manifest 和 RGB 构建合同不是运行配置。四设备目录输入必须指向各自的 `config/`，不能指向含 manifest 的设备根目录。C6 和六路继电器板的目录格式可直接作为配置来源。

## 部署与配置保护

四设备的配置来源、历史备份和实施状态见[四设备总说明](device-test-configs/README.md)。目录整理不改变配置内容、部署许可或验收状态；`deploymentAllowed=false` 的公开副本不能作为直接部署许可。部署以同设备的新鲜私有配置为基准，只合并已审核的差异，保留网络、身份、认证、MQTT、外设、规则、布局和非 Web 文件。

初次使用独立板卡样例，可先执行预检：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/deploy-config-bundle.ps1 `
  -Env esp32c6-F4R0 -Port COM11 `
  -BundlePath docs/c6-rgb-led-f4r0 -DryRun
```

`-DryRun` 会暂存和恢复 `data/config`，仍需遵守共享锁和冻结输入要求；不在构建期间并行运行。实际烧录前核对容量、分区、活动槽，完成私有备份并取得本次写入和读回校验证据。`scripts/deploy-multi-device.ps1 -AllDevices` 仍是五设备历史映射，包含 COM11/COM7，不是当前四设备任务入口。

公开配置不保存有效 WiFi、MQTT 或天气密钥，`auth.json` 和 `users.json` 仅留在被 Git 忽略的 `.device-config-backups/`。四设备私有备份按设备 ID 和已核实 MAC 隔离，两台 S3 不能共享一个环境目录键。历史失败、Flash 备份和验收报告按证据保留规则维护，不属于本次配置去重范围。

## 配置维护规则

1. 每台设备只更新上表中的唯一配置，不新增日期或“临时”配置副本。manifest 中的原来源路径仅记录历史，不作为第二套配置入口。
2. 修改公开配置后执行 `node scripts/validate-device-config-examples.js` 和 `node scripts/validate-project-hygiene.js`，检查 JSON、脱敏、关联引用及配置差异。
3. 改变字段、外设类型、规则或板级参数时同步说明及对应测试；构建和设备验收按实际影响范围执行。
4. `ecm50a-s3-f16r8/` 与 `waveshare-esp32s3-touch-lcd4/` 的硬件说明和历史记录已于 2026-10-15 随 docs 精简移除，历史见 git 记录；触控 RGB 变体构建链说明并入 `scripts/README.md` 的 `build-screen-variant.ps1` 条目。不得在 docs 下重建这两块板卡的第二套配置入口。
5. 通用外设片段、协议模板和专项例子用于独立功能说明，不是某台设备的完整配置副本；保留在对应专题中。

## 专项说明

- [通用 I²C 外设配置与验收](generic-i2c-peripherals/README.md)
- [工业 AI / DI / DO / AO 字段参考](https://fastbee.cn/doc/device/peripherals/adc-input.html)（已迁移至设备端文档站）
- [触控屏使用说明](https://fastbee.cn/doc/device/system/screen-panel.html)（已迁移至设备端文档站）
- [设备测试与发布流程](https://fastbee.cn/doc/device/deploy/testing.html)
- [外设配置说明](https://fastbee.cn/doc/device/peripherals/)
