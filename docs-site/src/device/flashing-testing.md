---
title: 烧录与部署
order: 11
---

# 烧录与部署

> PlatformIO 命令行烧录、打包和部署完整参考。

## 构建环境选择

请先根据硬件选择对应的 PlatformIO 环境。详见 [版本选择](./edition-comparison.md) 中的完整环境映射表。

## 烧录固件

```bash
# 编译并烧录（以 ESP32 为例）
pio run -e esp32-F4R0 -t upload

# 指定串口
pio run -e esp32-F4R0 -t upload --upload-port COM3

# 编译 + 烧录 + 串口监控
pio run -e esp32-F4R0 -t upload -t monitor
```

## 上传文件系统

```bash
# 构建并上传 LittleFS 镜像
pio run -e esp32-F4R0 -t uploadfs

# 指定串口
pio run -e esp32-F4R0 -t uploadfs --upload-port COM3
```

## 批量构建

```bash
# 依次编译多个环境
pio run -e esp32-F4R0
pio run -e esp32c3-F4R0
pio run -e esp32s3-F8R4
pio run -e esp32s3-F16R8
```

## 一键部署脚本

项目提供 PowerShell 部署脚本：

```powershell
# 完整部署（编译 + 上传 LittleFS + 烧录固件）
.\scripts\deploy.ps1 -Environment esp32s3-F8R4 -Port COM3

# 仅编译
.\scripts\deploy.ps1 -Environment esp32s3-F8R4 -BuildOnly

# 生成所有版本合并固件
.\scripts\build-all-artifacts.ps1
```

## 构建产物

编译完成后固件归档到 `dist/firmware/{env}/`：

```
dist/firmware/{env}/
├── factory.bin     # 完整固件（含 bootloader + 分区表）
├── firmware.bin    # 应用固件
└── partitions.bin  # 分区表
```

## 环境诊断

```bash
# 运行环境诊断
.\scripts\doctor.ps1

# 查看项目配置
pio project config

# 查看环境构建标志
pio run -e esp32-F4R0 --target envdump
```

## 验证脚本

| 脚本 | 说明 |
|------|------|
| `validate-build-matrix.js` | 全芯片编译验证 |
| `validate-config-defaults.js` | 配置默认值校验 |
| `validate-i18n.js` | 国际化完整性检查 |
| `validate-doc-links.js` | 文档链接有效性 |
| `validate-test-coverage.js` | 测试覆盖率检查 |
| `web-smoke-test.js` | Web 静态资源冒烟检查 |

> 测试相关命令请参阅 [测试验证](./testing.md)。
