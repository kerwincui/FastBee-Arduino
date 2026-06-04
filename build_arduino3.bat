@echo off
chcp 65001 >nul
echo ========================================
echo FastBee-Arduino Arduino 3.x 升级编译
echo ========================================
echo.

echo [1/3] 设置代理...
set HTTP_PROXY=http://127.0.0.1:7897
set HTTPS_PROXY=http://127.0.0.1:7897
echo 代理已设置: %HTTP_PROXY%
echo.

echo [2/3] 清理锁文件...
if exist "%USERPROFILE%\.platformio\packages.lock" (
    del /f /q "%USERPROFILE%\.platformio\packages.lock"
    echo 锁文件已清理
) else (
    echo 无需清理
)
echo.

echo [3/3] 开始编译 ESP32...
echo （首次编译会下载框架，请耐心等待 5-10 分钟）
echo.

cd /d d:\project\gitee\FastBee-Arduino
call pio run -e esp32

echo.
echo ========================================
if %ERRORLEVEL% EQU 0 (
    echo 编译成功！
    echo.
    echo 接下来可以继续编译其他芯片：
    echo   pio run -e esp32c3
    echo   pio run -e esp32s3
    echo   pio run -e esp32s2
) else (
    echo 编译失败！请检查错误信息
)
echo ========================================
echo.
pause
