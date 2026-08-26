# ESP32-S3 全功能部署验证报告（fastbee-s3-01）

**日期**：2026-08-22
**设备**：ECM50-A 工业网关（ESP32-S3，16MB Flash / 8MB PSRAM，MAC 14:C1:9F:C1:52:D8）
**固件环境**：`esp32s3-F16R8`（Full/Unified Profile，31 项功能全部 active，仅 BLE disabled）
**网络**：WiFi STA `fastbee`，IP 192.168.5.14，RSSI -55 ~ -60 dBm，mDNS `fastbee-3.local`（默认命名自动后缀）
**平台**：iot.fastbee.cn:1883，产品 2193，设备 FBE14C19FC152D8，AES 加密认证，QoS=1

---

## 1. 验收结果总表

| 验收项 | 标准 | 结果 |
|---|---|---|
| 首屏加载 | < 3s | **0.63s**（mDNS 域名，二次 0.09s）✅ |
| 登录/仪表盘 | 正常 | ✅（E2E 自动验证） |
| mDNS 解析 | TTL 响应 | ✅ `fastbee-3.local → 192.168.5.14`（舰队终排后默认命名） |
| AI×2 读数 | 稳定 | ✅ GPIO6/7，12bit，exec_001 每 60s 上报 |
| DI×2 状态 | 实时 | ✅ GPIO4/5 读数正常 |
| DO×2 控制 | Web/MQTT 可切换 | ✅ GPIO15/16 控制+回读一致 |
| Modbus 从站 | 启动无错 | ✅ Slave Addr:1 9600-8-N-1，TX17/RX18（CRC 物理验证需外部主站，注明豁免） |
| MQTT 连接 | QoS=1 稳定 | ✅ connected，lastError=0，reconnectCount=0 |
| 主题规范 | /2193/FBE14C19FC152D8/* | ✅ 7 发布 + 6 订阅全部 QoS=1 |
| 平台遥测 | 上行接收 | ✅ property/post 60s 周期 result=OK |
| 平台下行 | 指令到达 | ✅ 已收 `/ntp/get` 下行并回报 ntp_synced；继电器下行待用户平台操作 |
| 内存预算 | RAM<40% | ✅ PSRAM 8MB 启用，heap≈8.4MB，frag=2% |
| E2E | 全量通过 | ✅ full-deployment 12.9s 全过 + 证据截图 |
| Soak 24h | 无重启无泄漏 | ⏳ 21:44 起算，后台 10min 采样 `.pio/puzhong/s3-soak.csv` |

## 2. 外设配置（参照 ecm50a-networking.md）

| 外设 ID | 类型 | 引脚 | 说明 |
|---|---|---|---|
| ai_temp / ai_humi | ADC(26) | GPIO6 / 7 | 温湿度模拟量，attenuation=3，12bit |
| di_1 / di_2 | DI(11) | GPIO4 / 5 | 数字输入监测 |
| do_relay1 / do_relay2 | DO(12) | GPIO15 / 16 | 继电器，平台命令规则绑定 |
| rs485 | UART(1) | RX=18, TX=17 | Modbus RTU 总线 9600-8-N-1 |

所有引脚均不在 S3 Octal 保留列表（0,19,20,22-37）内。执行规则 3 条：
- `exec_001` 60s 定时遥测（temperature/humidity → property/post）
- `exec_010`/`exec_011` 平台命令 → DO1/DO2 切换并回报

## 3. MQTT 接入故障根因链（已全部解决）

1. **原凭据无效**：产品 5033 凭据 4 种认证组合 PC 直连全部被平台拒绝；同算法普中板凭据一次通过 → 凭据问题而非固件问题。用户改发产品 2193 凭据后验证通过。
2. **device.json 表单 handler 遮蔽 JSON handler**：`POST /api/device/config` 的表单版先匹配并把文件重置为仅 deviceName/deviceId → 改用 `/api/files/save` 直写。
3. **productNumber=0 → productId 回退 "1"**：clientId 与主题前缀错误 → 修正为 2193。
4. **ntpServer1 缺失**：AES 加密密码依赖平台时间 API → 补 `http://iot.fastbee.cn/prod-api/iot/tool/ntp`。

**平台行为发现**：向设备订阅主题直接 publish（同款或跨设备 clientId）会被平台踢下线并存在重连冷却，下行指令只能由平台后端推送；被顶掉后最快恢复手段是重启设备。

## 4. 舰队现状（2026-08-22 晚）

| 设备 | COM | IP | 状态 |
|---|---|---|---|
| S3（本次主角） | COM15 | 192.168.5.14 | ✅ 全功能在线，MQTT 已连平台（观察到两次瞬时断连，均 30s 内自动重连并重新订阅 6 主题成功；重连后 lastConnectedMs≈20ms），Soak 进行中 |
| 普中 ESP32 | COM16 | 192.168.5.34 | ✅ **已恢复（翻案）**：此前"射频物理故障"结论有误。用户指出该板历史 WiFi 正常，重刷 fs+固件（还原备份配置）后 STA 一次连上（RSSI -59），MQTT 已连平台（产品 2063）。真实根因：flash 配置损坏/驱动状态残留，非硬件故障 |
| C6 | COM11 | 192.168.5.31 | ✅ 已澄清：此前"重启循环"为 USB CDC 复位线抖动误报；RTS 复位后完整启动 18.1s，heap 158KB，RSSI -67，Web 全通。NeoPixel 运行时功能原被禁用导致 rgb_led 规则 FAILED，经 /api/features/apply 启用后 exec_001 定时变色灯 completed（status=RUNNING） |
| C3 | COM13 | 192.168.5.30 | 间歇在线（RSSI -82 弱射频），MQTT disabled 状态 |

## 5. 遗留事项

1. **S3 Soak 满 24h**（2026-08-23 21:44 后）：核对 `s3-soak.csv` uptime 单调、heap 无下行趋势、RSSI>-75、mqtt_connected=true。注意：CSV 中 uptime 字段单位为**毫秒**（21:44 重启后单调递增，中期核对无异常）。
2. **平台继电器下行**：请用户在平台控制台对产品 2193 设备下发 DO1/DO2 开关命令（exec_010/011 已绑定），观察 GPIO15/16 动作与状态回报。
3. **普中板下行验证**（pz-mqtt）：STA 已恢复（.34），待平台下发验证。
4. ~~普中板硬件检查~~：已翻案，非硬件故障（重刷恢复），无需现场检查。
5. **mDNS 同名冲突修复（已完成）**：本网络多播丢包严重，旧固件单目标 A 记录探测漏检，曾出现 3 台同时注册 fastbee。固件已改为“begin 后服务枚举预探测 + 注册后反查验证 + 运行时同名收敛（IP 大者让名）”（DNSManager.cpp），四台已重烧。**终态映射（全部默认命名，无专属域名）**：`fastbee.local → C3(.30)`、`fastbee-2.local → C6(.31)`、`fastbee-3.local → S3(.14)`，HTTP 访问均 200。普中板(.34)因内存限制（启用 mDNS 后 Web 持续 503）保持 mDNS 关闭，经 IP 访问正常（heap 50KB）；此前“射频故障”结论已翻案——重刷 fs 后 STA 一次连上（RSSI -60~-65），硬件无故障，根因为 flash 配置损坏。
   - **PZ 反复刷错镜像的根因（重要教训）**：PlatformIO 构建钩子 `scripts/fastbee-artifacts.py` 在 buildfs/uploadfs 时**从项目 `data/config` 重新生成 staging 并打包**（生成新 `.pio/fs-staging/full-*` 目录），手动修改旧 staging/run 目录无效；`$env:PLATFORMIO_DATA_DIR` 指向旧 run 目录同样无效。期间 PZ 被反复刷入 S3 配置（customDomain=fastbee-s3-01、S3 外设），表现为 uploadfs SUCCESS 但设备仍注册 fastbee-s3-01、串口出现 S3 外设告警。
   - **正确修复流程**：临时将 `data/config` 换成目标设备配置（普中备份 `config-backup-puzhong/`）→ buildfs 生成新镜像（验证 staging 内 config 正确）→ esptool 直写 LittleFS 分区 → 刷完还原 `data/config`（终态=bak-s3）。esptool 命令：`python -m esptool --chip esp32 --port COM16 --baud 460800 --before default_reset --after hard_reset write_flash 0x300000 .pio/build/esp32-F4R0/littlefs.bin`（分区 offset 见 `partitions/fastbee.csv`）。
   - **甄别镜像是否真的重打包**：对比 `.pio/build/<env>/littlefs.bin` 时间戳，uploadfs SUCCESS 但 bin 时间戳未变即为未重打包。
   - **烧录后不自启**：esptool/uploadfs 后设备可能停在 boot 态，用 pyserial RTS 脉冲复位（`s.dtr=False; s.rts=True; sleep(0.1); s.rts=False`）恢复。
6. Modbus 物理层 CRC 验证需外部主站硬件（从站侧无回环手段）。

## 6. 证据文件索引（.pio/puzhong/）

- `s3-boot1.log` / `s3-telemetry.log` / `s3-mqtt-diag.log`：串口启动、遥测、MQTT 日志
- `s3-soak.csv`：Soak 采样（10min 间隔，后台持续）；`s3-soak-flaps.csv`：MQTT 闪断事件自行累计（设备侧 reconnectCount 稳定 5 分钟后清零，不可作为闪断频率依据）；监控脚本 22:16 已升级重启（mqtt 列超时重试修复）
- `s3-peripherals.json` / `s3-protocol.json` / `s3-netstatus.json` 等：API 快照
- `config-backup-puzhong/`：普中板配置备份（8 文件）；`config.bak-s3/`：S3 配置备份（data/config 还原源）
- PZ mDNS 修复过程：`mdns-final-reorder.ps1`、`mdns-final-round2.ps1`（编排脚本，ASCII 注释）、`pz-buildfs2.log`、`pz-esptool-fs2.log`（esptool 直写成功：Hash verified + hard reset）、`pz-boot6.log`（串口抓包，用于甄别刷入的错误镜像）
- E2E 证据截图：`.pio/puzhong/` evidence 输出
- 修复脚本链：`s3-mqtt-fix2~fix6.ps1`、`s3-mqtt-pc-test*.js`（PC 交叉验证）、`s3-downlink-pub2.js`

---
*报告生成：2026-08-22；Soak 结论与平台下行验证结果将于后续补充。*
