# 项目目录与文件说明

本文档说明 FastBee-Arduino 仓库的主要目录、关键文件和生成产物。后续维护时优先按这里的边界修改，避免把生成文件、源码和发布产物混在一起。

## 顶层目录

| 路径 | 说明 |
|------|------|
| `include/` | C++ 头文件，定义核心框架、网络、外设、系统服务、安全认证和工具类接口 |
| `src/` | 固件源代码实现，按 `include/` 的模块划分组织 |
| `web-src/` | Web 控制台源文件，包含 CSS、页面片段、运行时模块、管理模块和 i18n |
| `data/` | LittleFS 文件系统源数据；`data/www` 是由 `web-src` 构建压缩后的设备端 Web 资源 |
| `scripts/` | 构建、部署、Web 资源处理、测试矩阵和诊断脚本 |
| `test/` | PlatformIO native/host 测试代码、mock 和测试辅助工具 |
| `docs/` | 用户手册、开发指南、外设/协议/场景说明和项目维护文档 |
| `dist/` | 发布固件输出目录，由脚本生成，不应手工维护 |
| `.pio/` | PlatformIO 构建缓存、库依赖、临时文件系统 staging 和测试输出，不应提交 |
| `platformio.ini` | PlatformIO 主配置，定义芯片环境、版本档位、构建标志、依赖库和 extra scripts |
| `README.md`、`README.en.md` | 项目中英文入口说明 |
| `setup-toolchain.ps1` | 本地工具链初始化辅助脚本 |

## 固件源码

### `include/`

| 路径 | 说明 |
|------|------|
| `include/core/` | FastBee 核心框架、芯片能力、外设配置、外设执行、任务和规则相关类型 |
| `include/network/` | 网络管理、Web 配置、路由上下文、请求处理器声明 |
| `include/security/` | 用户、角色、权限、会话和认证管理 |
| `include/systems/` | 文件系统、日志、OTA、NTP、配置、任务等系统服务接口 |
| `include/peripherals/` | 传感器、显示、GPIO、总线等外设抽象接口 |
| `include/utils/` | 通用工具，例如 PSRAM JSON 文档、字符串/响应辅助工具 |

### `src/`

| 路径 | 说明 |
|------|------|
| `src/main.cpp` | 固件启动入口，初始化框架、网络、Web、外设和系统任务 |
| `src/core/` | 框架生命周期、外设管理、外设执行器、规则调度等核心逻辑 |
| `src/network/` | WiFi/AP/STA、Web 服务器、路由注册、请求上下文和网络状态 |
| `src/network/handlers/` | REST API 路由实现，例如系统、网络、外设、批量、日志、文件、用户、角色 |
| `src/security/` | 用户管理、角色权限、登录会话、Token 和 Cookie 兼容逻辑 |
| `src/systems/` | 文件系统、日志、OTA、任务管理、时间同步、系统状态 |
| `src/peripherals/` | 具体外设驱动和传感器读写实现 |

修改 API 时应同时检查 `src/network/handlers/`、Web 前端调用、`scripts/device-api-test-matrix.json` 和相关文档。

## Web 控制台

| 路径 | 说明 |
|------|------|
| `web-src/css/` | Web 控制台源 CSS，统一控件样式在这里维护 |
| `web-src/js/` | 启动、请求治理、状态管理、通知、模块加载等公共运行时代码 |
| `web-src/modules/runtime/` | 设备运行态页面模块，例如仪表盘、网络、外设、外设执行 |
| `web-src/modules/admin/` | 管理页面模块，例如用户、角色、规则脚本 |
| `web-src/pages/` | HTML 页面和弹窗片段 |
| `web-src/i18n/` | 多语言资源 |
| `data/www/` | 构建后写入设备 LittleFS 的压缩资源，通常由脚本生成 |

Web 源码修改后，使用 `scripts\build-web-modules.js`、`scripts\gzip-www.js` 或 `scripts\deploy.ps1` 生成设备端资源。不要只改 `data/www/*.gz` 而不更新 `web-src` 源文件。

## 配置数据

| 路径 | 说明 |
|------|------|
| `data/config/peripherals.json` | 默认外设配置和模板 |
| `data/config/users.json` | 默认用户、角色、安全配置和会话策略 |
| `data/www/` | 设备端 Web 静态资源和页面 |

默认配置必须能在 `lite`、`standard`、`full` 三种档位启动。新增 full-only 能力时要确认 lite/standard 不会引用不存在的页面或 API。

## 脚本目录

详见 [`scripts/README.md`](../scripts/README.md)。核心分组如下：

| 分组 | 关键文件 |
|------|----------|
| 测试矩阵 | `test-all.ps1`、`device-api-test-matrix.json`、`smoke-test-device.ps1`、`soak-test-device.ps1` |
| 部署发布 | `deploy.ps1`、`build-all-artifacts.ps1`、`collect-artifacts.ps1`、`fastbee-artifacts.py` |
| Web 构建 | `build-web-modules.js`、`gzip-www.js`、`generate-sw-manifest.js`、`web-smoke-test.js` |
| 质量检查 | `check-utf8-text.js`、`validate-i18n.js`、`web-profile.js`、`web-asset-report.js` |
| 诊断工具 | `serial-diagnostics.py`、`kill-stale-processes.py` |

## 测试目录

详见 [`test/README.md`](../test/README.md)。

| 文件 | 覆盖重点 |
|------|----------|
| `test_main.cpp` | 测试入口和基础框架 |
| `test_web_api.cpp` | Web API 行为 |
| `test_network_config.cpp` | 网络配置 |
| `test_mqtt_protocol.cpp` | MQTT 配置和协议 |
| `test_system_stability.cpp` | 稳定性和边界条件 |
| `test_e2e_scenarios.cpp` | 组合场景 |

真实设备 API 测试不放在 `test/`，而是通过 `scripts\smoke-test-device.ps1` 和 `scripts\soak-test-device.ps1` 执行。

## 文档目录

| 路径 | 说明 |
|------|------|
| `docs/overview.md` | 项目概述 |
| `docs/quick-start.md` | 快速开始 |
| `docs/deployment.md` | 部署与版本验证 |
| `docs/testing.md` | 测试矩阵和发布前验证 |
| `docs/project-structure.md` | 本文件，目录和关键文件说明 |
| `docs/architecture.md` | 系统架构 |
| `docs/core-framework.md` | 核心框架说明 |
| `docs/development-guide.md` | 开发指南 |
| `docs/user-manual.md` | 用户手册 |
| `docs/examples/` | 外设和场景示例 |
| `docs/peripherals/` | 外设配置参考 |
| `docs/periph-exec/` | 外设执行规则、触发器和动作说明 |
| `docs/protocols/` | MQTT、Modbus 等协议说明 |
| `docs/system/` | 系统管理、版本档位和配置说明 |

## 生成产物与清理规则

| 路径/文件 | 类型 | 处理方式 |
|-----------|------|----------|
| `.pio/` | PlatformIO 构建缓存 | 可删除后重新生成 |
| `dist/firmware/` | 发布固件输出 | 由 `build-all-artifacts.ps1` 生成 |
| `data/www/*.gz` | Web 压缩资源 | 由 Web 构建脚本生成，发布前需要保留 |
| `scripts/*.pyc` | Python 运行缓存 | 可删除，不应提交 |
| `compile_commands.json` | 编译数据库 | 可由工具生成，用于 IDE/clangd |
| `upload_log.txt` | 本地烧录日志 | 调试产物，不应作为源码维护 |

## 修改同步原则

| 修改类型 | 需要同步检查 |
|----------|--------------|
| 新增 API | Web 调用、`scripts/device-api-test-matrix.json`、相关文档 |
| 修改认证/会话 | smoke/soak、浏览器登录、用户/角色页面 |
| 修改网络/4G/以太网 | 网络页面、MQTT 状态、设备冒烟矩阵 |
| 修改 Web 样式 | `web-src/css/main.css`、相关页面截图和 Web smoke |
| 修改版本裁剪 | `platformio.ini`、`docs/system/firmware-version-profiles.md`、发布产物 |
| 修改默认配置 | `data/config/*.json`、首次启动和恢复默认配置流程 |
