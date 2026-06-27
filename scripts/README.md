# scripts 目录说明

`scripts` 目录放置项目构建、部署、Web 资源处理、设备冒烟测试和维护诊断脚本。日常开发优先使用下面的统一入口，单用途脚本只在定位问题时使用。

> **跨平台提示**：以下 PowerShell 脚本支持 Windows / Linux / macOS，后两者需安装 [PowerShell Core](https://learn.microsoft.com/powershell/scripting/install/installing-powershell) 并使用 `pwsh` 替代 `powershell`。

## 推荐入口

| 脚本 | 用途 | 常用命令 |
|------|------|----------|
| `doctor.ps1` | 检查 PlatformIO、Node、Git、串口和 native 工具链，部署或测试前优先运行 | `powershell -ExecutionPolicy Bypass -File scripts\doctor.ps1 -Port COM6` |
| `test-all.ps1` | 统一测试矩阵入口，串起静态检查、native 测试、全版本编译、产物构建、设备冒烟和稳定性测试 | `powershell -ExecutionPolicy Bypass -Command ".\scripts\test-all.ps1 -Checks static,build"` |
| `deploy.ps1` | 构建并烧录指定 PlatformIO 环境的固件和 LittleFS 文件系统 | `powershell -ExecutionPolicy Bypass -File scripts\deploy.ps1 -Env esp32s3-F16R8 -Port COM6` |
| `build-all-artifacts.ps1` | 为所有版本生成发布固件包和 `manifest.json` | `powershell -ExecutionPolicy Bypass -File scripts\build-all-artifacts.ps1 -CleanOutput` |
| `flash-release.ps1` | 直接烧录 `dist\firmware\all-latest` 中的合并发布镜像，适合量产和现场恢复 | `powershell -ExecutionPolicy Bypass -File scripts\flash-release.ps1 -Env esp32s3-F16R8 -Port COM6` |
| `smoke-test-device.ps1` | 对已上线设备执行一轮 API 冒烟测试 | `powershell -ExecutionPolicy Bypass -File scripts\smoke-test-device.ps1 -BaseUrl http://192.168.5.116 -Profile full` |
| `soak-test-device.ps1` | 对已上线设备循环执行 API 稳定性测试并输出 CSV | `powershell -ExecutionPolicy Bypass -File scripts\soak-test-device.ps1 -BaseUrl http://192.168.5.116 -Profile full -Rounds 100` |

`deploy.ps1` 默认会清理继承的 `PLATFORMIO_DATA_DIR`，避免上传旧的 LittleFS staging；只有需要指定外部 PlatformIO 数据目录时才传入 `-DataDir <path>`。

## 测试矩阵

`device-api-test-matrix.json` 是设备 API 冒烟和稳定性测试共用的检查清单。新增 API 或改变版本裁剪策略时，应同步维护这个文件。

矩阵字段说明：

| 字段 | 说明 |
|------|------|
| `name` | 检查项名称，出现在测试报告中 |
| `method` | HTTP 方法 |
| `path` | API 路径 |
| `profiles` | 适用版本：`lite`、`standard`、`full` |
| `auth` | 是否需要登录后的 Bearer Token |
| `type` | 特殊检查类型：`multi-session`、`bearer-over-cookie`、`expect-unavailable`、`expect-error` |
| `jsonBody` | POST/PUT 请求体 |
| `allowedStatuses` | `expect-unavailable` 或 `expect-error` 允许的 HTTP 状态码 |

当前矩阵覆盖三类检查：

- **基础可用性**：登录、系统健康、系统指标、设备配置、网络状态、MQTT 状态、协议配置、外设、外设执行、批量请求。
- **版本功能面**：Standard/Full 的 Modbus，Full 的文件系统、日志、OTA、RuleScript、用户、角色和权限。
- **版本边界**：Lite/Standard 下 Full-only 接口应返回 404/405/501，缺少必要参数的接口应返回 400，避免功能裁剪后误暴露或错误成功。

## 构建与发布

| 脚本 | 说明 |
|------|------|
| `collect-artifacts.ps1` | 收集指定环境的固件产物 |
| `fastbee-artifacts.py` | PlatformIO extra script，生成合并固件和发布文件 |
| `platformio_ar_wrapper.py`、`platformio-ar-rename-workaround.py` | Windows/PlatformIO ar 工具兼容处理，供构建流程调用 |

## Web 资源处理

| 脚本 | 说明 |
|------|------|
| `build-app-bundle.js`、`build-admin-bundle.js`、`build-web-assets.js`、`build-web-modules.js` | 从 `web-src` 构建 `data/www` 资源 |
| `gzip-www.js` | 按版本档位裁剪并压缩 Web 静态资源 |
| `generate-sw-manifest.js` | 生成 Service Worker 资源清单 |
| `minify-js.js` | 轻量 JS 压缩器 |
| `validate-i18n.js` | 校验中英文 i18n 键完整性 |
| `validate-config-defaults.js` | 校验默认外设模板、执行规则和版本裁剪后的引用安全性；追加 `--staging-root .pio\fs-staging --latest-only` 可检查刚生成的 LittleFS staging |
| `web-smoke-test.js` | 校验生成后的 Web 资源引用和启动资源 |
| `web-profile.js` | 检查不同版本 Web profile 的裁剪结果 |
| `web-asset-report.js` | 统一 Web 资源体积报告；`--source` 查看源文件体积，`--json` 输出 JSON |

## 维护诊断

| 脚本 | 说明 |
|------|------|
| `check-utf8-text.js` | 检查文本文件 UTF-8、BOM 和替换字符 |
| `validate-mqtt-ntp-lifecycle.js` | 校验启动 NTP 同步事件和 MQTT 启用后的自动连接状态链路 |
| `kill-stale-processes.py` | 清理本地调试残留进程 |
| `serial-diagnostics.py` | 串口日志抓取和可选硬复位诊断，替代旧的串口嗅探脚本 |

## 已合并的旧脚本

以下历史脚本已经合并到统一入口，不再单独保留：

| 旧脚本 | 替代命令 |
|------|----------|
| `sniff_serial.py` | `python scripts\serial-diagnostics.py COM6 --duration 30` |
| `reset_and_sniff.py`、`monitor_s3_reset.py` | `python scripts\serial-diagnostics.py COM6 --reset --chip esp32s3 --duration 45` |
| `list_sizes.ps1`、`size_chunks_gz.js`、`size_each_minified.js` | `node scripts\web-asset-report.js` 或 `node scripts\web-asset-report.js --source` |

`*.pyc` 属于 Python 运行时缓存，不应提交；如本地出现可直接删除。
