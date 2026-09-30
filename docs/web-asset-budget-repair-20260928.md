# Web / LittleFS 发布体积门禁修复

日期：2026-09-28。目标构建：esp32s3-F16R8。

## 原因与处理

315 KiB 是 Web 发布资源的质量预算；本次失败并不表示 LittleFS 分区已满。
16MB.csv 中的 LittleFS 分区为 0x1F0000，即 2,031,616 字节。
质量门禁还分别限制触屏按需资源、base/shared、首屏和单文件体积，不能只看总包。

两条正式打包入口现在共同使用 scripts/web-gzip.js：构建电脑用固定版本
@gfx/zopfli 1.0.15 生成标准 gzip，与原 zlib level 9 比较并选择较小结果。
每个输出在写入之前必须解压并与输入逐字节一致。新增依赖仅用于构建，
不进入固件或浏览器资源，不在 ESP32 上运行压缩器。

- scripts/build-web-assets.js：发布、清理和门禁等待全部压缩完成。
- scripts/gzip-www.js：LittleFS staging 使用相同算法，等待结束后才复制资源。
- 两个入口在整个异步压缩和发布期间持有原有 Web 发布锁。
- 保留现有业务源文件、公开属性名、按需加载、缓存版本生成和所有预算。
- 局部变量改名仅做过尺寸试验；最终补丁采用 gzip 层优化。

## 实测

单位 KiB（1 KiB = 1024 B）；构建日志原有 KB 标签按此换算。

| 范围 | 修复前 | 修复后 | 预算 |
| --- | ---: | ---: | ---: |
| Web 总包 | 318.819 KiB | 306.881 KiB | 315 KiB |
| 触屏四个按需资源 | 36.895 KiB | 35.464 KiB | 35.5 KiB |
| base/shared | 281.925 KiB | 271.417 KiB | 279.5 KiB |
| 首屏 | 63.3 KiB | 60.784 KiB | 66 KiB |

首次完整回归的总包为 314,246 字节，减少 12,225 字节（约 11.94 KiB）。
重新构建会更新 index.html 和 sw.js 的缓存版本及构建上下文元数据，总大小可能有少量字节变化。
其余 52 个 gzip 资源与修复前解压 SHA-256 一致；全部 54 个资源通过每次构建的逐字节回读校验。

触屏专项预算只剩 **37 字节**，仍应严格检查后续触屏功能变更；不能用共享资源余量抵消专项超限。
本次节省的是 Flash 中的压缩资源及传输字节数，浏览器解压后的业务内容保持一致，
不据此声称设备运行时 RAM 降低。

## 验证与证据

- npm run test:web-assets：通过。包含 11 次能力组合构建、质量门禁、页面冒烟、gzip 发布与 staging 字节一致性检查。
- scripts/test-web-gzip.js：6 个输入用例，通过无损、确定性和体积不增检查。
- 真实 gzip staging 资源的触屏离线浏览器测试：18/18 通过，未启用源码替换；API 使用本地模拟。
- OTA 配置页面行为：8/8 通过；设备配置 UI 回归通过。
- 测试注册、UTF-8、资源预算元数据与仓库卫生检查通过。

证据目录：artifacts/web-size-repair-20260928。
baseline.json 保留原始体积与解压哈希；compression-result.json 记录首次比较；
web-assets.log、panel-browser.log 保留完整测试结果。
publish-lock.log 保留首次锁测试受到浏览器启动重建干扰的 ENOENT 失败。
publish-lock-serial.log 保留六个发布入口排队时触发原 30 秒测试等待上限的失败。
并发集成测试的等待时间调整为 90 秒、单进程上限 120 秒，以容纳构建端强压缩；
独立的短时限锁超时测试和生产锁策略不变。最终复测写入 publish-lock-final.log，
不覆盖旧日志。

本次目标是修复并验证本地打包；没有连接设备、刷机、重启或执行实物 OTA。

## 最终完整构建

2026-09-28 15:48（Asia/Shanghai），完整命令
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/deploy.ps1 -Env esp32s3-F16R8 -BuildOnly
成功退出：Web、LittleFS、固件以及 factory-with-fs 归档一致性检查均通过。
异步发布锁、排队、超时及恢复回归亦通过，见 publish-lock-final.log。

- 最终 Web 总量 314,261 字节（约 306.9 KiB），门禁 322,560 字节。
- 触屏专项 36,315 / 36,352 字节；base/shared 277,946 / 286,208 字节。
- 固件 3,706,256 字节，SHA-256：a0ce2d1ea07d8aa860bde950ef2143ecb63334354e3a9988ecfae8f273013338。
- littlefs.bin 为固定分区镜像长度 2,031,616 字节，不能当作资源实际占用；SHA-256：8bae9b0dbca9f04ec13eda8a3278138ceb514a2c7da0356b62c0619ffaab87db。
- .pio/build/esp32s3-F16R8/littlefs.bin 与 dist/firmware/esp32s3-F16R8/littlefs.bin 哈希一致。
- 已重新生成 dist/firmware/esp32s3-F16R8/factory-with-fs.bin，长度 16,777,216 字节。

最终结构化结果：artifacts/web-size-repair-20260928/final-summary.json。
OTA implementation-summary.json 已更新为完整本地构建通过，原 BLOCKED 结果保留在 previousWebFilesystemResult。

<!-- COM15_OTA_WEB_AND_BOOT_OFF_20260928 -->
## 后续COM15实机与启动验收

2026-09-28已完成新版本保留现场配置部署，written=true、verified=true、sourceStable=true。固件SHA256 0071a394f6fb34e835c393951f6f8a0810bdf9ddb22074de8a96ddf4de4f3df4，Web 9dc53b77bd9f9c56c158054596f7a621da2b34c2b041f4502d6b9b4e077486d8；资源314,678字节、触屏36,337字节，门槛未变。OTA配置实机10/10通过，五分钟设备/页面稳定性PASS，测量有效性仍FAIL。完整HTTP/HTTPS升级及TLS峰值仍待验证。

NC+COM通道一现已部署system_ready→逻辑off启动规则；两次软件重启从原始false回到true，其他通道和配置不变。冷上电瞬间与实际灯光仍待现场确认。新八小时观察单独运行，不拼接旧时长。

详见 [COM15实机与启动验收](com15-ota-web-device-acceptance-20260928.md)。结构化结果：artifacts/com15-ota-web-deploy-20260928/final-summary.json。此前阶段记录与失败证据保留。
