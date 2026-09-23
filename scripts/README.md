# scripts 目录说明

`scripts` 目录放置项目构建、部署、Web 资源处理、设备冒烟测试和维护诊断脚本。日常开发优先使用下面的统一入口，单用途脚本只在定位问题时使用。

> **跨平台提示**：以下 PowerShell 脚本支持 Windows / Linux / macOS，后两者需安装 [PowerShell Core](https://learn.microsoft.com/powershell/scripting/install/installing-powershell) 并使用 `pwsh` 替代 `powershell`。

## 推荐入口

| 脚本 | 用途 | 常用命令 |
|------|------|----------|
| `doctor.ps1` | 检查 PlatformIO、Node、Git、串口和 native 工具链，部署或测试前优先运行 | `powershell -ExecutionPolicy Bypass -File scripts\doctor.ps1 -Port <serial-port>` |
| `npm test` | 日常主机回归入口；运行静态门禁、Node/Python 单元测试、配置/UI/内存契约，与 CI 使用同一套清单 | `npm test` |
| `test-all.ps1` | 发布前统一入口；默认在主机回归后继续运行 native 测试和九个固件环境编译，真机测试必须显式提供 STA 地址 | `powershell -ExecutionPolicy Bypass -File scripts\test-all.ps1` |
| `deploy.ps1` | 构建并烧录指定 PlatformIO 环境的固件和 LittleFS 文件系统 | `powershell -ExecutionPolicy Bypass -File scripts\deploy.ps1 -Env <env> -Port <serial-port>` |
| `build-all-artifacts.ps1` | 为所有版本生成发布固件包和 `manifest.json` | `powershell -ExecutionPolicy Bypass -File scripts\build-all-artifacts.ps1 -CleanOutput` |
| `flash-release.ps1` | 直接烧录 `dist\firmware\all-latest` 中的合并发布镜像，适合量产和现场恢复 | `powershell -ExecutionPolicy Bypass -File scripts\flash-release.ps1 -Env <env> -Port <serial-port>` |
| `smoke-test-device.ps1` | 对已上线设备执行一轮 API 冒烟测试 | `powershell -ExecutionPolicy Bypass -File scripts\smoke-test-device.ps1 -BaseUrl http://<device-ip> -Profile unified -RequireNetworkConnected` |
| `device-web-smoke.ps1` | 按间隔检查发布清单中的全部 Web 资源并输出 CSV，可选强制清单与延迟门槛 | `powershell -ExecutionPolicy Bypass -File scripts\device-web-smoke.ps1 -BaseUrl http://<device-ip> -RequireManifest -Rounds 2 -DelayMs 1000 -TimeoutSec 15 -MaxElapsedMs 5000` |
| `test-screen-panel-device.mjs` | 实物设备的面板/历史/远程资源能力、鉴权与 API 契约检查；可选保存相同布局验证持久化和版本冲突，不派发外设动作 | `node scripts/test-screen-panel-device.mjs --url=http://<device-ip> --report=artifacts/panel-device.json` |
| `test-screen-panel-browser-device.mjs` | 实机浏览器导航、配置草稿和桌面/手机截图检查；默认只读。显式 `--exercise-switch` 仅在未绑定显示外设时临时启用面板，最终恢复原开关，不保存布局或执行控制 | `node scripts/test-screen-panel-browser-device.mjs --url=http://<device-ip> --output=artifacts/panel-browser` |
| `soak-test-device.ps1` | 对已上线设备循环执行 API 稳定性测试并输出 CSV | `powershell -ExecutionPolicy Bypass -File scripts\soak-test-device.ps1 -BaseUrl http://<device-ip> -Profile unified -Rounds 100` |
| `run-device-fleet-tests.ps1` | 对多台设备执行 STA/mDNS/API/资源汇总；支持 JSON 编队配置 | `powershell -ExecutionPolicy Bypass -File scripts\run-device-fleet-tests.ps1 -DeviceConfig <local-fleet-config.json> -BrowserTests:$false` |
| `long-term-device-monitor.ps1` | 严格只读的串口、STA HTTP GET、重启和覆盖缺口长期监测；支持 live fleet、逐设备 P95/P99、curl 18/28/HTTP 503、连续失败/恢复及断点续跑 | 见下方 24/72 小时严格只读命令 |
| `fleet-stability-monitor.mjs` | Playwright 登录、全部可见菜单、资源请求、内存趋势和响应时间长期验收；支持部署后 live fleet JSON。它会登录和注销，因此不属于零 POST 的严格只读监控 | 见下方 8 小时 Web 功能验收命令 |
| `e2e-ota-test.js` | OTA 升级端到端测试：状态查询/URL 校验/begin-cancel 会话；追加 `--firmware fw.bin` 执行完整升级与重启恢复验证 | `node scripts/e2e-ota-test.js --url http://<device-sta-ip> --user admin --pass 123456 --allow-begin` |

`deploy.ps1` 默认会清理继承的 `PLATFORMIO_DATA_DIR`，避免上传旧的 LittleFS staging；只有需要指定外部 PlatformIO 数据目录时才传入 `-DataDir <path>`。

`device-fleet.example.json` 中的五组 IP 与串口是当前实验室拓扑示例，不是设备的固定分配，也不声明菜单验收意图。使用 `run-device-fleet-tests.ps1`、`long-term-device-monitor.ps1 -FleetFile` 或 `fleet-stability-monitor.mjs --device-config` 前先生成本次部署的 live fleet 文件；每项必须包含 `label`、`ip`、`profile`、`domain`、`serial`。可选的 `expectedMenus` 是非空菜单名数组：配置后作为硬验收契约，即使设备因 `no-memory` 降级并隐藏菜单也会判定失败；省略时继续按 `/api/system/status` 的运行时功能状态推导菜单，仅用于兼容受限设备、旧清单或探索性监测。正式验收必须根据部署前已确认的 `features.json` 配置意图或部署清单显式生成 `expectedMenus`；不能在故障发生后从已经降级的运行态反推期望集合。用户明确关闭可选菜单时，契约应只列出实际要求的 7 个核心菜单，不能按 S3 等硬件型号强制写成 11 个。

### 8 小时 Web 功能验收

每次使用新的输出目录；`--min-duration-hours 8` 会按每台设备首末样本的实际时间窗执行门禁，不能用较多但不足 8 小时的周期代替。命令完成后只有退出码为 0 且 `fleet-stability-verdict.json` 为 `PASS` 才算通过：

```powershell
$runDir = "artifacts/final-stability-{0}" -f (Get-Date -Format "yyyyMMdd-HHmmss")
$fleetConfig = "artifacts/device-acceptance/fleet-live.json"
node test/browser/fleet-stability-monitor.mjs `
  --device-config $fleetConfig `
  --interval 180 --max-cycles 161 --min-duration-hours 8 `
  --report-every 8 --cycle-timeout 420 --output-dir $runDir
```

严格门禁要求 live fleet 中选定的设备全时段在线、浏览器每周期完整覆盖、登录和期望菜单成功，无重试、HTTP/控制台/页面错误、WebGuard 503、任何 `no-memory` 功能降级或异常重启；同时检查内部 DRAM、最大连续块、响应 p95 及其首尾趋势。报告分别保留周期前、周期后的 `no-memory` 历史峰值，并只把最后一个周期的采样标为“最新/最终”；最后周期缺采样时明确显示“无采样”，不会拿较早值冒充当前值。配置包含非空 `domain` 时监测器优先验证该域名，并以同项 `ip` 作为回退；任何域名失败后回退都会计入严格门禁。该命令本身是验收方法，不代表设备已经通过真机测试。

### 24/72 小时严格只读验收

该入口只发送公开 HTTP GET，并以 `--dtr 0 --rts 0` 读取串口，不登录、不注销、不执行外设动作、不重启、不修改配置。`summary.json` 按设备记录检查/周期成功率、P95/P99、HTTP 状态码、curl 18/28、HTTP 503、连续失败和恢复。中断后用原 live fleet 和运行目录续跑；停机间隔不计入要求的监控活跃时长。

```powershell
$fleetConfig = "artifacts/current-device-fleet-20260920.json"
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\long-term-device-monitor.ps1 `
  -DurationHours 24 -IntervalSeconds 60 -HttpTimeoutSec 10 -SerialSeconds 8 `
  -FleetFile $fleetConfig -OutputDir artifacts\long-term-monitor-final-24h

# 中断后从 state.json 对应的运行目录继续，不新建一轮：
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\long-term-device-monitor.ps1 `
  -ResumeRunDir artifacts\long-term-monitor-final-24h\<run-id> `
  -FleetFile $fleetConfig

# 24 小时 summary.json 为 completed、durationComplete=true、exitCode=0 后再启动 72 小时：
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\long-term-device-monitor.ps1 `
  -DurationHours 72 -IntervalSeconds 60 -HttpTimeoutSec 10 -SerialSeconds 8 `
  -FleetFile $fleetConfig -OutputDir artifacts\long-term-monitor-final-72h
```

## 测试矩阵

日常代码修改先运行 `npm test`。准备合并或发布时运行 `test-all.ps1` 的默认矩阵；它会复用同一个 `npm test` 清单，再执行 native 与全部固件环境构建。`device-smoke`、`device-soak` 和浏览器真机检查不会使用默认设备地址，必须通过 `-BaseUrl http://<device-sta-ip>` 显式指定 STA 地址，并拒绝 `192.168.4.1`。

`device-api-test-matrix.json` 是设备 API 冒烟和稳定性测试共用的检查清单。新增 API 或改变版本裁剪策略时，应同步维护这个文件。

矩阵字段说明：

| 字段 | 说明 |
|------|------|
| `name` | 检查项名称，出现在测试报告中 |
| `method` | HTTP 方法 |
| `path` | API 路径 |
| `profiles` | 检查适用的 API 矩阵；9 个正式发布环境使用 `unified`，旧 profile 值只保留给有针对性的兼容测试 |
| `auth` | 是否需要登录态；设备默认使用 HttpOnly `sessionId` Cookie，同时兼容 `session` 和 Authorization Bearer |
| `type` | 特殊检查类型：`multi-session`、`bearer-over-cookie`、`expect-unavailable`、`expect-error`、`capability-optional` 等 |
| `jsonBody` | POST/PUT 请求体 |
| `allowedStatuses` | `expect-*` 或 `capability-optional` 允许的 HTTP 状态码 |
| `allowedMessages` | 可选功能接口 `message`/`error` 字段允许的精确错误文案；与 `allowedStatuses` 同时匹配才视为预期禁用 |

当前矩阵覆盖三类检查：

- **基础可用性**: 登录、系统健康、系统指标、设备配置、网络状态、MQTT 状态、协议配置、外设、外设执行、批量请求。
- **运行时开关功能**: Modbus、RuleScript、用户权限等功能在功能配置页按需启用。
- **API 行为一致性**: 功能未启用时接口返回 404/405/501，缺少必要参数时返回 400。

## 构建与发布

| 脚本 | 说明 |
|------|------|
| `collect-artifacts.ps1` | 收集指定环境的固件产物 |
| `fastbee-artifacts.py` | PlatformIO extra script，生成合并固件和发布文件 |
| `platformio_ar_wrapper.py`、`platformio-ar-rename-workaround.py` | Windows/PlatformIO ar 工具兼容处理，供构建流程调用 |

## Web 资源处理

`data/www/assets/logo.png` 和 `data/www/assets/favicon.ico` 是受 Git 跟踪的源资产，属于 `data/www` 生成目录中的例外。隔离构建或复制源码时必须保留这两个文件；发布前还应校验页面图片引用与资源清单对应，避免登录页和侧边栏出现图片 404。

| 脚本 | 说明 |
|------|------|
| `build-app-bundle.js`、`build-admin-bundle.js`、`build-web-modules.js` | 从 `web-src` 生成 Web 模块；HTML/JS/CSS 源文件仍只在 `web-src` 维护 |
| `build-web-assets.js` | 生成完整的 gzip-only `data/www` 发布资源，不组装或上传 LittleFS staging |
| `gzip-www.js` | 压缩 Web 静态资源并组装 LittleFS staging；`data/www` 和 staging 中的 HTML/JS/CSS 仅保留 `.gz`，默认不访问设备，只有显式传入 `--upload --env=<环境>` 才烧录 |
| `generate-sw-manifest.js` | 生成 Service Worker 资源清单 |
| `minify-js.js` | 轻量 JS 压缩器 |
| `validate-i18n.js` | 校验中英文 i18n 键完整性 |
| `validate-config-defaults.js` | 校验默认外设模板、执行规则和环境编译裁剪后的引用安全性 |
| `web-smoke-test.js` | 校验生成后的 Web 资源引用和启动资源 |
| `test-gzip-www-contract.js` | 校验 `web-src` HTML 与发布 gzip 一一对应，并确保发布目录和 LittleFS staging 不含原始 HTML/JS/CSS |
| `web-profile.js` | 统一 Web 按目标实际编译能力生成资源；运行时可选菜单按功能的 `active` 状态显示 |
| `web-asset-report.js` | 统一 Web 资源体积报告；`--source` 查看源文件体积，`--json` 输出 JSON |

## 维护诊断

| 脚本 | 说明 |
|------|------|
| `check-utf8-text.js` | 检查文本文件 UTF-8、BOM 和替换字符 |
| `validate-mqtt-ntp-lifecycle.js` | 校验启动 NTP 同步事件和 MQTT 启用后的自动连接状态链路 |
| `kill-stale-processes.py` | 补齐工具链 PATH、应用 C6 OneWire 兼容补丁并诊断当前构建目录的锁定文件；不自动终止进程 |
| `serial-diagnostics.py` | 串口日志抓取和可选硬复位诊断，替代旧的串口嗅探脚本 |
| `test-http-device-latency.mjs` | 不登录、不改配置，串行读取健康状态、小页面、CSS 和能力接口，区分 TCP 连接、首字节和正文耗时；`--url=http://<device-ip> --rounds=3 --gap=1500 --output=artifacts/http-latency.json`。健康快照不能替代串口对瞬时内存压力的观测 |
| `read-device-flash-safe.py` | 仅读取完整 Flash；针对串口丢包使用单帧窗口，校验两次 MD5 后才保存，拒绝覆盖已有备份 |
| `extract-device-flash.py` | 从完整备份验证分区并提取 LittleFS，保留原配置用于部署；不会格式化备份或覆盖已有提取目录 |

面板实测脚本从 `FASTBEE_USERNAME` / `FASTBEE_PASSWORD` 读取凭据，默认沿用开发测试账号。显式传入 `--exercise-config` 才写入相同布局并检查持久化、无效配置拒绝及过期版本 `409`；该模式要求设备已开启开发者模式。报告不保存密码、登录令牌或配置内容。Flash 备份和提取文件可能含有真实网络/API 密钥，应仅保存在私有的 `artifacts` 目录。

## 已合并的旧脚本

以下历史脚本已经合并到统一入口，不再单独保留：

| 旧脚本 | 替代命令 |
|------|----------|
| `sniff_serial.py` | `python scripts\serial-diagnostics.py <serial-port> --duration 30` |
| `reset_and_sniff.py`、`monitor_s3_reset.py` | `python scripts\serial-diagnostics.py <serial-port> --reset --chip esp32s3 --duration 45` |
| `list_sizes.ps1`、`size_chunks_gz.js`、`size_each_minified.js` | `node scripts\web-asset-report.js` 或 `node scripts\web-asset-report.js --source` |

`*.pyc` 属于 Python 运行时缓存，不应提交；如本地出现可直接删除。
