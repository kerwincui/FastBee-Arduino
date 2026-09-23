# 触控屏卷边修复复核

2026-09-23 已针对 Waveshare ESP32-S3-Touch-LCD-4（Rev4，MAC `E8:3D:C1:F7:65:44`）重新构建并通过 OTA 更新触控屏固件。

## 修复内容

- 采用此前短时稳定的 RGB 配置：12 MHz、480×480、两块 16 行 bounce buffer，合计 30,720 B。
- 构建启用 `FASTBEE_RGB_REQUIRE_BOUNCE=1`；内部 RAM 或 cache-safe SDK 条件不满足时直接拒绝初始化，禁止静默退回 direct PSRAM 扫描路径。
- 保留 `CONFIG_LCD_RGB_ISR_IRAM_SAFE`、PSRAM XIP/指令/只读数据和 64 B cache line 配置；RGB ISR ELF 符号均位于 IRAM。
- 只更新活动 OTA 应用槽，未写 LittleFS、现场配置、分区表或恢复槽。

## 证据

- 构建报告：[rgb-resume-touch-roll-edge-build.json](../artifacts/rgb-resume-touch-roll-edge-build.json)
- 固件：[rgb-resume-touch-roll-edge-release/firmware.bin](../artifacts/rgb-resume-touch-roll-edge-release/firmware.bin)
- OTA 结果：[rgb-resume-touch-roll-edge-ota.log](../artifacts/rgb-resume-touch-roll-edge-ota.log)
- 部署后只读状态：[rgb-resume-touch-roll-edge-postflight.json](../artifacts/rgb-resume-touch-roll-edge-postflight.json)
- 6 次、约 2 分钟的只读稳定性采样：[rgb-resume-touch-roll-edge-short-stability.json](../artifacts/rgb-resume-touch-roll-edge-short-stability.json)

部署后设备固件大小为 3,611,520 B，运行接口返回 `screenPanel.runtime.running=true`、时间 `synced=true`、内存守卫 `NORMAL`；DRAM 最大连续块约 25.6 KB，温湿度质量和最近十点历史均正常。当前记录仍未替代肉眼静态画面、翻页、触摸滑动、息屏唤醒和 24/72 小时实体验收；天气卡片在当前组合负载下返回“Insufficient memory for weather request”，应作为独立内存/天气任务继续处理。
