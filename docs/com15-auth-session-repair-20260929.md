# COM15 八小时验收失败后的认证修复（2026-09-29）

## 2026-09-29 实机部署更新

用户明确授权“部署到com15”后，已完成 COM15 应用固件更新。以下首次本地阶段及原八小时失败记录继续保留，不用新部署覆盖旧验收。

- 部署报告：`artifacts/com15-web-configuration-20260929/auth-session-repair/deployment-20260929/deployment.json`。本次独立证据为 `written=true`、`verified=true`，仅写入 app0（0x10000），应用 SHA256 为 `8983c175a1513157f7bf35f5bee05c9e3ddd16bcf55818e6946cf7253242391a`。
- 串口芯片/MAC 与 HTTP 身份均核对为 COM15、ESP32-S3、`14:C1:9F:C1:52:D8`、`192.168.5.19`。未操作 COM8。共享固件锁在串口操作结束后正常释放。
- Flash 中应用写入区域 MD5 校验通过；整个文件系统前后 MD5 同为 `cf21fc320f0304248f63c559071c6f8d`。Web、NVS、分区表、OTA 元数据及引导程序保留，未重刷文件系统。
- `postcheck/report.json` 为 PASS：一次登录后 8 个成功认证响应均返回 Cookie 续期；同一 Cookie 身份保持不变，实际过期时间前移。设备、外设、规则、协议及用户配置五组核对一致（排除已说明的运行状态/登录时间字段）。光电规则保持启用。
- 首次刷写前登录曾发生 3 秒建连超时；该次尚未写入、未重启。被动串口确认仍连接原 IP 后，第二次预检成功并按正常重启流程提交配置。首次失败及重启期间的连接失败全部保留。
- 五分钟设备/浏览器只读检查已于北京时间 2026-09-29 16:05:47 后完成，目录为 `deployment-20260929/smoke-5m`。设备实测 301.406 秒、11/11 完整样本；浏览器实测 300.086 秒、6/6 完整样本。两端 `durationComplete=true`、`coveragePassed=true`、`stabilityStatus=PASS`；无 HTTP/传输错误、采样缺口，设备无重启/Modbus 缺值/继电器读数错误，浏览器无页面或显示错误。
- 湿度低电流使两端 `measurementStatus=FAIL`、总 `status=FAIL`，不能将短时稳定性通过表述为整机通过。六份观察快照与清单一致，原八小时五份启动/报告/采样文件哈希未变，短测进程均已退出。新的跨一小时及独立八小时验收尚未执行。汇总证据见 `deployment-20260929/summary.json`。
- 既有传输超时、湿度低电流、温度准确度、物理灯光及光电响应验收仍未关闭。

## 首次本地修复阶段结论（部署前记录）

本轮执行本地源码修复、离线回归和编译，不访问设备。COM15 目标仍为 `192.168.5.19`、MAC `14:C1:9F:C1:52:D8`；`192.168.5.18` 是 COM8，未访问。原 `soak-8h` 报告、采样、运行脚本快照及失败历史保留；原自动跟进 `com15` 已删除，不因本地测试重启。

原设备观察实际 28800.047 秒，119/961 次完整；浏览器实际 28800.0042234 秒，60/481 次完整。两端 durationComplete=true，但 coveragePassed=false，最终均为 FAIL。设备的 842 次 transportErrors 包含 832 次 HTTP 401 和 10 次 CONNECT_TIMEOUT，不能称为 842 次 TCP 超时。

## 已复现并修复的认证缺陷

`AuthManager::verifySession(..., true)` 随有效请求更新 lastAccessTime/expireTime，服务端实行活动续期；原 `AuthRouteHandler::loginTask` 只在登录时发送一次 `Set-Cookie; Max-Age=3600`。之后响应不续 Cookie。持续访问时服务端会话仍有效，但浏览器及 Python requests 的 CookieJar 在一小时后停止发送 Cookie，后续请求得到 401。

离线测试提取真实 C++ 认证函数、登录 Cookie 构造代码并编译执行，修复前稳定出现：

```text
Active cookie expired at 3600 seconds while server session remained valid
```

这证明源码缺陷足以解释本次两端在各自登录约一小时后失效的规律；原采样未记录当时发出的 Cookie 是否存在，不能用离线复现排除同一时间的其他设备故障或替代部署后验证。

本轮变更：

- 在全局准入中间件调用路由之后、同步响应发出之前补发当前有效 Cookie，沿用既有 cookieMaxAge、HttpOnly、Secure、SameSite=Strict、Path；服务端闲置超时配置不变，无额外保活/自动登录。
- 仅在该请求已通过认证、响应成功且未发出时处理；不覆盖路由已有的 Set-Cookie；登录、注销、改密、401/403/429/503、无响应和 bearer-only 请求不签发续期 Cookie。补发前复查有效性，不复活撤销或过期会话。
- 续 Cookie 的响应标记 Cache-Control=no-store，避免缓存凭证响应。
- 认证与注销使用配置的 Cookie 名及已验证身份；Cookie 解析按完整名称匹配，防止 `other_sessionId` 等后缀名称遮蔽正确 Cookie。显式 Bearer 的优先级保留。
- 测试另外复现了 uint32 millis 回绕时误过期，以及 updateSessionAccessTime 可复活已过期会话的问题。改用无符号时间差，触碰会话统一经 verifySession 验证。二者不是本次一小时失效的解释。

涉及文件：

- `src/security/AuthManager.cpp`、`include/security/AuthManager.h`
- `src/network/WebHandlerContext.cpp`、`include/network/WebHandlerContext.h`
- `src/network/WebConfigManager.cpp`、`src/network/handlers/AuthRouteHandler.cpp`
- `test/unit/test_auth_cookie_lifecycle.py`、`test/unit/cpp/auth_cookie_lifecycle_harness.cpp`

## 连接超时：保留未关闭状态

十次失败均为 requests 建连阶段 3 秒超时；首次在 2026-09-29 05:02:07（北京时间），早于 Cookie 失效。该次前后有效 uptime 从 1846 到 1905，无回退，浏览器同窗有完整样本。后九次发生于持续 401 期间，没有新有效 uptime，不能推断没有重启。

119 份有效设备样本的 heapFree 为 43916–50084，heapMaxAlloc 恒为 30708，guardLevel 均 NORMAL。这只覆盖仍有有效采样的时间窗口，不能排除瞬态资源问题，更不能推广到失联后约七小时。原证据不足以把超时归因于 Wi-Fi、网络丢包、TCP 连接池或固件。未修改 AsyncTCP、HTTP 超时、连接上限或自动恢复阈值。

工作区 `scripts/test-device-soak.py` 增加下次获授权验收使用的脱敏请求诊断：请求耗时、HTTP 状态、失败阶段、是否实际携带 Cookie/Authorization，以及是否收到 Set-Cookie。仅记录布尔标志，不保存凭证值或 URL 查询参数；不增加请求、不重试、不重新登录、不改变失败计数或覆盖率判定。旧观察运行快照保持不变。

仍保留旧 com15-onboard-relay 失败、本轮首版 DNS/SNTP 核心锁崩溃，以及 after-dns-guard（6 项资源后）和 after-dns-guard-accepted（49 项资源后）的连接超时；目录名称不代表 PASS。

## 湿度测量：需要现场证据

119 个有效采样窗口内，土壤湿度全部 quality=low，pinVoltageMv 为 586–599 mV。按本轮保存配置的 150 Ω、gain=1、offset=0 换算为约 3.907–3.993 mA，低于配置的 4 mA 下限。API 的 value=null、valueValid=false 及页面 `--` 是应保留的无效提示，不改成 0 或强行 PASS。温度这 119 份样本质量有效，但准确性仍待标定，失去认证后无新测量证据。

后续现场闭环应先记录湿度探头供电/信号、采样电阻实测值及 ADC 端电压，再用已知输入核对量程转换：若实际信号本身低于下限，处理探头/回路；若实测信号有效而 ADC 换算偏低，依据实测点和电路参数修正校准。不得用当前约 3.9 mA 单点反推或编造校准系数。外接继电器读数、GPIO 状态与实体灯光反馈分别验证，do1/NC+COM 及正常光电翻转不强制恢复。

## 本地验证

认证专项 9/9 通过：30/60 秒间隔各九小时虚拟时钟、真实 Python CookieJar 消费生产 Cookie 头、闲置过期、注销身份、撤销/锁定/禁用、错误响应、Bearer 优先级、自定义 Cookie 名、名称边界和 millis 回绕。此为时间模拟，不是新的九小时实机验收。

相关回归 56/56 通过，含原观察器的失败计数/覆盖率、认证/SSE、湿度判定以及新增的 4 项诊断测试。全量单测首轮 867 项：865 通过、1 跳过、1 个电机协议测试因头文件搜索路径编译失败；读取到的当前电机测试已包含该路径，其 3 项在随后专项复测中全部通过。本轮未修改电机代码或电机测试，保留首轮失败日志，不将它改写成一次全绿执行。

PlatformIO 首次在线构建在 Python 依赖初始化时失败，尚未进入 C++ 编译；已改用安装平台明确支持的 `PLATFORMIO_OFFLINE=1` 和本机既有依赖重试，没有修改平台代码、依赖版本或设备配置。最终构建结果、源码前后哈希及日志保存在 `artifacts/com15-web-configuration-20260929/auth-session-repair/repair-report.json` 所列文件中。编译产物仅为待部署候选，不沿用旧应用的 written=true/verified=true。

最终 `esp32s3-F16R8` 离线编译及链接成功（302.17 秒），构建输入前后 SHA256 一致，sourceStable=true。应用镜像 3,725,408 字节，分区容量 7,340,032 字节；静态 RAM 128,632/327,680 字节，不代表设备运行时空闲内存。`esptool image-info` 校验摘要有效。候选文件：

```text
artifacts/com15-web-configuration-20260929/auth-session-repair/firmware-auth-session-repair.bin
SHA256 8983c175a1513157f7bf35f5bee05c9e3ddd16bcf55818e6946cf7253242391a
```

本次未部署：written=false、verified=false（指实机写入/回读）。原运行目录五份启动/报告/采样文件哈希未变，六份观察快照仍与 manifest 一致。新候选来自当前共享工作区，已有其他修复一并保留；部署前仍按本次候选哈希和配置保留流程审查。

## 部署前制定的实机验证门槛（历史规划）

本规划写于首次本地修复后。当时的设备只读限制要求实机操作前取得授权；用户随后已明确授权 COM15 部署，本次应用写入和五分钟检查结果见文首。跨一小时及新的独立八小时验收仍待执行；COM8 不在本次范围内。

1. 保存身份、网络、认证、MQTT 及非 Web 配置；本次认证修复仅需应用固件，保留现有文件系统，不要求重刷 Web。序列化构建/串口操作，核对候选应用哈希；写入和回读必须有本次独立的 written/verified 证据。
2. 跨过原一小时边界验证 Cookie 实际续期；保留一次登录及原有超时，不以自动重登抵消认证失败。
3. 如需定位连接超时，在获授权的新验证中关联同窗网络、Web/TCP 诊断、重启原因和请求阶段，不先调大超时掩盖失败。
4. 设备/浏览器各自从零连续观察至少 28800 秒，durationComplete 和 coveragePassed 都必须为 true；任何错误仍保留，不拼接旧时长。
5. 分别判定设备稳定性、页面稳定性、测量有效性、实体动作和校准结果。原八小时 FAIL 不重写为 PASS，未关闭项不允许宣称整机通过。
