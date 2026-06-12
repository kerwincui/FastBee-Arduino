# 文档图片资产维护指南

本文用于维护 `docs/` 下的截图资产，说明图片存放位置、命名规则、替换流程和校验方法。当前图片来自一台已通过 COM6 烧录的 `esp32s3-F16R8` 实机，设备访问地址为 `http://192.168.5.116/`。

## 目录约定

| 目录 | 用途 | 示例 |
|---|---|---|
| `docs/images/` | 全局入口图、登录页、仪表盘、移动端截图 | `fastbee-login.png`、`fastbee-dashboard.png` |
| `docs/system/images/` | Web 控制台功能页截图 | `network-settings.png`、`peripheral-management.png` |

全局图用于概览、快速开始、部署和验收文档；系统图用于具体功能页、示例教程、外设手册和排障说明。流程图、导航图和架构图统一放在 `docs/images/`，便于跨目录复用。

![设备监控仪表盘](images/fastbee-dashboard.png)

![外设配置列表](system/images/peripheral-management.png)

## 命名规则

图片文件名使用小写英文、短横线和页面状态命名：

| 类型 | 命名建议 | 说明 |
|---|---|---|
| 页面总览 | `dashboard-overview.png` | 单页稳定入口 |
| 功能配置页 | `network-settings.png` | 页面或标签页名称 |
| 弹窗/表单 | `peripheral-add-dialog.png` | 动作 + 对象 + 状态 |
| 结果状态 | `mqtt-test-success.png` | 功能 + 动作 + 结果 |
| 移动端 | `fastbee-mobile-dashboard.png` | 加 `mobile` 标识 |
| 流程图 | `deployment-verification-flow.svg` | 使用 `flow`、`map` 等后缀 |
| 决策/验证图 | `version-selection-guide.svg` | 使用 `guide`、`pyramid`、`tree` 等后缀 |
| 协议/拓扑图 | `modbus-rtu-polling-flow.svg` | 使用协议名 + `flow` 或 `lifecycle` |
| 数据/产物图 | `sensor-data-pipeline.svg` | 使用 `pipeline`、`lifecycle`、`artifact-map` 等后缀 |
| 场景闭环图 | `scenario-automation-loop.svg` | 使用 `loop`、`map`、`pipeline` 等后缀 |
| 内部结构图 | `periph-exec-engine-internals.svg` | 使用模块名 + `internals`、`boundary-map` 等后缀 |
| 数据模型图 | `peripheral-config-data-model.svg` | 使用对象名 + `data-model`，说明字段、运行时和消费者关系 |
| 覆盖矩阵图 | `hardware-coverage-matrix.svg` | 使用 `coverage-matrix`，说明资料、功能和版本覆盖关系 |
| 物理原理图 | `ultrasonic-ranging-geometry.svg` | 使用 `geometry`、`wiring-check`，说明接线、测量或安装关系 |
| 发布守卫图 | `optimization-guardrail-map.svg` | 使用 `guardrail-map`，说明变更从证据到发布观察的闭环 |
| 规则模型图 | `periph-exec-rule-data-model.svg` | 使用规则对象名 + `data-model`，说明触发器、动作和运行态关系 |
| 并发流程图 | `two-phase-execution-lock-flow.svg` | 使用实现策略 + `flow`，说明锁、快照、执行和结果边界 |
| 触发器对比图 | `trigger-timing-comparison.svg` | 使用触发器领域 + `comparison`，说明选择规则和排查差异 |
| 表单检查图 | `rule-builder-safety-checklist.svg` | 使用页面对象 + `checklist`，说明启用前核对项 |
| 语法速查图 | `command-script-cheatsheet.svg` | 使用功能名 + `cheatsheet`，说明命令、限制和排查路径 |
| 参数矩阵图 | `peripheral-parameter-matrix.svg` | 使用对象领域 + `parameter-matrix`，说明类型参数和启用检查 |
| API 映射图 | `modbus-api-function-map.svg` | 使用模块名 + `api-function-map`，说明接口、功能码和数据区对应关系 |
| 路由地图 | `periph-exec-api-route-map.svg` | 使用模块名 + `api-route-map`，说明页面、路由、方法和落点 |
| 排障决策树 | `script-debug-decision-tree.svg` | 使用主题 + `debug-decision-tree`，说明故障归类和排查顺序 |

避免使用日期、随机编号、中文文件名或临时名。需要保留历史图时，优先在 Git 历史中追踪，不在目录中堆叠 `old`、`new`、`final` 文件。

## 截图要求

- 桌面截图建议保持 `1440x1000` 左右，能同时看到左侧菜单、页面标题和主要表单。
- 移动端截图建议保持真实手机视口比例，例如当前 `390x844`。
- 截图前登录设备，确认页面数据已加载完成，不保留加载中、报错或半刷新状态。
- 涉及密码、Token、云平台密钥、个人 WiFi 名称时，应先脱敏或使用测试值。
- 外设、规则、协议页面截图应优先使用默认禁用或演示配置，避免看起来像可以直接照抄到现场启用。

## 替换流程

1. 访问设备 Web 控制台并进入目标页面。
2. 确认页面状态稳定，必要时刷新并重新登录。
3. 使用相同视口截图，覆盖对应目录中的同名图片。
4. 检查引用该图片的文档语义是否仍然匹配。
5. 运行图片和链接校验，确认没有丢图或断链。

当前高复用图片包括：

| 图片 | 主要用途 |
|---|---|
| `system/images/peripheral-management.png` | 外设手册、示例教程、硬件覆盖检查 |
| `system/images/peripheral-add-dialog.png` | 新增外设流程、传感器/执行器配置 |
| `system/images/periph-exec-management.png` | 自动化规则、触发器、动作和场景教程 |
| `images/fastbee-dashboard.png` | 部署验收、设备在线状态、商业交付说明 |
| `images/peripheral-config-data-model.svg` | 外设配置、导入导出、规则引用和运行时排障 |
| `images/api-smoke-test-flow.svg` | 测试指南、发布检查清单、真实设备验收 |
| `images/soak-test-feedback-loop.svg` | 长稳测试、发布观察、现场问题复测 |
| `images/hardware-coverage-matrix.svg` | 示例总览、硬件资料覆盖、外设能力盘点 |
| `images/ultrasonic-ranging-geometry.svg` | HC-SR04 示例、超声波外设、距离/水位场景 |
| `images/modbus-register-map.svg` | Modbus 使用指南、寄存器地址排障、控制 API 说明 |
| `images/periph-exec-rule-data-model.svg` | 外设执行总览、规则 JSON、源码实现和用户手册 |
| `images/trigger-timing-comparison.svg` | 触发器说明、外设执行配置指南、用户手册 |
| `images/rule-builder-safety-checklist.svg` | 快速开始、外设执行页面、现场启用规则前检查 |
| `images/rule-script-template-map.svg` | 规则脚本语法、协议数据转换、脚本 FAQ |
| `images/command-script-cheatsheet.svg` | 命令脚本说明、脚本动作、脚本示例 |
| `images/peripheral-parameter-matrix.svg` | 外设配置长表、外设管理、现场接线和启用检查 |
| `images/modbus-api-function-map.svg` | Modbus 面板、REST API、功能码和寄存器调试 |
| `images/periph-exec-api-route-map.svg` | 外设执行 API 参考、页面问题定位、开发实现说明 |
| `images/script-debug-decision-tree.svg` | 脚本 FAQ、规则脚本页面、命令脚本调试 |

这些图片一旦替换，建议重点复查示例教程、外设手册、用户手册和发布检查清单。

## 校验方法

文档改动后至少执行：

```powershell
git diff --check -- docs
```

如需检查 Markdown 中的图片和本地链接，可运行仓库脚本或临时 Node 校验脚本，确认：

- 每个 Markdown 文档至少有一张图片。
- 所有 Markdown 图片引用的目标文件存在。
- 所有本地 Markdown 链接能解析到实际文件。

## 图片索引

通用入口图片见 [docs/images 图片说明](images/README.md)，系统页面图片见 [docs/system/images 图片说明](system/images/README.md)。完整页面操作导览见 [Web 控制台图文导览](web-console-visual-guide.md)。
