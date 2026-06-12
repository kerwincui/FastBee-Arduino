# 通用图片目录

本目录存放可被多个文档复用的全局入口截图，包括登录页、桌面仪表盘和移动端仪表盘。图片来自 `http://192.168.5.116/` 的实机 Web 控制台。

![设备监控仪表盘](fastbee-dashboard.png)

## 图片清单

| 图片 | 尺寸 | 推荐用途 |
|---|---:|---|
| ![登录页](fastbee-login.png) `fastbee-login.png` | `1440x1000` | 部署后首次访问、认证入口、LittleFS Web 文件系统验收 |
| ![设备监控仪表盘](fastbee-dashboard.png) `fastbee-dashboard.png` | `1440x1000` | 设备在线状态、IP/WiFi/MQTT/内存检查、商业交付截图 |
| ![移动端仪表盘](fastbee-mobile-dashboard.png) `fastbee-mobile-dashboard.png` | `390x844` | 移动端巡检、手机访问效果、响应式布局说明 |
| ![运行时架构图](runtime-architecture-map.svg) `runtime-architecture-map.svg` | `1440x820` | 架构说明、模块边界、运行时主链路 |
| ![部署与验收流程图](deployment-verification-flow.svg) `deployment-verification-flow.svg` | `1440x760` | 构建、烧录、登录、冒烟、备份流程 |
| ![Web 控制台导航图](web-console-navigation-map.svg) `web-console-navigation-map.svg` | `1440x840` | 页面入口、功能分类、培训导览 |
| ![外设执行规则链路图](periph-exec-rule-flow.svg) `periph-exec-rule-flow.svg` | `1440x760` | 触发器、规则匹配、动作执行和上报链路 |
| ![固件版本选型决策图](version-selection-guide.svg) `version-selection-guide.svg` | `1440x780` | Lite、Standard、Full 固件档位选择和交付边界判断 |
| ![配置文件生命周期流程图](config-lifecycle-flow.svg) `config-lifecycle-flow.svg` | `1440x760` | 配置导出、编辑、校验、上传、回滚和归档流程 |
| ![测试验证金字塔](testing-verification-pyramid.svg) `testing-verification-pyramid.svg` | `1440x820` | 静态检查、native 测试、构建、真机冒烟和长稳验证分层 |
| ![现场故障排查决策树](troubleshooting-decision-tree.svg) `troubleshooting-decision-tree.svg` | `1440x840` | 从 Web 可达性到网络、协议、外设和配置回滚的排障顺序 |
| ![网络配置与 AP 回退流程](network-provisioning-flow.svg) `network-provisioning-flow.svg` | `1440x780` | 设备启动、STA 联网、AP 回退、IP 获取和协议上线流程 |
| ![MQTT 连接与数据生命周期](mqtt-connection-lifecycle.svg) `mqtt-connection-lifecycle.svg` | `1440x800` | MQTT 网络就绪、Broker 认证、主题订阅、数据上报和异常重连 |
| ![Modbus RTU 主站轮询链路](modbus-rtu-polling-flow.svg) `modbus-rtu-polling-flow.svg` | `1440x820` | UART、RS485 总线、从站轮询、寄存器映射和规则联动 |
| ![外设接入与验证流程](peripheral-onboarding-flow.svg) `peripheral-onboarding-flow.svg` | `1440x800` | 外设硬件确认、类型选择、禁用保存、单项验证和规则接入 |
| ![外设执行规则生命周期](periph-exec-rule-lifecycle.svg) `periph-exec-rule-lifecycle.svg` | `1440x820` | 外设执行规则从草稿、校验、手动执行、启用到异常回滚的状态链路 |
| ![脚本与模板转换管道](script-transform-pipeline.svg) `script-transform-pipeline.svg` | `1440x800` | 平台消息、传感器缓存、模板解析、脚本转换和输出结果 |
| ![传感器数据采集与联动链路](sensor-data-pipeline.svg) `sensor-data-pipeline.svg` | `1440x820` | 传感器外设、驱动读取、缓存数据源、外设执行、显示和 MQTT 上报 |
| ![GPIO、ADC、PWM 接线校验图](gpio-adc-pwm-wiring-check.svg) `gpio-adc-pwm-wiring-check.svg` | `1440x800` | GPIO、ADC、PWM 类模块的引脚、电平、量程、供电和负载验证 |
| ![构建与发布产物地图](release-artifact-map.svg) `release-artifact-map.svg` | `1440x800` | 源码、配置、Web 资源、构建环境、中间产物和交付包关系 |
| ![外设类型选型地图](peripheral-type-selection-map.svg) `peripheral-type-selection-map.svg` | `1440x820` | 按开关量、模拟量、总线设备、RS485 和显示输出选择外设类型 |
| ![Modbus RTU 调试闭环](modbus-debug-loop.svg) `modbus-debug-loop.svg` | `1440x820` | Modbus 物理层、串口参数、从站映射、稳定轮询和规则联动排查 |
| ![显示输出管道](display-output-pipeline.svg) `display-output-pipeline.svg` | `1440x800` | 传感器、系统状态或平台命令到 OLED、TM1637、LCD 的显示输出路径 |
| ![按键与事件触发链路](event-trigger-flow.svg) `event-trigger-flow.svg` | `1440x800` | GPIO 输入、设备事件、传感器事件到规则匹配和动作执行 |
| ![典型场景自动化闭环](scenario-automation-loop.svg) `scenario-automation-loop.svg` | `1440x820` | 采集、判断、执行、上报和复核的场景配置闭环 |
| ![外设执行引擎内部结构](periph-exec-engine-internals.svg) `periph-exec-engine-internals.svg` | `1440x820` | Manager、Scheduler、Executor、WorkerPool、事件和资源守卫边界 |
| ![触发器与动作选择地图](trigger-action-selection-map.svg) `trigger-action-selection-map.svg` | `1440x820` | 平台、定时、事件、轮询触发与控制、采集、输出、系统动作选择 |
| ![命令脚本执行流程](command-script-execution-flow.svg) `command-script-execution-flow.svg` | `1440x800` | 命令脚本文本、解析校验、逐条执行、安全保护和日志反馈 |
| ![文档学习路径地图](docs-learning-path-map.svg) `docs-learning-path-map.svg` | `1440x820` | 新手、集成、开发和发布角色的文档阅读路径 |
| ![核心服务边界图](core-service-boundary-map.svg) `core-service-boundary-map.svg` | `1440x820` | Framework、Peripheral、PeriphExec、Network、Protocols、Security 和系统服务边界 |
| ![外设配置数据模型](peripheral-config-data-model.svg) `peripheral-config-data-model.svg` | `1440x820` | Web 表单、JSON、校验、运行时驱动、规则消费者和日志链路 |
| ![Modbus RTU 寄存器映射地图](modbus-register-map.svg) `modbus-register-map.svg` | `1440x820` | RTU 帧、四类寄存器区、功能码、0/1 基地址和 FastBee API 字段映射 |
| ![真实设备 API 冒烟测试流程](api-smoke-test-flow.svg) `api-smoke-test-flow.svg` | `1440x820` | 设备准备、认证、核心接口、版本边界、失败即停和报告归档 |
| ![长期稳定性测试反馈闭环](soak-test-feedback-loop.svg) `soak-test-feedback-loop.svg` | `1440x820` | Soak 循环、接口矩阵、CSV 指标、趋势判定、归因修复和复测 |
| ![硬件资料覆盖矩阵](hardware-coverage-matrix.svg) `hardware-coverage-matrix.svg` | `1440x820` | 普中 ESP32 模块到示例、外设类型、Web 配置、规则联动和版本档位的覆盖关系 |
| ![HC-SR04 超声波测距几何](ultrasonic-ranging-geometry.svg) `ultrasonic-ranging-geometry.svg` | `1440x820` | Trig、Echo、声波往返、距离计算、接线和安装排查点 |
| ![优化改进守卫地图](optimization-guardrail-map.svg) `optimization-guardrail-map.svg` | `1440x820` | 问题证据、修复实现、测试覆盖、真机复核和发布观察闭环 |
| ![外设执行规则数据模型](periph-exec-rule-data-model.svg) `periph-exec-rule-data-model.svg` | `1440x820` | PeriphExec 规则、触发器、动作、数据源、持久化和运行态关系 |
| ![两阶段锁执行流程](two-phase-execution-lock-flow.svg) `two-phase-execution-lock-flow.svg` | `1440x820` | 外设执行中规则快照、条件匹配、释锁执行动作和结果收集的并发边界 |
| ![触发器时序对比](trigger-timing-comparison.svg) `trigger-timing-comparison.svg` | `1440x820` | 平台、定时、事件、轮询触发器的输入来源、检查时机、适用场景和风险 |
| ![外设执行规则表单安全检查](rule-builder-safety-checklist.svg) `rule-builder-safety-checklist.svg` | `1440x820` | 规则启用前的基础信息、触发器、动作、结果上报和回滚检查 |
| ![规则脚本模板映射](rule-script-template-map.svg) `rule-script-template-map.svg` | `1440x820` | 输入 JSON、占位符、模板输出、协议方向和调试日志关系 |
| ![命令脚本语法速查](command-script-cheatsheet.svg) `command-script-cheatsheet.svg` | `1440x820` | GPIO、DELAY、PWM、PERIPH、MQTT、LOG 命令和安全边界 |
| ![外设类型参数速查矩阵](peripheral-parameter-matrix.svg) `peripheral-parameter-matrix.svg` | `1440x820` | GPIO、ADC、I2C、UART/RS485、显示、运动控制和虚拟外设的字段与启用检查 |
| ![Modbus API 与功能码映射](modbus-api-function-map.svg) `modbus-api-function-map.svg` | `1440x820` | Modbus REST API、功能码、数据区和页面动作的对应关系 |
| ![PeriphExec API 路由地图](periph-exec-api-route-map.svg) `periph-exec-api-route-map.svg` | `1440x820` | 外设执行页面、REST API、Manager 方法、配置文件和运行结果路径 |
| ![脚本排障决策树](script-debug-decision-tree.svg) `script-debug-decision-tree.svg` | `1440x820` | 规则脚本和命令脚本保存、触发、变量、动作和资源问题排查 |

## 使用建议

- 新手入口优先引用 `fastbee-login.png` 和 `fastbee-dashboard.png`，让读者先建立完整页面印象。
- 部署和验收文档引用仪表盘时，应说明要核对 IP、运行时间、Flash、Heap、LittleFS 和网络状态。
- 移动端截图只用于说明手机巡检体验，不要替代桌面端配置页面截图。
- 流程图适合放在章节开头，用于先建立认知路径；真实页面操作仍应配合系统截图。
- 选型、测试和排障类 SVG 可在多篇文档复用，保持同一套判断口径。
- 协议和外设类 SVG 适合和页面截图成组出现：截图说明入口，流程图说明排查和验证顺序。
- 接线、脚本和产物类 SVG 适合放在长文档开头或关键章节前，用于减少读者在大段参数表之间迷路。
- 场景类 SVG 适合放在示例和场景文档前部，先说明闭环，再展开 JSON 或 Web 操作步骤。
- 内部实现类 SVG 适合放在开发文档开头，用于先厘清模块边界，再进入代码和 API 细节。
- 测试和发布类 SVG 适合同时出现在 `testing.md` 与 `stability-release-checklist.md`，保持验收口径一致。
- 硬件覆盖和接线几何类 SVG 适合和系统截图搭配使用：截图说明页面入口，SVG 说明现场核对方法。
- PeriphExec 规则、触发器、锁流程和安全检查类 SVG 适合同时出现在系统手册、配置指南和开发实现文档中，让用户和开发者共用同一套术语。
- 脚本类 SVG 适合放在语法章节前部，先解释输入、模板、命令和限制，再展开具体示例。
- 参数矩阵和 API 映射类 SVG 适合放在长表格前，先建立查表路径，再进入字段细节。
- 排障决策树适合放在 FAQ 前面，用来帮助读者先归类问题，再阅读具体问答。

更多维护规则见 [文档图片资产维护指南](../image-assets.md)。
