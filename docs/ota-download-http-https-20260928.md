# OTA 下载配置迁移与 HTTP/HTTPS 兼容

日期：2026-09-28。

## 配置行为

- 编辑入口改为 **设备配置 → OTA升级 → 平台 OTA 设置**，字段名为“固件下载基址（HTTP/HTTPS）”。通信协议的 MQTT 表单不再编辑或提交该字段。
- 继续使用现有的 mqtt.otaBaseUrl 存储，保留旧配置；无需迁移文件或新增一份配置。
- 基址通过现有协议配置接口独立保存，只提交 mqtt_otaBaseUrl。独立保存不规范化或重建其他协议字段，不重启 MQTT、Modbus 或设备。
- OTA 页面通过 /api/protocol/config?section=ota&compact=true 读取过滤后的基址，不读取 MQTT 凭据。读取失败时禁用保存，避免空表单清空已有配置。
- 平台 OTA 命令执行时读取已保存的基址，新设置对下一次相对路径命令生效，不依赖 MQTT 重连。
- 完整 HTTP/HTTPS 固件地址直接使用；相对路径拼接保存的基址。基址留空时继续从 MQTT Broker 主机推导 **HTTPS**，不改变默认协议。
- 基址限制 512 字符，不接受账号、密码、查询参数或片段。完整下载 URL 允许查询参数，限制 768 字符。

## 下载行为

- HTTP 只创建普通 NetworkClient，不创建 TLS 客户端，也不要求 TLS 证书校验所需的系统时间。
- HTTPS 创建 WiFiClientSecure，继续校验 CA 证书与时间。握手失败不切换到 HTTP，不使用 setInsecure。
- 在创建下载客户端之前检查现有 OTA 内存准入条件；连接后在写入前再次检查。保留 1KB 流式写入、长度校验、停滞/总时限、后台执行和成功后重启流程。
- 保留现有 HTTPClient 的同协议重定向规则；跨协议跳转会被拒绝，包括 HTTPS → HTTP。HTTP 服务器若跳转到 HTTPS，应填写最终 HTTPS 地址。
- 网络支持范围仍为现有 Wi-Fi/以太网路径；本次没有新增蜂窝 OTA。
- HTTP 没有传输加密与服务器身份校验，适合可信局域网。没有在本次修改中宣称新增固件签名验证。

## 本地验证

- 原生回归：2378/2378，通过，见 artifacts/ota-http-https-20260928/native.log。
- 设备配置 UI、导入回归通过；新增 8 项 OTA 页面行为测试通过。
- 17 项 OTA 相关 Python 测试通过，包含编译生产下载函数的 HTTP/TLS 分支、失败资源释放、低内存拒绝，以及生产参数更新片段的配置保留测试。
- MQTT 页面切换/延迟加载回归另 3 项通过，迁移后的测试使用仍在 MQTT 页面上的 Broker 字段。
- Playwright：1440px、390px、360px 下 3/3 通过，验证真实表单提交、清空恢复默认、非法基址拒绝、无横向溢出和页面错误。测试使用本地模拟 API，不连接实物设备。

## 构建与部署边界

- 首次全量 BuildOnly 在 Web/LittleFS 发布门禁被阻止：总 gzip 318.8KB > 315.0KB；screenPanel 36.9KB > 35.5KB；base/shared 281.9KB > 279.5KB。此处保留原始失败记录。
- 该结果包含本次未修改的触屏资源包超限。没有提高预算或修改其他功能来绕过门禁，见 artifacts/ota-http-https-20260928/build.log。
- 单独 ESP32-S3-F16R8 固件 BuildOnly 编译通过，二进制 3,706,256 字节，应用分区容量 7,340,032 字节。日志见 artifacts/ota-http-https-20260928/firmware-build.log。
- 本次 5 个 Web 源文件按相同 JS 压缩器与 gzip 参数独立比较，净增 798 字节；这不是运行时内存用量。详情见 artifacts/ota-http-https-20260928/web-size-evidence.json。
- 本次未刷写或重启设备，未进行实物 HTTP/HTTPS 升级或升级期间内存峰值测试；不据此宣称节省的内存数值或实物验收通过。

## 后续 Web 体积修复

2026-09-28 已统一两条打包入口的无损 gzip 压缩：首次完整 Web 回归总包降到
306.881 KiB，触屏专项降到 35.464 KiB，base/shared 降到 271.417 KiB；预算保持原值。
11 次能力组合构建和 18 项真实 gzip 资源的触屏离线浏览器测试通过。
处理方法、余量及镜像构建结果见 [Web / LittleFS 体积修复记录](web-asset-budget-repair-20260928.md)。

后续完整 esp32s3-F16R8 BuildOnly 已通过，包含 firmware.bin、littlefs.bin、
factory-with-fs.bin 和归档校验；最终 Web 总量 314,261 字节（约 306.9 KiB）。
之前的 Web/LittleFS 阻塞已解除，实物部署及 HTTP/HTTPS OTA 验收仍未执行。

<!-- COM15_OTA_WEB_AND_BOOT_OFF_20260928 -->
## 后续COM15实机与启动验收

2026-09-28已完成新版本保留现场配置部署，written=true、verified=true、sourceStable=true。固件SHA256 0071a394f6fb34e835c393951f6f8a0810bdf9ddb22074de8a96ddf4de4f3df4，Web 9dc53b77bd9f9c56c158054596f7a621da2b34c2b041f4502d6b9b4e077486d8；资源314,678字节、触屏36,337字节，门槛未变。OTA配置实机10/10通过，五分钟设备/页面稳定性PASS，测量有效性仍FAIL。完整HTTP/HTTPS升级及TLS峰值仍待验证。

NC+COM通道一现已部署system_ready→逻辑off启动规则；两次软件重启从原始false回到true，其他通道和配置不变。冷上电瞬间与实际灯光仍待现场确认。新八小时观察单独运行，不拼接旧时长。

详见 [COM15实机与启动验收](com15-ota-web-device-acceptance-20260928.md)。结构化结果：artifacts/com15-ota-web-deploy-20260928/final-summary.json。此前阶段记录与失败证据保留。
