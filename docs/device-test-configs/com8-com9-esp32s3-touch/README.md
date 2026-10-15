# Waveshare触控屏测试配置

状态：已按本次完整Flash快照逐文件对账脱敏临时配置，尚未部署新候选；`deploymentAllowed=false`。不能把身份/快照完成或公开配置整理当作S09全部完成、设备部署或长稳通过。

- 目标环境：`esp32s3-F16R8`；端口范围：COM8 / COM9。
- 已核对COM8的bootloader芯片/MAC/16MiB Flash身份，并通过流MD5与独立Flash MD5校验；历史HTTP采样STA为`192.168.5.18`，不代表复位后的当前地址，启动槽仍需部署前核验。
- 当前配置规模：4个外设（4启用）、6条规则（1启用）。各启用状态按现场保留，没有主动加负载。
- 原公开参考：`docs/waveshare-esp32s3-touch-lcd4`（该目录已于 2026-10-15 随 docs 精简移除，历史见 git 记录；当前唯一配置见下方 config/）；本次私有来源：`.device-config-backups/four-device-20261005/com8-com9-esp32s3-touch/e83dc1f76544/serial-r1/extracted`。
- 配置：[config/](config/)；来源、逐文件SHA与验收状态：[manifest.json](manifest.json)。

## 实际配置与保全边界

当前9份运行配置（含network/protocol）均与完整Flash提取原文件脱敏对账，已无剩余模板。保留4外设/6规则且仅1规则启用、现场布局与历史配置。先前network/protocol两次HTTP超时仍保留在发现报告中；后续串口完整备份不将超时改写为通过。

公开device身份、WiFi/认证/MQTT/天气密钥已脱敏；需要部署时必须从同设备私有快照合并，不能把空值导入设备。认证/users从不进入本目录。features已取实际文件，不能重新按公开全量拓扑启用。来源SHA是完整Flash提取的私有原文件摘要，公开文件SHA另列，两者不要求相等。

[RGB构建合同](rgb-build-contract.json)保留板级16行双bounce要求；触控 RGB 固件变体自 screen-r1 起更名为 `esp32s3-F16R8-screen`（原 `esp32s3-F16R8-rgb-xip40-v12`，SDK 配置与 bounce 合同不变，历史环境与 sourceStable 字段仍指旧名，不是本轮构建证据）。系统API的PSRAM总量可能是XIP后可分配池，不能当作物理芯片容量变化。

## 变体构建链（2026-10-15 固化，2026-10-16 迁移入仓根）

该变体的唯一构建入口是 `scripts/build-screen-variant.ps1 -Candidate <名称>-r<N>`。要点：

- 变体不在 `platformio.ini` 正式矩阵内：`custom_sdkconfig`（XIP from PSRAM、`LCD_RGB_ISR_IRAM_SAFE` 等）要求独立 SDK 编译；SDK 已重编一次并冻结，每轮候选只重编应用层。
- 冻结核心目录 `fastbee-screen-core/`（仓根，独立 PlatformIO core：packages/platforms/cache，含 tool-cmake、tool-ninja、framework-espidf 与 XIP SDK 库；GB 级、已被 `.gitignore` 的 `/fastbee-*/` 覆盖不提交、**须保留不可删除**）；候选工程目录 `fastbee-screen-build/`（仓根，同不提交）。两者于 2026-10-16 从仓外 `D:/fastbee-screen-core` 与 `C:/p12` 迁入；r14 基线哈希断言见 `artifacts/rgb-xip40-v12-revalidated-r14.json`（其中绝对路径已同步更新）。若目录丢失，须按 pioarduino 55.03.38 / esp-idf v5.5.4 重装全部工具包并重走 SDK 重编与基线重验证；不能用默认 `~/.platformio` 包目录跑该构建（会因缺包触发 GitHub 下载并失败）。
- 包装脚本内部已固定：`PLATFORMIO_*` 指向仓内冻结目录、`PLATFORMIO_OFFLINE=1`（全离线，不经 GitHub）、penv 守卫（penv Scripts 置 PATH 最前、清除 `PYTHONHOME`/`PYTHONPATH`）、构建锁串行。
- 每轮候选派生两个文件：`artifacts/platformio-<Candidate>.ini`（候选环境定义，`extends = env:esp32s3-F16R8` 并追加 RGB 宏；须与 canonical 环境新增宏保持同步，如 screen-r3 补 `FASTBEE_SCREEN_LARGE_FONT=1`）与 `artifacts/build-<Candidate>.py`（冻结校验 + 源码镜像 + IRAM 符号证据 + 产物归档），构建报告为 `artifacts/<Candidate>-build.json`、产物目录 `artifacts/<Candidate>-release/`。
- 失败证据保留：重跑前把上一轮的 `artifacts/<Candidate>-build.json/.log` 移入本轮证据目录，不覆盖不删除。
- 2026-10-15 教训（防复发依据）：默认包目录缺 tool-cmake/tool-ninja 且 GitHub 不可达时构建会在 Tool Manager 阶段失败；解析错误包目录会得到 "RGB override cannot identify the active SDK memory profile"。两项都已由上面的固定入口杜绝。

完整备份报告：`artifacts/four-device-com8-snapshot-20261005-r1/report.json`；SHA256：`1372ae0d1b7de2ab9bd8bc351bbe4e90f953b3fae189be5c53c9a97b3f1800fd`；提取97个文件。此次只读备份未写入固件/FS；原HTTP快照及失败记录继续保留，不能用备份通过替代新候选部署。

## 现场前提

- COM8/COM9只计同一设备，核对Rev4.0及活动槽
- RGB480x480、GT911、16行双bounce=30720B，使用独立RGB构建合同
- 保留布局/十点历史/天气/MQTT，电机读写仍受真实接线与行程准入保护

## 下一步与恢复

按[四设备总说明](../README.md)完成私有有效配置合并、schema/能力/引脚/资源核验与S05～S07候选门禁；逐台串行写入、独立校验、配置读回和必要重启后才开始真实Web/功能测试。恢复只用此设备已校验备份，两个S3不能混用。

[低频观测说明](../monitoring.md)：健康30秒、资源60秒、页面600秒；最后2h→24h→168h。实物、浏览器、SSE、完整内存/栈及长稳分别留证，失败不可覆盖。
