# Web 优化变更记录

本文档保留 Web 体积与稳定性优化的阶段性记录。当前功能说明请优先阅读 [Web 精简版与稳定性说明](./web_refactor_plan.md)。

## 2026-05 当前状态

- 默认 Web 构建档位为精简版，classic ESP32、ESP32-C3、ESP32-S3 均优先使用该档位。
- 精简版保留设备监控、网络配置、设备配置、外设配置、外设执行、MQTT、Modbus RTU、设备控制和配置导入导出。
- 精简版移除 OTA、文件管理、日志查看、多用户/角色、RuleScript、TCP/HTTP/CoAP、独立全屏监控页等非核心资源。
- `data/www` 只保留上传硬件用的压缩产物；Web 源码统一维护在 `web-src/`。
- 构建脚本会清理精简版不需要的旧 `.gz` 产物，降低后续误传和调试混乱。

## 已完成优化

| 项目 | 当前结果 |
| --- | --- |
| Web 模块拆分 | `device-control`、`protocol`、`periph-exec` 已按核心/视图/重型功能拆分 |
| 首屏加载 | 登录后优先加载首页数据，其他页面后台低速预加载 |
| 请求分级 | `request-governor.js` 建立 `cheap`、`normal`、`heavy` 请求档位 |
| 批量缓存 | 缓存完整响应对象，避免刷新后页面只拿到 `data` |
| 配置导入 | 改为分片上传，避免大配置触发 413 |
| 精简产物 | 移除非核心页面和历史旧模块 |
| Modbus 控制 | 用户点击控制类请求提升优先级，减少被刷新请求阻塞 |
| CSS 清理 | 持续合并重复样式，并在精简构建中过滤部分完整版样式 |

## 验证方式

```powershell
node scripts/gzip-www.js --web-slim --no-upload --no-monitor
pio run -e esp32 -j 1
```

构建输出中应能看到精简 Web 文件数、gzip 总量和 staging 目录。ESP32 编译通过后，再按需要执行 LittleFS 上传和固件上传。

## 注意事项

- 不要直接编辑 `data/www/*.gz`。
- 如果 Web 页面表现异常，先确认固件和 LittleFS 是否来自同一次构建。
- 配置导入后建议重启设备，尤其是 `network.json`、`peripherals.json`、`periph_exec.json`、`protocol.json`。
- 完整版功能应放在 `esp32s3-full` 验证，不建议回塞到默认 ESP32 精简版。
