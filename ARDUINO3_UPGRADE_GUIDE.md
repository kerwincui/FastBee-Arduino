# Arduino 3.x 升级 - 编译指南

## ✅ 已完成的配置

| 配置项 | 状态 | 位置 |
|--------|------|------|
| 平台版本 | ✅ espressif32@^6.9.0 (已安装 7.0.1) | platformio.ini 第23行 |
| NimBLE-Arduino | ✅ 2.2.0 | platformio.ini 第34行 |
| ESPAsyncWebServer | ✅ ESP32Async 3.6.0 | platformio.ini 第40行 |
| AsyncTCP | ✅ ESP32Async 3.3.2 | platformio.ini 第41行 |

## 🚀 编译步骤

### 方式 1：双击批处理文件（推荐）

```
双击运行: d:\project\gitee\FastBee-Arduino\build_arduino3.bat
```

### 方式 2：PowerShell 手动执行

```powershell
# 设置代理
$env:HTTP_PROXY = "http://127.0.0.1:7897"
$env:HTTPS_PROXY = "http://127.0.0.1:7897"

# 清理锁文件
Remove-Item "$env:USERPROFILE\.platformio\packages.lock" -Force -ErrorAction SilentlyContinue

# 进入项目目录
cd d:\project\gitee\FastBee-Arduino

# 编译 ESP32
pio run -e esp32
```

### 方式 3：VS Code PlatformIO 插件

1. 点击左侧蚂蚁图标（PlatformIO）
2. 展开 Project Tasks → esp32
3. 点击 Build

## ⏱️ 预计时间

- **首次编译**: 下载框架 5-10 分钟 + 编译 2-3 分钟 = **总计 8-13 分钟**
- **后续编译**: 仅编译 = **1-2 分钟**

## ✅ 成功标志

```
Building in release mode
Compiling .pio\build\esp32\src\main.cpp.o
...
Linking .pio\build\esp32\firmware.elf
Building .pio\build\esp32\firmware.bin
RAM:   [==        ]  23.2% (used 76268 bytes from 327680 bytes)
Flash: [=======   ]  68.9% (used 1265432 bytes from 1835008 bytes)
========================= [SUCCESS] Took 143.45 seconds =========================
```

## ❌ 常见错误及解决

### 错误 1: 下载超时
```
Error: Download timeout
```
**解决**: 确保代理已启动，或稍后重试

### 错误 2: 锁文件冲突
```
Error: Lock file exists
```
**解决**: 删除锁文件后重试
```powershell
Remove-Item "$env:USERPROFILE\.platformio\packages.lock" -Force
```

### 错误 3: NimBLE API 不兼容
```
error: 'class BLEScan' has no member named 'getResults'
```
**解决**: 这是预期的，告知 AI 助手修复 API

## 📋 编译成功后

编译 ESP32 成功后，继续编译其他芯片：

```powershell
pio run -e esp32c3
pio run -e esp32s3
pio run -e esp32s2
```

## 📞 需要帮助？

编译完成后，请告诉 AI 助手：
- ✅ 成功：RAM/Flash 使用率
- ❌ 失败：错误信息截图或文本

---

**最后更新**: 2026-06-04
**平台版本**: espressif32@7.0.1
**Arduino 版本**: 3.x (ESP-IDF 5.1+)
